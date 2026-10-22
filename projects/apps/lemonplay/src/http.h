/* A small HTTP/1.0 client: enough for UPnP control requests and for
   streaming a transcode. Every call blocks, so run them off the render
   thread. */
#ifndef LEMONPLAY_HTTP_H
#define LEMONPLAY_HTTP_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    char host[64];
    uint16_t port;
    char path[768];
} url_t;

int url_parse(const char *text, url_t *out);
/* Resolve ref (absolute, /rooted or relative) against base into out. */
void url_resolve(const char *base, const char *ref, char *out, size_t size);

typedef struct {
    int fd;
    int status;
    int64_t content_length;   /* -1 when the server did not say */
    char content_type[64];
} http_t;

/* Connects, sends the request and reads the response headers. body may be
   NULL. Returns 0 with h filled in, or -1. */
int http_open(http_t *h, const char *url, const char *method, const char *extra_headers,
              const char *body, size_t body_len, int timeout_ms);
/* Reads body bytes: >0 count, 0 end of body, -1 error, -2 timed out or
   aborted (*abort set). */
int http_read(http_t *h, void *buf, size_t size, int timeout_ms, volatile int *abort);
void http_close(http_t *h);

/* Whole body into a NUL-terminated malloc'd buffer (at most max bytes).
   Returns NULL on failure; *status gets the HTTP status when known. */
char *http_fetch(const char *url, const char *method, const char *extra_headers,
                 const char *body, size_t *len, size_t max, int *status);

#endif
