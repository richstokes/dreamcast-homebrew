#define _GNU_SOURCE
/* KOS cannot send IP multicast (a 239.x destination is routed to the
   gateway), so the SSDP M-SEARCH goes out as a broadcast. Servers listen on
   0.0.0.0:1900, which receives broadcasts too, and answer by unicast. */
#include "upnp.h"

#include "gfx.h"
#include "http.h"
#include "netlink.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <kos.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define BROWSE_PAGE 100
#define MAX_BROWSE_BYTES (768 * 1024)

/* ---- XML helpers: just enough for UPnP's predictable documents ---- */

/* Find "<name" as a whole tag name at or after s, before end. */
static const char *find_tag(const char *s, const char *end, const char *name) {
    size_t n = strlen(name);
    for(const char *p = s; p && p < end; p++) {
        p = memchr(p, '<', end - p);
        if(!p) return NULL;
        if(!strncmp(p + 1, name, n)) {
            char c = p[1 + n];
            if(c == '>' || c == ' ' || c == '/' || c == '\t' || c == '\r' || c == '\n') return p;
        }
    }
    return NULL;
}

/* Copy the text of the first <name>...</name> inside [s, end) into out. */
static int tag_text(const char *s, const char *end, const char *name, char *out, size_t size) {
    const char *open = find_tag(s, end, name);
    if(!open) return -1;
    const char *body = memchr(open, '>', end - open);
    if(!body) return -1;
    body++;
    char close[48];
    snprintf(close, sizeof(close), "</%s", name);
    const char *stop = strstr(body, close);
    if(!stop || stop > end) return -1;
    size_t len = stop - body;
    if(len >= size) len = size - 1;
    memcpy(out, body, len);
    out[len] = '\0';
    return 0;
}

/* Copy attribute name="..." from the tag starting at tag into out. */
static int tag_attr(const char *tag, const char *name, char *out, size_t size) {
    const char *gt = strchr(tag, '>');
    size_t n = strlen(name);
    for(const char *p = tag; p && p < gt; p++) {
        p = strstr(p, name);
        if(!p || p > gt) return -1;
        if((p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\n') && p[n] == '=' && (p[n + 1] == '"' || p[n + 1] == '\'')) {
            char q = p[n + 1];
            const char *v = p + n + 2;
            const char *e = strchr(v, q);
            if(!e) return -1;
            size_t len = e - v;
            if(len >= size) len = size - 1;
            memcpy(out, v, len);
            out[len] = '\0';
            return 0;
        }
    }
    return -1;
}

static void put_utf8(char **d, unsigned cp) {
    char *o = *d;
    if(cp < 0x80) *o++ = cp;
    else if(cp < 0x800) { *o++ = 0xc0 | (cp >> 6); *o++ = 0x80 | (cp & 0x3f); }
    else { *o++ = 0xe0 | (cp >> 12); *o++ = 0x80 | ((cp >> 6) & 0x3f); *o++ = 0x80 | (cp & 0x3f); }
    *d = o;
}

/* Decode XML entities in place (the result is never longer). */
static void unescape(char *s) {
    char *d = s;
    while(*s) {
        if(*s != '&') {
            *d++ = *s++;
            continue;
        }
        char *semi = strchr(s, ';');
        if(!semi || semi - s > 10) {
            *d++ = *s++;
            continue;
        }
        if(!strncmp(s, "&lt;", 4)) *d++ = '<';
        else if(!strncmp(s, "&gt;", 4)) *d++ = '>';
        else if(!strncmp(s, "&amp;", 5)) *d++ = '&';
        else if(!strncmp(s, "&quot;", 6)) *d++ = '"';
        else if(!strncmp(s, "&apos;", 6)) *d++ = '\'';
        else if(s[1] == '#') {
            unsigned cp = s[2] == 'x' ? strtoul(s + 3, NULL, 16) : strtoul(s + 2, NULL, 10);
            if(cp > 0 && cp < 0x10000 && (size_t)(semi + 1 - s) >= 4) put_utf8(&d, cp);
        }
        else {
            *d++ = *s++;
            continue;
        }
        s = semi + 1;
    }
    *d = '\0';
}

/* ---- Discovery ---- */

int upnp_describe(const char *location, server_t *out) {
    size_t len;
    int status;
    char *xml = http_fetch(location, "GET", NULL, NULL, &len, 64 * 1024, &status);
    if(!xml) return -1;
    const char *end = xml + len;
    int rc = -1;
    char text[256], base[256];

    memset(out, 0, sizeof(*out));
    snprintf(out->location, sizeof(out->location), "%s", location);
    if(tag_text(xml, end, "friendlyName", text, sizeof(text)) == 0) {
        unescape(text);
        utf8_to_latin1(out->name, sizeof(out->name), text);
    }
    else {
        snprintf(out->name, sizeof(out->name), "Media server");
    }
    if(tag_text(xml, end, "URLBase", base, sizeof(base)) < 0) snprintf(base, sizeof(base), "%s", location);

    for(const char *svc = find_tag(xml, end, "service"); svc; svc = find_tag(svc + 1, end, "service")) {
        const char *svc_end = strstr(svc, "</service>");
        if(!svc_end) break;
        if(tag_text(svc, svc_end, "serviceType", text, sizeof(text)) == 0 && strstr(text, "ContentDirectory") &&
           tag_text(svc, svc_end, "controlURL", text, sizeof(text)) == 0) {
            unescape(text);
            url_resolve(base, text, out->control, sizeof(out->control));
            rc = 0;
            break;
        }
    }
    free(xml);
    return rc;
}

int upnp_discover(server_t *out, int max, int timeout_ms) {
    static const char search[] =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: urn:schemas-upnp-org:device:MediaServer:1\r\n"
        "\r\n";
    char locations[MAX_SERVERS][256];
    int found = 0;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) return 0;
    struct sockaddr_in any = {0};
    any.sin_family = AF_INET;
    any.sin_port = htons(0);
    bind(fd, (struct sockaddr *)&any, sizeof(any));

    uint32_t targets[2] = {0xffffffffu, link_broadcast()};
    uint64_t start = timer_ms_gettime64(), next_send = start;
    char buf[1024];
    while(timer_ms_gettime64() - start < (uint64_t)timeout_ms) {
        uint64_t now = timer_ms_gettime64();
        if(now >= next_send) {
            for(int i = 0; i < 2; i++) {
                if(i && targets[1] == targets[0]) break;
                struct sockaddr_in to = {0};
                to.sin_family = AF_INET;
                to.sin_port = htons(1900);
                to.sin_addr.s_addr = targets[i];
                sendto(fd, search, sizeof(search) - 1, 0, (struct sockaddr *)&to, sizeof(to));
            }
            next_send = now + 1000;
        }
        struct pollfd p = {fd, POLLIN, 0};
        if(poll(&p, 1, 100) <= 0) continue;
        ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, NULL, NULL);
        if(n <= 0) continue;
        buf[n] = '\0';
        for(char *line = buf; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL) {
            if(strncasecmp(line, "LOCATION:", 9)) continue;
            char *v = line + 9;
            while(*v == ' ') v++;
            size_t len = strcspn(v, "\r\n");
            if(!len || len >= sizeof(locations[0]) || found >= MAX_SERVERS) break;
            char loc[256];
            memcpy(loc, v, len);
            loc[len] = '\0';
            int seen = 0;
            for(int i = 0; i < found; i++) seen |= !strcmp(locations[i], loc);
            if(!seen) snprintf(locations[found++], sizeof(locations[0]), "%s", loc);
            break;
        }
    }
    close(fd);

    int described = 0;
    for(int i = 0; i < found && described < max; i++) {
        if(upnp_describe(locations[i], &out[described]) == 0) described++;
    }
    return described;
}

/* ---- Browsing ---- */

/* "Episode 1.mkv" -> "Episode 1": servers often use file names as titles. */
static void strip_extension(char *title) {
    char *dot = strrchr(title, '.');
    if(!dot || dot == title || strlen(dot) > 5 || strlen(dot) < 2) return;
    for(const char *c = dot + 1; *c; c++) {
        if(!isalnum((unsigned char)*c)) return;
    }
    *dot = '\0';
    /* and any spaces left before it */
    while(dot > title && dot[-1] == ' ') *--dot = '\0';
}

static int parse_duration(const char *text) {
    int h = 0, m = 0, s = 0;
    if(sscanf(text, "%d:%d:%d", &h, &m, &s) == 3) return h * 3600 + m * 60 + s;
    return -1;
}

static void parse_entry(const char *start, const char *end, int folder, entry_t *e) {
    char text[512];
    memset(e, 0, sizeof(*e));
    e->duration = -1;
    tag_attr(start, "id", e->id, sizeof(e->id));
    unescape(e->id);
    if(tag_text(start, end, "dc:title", text, sizeof(text)) == 0) {
        unescape(text);
        utf8_to_latin1(e->title, sizeof(e->title), text);
    }
    if(folder) {
        e->kind = ENTRY_FOLDER;
        return;
    }
    strip_extension(e->title);
    e->kind = ENTRY_OTHER;
    if(tag_text(start, end, "upnp:class", text, sizeof(text)) == 0) {
        if(strstr(text, "videoItem")) e->kind = ENTRY_VIDEO;
        else if(strstr(text, "audioItem")) e->kind = ENTRY_AUDIO;
        else if(strstr(text, "imageItem")) e->kind = ENTRY_IMAGE;
    }
    for(const char *res = find_tag(start, end, "res"); res; res = find_tag(res + 1, end, "res")) {
        const char *body = strchr(res, '>');
        const char *stop = body ? strstr(body, "</res>") : NULL;
        if(!stop || stop > end) break;
        size_t len = stop - body - 1;
        if(len >= sizeof(e->url)) continue;
        int dreamcast = memmem(body, len + 1, "/dreamcast/", 11) != NULL;
        if(e->url[0] && !dreamcast) continue;
        memcpy(e->url, body + 1, len);
        e->url[len] = '\0';
        unescape(e->url);
        e->playable = dreamcast;
        if(tag_attr(res, "duration", text, sizeof(text)) == 0) e->duration = parse_duration(text);
        if(dreamcast) break;
    }
}

/* Natural order: "Episode 2" before "Episode 10". */
static int natural_cmp(const char *a, const char *b) {
    while(*a && *b) {
        if(isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            while(*a == '0') a++;
            while(*b == '0') b++;
            size_t la = 0, lb = 0;
            while(isdigit((unsigned char)a[la])) la++;
            while(isdigit((unsigned char)b[lb])) lb++;
            if(la != lb) return la < lb ? -1 : 1;
            int c = strncmp(a, b, la);
            if(c) return c;
            a += la;
            b += lb;
            continue;
        }
        int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
        if(ca != cb) return ca - cb;
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int entry_cmp(const void *pa, const void *pb) {
    const entry_t *a = pa, *b = pb;
    if((a->kind == ENTRY_FOLDER) != (b->kind == ENTRY_FOLDER)) return a->kind == ENTRY_FOLDER ? -1 : 1;
    return natural_cmp(a->title, b->title);
}

static void xml_escape(char *out, size_t size, const char *in) {
    size_t n = 0;
    for(; *in && n + 7 < size; in++) {
        const char *rep = *in == '&' ? "&amp;" : *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : NULL;
        if(rep) {
            n += snprintf(out + n, size - n, "%s", rep);
        }
        else {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
}

int upnp_browse(const server_t *server, const char *object_id, entry_t *out, int max) {
    int count = 0;
    int start = 0;
    char id[160];
    xml_escape(id, sizeof(id), object_id);

    for(;;) {
        char body[900];
        snprintf(body, sizeof(body),
                 "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
                 "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                 "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
                 "<u:Browse xmlns:u=\"urn:schemas-upnp-org:service:ContentDirectory:1\">"
                 "<ObjectID>%s</ObjectID><BrowseFlag>BrowseDirectChildren</BrowseFlag>"
                 "<Filter>*</Filter><StartingIndex>%d</StartingIndex><RequestedCount>%d</RequestedCount>"
                 "<SortCriteria></SortCriteria></u:Browse></s:Body></s:Envelope>\r\n",
                 id, start, BROWSE_PAGE);
        size_t len;
        int status;
        char *xml = http_fetch(server->control, "POST",
                               "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                               "SOAPAction: \"urn:schemas-upnp-org:service:ContentDirectory:1#Browse\"\r\n",
                               body, &len, MAX_BROWSE_BYTES, &status);
        if(!xml) return count ? count : -1;
        if(status != 200) {
            free(xml);
            return count ? count : -1;
        }

        char numbers[16];
        int returned = 0, total = 0;
        if(tag_text(xml, xml + len, "NumberReturned", numbers, sizeof(numbers)) == 0) returned = atoi(numbers);
        if(tag_text(xml, xml + len, "TotalMatches", numbers, sizeof(numbers)) == 0) total = atoi(numbers);

        /* The DIDL-Lite document arrives escaped inside <Result>. */
        const char *open = find_tag(xml, xml + len, "Result");
        char *didl = open ? strchr(open, '>') : NULL;
        char *didl_end = didl ? strstr(didl, "</Result>") : NULL;
        if(didl && didl_end) {
            didl++;
            *didl_end = '\0';
            unescape(didl);
            const char *end = didl + strlen(didl);
            const char *p = didl;
            while(count < max) {
                const char *c = find_tag(p, end, "container");
                const char *i = find_tag(p, end, "item");
                const char *tag = c && (!i || c < i) ? c : i;
                if(!tag) break;
                int folder = tag == c;
                const char *close = strstr(tag, folder ? "</container>" : "</item>");
                if(!close) break;
                parse_entry(tag, close, folder, &out[count]);
                /* Only folders, video and music: LemonPlay can't show the rest. */
                int kind = out[count].kind;
                if(out[count].id[0] && (kind == ENTRY_FOLDER || kind == ENTRY_VIDEO || kind == ENTRY_AUDIO)) count++;
                p = close + 1;
            }
        }
        free(xml);
        start += returned;
        if(returned <= 0 || start >= total || count >= max) break;
    }
    qsort(out, count, sizeof(*out), entry_cmp);
    return count;
}
