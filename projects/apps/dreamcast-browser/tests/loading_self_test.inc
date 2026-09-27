/* Included by selftest.c only for BROWSER_LOADING_SELF_TEST builds.
   Run tests/serve_fixtures.py on the host first. No account or VMU is used.
   Supply the host LAN address, not guest loopback, as BROWSER_FIXTURE_BASE. */
#ifndef BROWSER_FIXTURE_BASE
#error "Loading self-test needs BROWSER_FIXTURE_BASE set to the fixture host URL"
#endif
#define BROWSER_LOADING_TEST_BASE BROWSER_FIXTURE_BASE

enum {
    LOADING_TEST_IDLE,
    LOADING_TEST_DRAFT,
    LOADING_TEST_CANCEL,
    LOADING_TEST_REPLACE,
    LOADING_TEST_BACK,
    LOADING_TEST_FORWARD,
    LOADING_TEST_READER
};

static int loading_test_mode;
static int loading_test_running;
static int loading_test_injected;
static int loading_test_scrolled;
static unsigned loading_test_ticks;
static uint64_t loading_test_armed_at;
static const char loading_test_draft[] =
    "typed while loading 0123456789 ABC abc!? / draft survives";

static void loading_test_type(const char *text) {
    kbd_mods_t none = { .raw = 0 };
    handle_key(KBD_KEY_F6, none, 0);
    for(; *text; ++text) handle_key(KBD_KEY_NONE, none, *text);
}

static void loading_test_arm(int mode) {
    loading_test_mode = mode;
    loading_test_injected = 0;
    loading_test_scrolled = 0;
    loading_test_ticks = 0;
    loading_test_armed_at = timer_ms_gettime64();
}

/* The frame loop calls this once per frame while something is loading. These
   actions use the normal input handlers while CURL remains on its worker. */
void loading_test_tick(void) {
    kbd_mods_t none = { .raw = 0 };
    int mode;
    if(!loading_test_running) return;
    loading_test_ticks++;
    if(loading_test_mode == LOADING_TEST_IDLE ||
       timer_ms_gettime64() - loading_test_armed_at < 100) return;
    mode = loading_test_mode;
    loading_test_mode = LOADING_TEST_IDLE;
    loading_test_injected++;
    switch(mode) {
    case LOADING_TEST_DRAFT: {
        int previous_scroll = scroll_y;
        handle_key(KBD_KEY_PGDOWN, none, 0);
        loading_test_scrolled = scroll_y > previous_scroll;
        loading_test_type(loading_test_draft);
        break;
    }
    case LOADING_TEST_CANCEL:
        handle_key(KBD_KEY_ESCAPE, none, 0);
        break;
    case LOADING_TEST_REPLACE:
        /* Two submitted intents in one frame must retain only the latest. */
        loading_test_type(BROWSER_LOADING_TEST_BASE "/target?discarded=1");
        handle_key(KBD_KEY_ENTER, none, 0);
        loading_test_type(BROWSER_LOADING_TEST_BASE "/target");
        handle_key(KBD_KEY_ENTER, none, 0);
        break;
    case LOADING_TEST_BACK:
        navigate_back();
        break;
    case LOADING_TEST_FORWARD:
        navigate_forward();
        break;
    case LOADING_TEST_READER:
        handle_key(KBD_KEY_F7, none, 0);
        break;
    default:
        break;
    }
}

static void loading_test_clear_intents(void) {
    loading_test_mode = LOADING_TEST_IDLE;
    memset(&pending, 0, sizeof(pending));
    wait_for_load();
    finish_field_edit(0);
    editing = 0;
    address_selected = 0;
    show_help = 0;
    exit_armed = escape_released = 0;
    close_osk();
}

static int loading_test_form(const char *method, const char *action,
                             const char *type) {
    char html[1024];
    loading_test_clear_intents();
    page_source = NULL;
    page_cache_leave();
    document_free(&document);
    document_init(&document, "https://source.example/page");
    snprintf(current_url, sizeof(current_url), "https://source.example/page");
    snprintf(address, sizeof(address), "%s", current_url);
    snprintf(html, sizeof(html),
             "<form method='%s' action='%s'><input type='%s' name='q' "
             "value='dreamcast homebrew'><input type='submit' value='Search'></form>",
             method, action, type);
    document_parse_html(&document, html, strlen(html), "text/html");
    for(int i = 0; i < document.field_count; ++i)
        if(document_field_is_submit(&document.fields[i])) return i;
    return -1;
}

/* Exercise the real shortcut and submission policy without sending any
   request. A permitted form becomes a deferred intent, which we inspect
   and discard; rejected forms never create an intent. */
static void loading_test_ctrl_enter(int field) {
    kbd_mods_t ctrl = { .raw = KBD_MOD_LCTRL };
    begin_loading("form policy test");
    begin_field_edit(field, 0);
    handle_key(KBD_KEY_ENTER, ctrl, 0);
    end_loading();
}

/* The fixture server counts what reaches it: connections opened and
   requests answered, both as they stood before this reading. */
static int loading_test_stats(int *connections, int *requests) {
    if(load_page(BROWSER_LOADING_TEST_BASE "/stats") != LOAD_OK) return -1;
    return sscanf(document.title, "connections=%d requests=%d",
                  connections, requests) == 2 ? 0 : -1;
}

static const document_item_t *loading_test_item(const char *text) {
    int i;
    for(i = 0; i < document.item_count; ++i)
        if(strstr(document.items[i].text, text)) return &document.items[i];
    return NULL;
}

/* Pages, images, a canceled load and a redirected form, several times
   over. Returns the heap in use once everything has been released and
   closed connections have outlived the SDK's 30-second TIME_WAIT. */
static size_t loading_test_round(int repeats) {
    uint64_t until;
    int i;
    for(i = 0; i < repeats; ++i) {
        if(load_page(BROWSER_LOADING_TEST_BASE "/") != LOAD_OK) return 0;
        if(load_page(BROWSER_LOADING_TEST_BASE "/gallery") != LOAD_OK) return 0;
        loading_test_arm(LOADING_TEST_IDLE);
        load_page_images();
        wait_for_load();
        loading_test_arm(LOADING_TEST_CANCEL);
        navigate_to(BROWSER_LOADING_TEST_BASE "/slow?unkept=1");
        wait_for_load();
        if(load_request(BROWSER_LOADING_TEST_BASE "/post-redirect/302", "q=round") !=
           LOAD_OK)
            return 0;
    }
    loading_test_clear_intents();
    back_count = forward_count = 0;
    load_internal(BOOKMARKS_URL, "");
    page_cache_clear();
    free(load.images.decoded.pixels);
    memset(&load.images.decoded, 0, sizeof(load.images.decoded));
    snprintf(status_text, sizeof(status_text), "Self-test: waiting for idle memory");
    redraw_needed = 1;
    until = timer_ms_gettime64() + 35000;
    while(timer_ms_gettime64() < until) frame_step(0);
    memory_report("idle after a round of loads");
    return (size_t)mallinfo().uordblks;
}

static void run_loading_self_test(void) {
    unsigned checks = 0;
    unsigned generation;
    const fetch_result_t *source;
    int saved_storage = storage_enabled, saved_reader = prefer_reader;
    int saved_toolbar = toolbar_hidden;
    int section_y, expected_y, saved_scroll, button, result;
    int passed = 0;
    kbd_mods_t none = { .raw = 0 };
    char body[512];
    size_t used = 0;
    const char *root = BROWSER_LOADING_TEST_BASE "/";
    const char *slow = BROWSER_LOADING_TEST_BASE "/slow";
    const char *slow_unkept = BROWSER_LOADING_TEST_BASE "/slow?unkept=1";
    const char *target = BROWSER_LOADING_TEST_BASE "/target";
    const char *image_page = BROWSER_LOADING_TEST_BASE "/image-page";
    const char *gallery = BROWSER_LOADING_TEST_BASE "/gallery";
    int connections, requests, connections_after, requests_after;
    size_t baseline = 0;

#define LOADING_CHECK(condition, label) do { \
    checks++; \
    if(!(condition)) { \
        printf("browser: LOADING SELF-TEST FAILED (%s; check %u)\n", label, checks); \
        goto cleanup; \
    } \
} while(0)

    storage_enabled = 0;
    printf("browser: LOADING SELF-TEST BEGIN (local fixture)\n");
    prefer_reader = 1;
    toolbar_hidden = 0;
    loading_test_running = 1;
    loading_test_clear_intents();
    back_count = forward_count = 0;
#ifdef BROWSER_LOADING_STRESS
    /* Repeats the heaviest traffic the fixture has: a page whose images
       include two that are cut off while the server is still sending. */
    for(int round = 0; round < BROWSER_LOADING_STRESS; ++round) {
        LOADING_CHECK(load_page(BROWSER_LOADING_TEST_BASE "/gallery") == 0,
                      "stress: gallery page");
        loading_test_arm(LOADING_TEST_IDLE);
        load_page_images();
        wait_for_load();
        LOADING_CHECK(document.images[0].loaded == 1 && document.images[6].loaded == 1,
                      "stress: gallery images");
        printf("browser: LOADING STRESS round %d of %d done\n", round + 1,
               BROWSER_LOADING_STRESS);
    }
#endif
    LOADING_CHECK(load_page(root) == 0 &&
                  !strcmp(document.title, "Dreamcast browser fixture"), "local fixture");
    baseline = (size_t)mallinfo().uordblks;
    LOADING_CHECK(document.link_count > 96, "links beyond original capacity");

    /* A draft entered while a response streams must survive its completion. */
    loading_test_arm(LOADING_TEST_DRAFT);
    navigate_to(slow);
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && loading_test_ticks >= 2,
                  "progress pump remains active");
    LOADING_CHECK(loading_test_scrolled, "previous page scrolls while response loads");
    LOADING_CHECK(!strcmp(current_url, slow) &&
                  !strcmp(document.title, "Slow response fixture"), "slow response completes");
    LOADING_CHECK(editing && !strcmp(address, loading_test_draft) &&
                  address_caret == (int)strlen(loading_test_draft), "draft preserved on completion");
    LOADING_CHECK(back_count == 1 && !strcmp(back_history[0].url, root),
                  "successful load records history once");
    cancel_address_edit();

    LOADING_CHECK(load_page(root) == 0, "restore fixture for cancellation");
    back_count = forward_count = 0;
    scroll_y = 240;
    clamp_scroll();
    saved_scroll = scroll_y;
    generation = document.generation;
    source = page_source;
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_to(slow);
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && !strcmp(current_url, root) &&
                  document.generation == generation && page_source == source,
                  "cancel retains document and source");
    LOADING_CHECK(scroll_y == saved_scroll && !back_count && !forward_count &&
                  pending.action == PENDING_NONE, "cancel retains scroll and history");

    /* Pages that are not kept have to be downloaded, and that can be
       canceled like any other load. */
    history_push(back_history, &back_count, slow_unkept, 32, "test back");
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_back();
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && back_count == 1 && !forward_count &&
                  !strcmp(back_history[0].url, slow_unkept) && !strcmp(current_url, root) &&
                  scroll_y == saved_scroll, "canceled back preserves history");
    back_count = 0;
    history_push(forward_history, &forward_count, slow_unkept, 48, "test forward");
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_forward();
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && forward_count == 1 && !back_count &&
                  !strcmp(forward_history[0].url, slow_unkept) && !strcmp(current_url, root) &&
                  scroll_y == saved_scroll, "canceled forward preserves history");
    LOADING_CHECK(page_cache_find(slow_unkept) == NULL && page_cache_find(root) == page_source,
                  "a canceled download is not kept");
    forward_count = 0;

    loading_test_arm(LOADING_TEST_REPLACE);
    navigate_to(slow);
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && pending.action == PENDING_NAVIGATE &&
                  !strcmp(pending.url, target) && !strcmp(current_url, root) &&
                  document.generation == generation && !back_count,
                  "replacement deferred until old worker joins");
    {
        unsigned ticks_before = loading_test_ticks;
        /* Fresh input can land after the old worker joins but before the
           main loop dispatches its queued replacement. No request should
           start until the latest intent has been selected. */
        navigate_to(BROWSER_LOADING_TEST_BASE "/target?newest=1");
        LOADING_CHECK(!loading_label && pending.action == PENDING_NAVIGATE &&
                      !strcmp(pending.url, BROWSER_LOADING_TEST_BASE "/target?newest=1") &&
                      !strcmp(current_url, root) && !back_count &&
                      loading_test_ticks == ticks_before,
                      "new input supersedes queued intent after worker completion");
        navigate_to(target);
        LOADING_CHECK(!strcmp(pending.url, target) && !strcmp(current_url, root) &&
                      loading_test_ticks == ticks_before,
                      "latest completion-gap intent wins without extra network");
    }
    dispatch_pending();
    wait_for_load();
    LOADING_CHECK(pending.action == PENDING_NONE && !strcmp(current_url, target) &&
                  !strcmp(document.title, "Target reached") && back_count == 1 &&
                  !strcmp(back_history[0].url, root) && !forward_count,
                  "latest replacement commits exactly once");

    /* Back/forward commands issued inside the wait pump also defer safely. */
    loading_test_arm(LOADING_TEST_BACK);
    navigate_to(slow);
    wait_for_load();
    LOADING_CHECK(pending.action == PENDING_BACK && !strcmp(current_url, target),
                  "back deferred during load");
    dispatch_pending();
    wait_for_load();
    LOADING_CHECK(!strcmp(current_url, root) && !back_count && forward_count == 1,
                  "deferred back restores prior page");
    loading_test_arm(LOADING_TEST_FORWARD);
    navigate_to(slow);
    wait_for_load();
    LOADING_CHECK(pending.action == PENDING_FORWARD && !strcmp(current_url, root),
                  "forward deferred during load");
    dispatch_pending();
    wait_for_load();
    LOADING_CHECK(!strcmp(current_url, target) && back_count == 1 && !forward_count,
                  "deferred forward restores next page");

    LOADING_CHECK(load_page(root) == 0, "restore fixture for anchors");
    back_count = forward_count = 0;
    section_y = document_anchor_y(&document, "#section");
    LOADING_CHECK(section_y >= 0, "fixture section anchor exists");
    generation = document.generation;
    source = page_source;
    expected_y = section_y < max_scroll() ? section_y : max_scroll();
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_to(BROWSER_LOADING_TEST_BASE "/#section");
    wait_for_load();
    LOADING_CHECK(!loading_test_ticks && !loading_test_injected &&
                  page_source == source && document.generation == generation &&
                  scroll_y == expected_y && back_count == 1,
                  "local anchor scrolls without fetching");
    loading_test_arm(LOADING_TEST_IDLE);
    navigate_back();
    wait_for_load();
    LOADING_CHECK(!loading_test_ticks && page_source == source &&
                  document.generation == generation && !strcmp(current_url, root) &&
                  scroll_y == 0 && !back_count && forward_count == 1,
                  "back from section to base page stays local");
    loading_test_arm(LOADING_TEST_IDLE);
    navigate_forward();
    wait_for_load();
    LOADING_CHECK(!loading_test_ticks && page_source == source &&
                  document.generation == generation &&
                  !strcmp(current_url, BROWSER_LOADING_TEST_BASE "/#section") &&
                  scroll_y == expected_y && back_count == 1 && !forward_count,
                  "forward to section stays local and restores scroll");
    loading_test_arm(LOADING_TEST_IDLE);
    handle_key(KBD_KEY_F5, none, 0);
    wait_for_load();
    LOADING_CHECK(loading_test_ticks > 0 && document.generation != generation &&
                  !strcmp(current_url, BROWSER_LOADING_TEST_BASE "/#section") &&
                  scroll_y == expected_y && back_count == 1 && !forward_count,
                  "F5 on anchored URL fetches fresh page without adding history");
    generation = document.generation;
    navigate_to(BROWSER_LOADING_TEST_BASE "/#missing-section");
    wait_for_load();
    LOADING_CHECK(back_count == 1 && document.generation == generation,
                  "missing anchor preserves page and history");

    /* An HTTP POST is rejected before any network request. It must still
       reach that policy check instead of being mistaken for a local jump. */
    result = load_request(BROWSER_LOADING_TEST_BASE "/#section", "q=fixture");
    LOADING_CHECK(result == LOAD_FAILED && !strcmp(document.title, "Page load failed"),
                  "fragment POST is not treated as local navigation");

    LOADING_CHECK(load_page(image_page) == 0 && document.image_count == 2,
                  "slow image fixture");
    back_count = forward_count = 0;
    source = page_source;
    loading_test_arm(LOADING_TEST_DRAFT);
    load_page_images();
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && loading_test_ticks >= 2 &&
                  editing && !strcmp(address, loading_test_draft),
                  "draft survives streaming image completion");
    LOADING_CHECK(document.images[0].loaded == 1 && document.images[1].loaded == 1 &&
                  document.images[0].width == 16 && document.images[0].height == 16 &&
                  page_source == source && !strcmp(current_url, image_page),
                  "images adopted without replacing page source");
    cancel_address_edit();

    LOADING_CHECK(load_page(image_page) == 0, "reset images for cancellation");
    source = page_source;
    loading_test_arm(LOADING_TEST_CANCEL);
    load_page_images();
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && loading_cancelled &&
                  document.images[0].loaded == -1 && document.images[1].loaded == -1 &&
                  page_source == source && !strcmp(current_url, image_page) &&
                  !back_count && !forward_count, "image cancellation retains page and history");

    LOADING_CHECK(load_page(image_page) == 0 && document.reader_active,
                  "reset images for reader switch");
    source = page_source;
    loading_test_arm(LOADING_TEST_READER);
    load_page_images();
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && pending.action == PENDING_READER &&
                  document.reader_active && !strcmp(current_url, image_page),
                  "reader switch waits for image worker to join");
    dispatch_pending();
    wait_for_load();
    LOADING_CHECK(!document.reader_active && !images_requested &&
                  document.images[0].loaded == 0 && page_source == source &&
                  pending.action == PENDING_NONE && !back_count,
                  "reader switch safely reparses retained source");

    prefer_reader = 1;
    LOADING_CHECK(load_page(image_page) == 0, "reset images for replacement navigation");
    loading_test_arm(LOADING_TEST_REPLACE);
    load_page_images();
    wait_for_load();
    LOADING_CHECK(loading_test_injected == 1 && pending.action == PENDING_NAVIGATE &&
                  !strcmp(current_url, image_page), "navigation defers during image transfer");
    dispatch_pending();
    wait_for_load();
    LOADING_CHECK(!strcmp(current_url, target) && back_count == 1 &&
                  !strcmp(back_history[0].url, image_page) && document.image_count == 0,
                  "image transfer cannot mutate replacement document");

    /* One connection serves a run of requests, and Back and Forward ask
       the network for nothing at all. */
    LOADING_CHECK(load_page(root) == 0, "restore fixture for connection reuse");
    back_count = forward_count = 0;
    LOADING_CHECK(loading_test_stats(&connections, &requests) == 0, "fixture counters");
    LOADING_CHECK(load_page(root) == 0 && load_page(target) == 0 && load_page(slow) == 0,
                  "three pages in a row");
    LOADING_CHECK(loading_test_stats(&connections_after, &requests_after) == 0 &&
                  requests_after == requests + 4 && connections_after == connections,
                  "one connection is reused for consecutive pages");
    LOADING_CHECK(load_page(root) == 0, "restore fixture for kept pages");
    back_count = forward_count = 0;
    scroll_y = 120;
    clamp_scroll();
    saved_scroll = scroll_y;
    navigate_to(target);
    wait_for_load();
    navigate_to(slow);
    wait_for_load();
    LOADING_CHECK(!strcmp(current_url, slow) && back_count == 2, "two pages deep");
    LOADING_CHECK(loading_test_stats(&connections, &requests) == 0, "counters before Back");
    /* Reading the counters showed their page without touching the history. */
    snprintf(current_url, sizeof(current_url), "%s", slow);
    loading_test_arm(LOADING_TEST_IDLE);
    navigate_back();
    LOADING_CHECK(load.phase == PHASE_IDLE && !strcmp(current_url, target) &&
                  !strcmp(document.title, "Target reached") && page_source &&
                  back_count == 1 && forward_count == 1, "Back shows the kept page at once");
    navigate_back();
    LOADING_CHECK(load.phase == PHASE_IDLE && !strcmp(current_url, root) &&
                  !strcmp(document.title, "Dreamcast browser fixture") &&
                  scroll_y == saved_scroll && !back_count && forward_count == 2,
                  "Back again restores the first page and its scroll position");
    navigate_forward();
    LOADING_CHECK(load.phase == PHASE_IDLE && !strcmp(current_url, target) &&
                  back_count == 1 && forward_count == 1 && !loading_test_ticks,
                  "Forward shows the kept page at once");
    handle_key(KBD_KEY_F7, none, 0);
    LOADING_CHECK(page_source && !strcmp(current_url, target),
                  "a kept page still switches between reader and full view");
    LOADING_CHECK(loading_test_stats(&connections_after, &requests_after) == 0 &&
                  requests_after == requests + 1,
                  "Back and Forward made no requests");
    LOADING_CHECK(page_cache_count() >= 3 && page_cache_bytes() <= PAGE_CACHE_BYTES,
                  "kept pages stay within their allowance");

    /* A form's redirect is followed as browsers follow it. */
    LOADING_CHECK(load_request(BROWSER_LOADING_TEST_BASE "/post", "q=fixture") == 0 &&
                  !strcmp(document.title, "POST received"), "form posts to the fixture");
    {
        static const char *const to_get[] = { "301", "302", "303" };
        static const char *const to_post[] = { "307", "308" };
        char url[MAX_URL], landed[MAX_URL];
        size_t k;
        for(k = 0; k < 3; ++k) {
            snprintf(url, sizeof(url), BROWSER_LOADING_TEST_BASE "/post-redirect/%s",
                     to_get[k]);
            snprintf(landed, sizeof(landed), BROWSER_LOADING_TEST_BASE "/landing?from=%s",
                     to_get[k]);
            LOADING_CHECK(load_request(url, "q=fixture") == 0 &&
                          !strcmp(document.title, "Landed after POST") &&
                          !strcmp(current_url, landed) &&
                          loading_test_item("Fetched with GET"),
                          "form redirect 301/302/303 is fetched with GET");
        }
        for(k = 0; k < 2; ++k) {
            snprintf(url, sizeof(url), BROWSER_LOADING_TEST_BASE "/post-redirect/%s",
                     to_post[k]);
            LOADING_CHECK(load_request(url, "q=fixture") == 0 &&
                          !strcmp(document.title, "POST received") &&
                          !strcmp(current_url, BROWSER_LOADING_TEST_BASE "/post") &&
                          loading_test_item("Received 9 bytes"),
                          "form redirect 307/308 posts the data again");
        }
    }
    LOADING_CHECK(loading_test_stats(&connections, &requests) == 0, "counters before refusal");
    LOADING_CHECK(load_request(BROWSER_LOADING_TEST_BASE "/post-redirect/away",
                               "q=fixture") == LOAD_FAILED &&
                  !strcmp(document.title, "Page load failed") &&
                  loading_test_item("redirected"),
                  "form data is never resent to another site");
    LOADING_CHECK(loading_test_stats(&connections_after, &requests_after) == 0 &&
                  requests_after == requests + 2, "the refused redirect was not followed");
    LOADING_CHECK(load_request(BROWSER_LOADING_TEST_BASE "/post-redirect/loop",
                               "q=fixture") == LOAD_FAILED &&
                  loading_test_item("too many times"), "endless form redirects stop");
    LOADING_CHECK(load_request(BROWSER_LOADING_TEST_BASE "/target", "q=fixture") ==
                      LOAD_FAILED && loading_test_item("Forms require HTTPS"),
                  "plain HTTP forms are refused outside the fixture's /post");

    /* Images: sizes the server does not declare, files over the limit,
       lazy and responsive sources, and scaling to the page. */
    prefer_reader = 1;
    LOADING_CHECK(load_page(gallery) == 0 && document.image_count == 7,
                  "gallery fixture");
    LOADING_CHECK(strstr(document.images[4].url, "/pixel.png?lazy=1") &&
                  strstr(document.images[5].url, "/wide.png?chosen=1") &&
                  strstr(document.images[6].url, "/pixel.png?picture=1"),
                  "lazy, responsive and picture sources chosen");
    LOADING_CHECK(loading_test_item("Console") && loading_test_item("|") &&
                  loading_test_item("|")->style == TEXT_SEPARATOR,
                  "data table cells are separated");
    back_count = forward_count = 0;
    loading_test_arm(LOADING_TEST_IDLE);
    load_page_images();
    wait_for_load();
    LOADING_CHECK(loading_test_ticks >= 2 && !loading_cancelled &&
                  document.images[0].loaded == 1 && document.images[0].width == 480 &&
                  document.images[0].height == 240, "wide image scaled to the page");
    LOADING_CHECK(document.images[1].loaded == 1 && document.images[1].width == 16,
                  "image without a declared length loads");
    LOADING_CHECK(document.images[2].loaded == -1 && document.images[3].loaded == -1 &&
                  !document.images[2].pixels && !document.images[3].pixels,
                  "oversized images are refused, declared or not");
    LOADING_CHECK(document.images[4].loaded == 1 && document.images[5].loaded == 1 &&
                  document.images[5].width == 480 && document.images[6].loaded == 1,
                  "images after a refused one still load");
    {
        const document_item_t *heading = loading_test_item("Console");
        int i, bottom = 0;
        for(i = 0; i < document.item_count; ++i)
            if(document.items[i].type == ITEM_IMAGE &&
               document.items[i].y + document.items[i].height > bottom)
                bottom = document.items[i].y + document.items[i].height;
        LOADING_CHECK(heading && heading->y >= bottom && document.height > heading->y,
                      "text is laid out below the loaded images");
    }
    LOADING_CHECK(!strcmp(current_url, gallery) && !back_count && page_source,
                  "loading images leaves page and history alone");
#ifdef BROWSER_FRAME_DUMP
    /* For looking at: scripts/frames-to-png.py turns these into pictures. */
    scroll_y = 0;
    snprintf(status_text, sizeof(status_text), "Gallery fixture, images loaded");
    compose();
    render_dump_frame("gallery-top");
    scroll_y = max_scroll();
    compose();
    render_dump_frame("gallery-end");
    scroll_y = 0;
#endif

    button = loading_test_form("get", "https://search.example/find", "search");
    LOADING_CHECK(button >= 0 && build_form_body(button, body, sizeof(body), &used) == 0 &&
                  !strcmp(body, "q=dreamcast%20homebrew"), "cross-origin HTTPS GET allowed");
    button = loading_test_form("post", "https://search.example/find", "text");
    LOADING_CHECK(button >= 0 && build_form_body(button, body, sizeof(body), &used) < 0,
                  "cross-origin POST refused");
    button = loading_test_form("post", "https://source.example/submit", "text");
    LOADING_CHECK(button >= 0 && build_form_body(button, body, sizeof(body), &used) == 0,
                  "same-origin HTTPS POST allowed");
    button = loading_test_form("get", "http://search.example/find", "text");
    LOADING_CHECK(button >= 0 && build_form_body(button, body, sizeof(body), &used) < 0,
                  "HTTP form refused");
    button = loading_test_form("get", "https://source.example/submit", "password");
    LOADING_CHECK(button >= 0 && build_form_body(button, body, sizeof(body), &used) < 0 &&
                  !body[0], "password GET refused and temporary body cleared");
    button = loading_test_form("post", "https://source.example/submit", "file");
    LOADING_CHECK(button >= 0 && build_form_body(button, body, sizeof(body), &used) < 0,
                  "unsupported form refused");

    button = loading_test_form("get", "https://search.example/find", "search");
    LOADING_CHECK(button >= 0 && document.field_count == 2, "Ctrl+Enter form fixture");
    snprintf(document.fields[button].name, sizeof(document.fields[button].name), "go");
    loading_test_ctrl_enter(0);
    LOADING_CHECK(editing_field < 0 && pending.action == PENDING_NAVIGATE && !pending.post &&
                  !strcmp(pending.url,
                    "https://search.example/find?q=dreamcast%20homebrew&go=Search"),
                  "Ctrl+Enter selects submit button and encodes cross-origin GET");
    button = loading_test_form("post", "https://source.example/submit", "text");
    LOADING_CHECK(button >= 0, "Ctrl+Enter POST fixture");
    loading_test_ctrl_enter(0);
    LOADING_CHECK(pending.action == PENDING_NAVIGATE && pending.post &&
                  !strcmp(pending.body, "q=dreamcast%20homebrew"),
                  "Ctrl+Enter copies permitted POST into deferred intent");
    button = loading_test_form("post", "https://search.example/find", "text");
    LOADING_CHECK(button >= 0, "Ctrl+Enter cross-origin POST fixture");
    loading_test_ctrl_enter(0);
    LOADING_CHECK(pending.action == PENDING_NONE && !loading_cancelled,
                  "Ctrl+Enter cannot bypass cross-origin POST policy");
    button = loading_test_form("get", "http://search.example/find", "text");
    LOADING_CHECK(button >= 0, "Ctrl+Enter HTTP fixture");
    loading_test_ctrl_enter(0);
    LOADING_CHECK(pending.action == PENDING_NONE && !loading_cancelled,
                  "Ctrl+Enter cannot bypass HTTPS policy");
    button = loading_test_form("get", "https://source.example/submit", "password");
    LOADING_CHECK(button >= 0, "Ctrl+Enter password fixture");
    loading_test_ctrl_enter(0);
    LOADING_CHECK(pending.action == PENDING_NONE && !loading_cancelled,
                  "Ctrl+Enter cannot bypass password GET policy");
    button = loading_test_form("get", "https://search.example/find", "text");
    LOADING_CHECK(button >= 0, "long GET fixture");
    memset(document.fields[0].value, 'x', sizeof(document.fields[0].value) - 1);
    document.fields[0].value[sizeof(document.fields[0].value) - 1] = 0;
    loading_test_ctrl_enter(0);
    LOADING_CHECK(pending.action == PENDING_NONE && !loading_cancelled &&
                  !strcmp(status_text, "Form query too long"),
                  "long GET rejected without partial navigation");
    /* Nothing may be left behind. The same work is done twice; whatever
       the first round left allocated (connections, DNS answers, cookies)
       the second must not add to. */
    {
        size_t first = loading_test_round(8);
        size_t second = loading_test_round(8);
        printf("browser: heap in use %lu KiB at the first page, %lu KiB after one "
               "round of loads, %lu KiB after two\n", (unsigned long)baseline / 1024,
               (unsigned long)first / 1024, (unsigned long)second / 1024);
        LOADING_CHECK(first && second, "memory rounds completed");
        LOADING_CHECK(second < first + 48 * 1024, "repeated loads do not leak");
        LOADING_CHECK(second < baseline + 512 * 1024,
                      "memory returns close to its starting level");
    }
    passed = 1;

cleanup:
    memset(body, 0, sizeof(body));
    loading_test_running = 0;
    loading_test_clear_intents();
    back_count = forward_count = 0;
    prefer_reader = saved_reader;
    if(load_page(root) < 0) {
        printf("browser: LOADING SELF-TEST FAILED (clean fixture reset)\n");
        passed = 0;
    }
    storage_enabled = saved_storage;
    toolbar_hidden = saved_toolbar;
    redraw_needed = 1;
    if(passed)
        printf("browser: LOADING SELF-TEST PASSED (%u checks: responsive input, "
               "draft, cancel, replacement, history, anchors, images, reader, "
               "connection reuse, kept pages, form redirects, form policy, "
               "Ctrl+Enter)\n", checks);
#undef LOADING_CHECK
}
