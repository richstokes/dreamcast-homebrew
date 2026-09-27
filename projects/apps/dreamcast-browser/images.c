#include "browser.h"

#include <kos/timer.h>
#include <stb_image/stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Decoding the old three-megapixel limit could allocate 9 MiB for RGB alone.
   Leave room for stb's intermediate buffers, TLS, the page, and the display. */
#define MAX_IMAGE_SOURCE_DIM 1024
#define MAX_IMAGE_SOURCE_PIXELS (512 * 1024)
#define MAX_IMAGE_HEIGHT 240
#define PAGE_IMAGE_TIMEOUT_MS 45000
#define IMAGE_TIMEOUT_MS 15000
#define SLOW_LINK_PATIENCE 4

static uint16_t rgb565(unsigned char r, unsigned char g, unsigned char b) {
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/* Next.js image URLs often default to a desktop-sized source even though the
   Dreamcast can display at most 640 pixels. Ask the image service for a useful
   size instead of downloading several hundred KiB only to discard it. */
static void select_dreamcast_image_size(char *url, size_t capacity) {
    char resized[MAX_URL];
    char *query;
    char *value = NULL;
    char *end;
    long width;
    size_t prefix;

    if(!strstr(url, "/_next/image?")) return;
    query = strchr(url, '?');
    while(query && *query) {
        if((query[0] == '?' || query[0] == '&') &&
           query[1] == 'w' && query[2] == '=') {
            value = query + 3;
            break;
        }
        query = strchr(query + 1, '&');
    }
    if(!value) return;

    width = strtol(value, &end, 10);
    if(end == value || width <= SCREEN_W) return;
    prefix = (size_t)(value - url);
    snprintf(resized, sizeof(resized), "%.*s%d%s",
             (int)prefix, url, SCREEN_W, end);
    snprintf(url, capacity, "%s", resized);
    printf("browser: selected %dpx responsive image\n", SCREEN_W);
}

static void skip_remaining_images(browser_document_t *doc, int first) {
    int i, skipped = 0;
    for(i = first; i < doc->image_count; ++i)
        if(!doc->images[i].loaded) {
            doc->images[i].loaded = -1;
            skipped++;
        }
    document_touch(doc);
    if(skipped)
        printf("browser: stopped asset loading; %d remaining image(s) use placeholders\n",
               skipped);
}

static int unsupported_url(const char *url) {
    static const char *extensions[] = {".svg", ".svgz", ".webp", ".avif"};
    size_t path_length = strcspn(url, "?#");
    size_t i;
    for(i = 0; i < sizeof(extensions) / sizeof(extensions[0]); ++i) {
        size_t length = strlen(extensions[i]);
        if(path_length >= length &&
           !strncasecmp(url + path_length - length, extensions[i], length))
            return 1;
    }
    return 0;
}

static int decoder_out_of_memory(void) {
    const char *reason = stbi_failure_reason();
    return reason && (strstr(reason, "outofmem") ||
                      strstr(reason, "Out of memory") ||
                      strstr(reason, "out of memory"));
}

/* Turns a downloaded file into pixels scaled for the page. This runs on the
   network worker, so a large picture never stalls input or drawing. */
static void decode_image(fetch_result_t *result, void *userdata) {
    image_loader_t *loader = userdata;
    image_decode_t *out = &loader->decoded;
    unsigned char *decoded;
    int source_w, source_h, channels;
    int target_w, target_h;
    int x, y;

    out->status = IMAGE_UNSUPPORTED;
    if(result->status < 200 || result->status >= 300 || result->truncated ||
       !result->data || !result->size ||
       !strncasecmp(result->content_type, "image/svg+xml", 13)) {
        printf("browser: unsupported image response (HTTP %ld%s)\n", result->status,
               result->truncated ? ", over the size limit" : "");
        return;
    }
    if(!stbi_info_from_memory(result->data, (int)result->size,
                              &source_w, &source_h, &channels)) {
        if(decoder_out_of_memory()) out->status = IMAGE_NO_MEMORY;
        printf("browser: unsupported image format\n");
        return;
    }
    if(source_w < 1 || source_h < 1 ||
       source_w > MAX_IMAGE_SOURCE_DIM || source_h > MAX_IMAGE_SOURCE_DIM ||
       (long long)source_w * source_h > MAX_IMAGE_SOURCE_PIXELS) {
        printf("browser: image of %dx%d is too large to decode\n", source_w, source_h);
        return;
    }

    target_w = source_w;
    target_h = source_h;
    if(target_w > PAGE_WIDTH) {
        target_h = target_h * PAGE_WIDTH / target_w;
        target_w = PAGE_WIDTH;
    }
    if(target_h > MAX_IMAGE_HEIGHT) {
        target_w = target_w * MAX_IMAGE_HEIGHT / target_h;
        target_h = MAX_IMAGE_HEIGHT;
    }
    if(target_w < 1) target_w = 1;
    if(target_h < 1) target_h = 1;
    if((size_t)target_w * target_h > loader->pixels_left) {
        /* The page has shown as many pixels as it may keep in memory. */
        out->status = IMAGE_NO_MEMORY;
        printf("browser: page image memory allowance used up\n");
        return;
    }

    decoded = stbi_load_from_memory(result->data, (int)result->size,
                                    &source_w, &source_h, &channels, 3);
    /* The file is no longer needed: release it before the pixel buffer. */
    free(result->data);
    result->data = NULL;
    if(!decoded) {
        if(decoder_out_of_memory()) out->status = IMAGE_NO_MEMORY;
        printf("browser: image decode failed: %s\n",
               stbi_failure_reason() ? stbi_failure_reason() : "unknown format");
        return;
    }
    out->pixels = malloc((size_t)target_w * target_h * sizeof(uint16_t));
    if(!out->pixels) {
        stbi_image_free(decoded);
        out->status = IMAGE_NO_MEMORY;
        printf("browser: not enough memory for image\n");
        return;
    }
    for(y = 0; y < target_h; ++y) {
        int sy = y * source_h / target_h;
        for(x = 0; x < target_w; ++x) {
            int sx = x * source_w / target_w;
            unsigned char *pixel = decoded + (sy * source_w + sx) * 3;
            out->pixels[y * target_w + x] = rgb565(pixel[0], pixel[1], pixel[2]);
        }
    }
    stbi_image_free(decoded);
    out->width = target_w;
    out->height = target_h;
    out->status = IMAGE_DECODED;
    printf("browser: image %dx%d -> %dx%d\n", source_w, source_h, target_w, target_h);
}

static void release_decoded(image_loader_t *loader) {
    free(loader->decoded.pixels);
    memset(&loader->decoded, 0, sizeof(loader->decoded));
}

void image_loader_begin(image_loader_t *loader) {
    int patience = network_slow_link() ? SLOW_LINK_PATIENCE : 1;
    release_decoded(loader);
    loader->index = -1;
    loader->bytes_left = MAX_PAGE_IMAGE_BYTES;
    loader->pixels_left = MAX_PAGE_IMAGE_PIXELS;
    loader->deadline = timer_ms_gettime64() +
                       (uint64_t)PAGE_IMAGE_TIMEOUT_MS * patience;
}

static int finished(image_loader_t *loader, browser_document_t *doc) {
    loader->index = -1;
    printf("browser: page image budget used %lu/%lu KiB\n",
           (unsigned long)((MAX_PAGE_IMAGE_BYTES - loader->bytes_left) / 1024),
           (unsigned long)(MAX_PAGE_IMAGE_BYTES / 1024));
    document_reflow(doc);
    return 0;
}

int image_loader_next(image_loader_t *loader, browser_document_t *doc,
                      fetch_request_t *request) {
    int patience = network_slow_link() ? SLOW_LINK_PATIENCE : 1;
    int i;
    for(i = 0; i < doc->image_count; ++i) {
        browser_image_t *image = &doc->images[i];
        uint64_t now;
        uint64_t timeout;

        /* Each slot gets one attempt per page. Repeated requests never leak
           an existing bitmap or restart an exhausted download budget. */
        if(image->loaded) continue;

        if(unsupported_url(image->url)) {
            image->loaded = -1;
            document_touch(doc);
            printf("browser: unsupported image format skipped: %s\n", image->url);
            continue;
        }
        now = timer_ms_gettime64();
        if(now >= loader->deadline) {
            printf("browser: page image time budget exhausted\n");
            skip_remaining_images(doc, i);
            break;
        }
        if(loader->bytes_left < MIN_IMAGE_FETCH_BYTES) {
            printf("browser: page image budget exhausted\n");
            skip_remaining_images(doc, i);
            break;
        }

        select_dreamcast_image_size(image->url, sizeof(image->url));
        timeout = loader->deadline - now;
        if(timeout > (uint64_t)IMAGE_TIMEOUT_MS * patience)
            timeout = (uint64_t)IMAGE_TIMEOUT_MS * patience;
        release_decoded(loader);
        loader->index = i;
        memset(request, 0, sizeof(*request));
        request->url = image->url;
        request->kind = FETCH_IMAGE;
        request->limit = loader->bytes_left < MAX_IMAGE_BYTES ? loader->bytes_left
                                                              : MAX_IMAGE_BYTES;
        request->timeout_ms = (unsigned)timeout;
        request->process = decode_image;
        request->userdata = loader;
        return 1;
    }
    return finished(loader, doc);
}

int image_loader_finish(image_loader_t *loader, browser_document_t *doc,
                        int code, fetch_result_t *result) {
    browser_image_t *image;
    int index = loader->index;
    int stop = 0;

    loader->index = -1;
    if(index < 0 || index >= doc->image_count) {
        release_decoded(loader);
        fetch_result_free(result);
        return -1;
    }
    image = &doc->images[index];
    loader->bytes_left -= result->size < loader->bytes_left ? result->size
                                                            : loader->bytes_left;
    if(code < 0) {
        stop = result->cancelled || result->out_of_memory;
        printf("browser: image skipped (%s): %s\n", image->url, result->error);
        release_decoded(loader);
    } else if(loader->decoded.status == IMAGE_DECODED) {
        size_t pixels = (size_t)loader->decoded.width * loader->decoded.height;
        free(image->pixels);
        image->pixels = loader->decoded.pixels;
        image->width = loader->decoded.width;
        image->height = loader->decoded.height;
        image->loaded = 1;
        loader->pixels_left -= pixels < loader->pixels_left ? pixels
                                                            : loader->pixels_left;
        memset(&loader->decoded, 0, sizeof(loader->decoded));
    } else {
        stop = loader->decoded.status == IMAGE_NO_MEMORY;
        printf("browser: image omitted: %s\n", image->url);
        release_decoded(loader);
    }
    fetch_result_free(result);
    if(image->loaded <= 0) image->loaded = -1;
    document_touch(doc);
    if(stop) {
        skip_remaining_images(doc, index + 1);
        finished(loader, doc);
        return -1;
    }
    return 0;
}
