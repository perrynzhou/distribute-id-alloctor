/*************************************************************************
    > File Name: kv_db_test.c
  > Author:perrynzhou
  > Mail:perrynzhou@gmail.com
  > Created Time: Sun 20 Feb 2022 11:01:42 AM UTC
 ************************************************************************/

#include <stdio.h>
#include <wiredtiger_ext.h>
#include <wiredtiger.h>
#include "hashfn.h"
#include "kv_db.h"
#include "dict.h"

int main()
{
  kv_db_t *db = kv_db_alloc("test", "/tmp");
  size_t count = sizeof(sys_schemas_meta) / sizeof(sys_schemas_meta[0]);
  int i = 0;
  for (; i < count; i++)
  {
    char *schema_name = sys_schemas_meta[0];
    kv_schema_t *schema = kv_schema_alloc(schema_name, db);
    kv_db_register_schema(db, schema);
    kv_schema_t *tmp_schema = kv_db_fetch_schema(db, schema_name);
    int ret = kv_db_put(db, tmp_schema, "abc", "123");
    void *val_ptr = NULL;
    ret = kv_db_get(db, schema_name, "abc", &val_ptr);
    kv_db_del(db,schema_name, "abc");
    ret = kv_db_get(db, schema_name, "abc", &val_ptr);
  }
}
