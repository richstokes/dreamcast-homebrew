/* Included by selftest.c only for BROWSER_SITE_SELF_TEST builds.
   Visits real sites and reports how long each step took, in guest
   milliseconds. The sites change, so this is a survey of real behavior and
   not a regression test: only outright failures fail it. */

static unsigned site_test_failures;

static uint64_t site_test_visit(const char *label, const char *url) {
    uint64_t start = timer_ms_gettime64();
    uint64_t taken;
    navigate_to(url);
    wait_for_load();
    taken = timer_ms_gettime64() - start;
    if(load.result != LOAD_OK) {
        site_test_failures++;
        printf("browser: SITE %s FAILED to load %s (%s)\n", label, url, status_text);
        return taken;
    }
    printf("browser: SITE %s: %lu ms, %lu KiB of HTML, %d items, %d links, %d images, "
           "reader %d, %lu KiB of page text, title \"%.40s\"\n",
           label, (unsigned long)taken,
           (unsigned long)(page_source ? page_source->size / 1024 : 0),
           document.item_count, document.link_count, document.image_count,
           document.reader_active, (unsigned long)document.arena_bytes / 1024,
           document.title);
    return taken;
}

static void site_test_images(const char *label) {
    uint64_t start = timer_ms_gettime64();
    int i, shown = 0;
    load_page_images();
    wait_for_load();
    for(i = 0; i < document.image_count; ++i)
        if(document.images[i].loaded > 0) shown++;
    printf("browser: SITE %s images: %d of %d shown in %lu ms\n", label, shown,
           document.image_count, (unsigned long)(timer_ms_gettime64() - start));
}

static void run_site_self_test(void) {
    uint64_t first, second, start, back;
    size_t before;

    printf("browser: SITE SELF-TEST BEGIN (live sites)\n");
    back_count = forward_count = 0;
    prefer_reader = 1;

    /* The same small page twice: the second request reuses the connection,
       so the difference is what a TLS handshake costs. */
    first = site_test_visit("Example, new connection", "https://example.com/");
    start = timer_ms_gettime64();
    handle_key(KBD_KEY_F5, (kbd_mods_t){ .raw = 0 }, 0);
    wait_for_load();
    second = timer_ms_gettime64() - start;
    if(load.result != LOAD_OK) site_test_failures++;
    printf("browser: SITE Example, same connection: %lu ms (a new connection took "
           "%lu ms)\n", (unsigned long)second, (unsigned long)first);

    site_test_visit("Wikipedia", "https://en.wikipedia.org/wiki/Dreamcast");
    site_test_images("Wikipedia");
    site_test_visit("Wikipedia, second article", "https://en.wikipedia.org/wiki/Sega");

    /* Back returns to the kept page without the network. */
    start = timer_ms_gettime64();
    navigate_back();
    back = timer_ms_gettime64() - start;
    if(load.phase != PHASE_IDLE || strcmp(current_url, "https://en.wikipedia.org/wiki/Dreamcast")) {
        site_test_failures++;
        printf("browser: SITE Back FAILED to show the kept page\n");
        wait_for_load();
    } else {
        printf("browser: SITE Back to Wikipedia: %lu ms, no request, %d items\n",
               (unsigned long)back, document.item_count);
    }
    handle_key(KBD_KEY_F7, (kbd_mods_t){ .raw = 0 }, 0);
    printf("browser: SITE full-page view: %d items, %d links, shortened %d\n",
           document.item_count, document.link_count, document.truncated);
    handle_key(KBD_KEY_F7, (kbd_mods_t){ .raw = 0 }, 0);

    site_test_visit("Hacker News", "https://news.ycombinator.com/");
    site_test_images("Hacker News");
    site_test_visit("BBC News", "https://www.bbc.com/news");
    site_test_images("BBC News");
    site_test_visit("DuckDuckGo search",
                    BROWSER_SEARCH_URL "dreamcast%20homebrew");

    /* A form answered with 302, as most sign-in pages answer. */
    start = timer_ms_gettime64();
    navigate_request("https://httpbin.org/redirect-to?url=%2Fhtml&status_code=302",
                     "user=dreamcast");
    wait_for_load();
    if(load.result != LOAD_OK || strcmp(current_url, "https://httpbin.org/html")) {
        site_test_failures++;
        printf("browser: SITE form redirect FAILED at %s (%s)\n", current_url, status_text);
    } else {
        printf("browser: SITE form redirect followed to %s in %lu ms\n", current_url,
               (unsigned long)(timer_ms_gettime64() - start));
    }

    before = available_memory();
    memory_report("after the site survey");
    printf("browser: SITE history %d back, %d kept pages in %lu KiB, %lu KiB available\n",
           back_count, page_cache_count(), (unsigned long)page_cache_bytes() / 1024,
           (unsigned long)before / 1024);
    if(site_test_failures)
        printf("browser: SITE SELF-TEST FAILED (%u steps)\n", site_test_failures);
    else
        printf("browser: SITE SELF-TEST PASSED (pages, images, connection reuse, "
               "kept pages, form redirect)\n");
    redraw_needed = 1;
}
