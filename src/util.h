/*************************************************************************
    > File Name: util.h
  > Author:perrynzhou 
  > Mail:perrynzhou@gmail.com 
  > Created Time: Sun 20 Feb 2022 09:32:10 AM UTC
 ************************************************************************/

#ifndef _UTIL_H
#define _UTIL_H
#define container_of(ptr, type, member) ({ \
                        const typeof( ((type*)0)->member ) \
                        * __mptr = ((void*)(ptr)); \
                        (type*)( (char*)__mptr - \
                        offsetof(type, member) ); \
                        })



#define atomic_add(ptr,val) (__sync_fetch_and_add(ptr,val))
#define atomic_sub(ptr,val) (__sync_fetch_and_sub(ptr,val))

#endif
