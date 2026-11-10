/*
 * http.c — see http.h. A request is read with recv into a fixed 16 KiB head buffer and one
 * malloc'd body; responses go out with one sendmsg each (headers and body as two iovecs).
 */
#define _GNU_SOURCE
#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "arc_llm.h"

/* send_all: write every byte of the iovecs; on failure mark the client gone. */
static void send_all(http_conn *c, struct iovec *iov, int n) {
    while (!c->gone && n > 0) {
        struct msghdr msg = {.msg_iov = iov, .msg_iovlen = (size_t)n};
        ssize_t sent = sendmsg(c->fd, &msg, MSG_NOSIGNAL);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            c->gone = true;
            return;
        }
        while (n > 0 && (size_t)sent >= iov->iov_len) {
            sent -= (ssize_t)iov->iov_len;
            iov++;
            n--;
        }
        if (n > 0) {
            iov->iov_base = (char *)iov->iov_base + sent;
            iov->iov_len -= (size_t)sent;
        }
    }
}

/* recv_some: one recv into buf; returns bytes read, 0 at end of stream or on error. */
static size_t recv_some(http_conn *c, char *buf, size_t len) {
    for (;;) {
        ssize_t n = recv(c->fd, buf, len, 0);
        if (n < 0 && errno == EINTR && !arc_llm_stopping) {
            continue;
        }
        return n > 0 ? (size_t)n : 0;
    }
}

/* header_value: the value of header `name` in the header block (case-insensitive name), or NULL. */
static const char *header_value(const char *headers, const char *name, size_t *len) {
    size_t name_len = strlen(name);
    for (const char *line = headers; line != NULL && *line != '\0';) {
        const char *end = strstr(line, "\r\n");
        if (end == NULL || end == line) {
            return NULL;
        }
        if ((size_t)(end - line) > name_len && strncasecmp(line, name, name_len) == 0 && line[name_len] == ':') {
            const char *v = line + name_len + 1;
            while (v < end && (*v == ' ' || *v == '\t')) {
                v++;
            }
            const char *e = end;
            while (e > v && (e[-1] == ' ' || e[-1] == '\t')) {
                e--;
            }
            *len = (size_t)(e - v);
            return v;
        }
        line = end + 2;
    }
    return NULL;
}

/* parse_request_line: "METHOD SP path SP HTTP/1.x"; false if malformed. */
static bool parse_request_line(http_conn *c, const char *line, size_t len) {
    const char *sp1 = memchr(line, ' ', len);
    const char *sp2 = sp1 ? memchr(sp1 + 1, ' ', len - (size_t)(sp1 + 1 - line)) : NULL;
    if (sp1 == NULL || sp2 == NULL) {
        return false;
    }
    size_t mlen = (size_t)(sp1 - line), plen = (size_t)(sp2 - sp1 - 1), vlen = len - (size_t)(sp2 + 1 - line);
    if (mlen == 0 || mlen >= sizeof(c->method) || plen == 0 || plen >= sizeof(c->path) || sp1[1] != '/' || vlen != 8 ||
        strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
        return false;
    }
    memcpy(c->method, line, mlen);
    c->method[mlen] = '\0';
    memcpy(c->path, sp1 + 1, plen);
    c->path[plen] = '\0';
    return true;
}

/* parse_length: a Content-Length value, digits only; false if malformed. */
static bool parse_length(const char *v, size_t len, size_t *out) {
    if (len == 0 || len > 12) {
        return false;
    }
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (v[i] < '0' || v[i] > '9') {
            return false;
        }
        n = n * 10 + (size_t)(v[i] - '0');
    }
    *out = n;
    return true;
}

int http_read_request(http_conn *c, const char **why) {
    char *end = NULL;
    c->head_len = 0;
    while (end == NULL) {
        if (c->head_len == HTTP_HEAD_MAX) {
            *why = "request headers larger than 16 KiB";
            return 431;
        }
        size_t n = recv_some(c, c->head + c->head_len, HTTP_HEAD_MAX - c->head_len);
        if (n == 0) {
            *why = "connection closed before the request was complete";
            return 400;
        }
        size_t from = c->head_len > 3 ? c->head_len - 3 : 0;
        c->head_len += n;
        c->head[c->head_len] = '\0';
        end = memmem(c->head + from, c->head_len - from, "\r\n\r\n", 4);
    }
    const char *line_end = strstr(c->head, "\r\n");
    if (!parse_request_line(c, c->head, (size_t)(line_end - c->head))) {
        *why = "malformed request line";
        return 400;
    }
    end[2] = '\0'; /* headers end with their last CRLF; body bytes already read follow end + 4 */
    const char *headers = line_end + 2;
    size_t vlen = 0;
    if (header_value(headers, "Transfer-Encoding", &vlen) != NULL) {
        *why = "chunked request bodies are not supported; send Content-Length";
        return 501;
    }
    const char *cl = header_value(headers, "Content-Length", &vlen);
    size_t body_len = 0;
    if (cl != NULL && !parse_length(cl, vlen, &body_len)) {
        *why = "malformed Content-Length";
        return 400;
    }
    if (cl == NULL && strcmp(c->method, "POST") == 0) {
        *why = "a POST needs Content-Length";
        return 411;
    }
    if (body_len > ARC_LLM_MAX_BODY) {
        *why = "request body larger than 1 MiB";
        return 413;
    }
    const char *expect = header_value(headers, "Expect", &vlen);
    if (expect != NULL && vlen == 12 && strncasecmp(expect, "100-continue", 12) == 0) {
        struct iovec iov = {.iov_base = "HTTP/1.1 100 Continue\r\n\r\n", .iov_len = 25};
        send_all(c, &iov, 1);
    }
    c->body = malloc(body_len + 1);
    if (c->body == NULL) {
        *why = "out of memory";
        return 500;
    }
    size_t have = c->head_len - (size_t)(end + 4 - c->head);
    if (have > body_len) {
        have = body_len; /* a client may not pipeline: one request per connection */
    }
    memcpy(c->body, end + 4, have);
    while (have < body_len) {
        size_t n = recv_some(c, c->body + have, body_len - have);
        if (n == 0) {
            *why = "connection closed before the body was complete";
            return 400;
        }
        have += n;
    }
    c->body[body_len] = '\0';
    c->body_len = body_len;
    return 0;
}

void http_free(http_conn *c) {
    free(c->body);
    c->body = NULL;
    c->body_len = 0;
}

static const char *reason(int status) {
    switch (status) {
    case 100:
        return "Continue";
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 411:
        return "Length Required";
    case 413:
        return "Content Too Large";
    case 431:
        return "Request Header Fields Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 507:
        return "Insufficient Storage";
    default:
        return "Error";
    }
}

void http_respond(http_conn *c, int status, const char *content_type, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status,
                     reason(status), content_type, len);
    struct iovec iov[2] = {{.iov_base = head, .iov_len = (size_t)n}, {.iov_base = (void *)body, .iov_len = len}};
    send_all(c, iov, len ? 2 : 1);
}

void http_stream_begin(http_conn *c, int status, const char *content_type) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nCache-Control: no-cache\r\nTransfer-Encoding: chunked\r\n"
                     "Connection: close\r\n\r\n",
                     status, reason(status), content_type);
    struct iovec iov = {.iov_base = head, .iov_len = (size_t)n};
    send_all(c, &iov, 1);
}

void http_sse_event(http_conn *c, const char *data, size_t len) {
    char size[24];
    int n = snprintf(size, sizeof(size), "%zx\r\n", len + 8); /* "data: " + data + "\n\n" */
    struct iovec iov[4] = {{.iov_base = size, .iov_len = (size_t)n},
                           {.iov_base = "data: ", .iov_len = 6},
                           {.iov_base = (void *)data, .iov_len = len},
                           {.iov_base = "\n\n\r\n", .iov_len = 4}};
    send_all(c, iov, 4);
}

void http_stream_end(http_conn *c) {
    struct iovec iov = {.iov_base = "0\r\n\r\n", .iov_len = 5};
    send_all(c, &iov, 1);
}
