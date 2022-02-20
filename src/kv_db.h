/*************************************************************************
    > File Name: kv_db.h
  > Author:perrynzhou
  > Mail:perrynzhou@gmail.com
  > Created Time: Sun 20 Feb 2022 05:10:58 AM UTC
 ************************************************************************/

#ifndef _KV_DB_H
#define _KV_DB_H
#include <stdio.h>
#include <wiredtiger_ext.h>
#include <wiredtiger.h>
#include "dict.h"
#define SCHEMA_ENTRIES 0
#define SCHEMA_DOCS 1
#define SCHEMA_STATE 2

#define SCHEMA_LIMIT (1024)

typedef struct kv_schema_t
{
 
  WT_CONNECTION *conn;
  WT_SESSION *session;
  WT_CURSOR *cursor;
  void *ctx;
   char *schema_name[0]
} kv_schema;

typedef struct kv_db_t
{
  char *database_name;
  char *database_dir;
  dict *schema_ctx;
  // struct kv_schema **schema_ctx;
} kv_db;

const char *sys_schemas_meta[] = {
    "entries",
    "docs",
    "state",
};

kv_schema *kv_schema_alloc(int index, const char *name, void *ctx);
void kv_schema_destroy(kv_schema *schema);

kv_db *kv_db_alloc(const char *database_name, const char *database_dir);
int kv_db_register_schema(kv_db *db, kv_schema *schema);
void kv_db_unregister_schema(kv_db *db, char *schema_name);
int kv_db_put(kv_db *db, char *schema_name, char *key, void *val);
int kv_db_del(kv_db *db, char *schema_name, char *key);
void *kv_db_destroy(kv_db *db);
#endif
