#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <kos.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define CONNECT_TIMEOUT_MS 4000

int url_parse(const char *text, url_t *out) {
    memset(out, 0, sizeof(*out));
    if(strncasecmp(text, "http://", 7)) return -1;
    const char *host = text + 7;
    const char *end = host + strcspn(host, ":/");
    size_t host_len = end - host;
    if(!host_len || host_len >= sizeof(out->host)) return -1;
    memcpy(out->host, host, host_len);
    out->port = 80;
    if(*end == ':') {
        out->port = (uint16_t)strtoul(end + 1, (char **)&end, 10);
    }
    snprintf(out->path, sizeof(out->path), "%s", *end == '/' ? end : "/");
    return 0;
}

void url_resolve(const char *base, const char *ref, char *out, size_t size) {
    if(!strncasecmp(ref, "http://", 7)) {
        snprintf(out, size, "%s", ref);
        return;
    }
    url_t b;
    if(url_parse(base, &b) < 0) {
        snprintf(out, size, "%s", ref);
        return;
    }
    if(ref[0] == '/') {
        snprintf(out, size, "http://%s:%u%s", b.host, b.port, ref);
        return;
    }
    char *slash = strrchr(b.path, '/');
    if(slash) slash[1] = '\0';
    snprintf(out, size, "http://%s:%u%s%s", b.host, b.port, b.path, ref);
}

static int wait_fd(int fd, short events, int timeout_ms) {
    struct pollfd p = {fd, events, 0};
    int r = poll(&p, 1, timeout_ms);
    if(r <= 0) return r;
    return 1;
}

static int connect_to(const url_t *u) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u->port);
    if(inet_pton(AF_INET, u->host, &addr.sin_addr) != 1) return -1;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) return -1;
    fcntl(fd, F_SETFL, O_NONBLOCK);
    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if(wait_fd(fd, POLLOUT, CONNECT_TIMEOUT_MS) <= 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *data, size_t len) {
    while(len) {
        if(wait_fd(fd, POLLOUT, 5000) <= 0) return -1;
        ssize_t n = send(fd, data, len, 0);
        if(n < 0) {
            if(errno == EAGAIN) continue;
            return -1;
        }
        data += n;
        len -= n;
    }
    return 0;
}

int http_open(http_t *h, const char *url, const char *method, const char *extra_headers,
              const char *body, size_t body_len, int timeout_ms) {
    url_t u;
    memset(h, 0, sizeof(*h));
    h->fd = -1;
    h->content_length = -1;
    if(url_parse(url, &u) < 0) return -1;
    int fd = connect_to(&u);
    if(fd < 0) return -1;

    char head[1200];
    int n = snprintf(head, sizeof(head),
                     "%s %s HTTP/1.0\r\nHost: %s:%u\r\nUser-Agent: LemonPlay/1.0 Dreamcast DLNADOC/1.50\r\n"
                     "Connection: close\r\n%s",
                     method, u.path, u.host, u.port, extra_headers ? extra_headers : "");
    if(body) n += snprintf(head + n, sizeof(head) - n, "Content-Length: %u\r\n", (unsigned)body_len);
    n += snprintf(head + n, sizeof(head) - n, "\r\n");
    if(n >= (int)sizeof(head) || send_all(fd, head, n) < 0 || (body && send_all(fd, body, body_len) < 0)) {
        close(fd);
        return -1;
    }

    /* Read headers a byte at a time: they are short and this leaves the
       socket positioned exactly at the body. */
    char line[512];
    size_t len = 0;
    int first = 1;
    for(;;) {
        if(wait_fd(fd, POLLIN, timeout_ms) <= 0) goto fail;
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if(r < 0 && errno == EAGAIN) continue;
        if(r <= 0) goto fail;
        if(c != '\n') {
            if(len + 1 < sizeof(line)) line[len++] = c;
            continue;
        }
        if(len && line[len - 1] == '\r') len--;
        line[len] = '\0';
        if(first) {
            const char *sp = strchr(line, ' ');
            h->status = sp ? atoi(sp + 1) : 0;
            first = 0;
        }
        else if(!len) {
            break;
        }
        else if(!strncasecmp(line, "Content-Length:", 15)) {
            h->content_length = strtoll(line + 15, NULL, 10);
        }
        else if(!strncasecmp(line, "Content-Type:", 13)) {
            const char *v = line + 13;
            while(*v == ' ') v++;
            snprintf(h->content_type, sizeof(h->content_type), "%.63s", v);
        }
        len = 0;
    }
    h->fd = fd;
    return 0;

fail:
    close(fd);
    return -1;
}

int http_read(http_t *h, void *buf, size_t size, int timeout_ms, volatile int *abort) {
    int waited = 0;
    for(;;) {
        if(abort && *abort) return -2;
        int r = wait_fd(h->fd, POLLIN, 50);
        if(r < 0) return -1;
        if(r == 0) {
            waited += 50;
            if(waited >= timeout_ms) return -2;
            continue;
        }
        ssize_t n = recv(h->fd, buf, size, 0);
        if(n < 0 && errno == EAGAIN) continue;
        if(n < 0) return -1;
        return (int)n;
    }
}

void http_close(http_t *h) {
    if(h->fd >= 0) close(h->fd);
    h->fd = -1;
}

char *http_fetch(const char *url, const char *method, const char *extra_headers,
                 const char *body, size_t *len, size_t max, int *status) {
    http_t h;
    if(status) *status = 0;
    if(http_open(&h, url, method, extra_headers, body, body ? strlen(body) : 0, 8000) < 0) return NULL;
    if(status) *status = h.status;
    size_t cap = h.content_length > 0 && (size_t)h.content_length < max ? (size_t)h.content_length + 1 : 16384;
    size_t used = 0;
    char *data = malloc(cap);
    while(data) {
        if(used + 1 >= cap) {
            if(cap >= max) break;
            cap = cap * 2 > max ? max : cap * 2;
            char *grown = realloc(data, cap);
            if(!grown) {
                free(data);
                data = NULL;
                break;
            }
            data = grown;
        }
        int n = http_read(&h, data + used, cap - used - 1, 8000, NULL);
        if(n == 0) break;
        if(n < 0) {
            free(data);
            data = NULL;
            break;
        }
        used += n;
    }
    http_close(&h);
    if(!data) return NULL;
    data[used] = '\0';
    if(len) *len = used;
    return data;
}
