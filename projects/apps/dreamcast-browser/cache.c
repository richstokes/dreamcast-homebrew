/* Recently shown pages, kept as the HTML that was downloaded, so Back and
   Forward redraw a page without asking the network for it again. */

#include "browser.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char url[MAX_URL]; /* without its fragment */
    fetch_result_t source;
    unsigned used; /* larger is more recent; 0 marks a free slot */
} cached_page_t;

static cached_page_t pages[PAGE_CACHE_ENTRIES];
static unsigned clock_now;
static int current = -1; /* the page on screen is never evicted */

static void page_key(const char *url, char *key, size_t size) {
    size_t length = strcspn(url, "#");
    if(length >= size) length = size - 1;
    memcpy(key, url, length);
    key[length] = 0;
}

static void release(int index) {
    fetch_result_free(&pages[index].source);
    pages[index].url[0] = 0;
    pages[index].used = 0;
    if(current == index) current = -1;
}

static int lookup(const char *key) {
    int i;
    for(i = 0; i < PAGE_CACHE_ENTRIES; ++i)
        if(pages[i].used && !strcmp(pages[i].url, key)) return i;
    return -1;
}

/* The least recently shown page other than the one on screen. */
static int oldest(void) {
    int i, found = -1;
    for(i = 0; i < PAGE_CACHE_ENTRIES; ++i)
        if(pages[i].used && i != current &&
           (found < 0 || pages[i].used < pages[found].used))
            found = i;
    return found;
}

size_t page_cache_bytes(void) {
    size_t total = 0;
    int i;
    for(i = 0; i < PAGE_CACHE_ENTRIES; ++i)
        if(pages[i].used) total += pages[i].source.size;
    return total;
}

int page_cache_count(void) {
    int i, count = 0;
    for(i = 0; i < PAGE_CACHE_ENTRIES; ++i)
        if(pages[i].used) count++;
    return count;
}

int page_cache_evict(void) {
    int index = oldest();
    if(index < 0) return 0;
#ifndef BROWSER_QUIET
    printf("browser: page cache released %s (%lu KiB)\n", pages[index].url,
           (unsigned long)(pages[index].source.size / 1024));
#endif
    release(index);
    return 1;
}

void page_cache_clear(void) {
    int i;
    for(i = 0; i < PAGE_CACHE_ENTRIES; ++i)
        if(pages[i].used) release(i);
    current = -1;
}

void page_cache_leave(void) {
    current = -1;
}

const fetch_result_t *page_cache_find(const char *url) {
    char key[MAX_URL];
    int index;
    page_key(url, key, sizeof(key));
    index = lookup(key);
    if(index < 0) return NULL;
    pages[index].used = ++clock_now;
    current = index;
    return &pages[index].source;
}

const fetch_result_t *page_cache_store(const char *url, fetch_result_t *source) {
    char key[MAX_URL];
    int index, i;
    page_key(url, key, sizeof(key));
    index = lookup(key);
    if(index >= 0) {
        fetch_result_free(&pages[index].source);
    } else {
        for(i = 0; i < PAGE_CACHE_ENTRIES && index < 0; ++i)
            if(!pages[i].used) index = i;
        if(index < 0) {
            /* Every slot is taken. The page on screen is being replaced, so
               it may go as well if it happens to be the oldest. */
            current = -1;
            index = oldest();
            release(index);
        }
    }
    snprintf(pages[index].url, sizeof(pages[index].url), "%s", key);
    pages[index].source = *source;
    pages[index].used = ++clock_now;
    memset(source, 0, sizeof(*source));
    current = index;
    while(page_cache_bytes() > PAGE_CACHE_BYTES && page_cache_evict())
        ;
    return &pages[index].source;
}
