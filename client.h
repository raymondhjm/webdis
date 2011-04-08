#ifndef CLIENT_H
#define CLIENT_H

#include <event.h>
#include <arpa/inet.h>
#include "http_parser.h"

struct http_header;
struct server;

typedef enum {
	LAST_CB_NONE = 0,
	LAST_CB_KEY = 1,
	LAST_CB_VAL = 2} last_cb_t;

struct http_client {

	int fd;
	in_addr_t addr;
	struct event ev;

	struct worker *w;
	struct server *s;


	/* HTTP parsing */
	struct http_parser parser;
	struct http_parser_settings settings;
	char *buffer;
	size_t sz;
	last_cb_t last_cb;

	/* various flags. TODO: bit map */
	int keep_alive;
	int broken;
	int is_websocket;

	/* HTTP data */
	char *path;
	size_t path_sz;

	/* headers */
	struct http_header *headers;
	int header_count;

	char *body;
	size_t body_sz;

	char *type; /* forced output content-type */
	char *jsonp; /* jsonp wrapper */
};

struct http_client *
http_client_new(struct worker *w, int fd, in_addr_t addr);

void
http_client_reset(struct http_client *c);

void
http_client_free(struct http_client *c);

int
http_client_read(struct http_client *c);

int
http_client_execute(struct http_client *c);

int
http_client_set_body(struct http_client *c, const char *at, size_t sz);

const char *
client_get_header(struct http_client *c, const char *key);


#endif
