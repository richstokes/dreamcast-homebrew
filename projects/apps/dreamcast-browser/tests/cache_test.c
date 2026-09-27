#include "browser.h"
#include "test.h"

#include <stdlib.h>

static fetch_result_t page(size_t size, char fill) {
    fetch_result_t source;
    memset(&source, 0, sizeof(source));
    source.data = malloc(size + 1);
    memset(source.data, fill, size);
    source.data[size] = 0;
    source.size = size;
    source.status = 200;
    snprintf(source.content_type, sizeof(source.content_type), "text/html");
    return source;
}

static const fetch_result_t *store(const char *url, size_t size, char fill) {
    fetch_result_t source = page(size, fill);
    const fetch_result_t *kept;
    snprintf(source.effective_url, sizeof(source.effective_url), "%s", url);
    kept = page_cache_store(url, &source);
    /* The cache owns the buffer now; the caller's copy is emptied. */
    CHECK(source.data == NULL && source.size == 0);
    return kept;
}

void cache_tests(void) {
    const fetch_result_t *a, *b, *found;
    char url[64];
    int i;

    page_cache_clear();
    CHECK(page_cache_count() == 0 && page_cache_bytes() == 0);
    CHECK(page_cache_find("https://a.test/") == NULL);
    CHECK(!page_cache_evict());

    a = store("https://a.test/", 1000, 'a');
    CHECK(a && a->size == 1000 && a->data[0] == 'a' && a->status == 200);
    CHECK_STR(a->content_type, "text/html");
    b = store("https://b.test/page#part", 2000, 'b');
    CHECK(page_cache_count() == 2 && page_cache_bytes() == 3000);

    /* A section of a page is the same page. */
    found = page_cache_find("https://b.test/page");
    CHECK(found == b);
    found = page_cache_find("https://b.test/page#other");
    CHECK(found == b && found->data[0] == 'b');
    CHECK(page_cache_find("https://b.test/page?x=1") == NULL);
    CHECK(page_cache_find("https://a.test/") == a);

    /* Loading an address again replaces what was kept for it. */
    a = store("https://a.test/", 500, 'A');
    CHECK(page_cache_count() == 2 && page_cache_bytes() == 2500 && a->data[0] == 'A');

    /* The page on screen is never released; older ones go first. */
    CHECK(page_cache_evict() && page_cache_count() == 1);
    CHECK(page_cache_find("https://b.test/page") == NULL);
    CHECK(!page_cache_evict() && page_cache_count() == 1);
    page_cache_leave();
    CHECK(page_cache_evict() && page_cache_count() == 0);

    /* More pages than slots: the least recently shown is replaced. */
    for(i = 0; i < PAGE_CACHE_ENTRIES; ++i) {
        snprintf(url, sizeof(url), "https://site.test/%d", i);
        store(url, 100, (char)('0' + i));
    }
    CHECK(page_cache_count() == PAGE_CACHE_ENTRIES);
    CHECK(page_cache_find("https://site.test/0") != NULL); /* 0 is recent again */
    store("https://site.test/new", 100, 'n');
    CHECK(page_cache_count() == PAGE_CACHE_ENTRIES);
    CHECK(page_cache_find("https://site.test/1") == NULL);
    CHECK(page_cache_find("https://site.test/0") != NULL);
    CHECK(page_cache_find("https://site.test/new") != NULL);

    /* The byte allowance holds however large the pages are. */
    page_cache_clear();
    for(i = 0; i < 4; ++i) {
        snprintf(url, sizeof(url), "https://big.test/%d", i);
        store(url, PAGE_CACHE_BYTES / 2 - 1000, 'x');
        CHECK(page_cache_bytes() <= PAGE_CACHE_BYTES);
    }
    CHECK(page_cache_count() == 2);
    CHECK(page_cache_find("https://big.test/3") != NULL &&
          page_cache_find("https://big.test/2") != NULL &&
          page_cache_find("https://big.test/1") == NULL);
    /* One page larger than the allowance is still shown. */
    found = store("https://big.test/huge", MAX_DOCUMENT_BYTES, 'h');
    CHECK(found && found->size == MAX_DOCUMENT_BYTES);
    CHECK(page_cache_find("https://big.test/huge") == found);

    page_cache_clear();
    CHECK(page_cache_count() == 0 && page_cache_bytes() == 0);
}
