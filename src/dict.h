/*************************************************************************
  > File Name: dict.h
  > Author:perrynzhou 
  > Mail:perrynzhou@gmail.com 
  > Created Time: 六  9/12 23:08:14 2020
 ************************************************************************/

#ifndef _DICT_H
#define _DICT_H
#include <stdint.h>
#include <stdio.h>
typedef uint32_t (*dict_hash_fn)(const char *key, size_t len);
typedef int (*dict_cb_fn)(char *key,void *data);
typedef void (dict_data_free_fn)(void *data);
typedef struct dict_t
{
  uint64_t max_count;
  uint32_t count;
  uint32_t *member_count;
  void **members;
  dict_hash_fn hash_fn;
} dict;
int dict_init(dict *d, uint32_t max_count, dict_hash_fn hash_fn);
dict *dict_create(uint32_t max_count, dict_hash_fn hash_fn);
void *dict_put(dict *d, char *key, void *data,size_t data_len);
void *dict_get(dict *d, char *key);
int dict_del(dict *d, char *key,dict_data_free_fn fn);
void dict_deinit(dict *d,dict_data_free_fn free_cb);
void dict_dump(dict *d,dict_cb_fn cb);
void dict_destroy(dict *d,dict_data_free_fn free_cb);
#endif
