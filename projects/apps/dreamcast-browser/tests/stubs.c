/* Host stand-ins for the libcurl-backed helpers in net.c. */

#include "browser.h"

#include <stdio.h>
#include <string.h>

int resolve_url(const char *base, const char *reference, char *out, size_t out_size) {
    const char *slash;
    size_t origin;

    if(!reference || !reference[0]) return -1;
    if(reference[0] == '#') {
        const char *fragment = strchr(base, '#');
        size_t length = fragment ? (size_t)(fragment - base) : strlen(base);
        if(length + strlen(reference) >= out_size) return -1;
        snprintf(out, out_size, "%.*s%s", (int)length, base, reference);
        return 0;
    }
    if(!strncmp(reference, "about:", 6)) {
        snprintf(out, out_size, "%s", reference);
        return 0;
    }
    if(!strncmp(reference, "http://", 7) || !strncmp(reference, "https://", 8)) {
        snprintf(out, out_size, "%s", reference);
        return 0;
    }
    if(strchr(reference, ':') &&
       strchr(reference, ':') < (strchr(reference, '/') ? strchr(reference, '/') :
                                 reference + strlen(reference)))
        return -1;
    slash = strncmp(base, "http", 4) ? NULL : strstr(base, "://");
    if(!slash) return -1;
    slash = strchr(slash + 3, '/');
    origin = slash ? (size_t)(slash - base) : strlen(base);
    if(reference[0] == '/') {
        snprintf(out, out_size, "%.*s%s", (int)origin, base, reference);
    } else {
        const char *last = strrchr(base + origin, '/');
        size_t directory = last ? (size_t)(last - base) + 1 : origin;
        snprintf(out, out_size, "%.*s%s%s", (int)directory, base,
                 last ? "" : "/", reference);
    }
    return 0;
}
