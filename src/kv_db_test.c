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

typedef struct
{
  int id;
  char name[256];
  uint ver;
} demo_t;
int main(int argc, char *argv[])
{

const  char *schemas_meta[] = {
    "entries",
    "docs",
    "state",
};

  kv_db_t *db = kv_db_alloc(argv[1], argv[2]);
  size_t count = sizeof(schemas_meta) / sizeof(schemas_meta[0]);
  int i = 0;
  for (; i < count; i++)
  {
    char *schema_name = schemas_meta[i];
    kv_schema_t *schema = kv_schema_alloc(schema_name, db,false);
    kv_db_register_schema(db, schema);
    /*
    kv_schema_t *tmp_schema = kv_db_fetch_schema(db, schema_name);
    fprintf(stdout, "p1=%p,p2=%p\n", schema, tmp_schema);

    int ret = kv_db_put(db, schema_name, "abc", "123");
    void *val_ptr = NULL;
    ret = kv_db_get(db, schema_name, "abc", &val_ptr);
    kv_db_del(db, schema_name, "abc");
    ret = kv_db_get(db, schema_name, "abc", &val_ptr);
  */
    demo_t inst = {
        .id = i,
        .name = "abcd",
        .ver = i + 1,
    };
            WT_ITEM key, value;
    key.data = strdup("abcd");
    key.size = strlen(key.data);

    value.data = &inst;
    value.size = sizeof(inst);
    fprintf(stdout, "set demo_t info :id=%d,name=%s,ver=%d\n", inst.id, inst.name, inst.ver);

    int ret = kv_db_put(db, schema_name, &key, &value);
     WT_ITEM value2;
    ret = kv_db_get(db, schema_name, &key, &value2);
    demo_t *inst2 =  value2.data;
    fprintf(stdout, "get demo_t info :id=%d,name=%s,ver=%d\n", inst2->id, inst2->name, inst2->ver);
    kv_db_del(db, schema_name, "demo");
  }
}
