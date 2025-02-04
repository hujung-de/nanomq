#ifndef CONF_H
#define CONF_H

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zmalloc.h"

#define PID_PATH_NAME "/tmp/nanomq/nanomq.pid"
#define CONF_PATH_NAME "/etc/nanomq.conf"
#define CONF_AUTH_PATH_NAME "/etc/nanomq_auth_username.conf"
#define CONF_BRIDGE_PATH_NAME "/etc/nanomq_bridge.conf"

#define CONF_TCP_URL_DEFAULT "broker+tcp://0.0.0.0:1883"
#define CONF_WS_URL_DEFAULT "nmq+ws://0.0.0.0:8083/mqtt"

#define TCP_URL_PREFIX "broker+tcp"
#define WS_URL_PREFIX "nmq+ws"

#define FREE_NONULL(p)    \
	if (p) {          \
		free(p);  \
		p = NULL; \
	}

struct conf_auth {
	int    count;
	char **usernames;
	char **passwords;
};
typedef struct conf_auth conf_auth;

struct conf_http_server {
	bool     enable;
	uint16_t port;
	char *   username;
	char *   password;
};

typedef struct conf_http_server conf_http_server;

struct conf_websocket {
	bool  enable;
	char *url;
};

typedef struct conf_websocket conf_websocket;

typedef struct {
	char *   topic;
	uint32_t topic_len;
	uint8_t  qos;
} subscribe;

struct conf_bridge {
	bool       bridge_mode;
	char *     address;
	uint8_t    proto_ver;
	char *     clientid;
	bool       clean_start;
	char *     username;
	char *     password;
	uint16_t   keepalive;
	size_t     forwards_count;
	char **    forwards;
	size_t     sub_count;
	subscribe *sub_list;
	uint64_t   parallel;
};

typedef struct conf_bridge conf_bridge;

struct conf {
	char *   conf_file;
	char *   bridge_file;
	char *   auth_file;
	char *   url;
	int      num_taskq_thread;
	int      max_taskq_thread;
	uint64_t parallel;
	int      property_size;
	int      msq_len;
	int      qos_duration;
	void *   db_root;
	bool     allow_anonymous;
	bool     daemon;

	conf_http_server http_server;
	conf_websocket   websocket;
	conf_bridge      bridge;

	conf_auth auths;
};

typedef struct conf conf;

extern bool conf_parser(conf *nanomq_conf);
extern bool conf_bridge_parse(conf *nanomq_conf);
extern void print_bridge_conf(conf_bridge *bridge);
extern void conf_init(conf *nanomq_conf);
extern void print_conf(conf *nanomq_conf);
extern void conf_fini(conf *nanomq_conf);

extern void conf_auth_parser(conf *);

extern int string_trim(char **dst, char *str);

#endif
