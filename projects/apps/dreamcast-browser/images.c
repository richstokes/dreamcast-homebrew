#include "browser.h"

#include <stb_image/stb_image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    int i;
    for(i = first; i < doc->image_count; ++i)
        doc->images[i].loaded = -1;
    if(first < doc->image_count)
        printf("browser: stopped asset loading; %d remaining image(s) use placeholders\n",
               doc->image_count - first);
}

void document_load_images(browser_document_t *doc) {
    int i;
    size_t remaining = MAX_PAGE_IMAGE_BYTES;
    for(i = 0; i < doc->image_count; ++i) {
        browser_image_t *image = &doc->images[i];
        fetch_result_t result;
        unsigned char *decoded;
        int source_w, source_h, channels;
        int target_w, target_h;
        int x, y;
        size_t fetch_limit;

        if(remaining < MIN_IMAGE_FETCH_BYTES) {
            image->loaded = -1;
            printf("browser: page image budget exhausted\n");
            skip_remaining_images(doc, i + 1);
            break;
        }

        select_dreamcast_image_size(image->url, sizeof(image->url));
        fetch_limit = remaining < MAX_IMAGE_BYTES ? remaining : MAX_IMAGE_BYTES;
        if(network_fetch(image->url, fetch_limit, &result) < 0) {
            image->loaded = -1;
            printf("browser: image skipped (%s): %s\n", image->url, result.error);
            skip_remaining_images(doc, i + 1);
            break;
        }
        remaining -= result.size;
        if(result.status < 200 || result.status >= 300 || result.truncated ||
           !stbi_info_from_memory(result.data, (int)result.size,
                                  &source_w, &source_h, &channels) ||
           source_w < 1 || source_h < 1 || source_w > 2048 || source_h > 2048 ||
           (long long)source_w * source_h > 3000000) {
            image->loaded = -1;
            printf("browser: unsupported or oversized image: %s\n", image->url);
            fetch_result_free(&result);
            skip_remaining_images(doc, i + 1);
            break;
        }

        decoded = stbi_load_from_memory(result.data, (int)result.size,
                                        &source_w, &source_h, &channels, 3);
        fetch_result_free(&result);
        if(!decoded) {
            image->loaded = -1;
            printf("browser: image decode failed: %s\n", stbi_failure_reason());
            skip_remaining_images(doc, i + 1);
            break;
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
        printf("browser: image %dx%d -> %dx%d\n", source_w, source_h, target_w, target_h);
    }
    printf("browser: page image budget used %lu/%lu KiB\n",
           (unsigned long)((MAX_PAGE_IMAGE_BYTES - remaining) / 1024),
           (unsigned long)(MAX_PAGE_IMAGE_BYTES / 1024));
    document_reflow(doc);
}
