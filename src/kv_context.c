/*************************************************************************
  > File Name: context.c
  > Author:perrynzhou 
  > Mail:perrynzhou@gmail.com 
  > Created Time: Thu 30 Dec 2021 04:27:23 PM CST
 ************************************************************************/

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <assert.h>
#include <wiredtiger_ext.h>
#include <wiredtiger.h>
#include <string.h>
#include "kv_context.h"
#include "kv_schema.h"

int kv_context_init(struct kv_context **ctx, const char *database_name, const char *database_dir)
{
  if (ctx != NULL && database_name != NULL && database_dir != NULL)
  {

    if (access(database_dir, F_OK) != 0)
    {
      return -1;
    }
    struct kv_context *ctx_ptr = calloc(1, sizeof(struct kv_context));
    assert(ctx_ptr != NULL);

    strncpy(ctx_ptr->database_name, database_name, strlen(database_name));
    strncpy(ctx_ptr->database_dir, database_dir, strlen(database_dir));
    int schema_count = sizeof(sys_schema) / sizeof(struct scheme_index  *);
    for (int i = 0; i < schema_count; i++)
    {
      ctx_ptr->schema_handles[i].id = sys_schema[i]->id;
      ctx_ptr->schema_handles[i].name = strdup(sys_schema[i]->name);
      WT_CONNECTION *conn = ctx_ptr->schema_handles[i].conn;
      WT_CURSOR *cursor = ctx_ptr->schema_handles[i].cursor;
      WT_SESSION *session = ctx_ptr->schema_handles[i].session;
      char schema_name[512] = {'\0'};
      snrpintf(&schema_name, 512, "table:%s", sys_schema[i]);
      assert(wiredtiger_open(database_dir, NULL, "create", &conn) != -1);

      assert(conn->open_session(conn, NULL, NULL, &session) != -1);

      assert(session->create(session, &schema_name, "key_format=S,value_format=S") != -1);

      assert(session->open_cursor(session, &schema_name, NULL, NULL, &cursor) != -1);
    }
    *ctx = ctx_ptr;

    return 0;
  }
  return -1;
}
int kv_context_put(struct kv_context *ctx,int schema_id, char *key, char *value)
{
  return 0;
}
void kv_context_del(struct kv_context *ctx,int schema_id, char *key)
{
}
void context_deinit(struct kv_context *ctx)
{
  if (ctx != NULL)
  {
    slice_deinit(ctx->database_name);
    slice_deinit(ctx->database_dir);
    free(ctx);
    ctx = NULL;
  }
}