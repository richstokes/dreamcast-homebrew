/* Decides whether address-bar text is a location or a search, the way
   desktop browsers' combined address bars do. */

#include "browser.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static int is_ipv4(const char *host, size_t length) {
    int dots = 0;
    int digits = 0;
    size_t i;
    for(i = 0; i < length; ++i) {
        if(host[i] == '.') {
            if(!digits) return 0;
            dots++;
            digits = 0;
        } else if(isdigit((unsigned char)host[i]) && digits < 3) {
            digits++;
        } else {
            return 0;
        }
    }
    return dots == 3 && digits;
}

/* A host looks real when its last label is an alphabetic top-level domain. */
static int has_domain(const char *host, size_t length) {
    const char *dot = NULL;
    size_t i;
    size_t tld;
    for(i = 0; i < length; ++i) {
        if(host[i] == '.') {
            if(!i || host[i - 1] == '.') return 0;
            dot = host + i;
        } else if(!isalnum((unsigned char)host[i]) && host[i] != '-') {
            return 0;
        }
    }
    if(!dot) return 0;
    tld = (size_t)(host + length - dot - 1);
    if(tld < 2) return 0;
    for(i = 1; i <= tld; ++i)
        if(!isalpha((unsigned char)dot[i])) return 0;
    return 1;
}

static void trim(const char *text, const char **start, size_t *length) {
    size_t n;
    while(*text == ' ') text++;
    n = strlen(text);
    while(n && text[n - 1] == ' ') n--;
    *start = text;
    *length = n;
}

int address_is_search(const char *text) {
    const char *start;
    size_t length;
    size_t host;
    size_t port;

    trim(text, &start, &length);
    if(!length) return 0;
    if(start[0] == '?') return 1;
    if(!strncasecmp(start, "about:", 6)) return 0;
    if(memchr(start, ' ', length)) return 1;
    {
        const char *scheme = strstr(start, "://");
        if(scheme && scheme < start + length) return 0;
    }
    host = strcspn(start, "/?#");
    if(host > length) host = length;
    port = host;
    {
        const char *colon = memchr(start, ':', host);
        if(colon) {
            size_t i;
            port = (size_t)(colon - start);
            for(i = port + 1; i < host; ++i)
                if(!isdigit((unsigned char)start[i])) return 1;
        }
    }
    if(port == 9 && !strncasecmp(start, "localhost", 9)) return 0;
    if(is_ipv4(start, port)) return 0;
    return !has_domain(start, port);
}

/* Produces the URL to open: searches go through search_prefix with the
   query form-encoded; bare hosts default to HTTPS. */
int address_resolve(const char *text, const char *search_prefix, char *out,
                    size_t out_size) {
    const char *start;
    size_t length;
    size_t n;
    size_t i;

    trim(text, &start, &length);
    if(!length || !out_size) return -1;
    if(!address_is_search(text)) {
        const char *scheme = strstr(start, "://");
        int has_scheme = !strncasecmp(start, "about:", 6) ||
                         (scheme && scheme < start + length);
        n = (size_t)snprintf(out, out_size, "%s%.*s", has_scheme ? "" : "https://",
                             (int)length, start);
        return n < out_size ? 0 : -1;
    }
    if(start[0] == '?') {
        start++;
        length--;
        while(length && *start == ' ') { start++; length--; }
        if(!length) return -1;
    }
    n = (size_t)snprintf(out, out_size, "%s", search_prefix);
    if(n >= out_size) return -1;
    for(i = 0; i < length; ++i) {
        unsigned char c = (unsigned char)start[i];
        char encoded[16];
        size_t count;
        if(isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded[0] = (char)c;
            count = 1;
        } else if(c == ' ') {
            encoded[0] = '+';
            count = 1;
        } else if(c >= 0x80) {
            /* Typed text is ISO-8859-1; queries travel as UTF-8. */
            count = (size_t)snprintf(encoded, sizeof(encoded), "%%%02X%%%02X",
                                     0xc0 | (c >> 6), 0x80 | (c & 0x3f));
        } else {
            count = (size_t)snprintf(encoded, sizeof(encoded), "%%%02X", c);
        }
        if(n + count >= out_size) return -1;
        memcpy(out + n, encoded, count);
        n += count;
    }
    out[n] = 0;
    return 0;
}
