/**
 * Copyright (c) 2015, Willem-Hendrik Thiart
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <uv.h>
#include "kv_db.h"
#include "options.h"
#include "lmdb.h"
#include "lmdb_helpers.h"
#include "raft.h"
#include "uv_helpers.h"
#include "uv_multiplex.h"
#include "tpl.h"
#include "arraytools.h"

#include "usage.c"
#include "parse_addr.c"

#define VERSION "0.1.0"
#define ANYPORT 65535
#define MAX_PEER_CONNECTIONS 128
#define IPV4_STR_LEN 3 * 4 + 3 + 1
#define PERIOD_MSEC 1000
#define RAFT_BUFLEN 512
#define LEADER_URL_LEN 512
#define IPC_PIPE_NAME "ticketd_ipc"
#define IP_STR_LEN 16

typedef enum
{
    SCHEMA_ENTRIES_IDX = 0,
    SCHEMA_DOCS_IDX,
    SCHEMA_STATE_IDX,
} sys_schema_meta_index_e;
static char *sys_schemas_meta[] = {
    "entries",
    "docs",
    "state",
};

typedef enum
{
    HANDSHAKE_FAILURE,
    HANDSHAKE_SUCCESS,
} handshake_state_e;

/** Message types used for peer to peer traffic
 * These values are used to identify message types during deserialization */
typedef enum
{
    /** Handshake is a special non-raft message type
     * We send a handshake so that we can identify ourselves to our peers */
    MSG_HANDSHAKE,
    /** Successful responses mean we can start the Raft periodic callback */
    MSG_HANDSHAKE_RESPONSE,
    /** Tell leader we want to leave the cluster */
    /* When instance is ctrl-c'd we have to gracefuly disconnect */
    MSG_LEAVE,
    /* Receiving a leave response means we can shutdown */
    MSG_LEAVE_RESPONSE,
    MSG_REQUESTVOTE,
    MSG_REQUESTVOTE_RESPONSE,
    MSG_APPENDENTRIES,
    MSG_APPENDENTRIES_RESPONSE,
} peer_message_type_e;

/** Peer protocol handshake
 * Send handshake after connecting so that our peer can identify us */
typedef struct
{
    int raft_port;
    int node_id;
} msg_handshake_t;

typedef struct
{
    int success;

    /* leader's Raft port */
    int leader_port;

    /* my Raft node ID.
     * Sometimes we don't know who we did the handshake with */
    int node_id;

    char leader_host[IP_STR_LEN];
} msg_handshake_response_t;

/** Add/remove Raft peer */
typedef struct
{
    int raft_port;
    int node_id;
    char host[IP_STR_LEN];
} entry_cfg_change_t;

typedef struct
{
    int type;
    union
    {
        msg_handshake_t hs;
        msg_handshake_response_t hsr;
        msg_requestvote_t rv;
        msg_requestvote_response_t rvr;
        msg_appendentries_t ae;
        msg_appendentries_response_t aer;
    };
    int padding[100];
} msg_t;

typedef enum
{
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
} conn_status_e;

typedef struct peer_connection_s peer_connection_t;

struct peer_connection_s
{
    /* peer's address */
    struct sockaddr_in addr;

    int raft_port;

    /* gather TPL message */
    tpl_gather_t *gt;

    /* tell if we need to connect or not */
    conn_status_e connection_status;

    /* peer's raft node_idx */
    raft_node_t *node;

    /* number of entries currently expected.
     * this counts down as we consume entries */
    int n_expected_entries;

    /* remember most recent append entries msg, we refer to this msg when we
     * finish reading the log entries.
     * used in tandem with n_expected_entries */
    msg_t ae;

    uv_stream_t *stream;

    uv_loop_t *loop;

    peer_connection_t *next;
};

typedef struct
{
    /* the server's node ID */
    int node_id;

    raft_server_t *raft;

    /* Set of tickets that have been issued
     * We store unsigned ints in here */
    MDB_dbi tickets;

    /* Persistent state for voted_for and term
     * We store string keys (eg. "term") with int values */
    MDB_dbi state;

    /* Entries that have been appended to our log
     * For each log entry we store two things next to each other:
     *  - TPL serialized raft_entry_t
     *  - raft_entry_data_t */
    MDB_dbi entries;

    /* LMDB database environment */
    MDB_env *db_env;

    /* Raft isn't multi-threaded, therefore we use a global lock */
    uv_mutex_t raft_lock;

    /* When we receive an entry from the client we need to block until the
     * entry has been committed. This condition is used to wake us up. */
    uv_cond_t appendentries_received;

    uv_loop_t peer_loop;

    /* Link list of peer connections */
    peer_connection_t *conns;

    /*  add wiretiger engine */
    kv_db_t *db;
} server_t;

options_t opts;
server_t server;
server_t *sv = &server;

static peer_connection_t *new_connection(server_t *sv);
static void connect_to_peer(peer_connection_t *conn);
static void connection_set_peer(peer_connection_t *conn, char *host, int port);
static void connect_to_peer_at_host(peer_connection_t *conn, char *host, int port);
static void start_raft_periodic_timer(server_t *sv);
static int send_handshake_response(peer_connection_t *conn,
                                     handshake_state_e success,
                                     raft_node_t *leader);
static int send_leave_response(peer_connection_t *conn);

static void drop_kv_db(server_t *sv);

/** Serialize a peer message using TPL
 * @param[out] bufs libuv buffer to insert serialized message into
 * @param[out] buf Buffer to write serialized message into */
static size_t peer_msg_serialize(tpl_node *tn, uv_buf_t *buf, char *data)
{
    size_t sz;
    tpl_pack(tn, 0);
    tpl_dump(tn, TPL_GETSIZE, &sz);
    tpl_dump(tn, TPL_MEM | TPL_PREALLOCD, data, RAFT_BUFLEN);
    tpl_free(tn);
    buf->len = sz;
    buf->base = data;
    return sz;
}

static void peer_msg_send(uv_stream_t *s, tpl_node *tn, uv_buf_t *buf, char *data)
{
    peer_msg_serialize(tn, buf, data);
    int e = uv_try_write(s, buf, 1);
    if (e < 0)
        uv_fatal(e);
}

/** Check if the ticket has already been issued
 * @return 0 if not unique; otherwise 1 */
static int check_if_ticket_exists(kv_db_t *db,const unsigned int *ticket)

    void *ptr = kv_db_get(db,sys_schemas_meta[SCHEMA_DOCS_IDX],"ticket",6);
    if(ptr==NULL) {
        return 0;
    }
    ticket =(int *)ptr;
    return 1;
}

static unsigned int generate_ticket(server_t *sv)
{
    unsigned int ticket;
    kv_db_t *db = sv->db;
    
    kv_schema_t *schema = kv_db_fetch_schema(db,sys_schemas_meta[SCHEMA_DOCS]);
    do
    {
        // TODO need better random number generator
        ticket = rand();
    } while (check_if_ticket_exists(db,&ticket));
    return ticket;
}

/*

static int __http_get_id(h2o_handler_t *self, h2o_req_t *req)
{
    static h2o_generator_t generator = { NULL, NULL };

    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("POST")))
        return -1;

    raft_node_t* leader = raft_get_current_leader_node(sv->raft);
    if (!leader)
        return h2oh_respond_with_error(req, 503, "Leader unavailable");
    else if (raft_node_get_id(leader) != sv->node_id)
    {
        peer_connection_t* leader_conn = raft_node_get_udata(leader);
        char leader_url[LEADER_URL_LEN];
        static h2o_generator_t generator = { NULL, NULL };
        static h2o_iovec_t body = { .base = "", .len = 0 };
        req->res.status = 301;
        req->res.reason = "Moved Permanently";
        h2o_start_response(req, &generator);
        snprintf(leader_url, LEADER_URL_LEN, "http://%s:%d/",
                 inet_ntoa(leader_conn->addr.sin_addr),
                 leader_conn->http_port);
        h2o_add_header(&req->pool,
                       &req->res.headers,
                       H2O_TOKEN_LOCATION,
                       NULL,
                       leader_url,
                       strlen(leader_url));
        h2o_send(req, &body, 1, 1);
        return 0;
    }

    int e;

    unsigned int ticket = generate_ticket();

    msg_entry_t entry = {};
    entry.id = rand();
    entry.data.buf = (void*)&ticket;
    entry.data.len = sizeof(ticket);

    uv_mutex_lock(&sv->raft_lock);

    msg_entry_response_t r;
    e = raft_recv_entry(sv->raft, &entry, &r);
    if (0 != e)
        return h2oh_respond_with_error(req, 500, "BAD");

    int done = 0, tries = 0;
    do
    {
        if (3 < tries)
        {
            printf("ERROR: failed to commit entry\n");
            uv_mutex_unlock(&sv->raft_lock);
            return h2oh_respond_with_error(req, 400, "TRY AGAIN");
        }

        uv_cond_wait(&sv->appendentries_received, &sv->raft_lock);
        e = raft_msg_entry_response_committed(sv->raft, &r);
        tries += 1;
        switch (e)
        {
        case 0:
            break;
        case 1:
            done = 1;
            uv_mutex_unlock(&sv->raft_lock);
            break;
        case -1:
            uv_mutex_unlock(&sv->raft_lock);
            return h2oh_respond_with_error(req, 400, "TRY AGAIN");
        }
    }
    while (!done);

    char id_str[100];
    h2o_iovec_t body;
    sprintf(id_str, "%d", entry.id);
    body = h2o_iovec_init(id_str, strlen(id_str));

    req->res.status = 200;
    req->res.reason = "OK";
    h2o_start_response(req, &generator);
    h2o_send(req, &body, 1, 1);
    return 0;
}
 */

/** Initiate connection if we are disconnected */
static int connect_if_needed(peer_connection_t *conn)
{
    if (CONNECTED != conn->connection_status)
    {
        if (DISCONNECTED == conn->connection_status)
            connect_to_peer(conn);
        return -1;
    }
    return 0;
}

/** Raft callback for sending request vote message */
static int raft_send_requestvote_func(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node,
    msg_requestvote_t *m)
{
    peer_connection_t *conn = raft_node_get_udata(node);

    int e = connect_if_needed(conn);
    if (-1 == e)
        return 0;

    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg;
    msg.type = MSG_REQUESTVOTE,
    msg.rv = *m;
    peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), bufs, buf);
    return 0;
}

/** Raft callback for sending appendentries message */
static int raft_send_appendentries_func(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node,
    msg_appendentries_t *m)
{
    uv_buf_t bufs[3];
    peer_connection_t *conn = raft_node_get_udata(node);

    int e = connect_if_needed(conn);
    if (-1 == e)
        return 0;

    char buf[RAFT_BUFLEN], *ptr = buf;
    msg_t msg = {};
    msg.type = MSG_APPENDENTRIES;
    msg.ae.term = m->term;
    msg.ae.prev_log_idx = m->prev_log_idx;
    msg.ae.prev_log_term = m->prev_log_term;
    msg.ae.leader_commit = m->leader_commit;
    msg.ae.n_entries = m->n_entries;
    ptr += peer_msg_serialize(tpl_map("S(I$(IIIII))", &msg), bufs, ptr);

    /* appendentries with payload */
    if (0 < m->n_entries)
    {
        tpl_bin tb = {
            .sz = m->entries[0].data.len,
            .addr = m->entries[0].data.buf};

        /* list of entries */
        tpl_node *tn = tpl_map("IIIB",
                               &m->entries[0].id,
                               &m->entries[0].term,
                               &m->entries[0].type,
                               &tb);
        size_t sz;
        tpl_pack(tn, 0);
        tpl_dump(tn, TPL_GETSIZE, &sz);
        e = tpl_dump(tn, TPL_MEM | TPL_PREALLOCD, ptr, RAFT_BUFLEN);
        assert(0 == e);
        bufs[1].len = sz;
        bufs[1].base = ptr;
        e = uv_try_write(conn->stream, bufs, 2);
        if (e < 0) {
            uv_fatal(e);

        }

        tpl_free(tn);
    }
    else
    {
        /* keep alive appendentries only */
        e = uv_try_write(conn->stream, bufs, 1);
        if (e < 0)
            uv_fatal(e);
    }

    return 0;
}

static void __delete_connection(server_t *sv, peer_connection_t *conn)
{
    peer_connection_t *prev = NULL;
    if (sv->conns == conn)
        sv->conns = conn->next;
    else if (sv->conns != conn)
    {
        for (prev = sv->conns; prev->next != conn; prev = prev->next)
            ;
        prev->next = conn->next;
    }
    else
        assert(0);

    if (conn->node)
        raft_node_set_udata(conn->node, NULL);

    // TODO: make sure all resources are freed
    free(conn);
}

static peer_connection_t *find_connection(server_t *sv, const char *host, int raft_port)
{
    peer_connection_t *conn;
    for (conn = sv->conns;
         conn && (0 != strcmp(host, inet_ntoa(conn->addr.sin_addr)) ||
                  conn->raft_port != raft_port);
         conn = conn->next)
        ;
    return conn;
}

static int offer_cfg_change(server_t *sv,
                              raft_server_t *raft,
                              const unsigned char *data,
                              raft_logtype_e change_type)
{
    entry_cfg_change_t *change = (void *)data;
    peer_connection_t *conn = find_connection(sv, change->host, change->raft_port);

    /* Node is being removed */
    if (RAFT_LOGTYPE_REMOVE_NODE == change_type)
    {
        raft_remove_node(raft, raft_get_node(sv->raft, change->node_id));
        if (conn)
            conn->node = NULL;
        /* __delete_connection(sv, conn); */
        return 0;
    }

    /* Node is being added */
    if (!conn)
    {
        conn = new_connection(sv);
        connection_set_peer(conn, change->host, change->raft_port);
    }

    int is_self = change->node_id == sv->node_id;

    switch (change_type)
    {
    case RAFT_LOGTYPE_ADD_NONVOTING_NODE:
        conn->node = raft_add_non_voting_node(raft, conn, change->node_id, is_self);
        break;
    case RAFT_LOGTYPE_ADD_NODE:
        conn->node = raft_add_node(raft, conn, change->node_id, is_self);
        break;
    default:
        assert(0);
    }

    raft_node_set_udata(conn->node, conn);

    return 0;
}

/** Raft callback for applying an entry to the finite state machine */
static int raft_applylog_func(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *ety)
{

    /* Check if it's a configuration change */
    if (raft_entry_is_cfg_change(ety))
    {
        entry_cfg_change_t *change = ety->data.buf;
        if (RAFT_LOGTYPE_REMOVE_NODE != ety->type || !raft_is_leader(sv->raft))
            goto commit;

        peer_connection_t *conn = find_connection(sv, change->host, change->raft_port);
        send_leave_response(conn);
        goto commit;
    }
    kv_db_t *db = sv->db;
    kv_db_set(db,sys_schemas_meta[SCHEMA_DOCS_IDX], ety->data.buf, ety->data.len,"\0",0);
commit:
    /* We save the commit idx for performance reasons.
     * Note that Raft doesn't require this as it can figure it out itself. */
    int commit_idx = raft_get_commit_idx(raft);
    kv_db_set(db,sys_schemas_meta[SCHEMA_STATE_IDX], "commit_idx",10, &commit_idx,sizeof(int));


    return 0;
}

/** Raft callback for saving term field to disk.
 * This only returns when change has been made to disk. */
static int raft_persist_term_func(
    raft_server_t *raft,
    void *udata,
    const int current_term)
{
    kv_db_t *db = sv->db;
    return kv_db_set(db,sys_schemas_meta[SCHEMA_STATE_IDX], "term",4, &current_term,sizeof(int));

}

/** Raft callback for saving voted_for field to disk.
 * This only returns when change has been made to disk. */
static int raft_persist_vote_func(
    raft_server_t *raft,
    void *udata,
    const int voted_for)
{
    
    kv_db_t *db = sv->db;
    return kv_db_set(db,sys_schemas_meta[SCHEMA_STATE_IDX], "voted_for",9, &voted_for,sizeof(int)); 
}

static void peer_alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf)
{
    buf->len = size;
    buf->base = malloc(size);
}

static int append_cfg_change(server_t *sv,
                               raft_logtype_e change_type,
                               char *host,
                               int raft_port,
                               int node_id)
{
    entry_cfg_change_t *change = calloc(1, sizeof(*change));
    change->raft_port = raft_port;
    change->node_id = node_id;
    strcpy(change->host, host);
    change->host[IP_STR_LEN - 1] = 0;

    msg_entry_t entry;
    entry.id = rand();
    entry.data.buf = (void *)change;
    entry.data.len = sizeof(*change);
    entry.type = change_type;
    msg_entry_response_t r;
    int e = raft_recv_entry(sv->raft, &entry, &r);
    if (0 != e)
        return -1;
    return 0;
}

/** Deserialize a single log entry from appendentries message */
static void deserialize_appendentries_payload(msg_entry_t *out,
                                                peer_connection_t *conn,
                                                void *img,
                                                size_t sz)
{
    tpl_bin tb;
    tpl_node *tn = tpl_map(tpl_peek(TPL_MEM, img, sz),
                           &out->id,
                           &out->term,
                           &out->type,
                           &tb);
    tpl_load(tn, TPL_MEM, img, sz);
    tpl_unpack(tn, 0);
    out->data.buf = tb.addr;
    out->data.len = tb.sz;
}

/** Parse raft peer traffic using binary protocol, and respond to message */
static int deserialize_and_handle_msg(void *img, size_t sz, void *data)
{
    peer_connection_t *conn = data;
    msg_t m;
    int e;

    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];

    /* special case: handle appendentries payload */
    if (0 < conn->n_expected_entries)
    {
        msg_entry_t entry;

        deserialize_appendentries_payload(&entry, conn, img, sz);

        conn->ae.ae.entries = &entry;
        msg_t msg = {.type = MSG_APPENDENTRIES_RESPONSE};
        e = raft_recv_appendentries(sv->raft, conn->node, &conn->ae.ae, &msg.aer);

        /* send response */
        uv_buf_t bufs[1];
        char buf[RAFT_BUFLEN];
        peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), bufs, buf);

        conn->n_expected_entries = 0;
        return 0;
    }

    /* deserialize message */
    tpl_node *tn = tpl_map(tpl_peek(TPL_MEM, img, sz), &m);
    tpl_load(tn, TPL_MEM, img, sz);
    tpl_unpack(tn, 0);

    switch (m.type)
    {
    case MSG_HANDSHAKE:
    {
        peer_connection_t *nconn = find_connection(
            sv, inet_ntoa(conn->addr.sin_addr), m.hs.raft_port);
        if (nconn && conn != nconn)
            __delete_connection(sv, nconn);

        conn->connection_status = CONNECTED;
        conn->raft_port = m.hs.raft_port;

        raft_node_t *leader = raft_get_current_leader_node(sv->raft);

        /* Is this peer in our configuration already? */
        raft_node_t *node = raft_get_node(sv->raft, m.hs.node_id);
        if (node)
        {
            raft_node_set_udata(node, conn);
            conn->node = node;
        }

        if (!leader)
        {
            return send_handshake_response(conn, HANDSHAKE_FAILURE, NULL);
        }
        else if (raft_node_get_id(leader) != sv->node_id)
        {
            return send_handshake_response(conn, HANDSHAKE_FAILURE, leader);
        }
        else if (node)
        {
            return send_handshake_response(conn, HANDSHAKE_SUCCESS, NULL);
        }
        else
        {
            int e = append_cfg_change(sv, RAFT_LOGTYPE_ADD_NONVOTING_NODE,
                                        inet_ntoa(conn->addr.sin_addr),
                                        m.hs.raft_port, m.hs.node_id);
            if (0 != e)
                return send_handshake_response(conn, HANDSHAKE_FAILURE, NULL);
            return send_handshake_response(conn, HANDSHAKE_SUCCESS, NULL);
        }
    }
    break;
    case MSG_HANDSHAKE_RESPONSE:
        if (0 == m.hsr.success)
        {

            /* We're being redirected to the leader */
            if (m.hsr.leader_port)
            {
                peer_connection_t *nconn =
                    find_connection(sv, m.hsr.leader_host, m.hsr.leader_port);
                if (!nconn)
                {
                    nconn = new_connection(sv);
                    printf("Redirecting to %s:%d...\n",
                           m.hsr.leader_host, m.hsr.leader_port);
                    connect_to_peer_at_host(nconn, m.hsr.leader_host,
                                              m.hsr.leader_port);
                }
            }
        }
        else
        {
            printf("Connected to leader: %s:%d\n",
                   inet_ntoa(conn->addr.sin_addr), conn->raft_port);
            if (!conn->node)
                conn->node = raft_get_node(sv->raft, m.hsr.node_id);
        }
        break;
    case MSG_LEAVE:
    {
        if (!conn->node)
        {
            printf("ERROR: no node\n");
            return 0;
        }
        int e = append_cfg_change(sv, RAFT_LOGTYPE_REMOVE_NODE,
                                    inet_ntoa(conn->addr.sin_addr),
                                    conn->raft_port,
                                    raft_node_get_id(conn->node));
        if (0 != e)
            printf("ERROR: Leave request failed\n");
    }
    break;
    case MSG_LEAVE_RESPONSE:
        drop_kv_db(sv);
        printf("Shutdown complete. Quitting...\n");
        exit(0);
        break;
    case MSG_REQUESTVOTE:
    {
        msg_t msg = {.type = MSG_REQUESTVOTE_RESPONSE};
        e = raft_recv_requestvote(sv->raft, conn->node, &m.rv, &msg.rvr);
        peer_msg_send(conn->stream, tpl_map("S(I$(II))", &msg), bufs, buf);
    }
    break;
    case MSG_REQUESTVOTE_RESPONSE:
        e = raft_recv_requestvote_response(sv->raft, conn->node, &m.rvr);
        break;
    case MSG_APPENDENTRIES:
        /* special case: get ready to handle appendentries payload */
        if (0 < m.ae.n_entries)
        {
            conn->n_expected_entries = m.ae.n_entries;
            memcpy(&conn->ae, &m, sizeof(msg_t));
            return 0;
        }

        /* this is a keep alive message */
        msg_t msg = {.type = MSG_APPENDENTRIES_RESPONSE};
        e = raft_recv_appendentries(sv->raft, conn->node, &m.ae, &msg.aer);
        peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), bufs, buf);
        break;
    case MSG_APPENDENTRIES_RESPONSE:
        e = raft_recv_appendentries_response(sv->raft, conn->node, &m.aer);
        uv_cond_signal(&sv->appendentries_received);
        break;
    default:
        printf("unknown msg\n");
        exit(0);
    }
    return 0;
}

/** Read raft traffic using binary protocol */
static void peer_read_cb(uv_stream_t *tcp, ssize_t nread, const uv_buf_t *buf)
{
    peer_connection_t *conn = tcp->data;

    if (nread < 0)
        switch (nread)
        {
        case UV__EOF:
            conn->connection_status = DISCONNECTED;
            return;
        default:
            uv_fatal(nread);
        }

    if (0 <= nread)
    {
        assert(conn);
        uv_mutex_lock(&sv->raft_lock);
        tpl_gather(TPL_GATHER_MEM, buf->base, nread, &conn->gt,
                   deserialize_and_handle_msg, conn);
        uv_mutex_unlock(&sv->raft_lock);
    }
}

static void send_leave(peer_connection_t *conn)
{
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg = {};
    msg.type = MSG_LEAVE;
    peer_msg_send(conn->stream, tpl_map("S(I)", &msg), &bufs[0], buf);
}

static void send_handshake(peer_connection_t *conn)
{
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg = {};
    msg.type = MSG_HANDSHAKE;
    msg.hs.raft_port = atoi(opts.raft_port);
    msg.hs.node_id = sv->node_id;
    peer_msg_send(conn->stream, tpl_map("S(I$(IIII))", &msg), &bufs[0], buf);
}

static int send_leave_response(peer_connection_t *conn)
{
    if (!conn)
    {
        printf("no connection??\n");
        return -1;
    }
    if (!conn->stream)
        return -1;
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    msg_t msg = {};
    msg.type = MSG_LEAVE_RESPONSE;
    peer_msg_send(conn->stream, tpl_map("S(I)", &msg), &bufs[0], buf);
    return 0;
}

static int send_handshake_response(peer_connection_t *conn,
                                     handshake_state_e success,
                                     raft_node_t *leader)
{
    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];

    msg_t msg = {};
    msg.type = MSG_HANDSHAKE_RESPONSE;
    msg.hsr.success = success;
    msg.hsr.leader_port = 0;
    msg.hsr.node_id = sv->node_id;

    /* allow the peer to redirect to the leader */
    if (leader)
    {
        peer_connection_t *leader_conn = raft_node_get_udata(leader);
        if (leader_conn)
        {
            msg.hsr.leader_port = leader_conn->raft_port;
            snprintf(msg.hsr.leader_host, IP_STR_LEN, "%s",
                     inet_ntoa(leader_conn->addr.sin_addr));
        }
    }

    peer_msg_send(conn->stream, tpl_map("S(I$(IIIIs))", &msg), bufs, buf);

    return 0;
}

/** Raft peer has connected to us.
 * Add them to our list of nodes */
static void on_peer_connection(uv_stream_t *listener, const int status)
{
    int e;

    if (0 != status)
        uv_fatal(status);

    uv_tcp_t *tcp = calloc(1, sizeof(uv_tcp_t));
    e = uv_tcp_init(listener->loop, tcp);
    if (0 != e)
        uv_fatal(e);

    e = uv_accept(listener, (uv_stream_t *)tcp);
    if (0 != e)
        uv_fatal(e);

    peer_connection_t *conn = new_connection(sv);
    conn->node = NULL;
    conn->loop = listener->loop;
    conn->stream = (uv_stream_t *)tcp;
    tcp->data = conn;

    int namelen = sizeof(conn->addr);
    e = uv_tcp_getpeername(tcp, (struct sockaddr *)&conn->addr, &namelen);
    if (0 != e)
        uv_fatal(e);

    e = uv_read_start((uv_stream_t *)tcp, peer_alloc_cb, peer_read_cb);
    if (0 != e)
        uv_fatal(e);
}

/** Our connection attempt to raft peer has succeeded */
static void on_connection_accepted_by_peer(uv_connect_t *req,
                                             const int status)
{
    peer_connection_t *conn = req->data;
    int e;

    switch (status)
    {
    case 0:
        break;
    case -ECONNREFUSED:
        return;
    default:
        uv_fatal(status);
    }

    send_handshake(conn);

    int nlen = sizeof(conn->addr);
    e = uv_tcp_getpeername((uv_tcp_t *)req->handle, (struct sockaddr *)&conn->addr, &nlen);
    if (0 != e)
        uv_fatal(e);

    /* start reading from peer */
    conn->connection_status = CONNECTED;
    e = uv_read_start(conn->stream, peer_alloc_cb, peer_read_cb);
    if (0 != e) {
        uv_fatal(e);
    }
}

static peer_connection_t *new_connection(server_t *sv)
{
    peer_connection_t *conn = calloc(1, sizeof(peer_connection_t));
    conn->loop = &sv->peer_loop;
    conn->next = sv->conns;
    sv->conns = conn;
    return conn;
}

/** Connect to raft peer */
static void connect_to_peer(peer_connection_t *conn)
{
    int e;

    uv_tcp_t *tcp = calloc(1, sizeof(uv_tcp_t));
    tcp->data = conn;
    e = uv_tcp_init(conn->loop, tcp);
    if (0 != e)
        uv_fatal(e);

    conn->stream = (uv_stream_t *)tcp;
    conn->connection_status = CONNECTING;

    uv_connect_t *c = calloc(1, sizeof(uv_connect_t));
    c->data = conn;

    e = uv_tcp_connect(c, (uv_tcp_t *)conn->stream,
                       (struct sockaddr *)&conn->addr,
                       on_connection_accepted_by_peer);
    if (0 != e)
        uv_fatal(e);
}

static void connection_set_peer(peer_connection_t *conn, char *host, int port)
{
    conn->raft_port = port;
    printf("Connecting to %s:%d\n", host, port);
    int e = uv_ip4_addr(host, port, &conn->addr);
    if (0 != e)
        uv_fatal(e);
}

static void connect_to_peer_at_host(peer_connection_t *conn, char *host,
                                      int port)
{
    connection_set_peer(conn, host, port);
    connect_to_peer(conn);
}

/** Raft callback for displaying debugging information */
void raft_log_func(raft_server_t *raft, raft_node_t *node, void *udata,
                const char *buf)
{
    if (opts.debug)
        printf("raft: %s\n", buf);
}

/** Raft callback for appending an item to the log */
static int raft_logentry_offer_func(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *ety,
    int ety_idx)
{
    MDB_txn *txn;

    if (raft_entry_is_cfg_change(ety))
        offer_cfg_change(sv, raft, ety->data.buf, ety->type);

    int e = mdb_txn_begin(sv->db_env, NULL, 0, &txn);
    if (0 != e)
        mdb_fatal(e);

    uv_buf_t bufs[1];
    char buf[RAFT_BUFLEN];
    peer_msg_serialize(tpl_map("S(III)", ety), bufs, buf);

    /* 1. put metadata */
    ety_idx <<= 1;
    MDB_val key = {.mv_size = sizeof(ety_idx), .mv_data = (void *)&ety_idx};
    MDB_val val = {.mv_size = bufs->len, .mv_data = bufs->base};

    e = mdb_put(txn, sv->entries, &key, &val, 0);
    switch (e)
    {
    case 0:
        break;
    case MDB_MAP_FULL:
    {
        mdb_txn_abort(txn);
        return -1;
    }
    default:
        mdb_fatal(e);
    }

    /* 2. put entry */
    ety_idx |= 1;
    key.mv_size = sizeof(ety_idx);
    key.mv_data = (void *)&ety_idx;
    val.mv_size = ety->data.len;
    val.mv_data = ety->data.buf;

    e = mdb_put(txn, sv->entries, &key, &val, 0);
    switch (e)
    {
    case 0:
        break;
    case MDB_MAP_FULL:
    {
        mdb_txn_abort(txn);
        return -1;
    }
    default:
        mdb_fatal(e);
    }

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);

    /* So that our entry points to a valid buffer, get the mmap'd buffer.
     * This is because the currently pointed to buffer is temporary. */
    e = mdb_txn_begin(sv->db_env, NULL, 0, &txn);
    if (0 != e)
        mdb_fatal(e);

    e = mdb_get(txn, sv->entries, &key, &val);
    switch (e)
    {
    case 0:
        break;
    default:
        mdb_fatal(e);
    }
    ety->data.buf = val.mv_data;
    ety->data.len = val.mv_size;

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);

    return 0;
}

/** Raft callback for removing the first entry from the log
 * @note this is provided to support log compaction in the future */
static int raft_logentry_poll_func(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *entry,
    int ety_idx)
{
    MDB_val k, v;

    mdb_poll(sv->db_env, sv->entries, &k, &v);

    return 0;
}

/** Raft callback for deleting the most recent entry from the log.
 * This happens when an invalid leader finds a valid leader and has to delete
 * superseded log entries. */
static int raft_logentry_pop_func(
    raft_server_t *raft,
    void *udata,
    raft_entry_t *entry,
    int ety_idx)
{
    MDB_val k, v;

    mdb_pop(sv->db_env, sv->entries, &k, &v);

    return 0;
}

/** Non-voting node now has enough logs to be able to vote.
 * Append a finalization cfg log entry. */
static void raft_node_has_sufficient_logs_func(
    raft_server_t *raft,
    void *user_data,
    raft_node_t *node)
{
    peer_connection_t *conn = raft_node_get_udata(node);
    append_cfg_change(sv, RAFT_LOGTYPE_ADD_NODE,
                        inet_ntoa(conn->addr.sin_addr),
                        conn->raft_port,
                        raft_node_get_id(conn->node));
}

raft_cbs_t raft_funcs = {
    .send_requestvote = raft_send_requestvote_func,
    .send_appendentries = raft_send_appendentries_func,
    .applylog = raft_applylog_func,
    .persist_vote = raft_persist_vote_func,
    .persist_term = raft_persist_term_func,
    .log_offer = raft_logentry_offer_func,
    .log_poll = raft_logentry_poll_func,
    .log_pop = raft_logentry_pop_func,
    .node_has_sufficient_logs = raft_node_has_sufficient_logs_func,
    .log = raft_log_func,
};

/** Raft callback for handling periodic logic */
static void __periodic(uv_timer_t *handle)
{
    uv_mutex_lock(&sv->raft_lock);

    raft_periodic(sv->raft, PERIOD_MSEC);

    if (opts.type_info.type == OPTION_LEAVE)
    {
        raft_node_t *leader = raft_get_current_leader_node(sv->raft);
        if (leader)
        {
            peer_connection_t *leader_conn = raft_node_get_udata(leader);
            assert(raft_node_get_id(leader) != sv->node_id);
            send_leave(leader_conn);
        }
    }

    raft_apply_all(sv->raft);

    uv_mutex_unlock(&sv->raft_lock);
}

/** Load all log entries we have persisted to disk */
static void load_commit_log(server_t *sv)
{
    MDB_cursor *curs;
    MDB_txn *txn;
    MDB_val k, v;
    int e;

    e = mdb_txn_begin(sv->db_env, NULL, MDB_RDONLY, &txn);
    if (0 != e)
        mdb_fatal(e);

    e = mdb_cursor_open(txn, sv->entries, &curs);
    if (0 != e)
        mdb_fatal(e);

    e = mdb_cursor_get(curs, &k, &v, MDB_FIRST);
    switch (e)
    {
    case 0:
        break;
    case MDB_NOTFOUND:
        return;
    default:
        mdb_fatal(e);
    }

    raft_entry_t ety;
    ety.id = 0;

    int n_entries = 0;

    do
    {
        if (!(*(int *)k.mv_data & 1))
        {
            /* entry metadata */
            tpl_node *tn =
                tpl_map(tpl_peek(TPL_MEM, v.mv_data, v.mv_size), &ety);
            tpl_load(tn, TPL_MEM, v.mv_data, v.mv_size);
            tpl_unpack(tn, 0);
        }
        else
        {
            /* entry data for FSM */
            ety.data.buf = v.mv_data;
            ety.data.len = v.mv_size;
            raft_append_entry(sv->raft, &ety);
            n_entries++;
        }

        e = mdb_cursor_get(curs, &k, &v, MDB_NEXT);
    } while (0 == e);

    mdb_cursor_close(curs);

    e = mdb_txn_commit(txn);
    if (0 != e)
        mdb_fatal(e);

    MDB_val val;
    mdb_gets(sv->db_env, sv->state, "commit_idx", &val);
    if (val.mv_data)
        raft_set_commit_idx(sv->raft, *(int *)val.mv_data);

    raft_apply_all(sv->raft);
}

/** Load voted_for and term raft fields */
static void load_persistent_state(server_t *sv)
{
    int val = -1;


   kv_db_t *db = sv->db;
    kv_db_get(db,SCHEMA_STATE, "voted_for", &val);
        raft_vote_for_nodeid(sv->raft, val);

    kv_db_get(db,SCHEMA_STATE,  "term", &val);
        raft_set_current_term(sv->raft, val);
}

static int load_opts(server_t *sv, options_t *opts)
{
    kv_db_t *db = sv->db;
    sv->node_id=  *(int *)kv_db_get(db,SCHEMA_STATE, "id", 2);
    opts->raft_port = (char *)kv_db_get(db,SCHEMA_STATE,  "raft_port", 4);
    return 0;
}

static void drop_kv_db(server_t *sv)
{

    kv_db_t *db = sv->db;
    size_t count = sizeof(sys_schemas_meta) / sizeof(sys_schemas_meta[0]);
    int i = 0;
    for (; i < count; i++)
    {
        char *schema_name = sys_schemas_meta[i];
        kv_schema_t *schema = (db, schema_name);
        kv_db_unregister_schema(db, schema_name);
        kv_schema_destroy(schema);
    }
}

static void start_raft_periodic_timer(server_t *sv)
{
    uv_timer_t *periodic_req = calloc(1, sizeof(uv_timer_t));
    periodic_req->data = sv;
    uv_timer_init(&sv->peer_loop, periodic_req);
    uv_timer_start(periodic_req, __periodic, 0, PERIOD_MSEC);
    raft_set_election_timeout(sv->raft, 2000);
}

static void int_handler(int dummy)
{
    uv_mutex_lock(&sv->raft_lock);
    raft_node_t *leader = raft_get_current_leader_node(sv->raft);
    if (leader)
    {
        if (raft_node_get_id(leader) == sv->node_id)
        {
            printf("I'm the leader, I can't leave the cluster...\n");
            goto done;
        }

        peer_connection_t *leader_conn = raft_node_get_udata(leader);
        if (leader_conn)
        {
            printf("Leaving cluster...\n");
            send_leave(leader_conn);
            goto done;
        }
    }
    printf("Try again no leader at the moment...\n");
done:
    uv_mutex_unlock(&sv->raft_lock);
}

static void new_kv_db(server_t *sv)
{
    sv->db = kv_db_alloc(opts.database_name, opts.database_dir);
    assert(sv->db != NULL);

    size_t count = sizeof(sys_schemas_meta) / sizeof(sys_schemas_meta[0]);
    int i = 0;
    for (; i < count; i++)
    {
        char *schema_name = sys_schemas_meta[i];
        kv_schema_t *schema = kv_schema_alloc(schema_name, sv->db, false);
        kv_db_register_schema(sv->db, schema);
    }
    /*
    mdb_db_create(&sv->entries, sv->db_env, "entries");
    mdb_db_create(&sv->tickets, sv->db_env, "docs");
    mdb_db_create(&sv->state, sv->db_env, "state");
    */
}

static void start_peer_socket(server_t *sv, const char *host, int port, uv_tcp_t *listen)
{
    memset(&sv->peer_loop, 0, sizeof(uv_loop_t));
    int e = uv_loop_init(&sv->peer_loop);
    if (0 != e)
        uv_fatal(e);

    uv_bind_listen_socket(listen, host, port, &sv->peer_loop);
    e = uv_listen((uv_stream_t *)listen, MAX_PEER_CONNECTIONS,
                  on_peer_connection);
    if (0 != e) {
        uv_fatal(e);
    }
       
}

static void save_opts(server_t *sv, options_t *opts)
{
    kv_db_t *db = sv->db;
    kv_db_set(db,sys_schemas_meta[SCHEMA_STATE_IDX],"id",2,&sv->node_id,sizeof(sv->node_id));
    kv_db_set(db,sys_schemas_meta[SCHEMA_STATE_IDX],"raft_port",4,&opts->raft_port,sizeof(opts->raft_port));

}

// raft服务入口
int main(int argc, char **argv)
{
    memset(sv, 0, sizeof(server_t));

    // 解析参数
    int e = options_init(argc, argv, &opts);
    if (-1 == e) {
        exit(-1);
    }
       
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, int_handler);

    // 初始化一个raft
    sv->raft = raft_new();
    // 设置raft的回调函数
    raft_set_callbacks(sv->raft, &raft_funcs, sv);

    srand(time(NULL));

    // 初始化kv stoare
    new_kv_db(sv);

/*
    if (opts.drop)
    {
        drop_kv_db(sv);
        exit(0);
    }
*/
    //  handler->on_req = __http_get_id;

    uv_mutex_init(&sv->raft_lock);
    uv_cond_init(&sv->appendentries_received);

    uv_tcp_t peer_listen;
    uv_multiplex_t m;

    /* get ID */
    if(opts.type_info.type==OPTION_START ||opts.type_info.type==OPTION_JOIN  )
    {
        if (opts.id) {
            sv->node_id = atoi(opts.id);
        }
    }
    else
    {
        e = load_opts(sv, &opts);
        if (0 != e)
        {
            printf("ERROR: No database available.\n"
                   "Please start or join a cluster.\n");
            abort();
        }
    }

    // 把自己添加到raft集群中
    raft_add_node(sv->raft, NULL, sv->node_id, 1);

    if(opts.type_info.type==OPTION_START ||opts.type_info.type==OPTION_JOIN)
    {
        drop_kv_db(sv);
        new_kv_db(sv);
        save_opts(sv, &opts);

        start_peer_socket(sv, opts.host, atoi(opts.raft_port), &peer_listen);

        // raft中第一个节点
        if (opts.type_info.type==OPTION_START)
        {
            // 开始选主，反正自己是集群中第一个节点
            raft_become_leader(sv->raft);
            /* We store membership configuration inside the Raft log.
             * This configuration change is going to be the initial membership
             * configuration (ie. original node) inside the Raft log. The
             * first configuration is for a cluster of 1 node. */
            append_cfg_change(sv, RAFT_LOGTYPE_ADD_NODE,
                                opts.host,
                                atoi(opts.raft_port),
                                sv->node_id);
        }
        else
        {
            addr_parse_result_t res;
            parse_addr(opts.peer, strlen(opts.peer), &res);
            res.host[res.host_len] = '\0';

            peer_connection_t *conn = new_connection(sv);
            connect_to_peer_at_host(conn, res.host, atoi(res.port));
        }
    }
    /* Reload cluster information and rejoin cluster */
    else
    {
        start_peer_socket(sv, opts.host, atoi(opts.raft_port), &peer_listen);
        load_commit_log(sv);
        load_persistent_state(sv);

        if (1 == raft_get_num_nodes(sv->raft))
        {
            raft_become_leader(sv->raft);
        }
        else
        {
            for (int i = 0; i < raft_get_num_nodes(sv->raft); i++)
            {
                raft_node_t *node = raft_get_node_from_idx(sv->raft, i);
                if (raft_node_get_id(node) == sv->node_id)
                    continue;
                connect_to_peer(raft_node_get_udata(node));
            }
        }
    }

    start_raft_periodic_timer(sv);

    uv_run(&sv->peer_loop, UV_RUN_DEFAULT);
}
