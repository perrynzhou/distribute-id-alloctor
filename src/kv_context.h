/*************************************************************************
  > File Name: context.h
  > Author:perrynzhou 
  > Mail:perrynzhou@gmail.com 
  > Created Time: Thu 30 Dec 2021 04:27:18 PM CST
 ************************************************************************/

#ifndef _CONTEXT_H
#define _CONTEXT_H
#include <wiredtiger_ext.h>
#include <wiredtiger.h>
struct kv_schema_ctx
{
  int id;
  char *name;
  WT_CONNECTION *conn;
  WT_CURSOR *cursor;
  WT_SESSION *session;
};
struct kv_context
{
  char *database_name;
  char  *database_dir;
  struct kv_schema_ctx *schema_handles;
};
int kv_context_init(struct kv_context **ctx, const char *database_name, const char *database_dir);
int kv_context_put(struct kv_context *ctx,int schema_id, char *key, char *value);
void kv_context_del(struct kv_context *ctx,int schema_id, char  *key);
void context_deinit(struct kv_context *ctx);
#endif
