/*
 * http.c — the HTTP/1.1 arc-llm speaks: one request per connection, a Content-Length body of at
 * most ARC_LLM_MAX_BODY, and a response with Content-Length, or chunked for a stream; always
 * `Connection: close`. Sends use MSG_NOSIGNAL, so a client that went away is an error, not SIGPIPE.
 */
#ifndef ARCHOS_STACK_ARC_LLM_HTTP_H
#define ARCHOS_STACK_ARC_LLM_HTTP_H

#include <stdbool.h>
#include <stddef.h>

#define HTTP_HEAD_MAX 16384 /* request line and headers */

typedef struct {
    int fd;
    char head[HTTP_HEAD_MAX + 1];
    size_t head_len; /* bytes in head[], including any body bytes read with it */
    char method[16];
    char path[256];
    char *body; /* malloc'd, NUL-terminated; NULL without a body */
    size_t body_len;
    bool gone; /* a send failed: the client went away or stopped reading */
} http_conn;

/*
 * http_read_request: read the request on c->fd. Returns 0, or the HTTP status to answer with
 * (400 malformed, 411 no length for a body, 413 body too large, 431 headers too large, 501
 * transfer encodings) and *why. On 0 c->method, c->path and c->body are set.
 */
int http_read_request(http_conn *c, const char **why);

/* http_free: free the body. */
void http_free(http_conn *c);

/* http_respond: a whole response: status line, Content-Type, Content-Length, body. */
void http_respond(http_conn *c, int status, const char *content_type, const char *body, size_t len);

/* http_stream_begin: status line and headers of a chunked response. */
void http_stream_begin(http_conn *c, int status, const char *content_type);

/* http_sse_event: one server-sent event, `data: <data>` and a blank line, as one chunk. Sets c->gone if the client went away. */
void http_sse_event(http_conn *c, const char *data, size_t len);

/* http_stream_end: the last, empty chunk. */
void http_stream_end(http_conn *c);

#endif /* ARCHOS_STACK_ARC_LLM_HTTP_H */
