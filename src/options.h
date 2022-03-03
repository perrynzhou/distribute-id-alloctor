/*************************************************************************
  > File Name: options.h
  > Author:perrynzhou 
  > Mail:perrynzhou@gmail.com 
  > Created Time: Mon 21 Feb 2022 05:41:07 PM CST
 ************************************************************************/

#ifndef _OPTIONS_H
#define _OPTIONS_H
#include <stdbool.h>

typedef enum {
	OPTION_START=0,
	OPTION_JOIN,
	OPTION_LEAVE,
	OPTION_DROP,
}option_type_e;

typedef struct  {
	char *name;
	int  type;
}option_type_info_t;
typedef struct
{
	option_type_info_t type_info;
	int daemonize;
	int debug;
	char *host;
	char *id;
	char *database_dir;
	char *database_name;
	char *pid_file;
	char *raft_port;
	char *service_port;
	// 主节点的地址
	char *peer;

} options_t;
/*
ticketd join 127.0.0.1:9001 --id 2 --raft_port 9002 --http_port 8002
(gdb) p opts
$1 = {drop = 0, join = 1, leave = 0, start = 0, daemonize = 0, debug = 0, help = 0, version = 0, db_size = 0x55555558e2a0 "1000", host = 0x55555558e2c0 "127.0.0.1", 
  http_port = 0x55555558e3c0 "8002", id = 0x55555558e380 "2", path = 0x55555558e300 "store", pid_file = 0x55555558e320 "/var/run/pearl.pid", 
  raft_port = 0x55555558e3a0 "9002", PEER = 0x55555558e360 "127.0.0.1:9001"}
*/
int options_init(options_t *opt,int argc,char *argv[]);
void options_dump(options_t *opt);
#endif
