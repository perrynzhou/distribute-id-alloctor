/*************************************************************************
  > File Name: dict.c
  > Author:perrynzhou
  > Mail:perrynzhou@gmail.com
  > Created Time: 六  9/12 23:08:20 2020
 ************************************************************************/

#include "dict.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DM_DELTA 0x9E3779B9
#define DM_FULLROUNDS 10
#define DM_PARTROUNDS 6

typedef struct dict_data_pair_t
{
  struct dict_data_pair_t *next;
  char *key;
  uint32_t hash;
  char data[0];
} dict_data_pair;
inline static dict_data_pair *dict_data_pair_create(const char *key, uint32_t hash, size_t len)
{
  dict_data_pair *dp = (dict_data_pair *)calloc(1, sizeof(dict_data_pair) + len);
  dp->next = NULL;
  dp->hash = hash;
  dp->key = strdup(key);
  return dp;
}
inline static uint32_t __pad(int len)
{
  uint32_t pad = 0;

  pad = (uint32_t)len | ((uint32_t)len << 8);
  pad |= pad << 16;

  return pad;
}
static int dm_round(int rounds, uint32_t *array, uint32_t *h0, uint32_t *h1)
{
  uint32_t sum = 0;
  int n = 0;
  uint32_t b0 = 0;
  uint32_t b1 = 0;

  b0 = *h0;
  b1 = *h1;

  n = rounds;

  do
  {
    sum += DM_DELTA;
    b0 += ((b1 << 4) + array[0]) ^ (b1 + sum) ^ ((b1 >> 5) + array[1]);
    b1 += ((b0 << 4) + array[2]) ^ (b0 + sum) ^ ((b0 >> 5) + array[3]);
  } while (--n);

  *h0 += b0;
  *h1 += b1;

  return 0;
}
static uint32_t dict_default_hash_fn(const char *msg, int len)
{
  uint32_t h0 = 0x9464a485;
  uint32_t h1 = 0x542e1a94;
  uint32_t array[4];
  uint32_t pad = 0;
  int i = 0;
  int j = 0;
  int full_quads = 0;
  int full_words = 0;
  int full_bytes = 0;
  uint32_t *intmsg = NULL;
  int word = 0;

  intmsg = (uint32_t *)msg;
  pad = __pad(len);

  full_bytes = len;
  full_words = len / 4;
  full_quads = len / 16;

  for (i = 0; i < full_quads; i++)
  {
    for (j = 0; j < 4; j++)
    {
      word = *intmsg;
      array[j] = word;
      intmsg++;
      full_words--;
      full_bytes -= 4;
    }
    dm_round(DM_PARTROUNDS, &array[0], &h0, &h1);
  }

  for (j = 0; j < 4; j++)
  {
    if (full_words)
    {
      word = *intmsg;
      array[j] = word;
      intmsg++;
      full_words--;
      full_bytes -= 4;
    }
    else
    {
      array[j] = pad;
      while (full_bytes)
      {
        array[j] <<= 8;
        array[j] |= msg[len - full_bytes];
        full_bytes--;
      }
    }
  }
  dm_round(DM_FULLROUNDS, &array[0], &h0, &h1);

  return (uint32_t)(h0 ^ h1);
}
static uint64_t dict_jump_consistent(uint64_t key, uint32_t num_buckets)
{

  int64_t b = -1, j = 0;
  uint32_t value = 0;

  while (j < num_buckets)
  {
    b = j;
    key = key * 2862933555777941757ULL + 1;
    j = (int64_t)((b + 1) * ((double)(1LL << 31) / (double)((key >> 33) + 1)));
  }
  value = (b < 0) ? (~b + 1) : b;
  return value;
}
int dict_init(dict_t *d, uint32_t max_count, dict_hash_fn hash_fn)
{
  memset(d, 0, sizeof(*d));
  d->max_count = max_count;
  d->count = 0;
  d->hash_fn = (hash_fn == NULL) ? (dict_hash_fn)dict_default_hash_fn : hash_fn;
  d->members = (void *)calloc(max_count, sizeof(void *));
  d->member_count = (uint32_t *)calloc(max_count, sizeof(uint32_t));
  return 0;
}
dict_t *dict_create(uint32_t max_count, dict_hash_fn hash_fn)
{
  dict_t *d = (dict_t *)calloc(1, sizeof(dict_t));
  if (dict_init(d, max_count, hash_fn) != 0)
  {
    free(d);
    d = NULL;
  }
  return d;
}
static dict_data_pair *dict_fetch(dict_t *d, const char *key, size_t *key_len_ptr, uint32_t *hash_ptr, uint32_t *index_ptr, dict_data_pair **prev)
{
  size_t key_len = strlen(key);
  uint32_t hash = d->hash_fn(key, key_len);
  uint32_t index = dict_jump_consistent(hash, d->max_count);
  *index_ptr = index;
  *hash_ptr = hash;
  *key_len_ptr = key_len;
  dict_data_pair *cur = NULL;
  dict_data_pair *data = NULL;
  if (d->member_count[index] > 0)
  {
    for (cur = (dict_data_pair *)d->members[index]; cur != NULL; cur = cur->next)
    {
      if (hash == cur->hash && strncmp(cur->key, key, key_len) == 0)
      {
        data = cur;
        break;
      }
      *prev = cur;
    }
  }
  return data;
}
void *dict_put(dict_t *d, char *key, void *val, size_t val_len)
{
  dict_data_pair *prev = NULL;
  uint32_t hash, index;
  size_t key_len;
  dict_data_pair *data = dict_fetch(d, key, &key_len, &hash, &index, &prev);
  if (data != NULL)
  {
    return NULL;
  }
  data = dict_data_pair_create(key, hash, val_len);
  memcpy(data->data, val, val_len);
  if (d->members[index] == NULL)
  {
    d->members[index] = data;
  }
  else
  {
    data->next = d->members[index];
    d->members[index] = data;
  }
  __sync_fetch_and_add(&d->member_count[index], 1);
  __sync_fetch_and_add(&d->count, 1);
  return (void *)&data->data;
}
void *dict_get(dict_t *d, char *key)
{
  void *data = NULL;
  dict_data_pair *prev = NULL;
  uint32_t hash, index;
  size_t key_len;
  dict_data_pair *dp = dict_fetch(d, key, &key_len, &hash, &index, &prev);
  if (dp != NULL)
  {
    data = dp->data;
  }
  return data;
}
int dict_del(dict_t *d, char *key, dict_data_free_fn fn)
{
  dict_data_pair *prev = NULL;
  uint32_t hash, index;
  size_t key_len;
  dict_data_pair *dp = dict_fetch(d, key, &key_len, &hash, &index, &prev);
  if (dp != NULL)
  {
    if (prev == NULL)
    {
      d->members[index] = dp->next;
    }
    else
    {
      prev->next = dp->next;
    }
    dp->next = NULL;
    free(dp->key);
    if (fn != NULL)
    {
      fn(dp->data);
    }
    free(dp);
    __sync_fetch_and_sub(&d->member_count[index], 1);
    __sync_fetch_and_sub(&d->count, 1);
    return 0;
  }
  return -1;
}
void dict_dump(dict_t *d, dict_cb_fn cb)
{
  size_t i = 0;
  for (; i < d->max_count; i++)
  {
    if (d->member_count[i] > 0)
    {
      fprintf(stdout, "---------------dump index =%d ---------------\n", i);
      dict_data_pair *dp = (dict_data_pair *)d->members[i];
      while (dp != NULL)
      {
        char *key = dp->key;
        void *data = (void *)&dp->data;
        cb(key, data);
        dp = dp->next;
      }
    }
  }
}
void dict_deinit(dict_t *d, dict_data_free_fn free_cb)
{
  size_t i;
  for (i = 0; i < d->max_count; i++)
  {
    if (d->member_count[i] > 0)
    {
      dict_data_pair *dp = (dict_data_pair *)d->members[i];
      while (dp != NULL)
      {
        dict_data_pair *next = dp->next;
        free(dp->key);
        if (free_cb != NULL)
        {
          void *val = dp->data;
          free_cb(val);
        }

        free(dp);
        dp = next;
        __sync_fetch_and_sub(&d->member_count[i], 1);
        __sync_fetch_and_sub(&d->count, 1);
      }
    }
  }
}
void dict_destroy(dict_t *d, dict_data_free_fn free_cb)
{
  if (d != NULL)
  {
    dict_deinit(d, free_cb);
    free(d);
    d = NULL;
  }
}
