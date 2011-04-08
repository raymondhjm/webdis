#include "client.h"
#include "http_parser.h"
#include "http.h"
#include "server.h"
#include "worker.h"
#include "websocket.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
http_client_on_url(struct http_parser *p, const char *at, size_t sz) {

	struct http_client *c = p->data;

	c->path = realloc(c->path, c->path_sz + sz + 1);
	memcpy(c->path + c->path_sz, at, sz);
	c->path_sz += sz;
	c->path[c->path_sz] = 0;

	return 0;
}

static int
http_client_on_body(struct http_parser *p, const char *at, size_t sz) {

	struct http_client *c = p->data;
	return http_client_set_body(c, at, sz);
}

int
http_client_set_body(struct http_client *c, const char *at, size_t sz) {

	c->body = realloc(c->body, c->body_sz + sz + 1);
	memcpy(c->body + c->body_sz, at, sz);
	c->body_sz += sz;
	c->body[c->body_sz] = 0;

	return 0;
}

static int
http_client_on_header_name(struct http_parser *p, const char *at, size_t sz) {

	struct http_client *c = p->data;
	size_t n = c->header_count;

	if(c->last_cb != LAST_CB_KEY) {
		n = ++c->header_count;
		c->headers = realloc(c->headers, n * sizeof(struct http_header));
		memset(&c->headers[n-1], 0, sizeof(struct http_header));
	}

	c->headers[n-1].key = realloc(c->headers[n-1].key,
			c->headers[n-1].key_sz + sz + 1);
	memcpy(c->headers[n-1].key + c->headers[n-1].key_sz, at, sz);
	c->headers[n-1].key_sz += sz;
	c->headers[n-1].key[c->headers[n-1].key_sz] = 0;

	c->last_cb = LAST_CB_KEY;

	return 0;
}

static int
http_client_on_query_string(struct http_parser *parser, const char *at, size_t sz) {

	struct http_client *c = parser->data;
	const char *p = at;

	while(p < at + sz) {

		const char *key = p, *val;
		int key_len, val_len;
		char *eq = memchr(key, '=', sz - (p-at));
		if(!eq || eq > at + sz) { /* last argument */
			break;
		} else { /* found an '=' */
			char *amp;
			val = eq + 1;
			key_len = eq - key;
			p = eq + 1;

			amp = memchr(p, '&', sz - (p-at));
			if(!amp || amp > at + sz) {
				val_len = at + sz - p; /* last arg */
			} else {
				val_len = amp - val; /* cur arg */
				p = amp + 1;
			}

			if(key_len == 4 && strncmp(key, "type", 4) == 0) {
				c->type = calloc(1 + val_len, 1);
				memcpy(c->type, val, val_len);
			} else if(key_len == 5 && strncmp(key, "jsonp", 5) == 0) {
				c->jsonp = calloc(1 + val_len, 1);
				memcpy(c->jsonp, val, val_len);
			}

			if(!amp) {
				break;
			}
		}
	}
	return 0;
}

static int
http_client_on_header_value(struct http_parser *p, const char *at, size_t sz) {

	struct http_client *c = p->data;
	size_t n = c->header_count;

	c->headers[n-1].val = realloc(c->headers[n-1].val,
			c->headers[n-1].val_sz + sz + 1);
	memcpy(c->headers[n-1].val + c->headers[n-1].val_sz, at, sz);
	c->headers[n-1].val_sz += sz;
	c->headers[n-1].val[c->headers[n-1].val_sz] = 0;

	c->last_cb = LAST_CB_VAL;


	if(strncmp("Expect", c->headers[n-1].key, c->headers[n-1].key_sz) == 0) {
		if(sz == 12 && strncasecmp(at, "100-continue", sz) == 0) {
			/* support HTTP file upload */
			char http100[] = "HTTP/1.1 100 Continue\r\n\r\n";
			int ret = write(c->fd, http100, sizeof(http100)-1);
			(void)ret;
		}
	} else if(strncasecmp("Connection", c->headers[n-1].key, c->headers[n-1].key_sz) == 0) {
		if(sz == 10 && strncasecmp(at, "Keep-Alive", sz) == 0) {
			c->keep_alive = 1;
		}
	}

	return 0;
}

static int
http_client_on_message_complete(struct http_parser *p) {

	struct http_client *c = p->data;

	/* keep-alive detection */
	if(c->parser.http_major == 1 && c->parser.http_minor == 1) { /* 1.1 */
		c->keep_alive = 1;
	}

	if(p->upgrade) { /* WebSocket, don't execute just yet */
		c->is_websocket = 1;
		return 0;
	}
	worker_process_client(c);
	http_client_reset(c);

	return 0;
}

struct http_client *
http_client_new(struct worker *w, int fd, in_addr_t addr) {

	struct http_client *c = calloc(1, sizeof(struct http_client));

	c->fd = fd;
	c->w = w;
	c->addr = addr;
	c->s = w->s;

	/* parser */
	http_parser_init(&c->parser, HTTP_REQUEST);
	c->parser.data = c;

	/* callbacks */
	c->settings.on_url = http_client_on_url;
	c->settings.on_query_string = http_client_on_query_string;
	c->settings.on_body = http_client_on_body;
	c->settings.on_message_complete = http_client_on_message_complete;
	c->settings.on_header_field = http_client_on_header_name;
	c->settings.on_header_value = http_client_on_header_value;

	c->last_cb = LAST_CB_NONE;

	return c;
}


void
http_client_reset(struct http_client *c) {

	int i;

	/* headers */
	for(i = 0; i < c->header_count; ++i) {
		free(c->headers[i].key);
		free(c->headers[i].val);
	}
	free(c->headers);
	c->headers = NULL;
	c->header_count = 0;

	/* other data */
	free(c->body); c->body = NULL;
	c->body_sz = 0;
	free(c->path); c->path = NULL;
	c->path_sz = 0;
	free(c->type); c->type = NULL;
	free(c->jsonp); c->jsonp = NULL;

	/* no last known header callback */
	c->last_cb = LAST_CB_NONE;

	/* mark as broken if client doesn't support Keep-Alive. */
	if(c->keep_alive == 0) {
		c->broken = 1;
	}
}

void
http_client_free(struct http_client *c) {

	/* printf("client free: %p\n", (void*)c); */

	http_client_reset(c);
	free(c->buffer);

	/* close(c->fd); */

	free(c);
}

int
http_client_read(struct http_client *c) {

	char buffer[4096];
	int ret;

	ret = read(c->fd, buffer, sizeof(buffer));
	if(ret <= 0) {
		/* printf("Broken read on c=%p, fd=%d.\n", (void*)c, c->fd); */
		close(c->fd);
		http_client_free(c);
		return -1;
	}

	c->buffer = realloc(c->buffer, c->sz + ret);
	memcpy(c->buffer + c->sz, buffer, ret);
	c->sz += ret;

#if 0
	printf("read %d bytes\n", ret);
	write(1, "\n[", 2);
	write(1, buffer, ret);
	write(1, "]\n", 2);
#endif

	return ret;
}

int
http_client_execute(struct http_client *c) {

	int nparsed = http_parser_execute(&c->parser, &c->settings, c->buffer, c->sz);
	/* printf("nparsed=%d\n", nparsed); */

	if(!c->is_websocket) {
		/* removed consumed data */
		free(c->buffer);
		c->buffer = NULL;
		c->sz = 0;
	}
	return nparsed;
}

const char *
client_get_header(struct http_client *c, const char *key) {

	int i;
	size_t sz = strlen(key);

	for(i = 0; i < c->header_count; ++i) {

		if(sz == c->headers[i].key_sz &&
			strncasecmp(key, c->headers[i].key, sz) == 0) {
			return c->headers[i].val;
		}

	}

	return NULL;
}

