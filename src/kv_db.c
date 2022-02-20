/*************************************************************************
    > File Name: kv_db.c
  > Author:perrynzhou
  > Mail:perrynzhou@gmail.com
  > Created Time: Sun 20 Feb 2022 05:15:21 AM UTC
 ************************************************************************/

#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <assert.h>
#include "kv_db.h"
#include "hashfn.h"
kv_schema *kv_schema_alloc(int index, const char *schema_name, void *ctx)
{

  assert(ctx != NULL);
  kv_db *db = (kv_db *)ctx;
  size_t schema_sz = strlen(schema_name);

  kv_schema *schema = (kv_schema *)calloc(1, sizeof(kv_schema) + schema_sz + 1);
  assert(schema != NULL);
  assert(wiredtiger_open(db->database_dir, NULL, "create", &schema->conn) != -1);
  assert(schema->conn->open_session(schema->conn, NULL, NULL, &schema->session) != -1);
  assert(schema->session->create(schema->session, &schema_name, "key_format=S,value_format=S") != -1);
  snprintf((char *)&schema->schema_name, schema_sz, "%s", schema_name);
  schema->ctx = ctx;

  return schema;
}
void kv_schema_destroy(kv_schema *schema)
{
  if (schema != NULL)
  {
    kv_db *db = (kv_db *)schema->ctx;
    schema->cursor->close(schema->cursor);
    free(schema);
    schema = NULL;
  }
}

kv_db *kv_db_alloc(const char *database_name, const char *database_dir)
{
  if (database_name != NULL && database_dir != NULL)
  {

    if (access(database_dir, F_OK) != 0)
    {
      return -1;
    }
    kv_db *db = calloc(1, sizeof(kv_db));
    assert(db != NULL);

    db->database_name = strdup(database_name);

    db->database_dir = strdup(database_dir);
    dict_init(db->schema_ctx, SCHEMA_LIMIT, hash_fnv1_64);
    return db;
  }
  return NULL;
}
int kv_db_put(kv_db *db, char *schema_name, char *key, void *val)
{

  kv_schema *schema = (kv_schema *)dict_get(db->schema_ctx, key);
  WT_CURSOR *cursor = schema->cursor;
  cursor->set_key(cursor, key);
  cursor->set_value(cursor, val);
  return cursor->insert(cursor);
}
int kv_db_del(kv_db *db, char *schema_name, char *key)
{
  kv_schema *schema = (kv_schema *)dict_get(db->schema_ctx, key);
  WT_CURSOR *cursor = schema->cursor;
  cursor->set_key(cursor, key);
  return cursor->remove(cursor);
}
inline static void kv_schema_free_cb(void *ptr) {
  kv_schema *schema = (kv_schema *)ptr;
  kv_schema_destroy(schema);
}
void *kv_db_destroy(kv_db *db)
{
   if(db !=NULL) {

     dict_deinit(db->schema_ctx,kv_schema_free_cb);
     free(db->database_dir);
     free(db->database_name);
   }
}
int kv_db_register_schema(kv_db *db, kv_schema *schema)
{
  if (db == NULL || schema == NULL)
  {
    return -1;
  }
  char *schema_name = &schema->schema_name;
  if (dict_put(db->schema_ctx, schema_name, schema, sizeof(kv_schema *)) != NULL)
  {
    return 0;
  }

  return -1;
}
void kv_db_unregister_schema(kv_db *db, char *schema_name)
{
  if (db == NULL || schema_name == NULL)
  {
    return -1;
  }
  dict_del(db->schema_ctx, schema_name, NULL);
}