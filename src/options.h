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
	/* arguments */
	char *PEER;

} options_t;
int options_init(options_t *opt,int argc,char *argv[]);
void options_dump(options_t *opt);
#endif
