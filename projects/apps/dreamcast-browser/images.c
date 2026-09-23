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
#define PAGE_IMAGE_TIMEOUT_MS 12000
#define IMAGE_TIMEOUT_MS 8000

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

void document_load_images(browser_document_t *doc) {
    int i;
    size_t remaining = MAX_PAGE_IMAGE_BYTES;
    uint64_t deadline = timer_ms_gettime64() + PAGE_IMAGE_TIMEOUT_MS;
    for(i = 0; i < doc->image_count; ++i) {
        browser_image_t *image = &doc->images[i];
        fetch_result_t result;
        unsigned char *decoded;
        int source_w, source_h, channels;
        int target_w, target_h;
        int x, y;
        size_t fetch_limit;
        unsigned timeout;
        uint64_t now;
        int fetched;
        int valid_info;

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
        if(now >= deadline) {
            printf("browser: page image time budget exhausted\n");
            skip_remaining_images(doc, i);
            break;
        }

        if(remaining < MIN_IMAGE_FETCH_BYTES) {
            image->loaded = -1;
            document_touch(doc);
            printf("browser: page image budget exhausted\n");
            skip_remaining_images(doc, i + 1);
            break;
        }

        select_dreamcast_image_size(image->url, sizeof(image->url));
        fetch_limit = remaining < MAX_IMAGE_BYTES ? remaining : MAX_IMAGE_BYTES;
        timeout = (unsigned)(deadline - now);
        if(timeout > IMAGE_TIMEOUT_MS) timeout = IMAGE_TIMEOUT_MS;
        fetched = network_fetch_image(image->url, fetch_limit, timeout, &result);
        remaining -= result.size < remaining ? result.size : remaining;
        if(fetched < 0) {
            int stop = result.cancelled || result.out_of_memory;
            image->loaded = -1;
            document_touch(doc);
            printf("browser: image skipped (%s): %s\n", image->url, result.error);
            fetch_result_free(&result);
            if(stop) {
                skip_remaining_images(doc, i + 1);
                break;
            }
            continue;
        }
        if(result.status < 200 || result.status >= 300 || result.truncated ||
           !result.data || !result.size ||
           !strncasecmp(result.content_type, "image/svg+xml", 13)) {
            image->loaded = -1;
            document_touch(doc);
            printf("browser: unsupported image response: %s\n", image->url);
            fetch_result_free(&result);
            continue;
        }
        valid_info = stbi_info_from_memory(result.data, (int)result.size,
                                           &source_w, &source_h, &channels);
        if(!valid_info ||
           source_w < 1 || source_h < 1 ||
           source_w > MAX_IMAGE_SOURCE_DIM || source_h > MAX_IMAGE_SOURCE_DIM ||
           (long long)source_w * source_h > MAX_IMAGE_SOURCE_PIXELS) {
            int stop = !valid_info && decoder_out_of_memory();
            image->loaded = -1;
            document_touch(doc);
            printf("browser: unsupported or oversized image: %s\n", image->url);
            fetch_result_free(&result);
            if(stop) {
                skip_remaining_images(doc, i + 1);
                break;
            }
            continue;
        }

        decoded = stbi_load_from_memory(result.data, (int)result.size,
                                        &source_w, &source_h, &channels, 3);
        fetch_result_free(&result);
        if(!decoded) {
            int stop = decoder_out_of_memory();
            image->loaded = -1;
            document_touch(doc);
            printf("browser: image decode failed: %s\n",
                   stbi_failure_reason() ? stbi_failure_reason() : "unknown format");
            if(stop) {
                skip_remaining_images(doc, i + 1);
                break;
            }
            continue;
        }

        target_w = source_w;
        target_h = source_h;
        if(target_w > PAGE_WIDTH) {
            target_h = target_h * PAGE_WIDTH / target_w;
            target_w = PAGE_WIDTH;
        }
        if(target_h > 240) {
            target_w = target_w * 240 / target_h;
            target_h = 240;
        }
        if(target_w < 1) target_w = 1;
        if(target_h < 1) target_h = 1;
        image->pixels = malloc((size_t)target_w * target_h * sizeof(uint16_t));
        if(!image->pixels) {
            stbi_image_free(decoded);
            image->loaded = -1;
            document_touch(doc);
            printf("browser: not enough memory for image: %s\n", image->url);
            skip_remaining_images(doc, i + 1);
            break;
        }

        for(y = 0; y < target_h; ++y) {
            int sy = y * source_h / target_h;
            for(x = 0; x < target_w; ++x) {
                int sx = x * source_w / target_w;
                unsigned char *pixel = decoded + (sy * source_w + sx) * 3;
                image->pixels[y * target_w + x] = rgb565(pixel[0], pixel[1], pixel[2]);
            }
        }
        stbi_image_free(decoded);
        image->width = target_w;
        image->height = target_h;
        image->loaded = 1;
        document_touch(doc);
        printf("browser: image %dx%d -> %dx%d\n", source_w, source_h, target_w, target_h);
    }
    printf("browser: page image budget used %lu/%lu KiB\n",
           (unsigned long)((MAX_PAGE_IMAGE_BYTES - remaining) / 1024),
           (unsigned long)(MAX_PAGE_IMAGE_BYTES / 1024));
    document_reflow(doc);
}
