/* Included by dreamcast-browser.c only for BROWSER_LOADING_SELF_TEST builds.
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

/* The real main-thread progress/input pump calls this once per frame. These
   actions use the normal input handlers while CURL remains on its worker. */
static void loading_test_tick(void) {
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
    fetch_result_free(&page_source);
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

static void run_loading_self_test(void) {
    unsigned checks = 0;
    unsigned generation;
    unsigned char *source;
    int saved_storage = storage_enabled, saved_reader = prefer_reader;
    int saved_toolbar = toolbar_hidden;
    int section_y, expected_y, saved_scroll, button, result;
    int passed = 0;
    kbd_mods_t none = { .raw = 0 };
    char body[512];
    size_t used = 0;
    const char *root = BROWSER_LOADING_TEST_BASE "/";
    const char *slow = BROWSER_LOADING_TEST_BASE "/slow";
    const char *target = BROWSER_LOADING_TEST_BASE "/target";
    const char *image_page = BROWSER_LOADING_TEST_BASE "/image-page";

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
    LOADING_CHECK(load_page(root) == 0 &&
                  !strcmp(document.title, "Dreamcast browser fixture"), "local fixture");
    LOADING_CHECK(document.link_count > 96, "links beyond original capacity");

    /* A draft entered while a response streams must survive its completion. */
    loading_test_arm(LOADING_TEST_DRAFT);
    navigate_to(slow);
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
    source = page_source.data;
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_to(slow);
    LOADING_CHECK(loading_test_injected == 1 && !strcmp(current_url, root) &&
                  document.generation == generation && page_source.data == source,
                  "cancel retains document and source");
    LOADING_CHECK(scroll_y == saved_scroll && !back_count && !forward_count &&
                  pending.action == PENDING_NONE, "cancel retains scroll and history");

    history_push(back_history, &back_count, slow, 32, "test back");
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_back();
    LOADING_CHECK(loading_test_injected == 1 && back_count == 1 && !forward_count &&
                  !strcmp(back_history[0].url, slow) && !strcmp(current_url, root) &&
                  scroll_y == saved_scroll, "canceled back preserves history");
    back_count = 0;
    history_push(forward_history, &forward_count, slow, 48, "test forward");
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_forward();
    LOADING_CHECK(loading_test_injected == 1 && forward_count == 1 && !back_count &&
                  !strcmp(forward_history[0].url, slow) && !strcmp(current_url, root) &&
                  scroll_y == saved_scroll, "canceled forward preserves history");
    forward_count = 0;

    loading_test_arm(LOADING_TEST_REPLACE);
    navigate_to(slow);
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
    LOADING_CHECK(pending.action == PENDING_NONE && !strcmp(current_url, target) &&
                  !strcmp(document.title, "Target reached") && back_count == 1 &&
                  !strcmp(back_history[0].url, root) && !forward_count,
                  "latest replacement commits exactly once");

    /* Back/forward commands issued inside the wait pump also defer safely. */
    loading_test_arm(LOADING_TEST_BACK);
    navigate_to(slow);
    LOADING_CHECK(pending.action == PENDING_BACK && !strcmp(current_url, target),
                  "back deferred during load");
    dispatch_pending();
    LOADING_CHECK(!strcmp(current_url, root) && !back_count && forward_count == 1,
                  "deferred back restores prior page");
    loading_test_arm(LOADING_TEST_FORWARD);
    navigate_to(slow);
    LOADING_CHECK(pending.action == PENDING_FORWARD && !strcmp(current_url, root),
                  "forward deferred during load");
    dispatch_pending();
    LOADING_CHECK(!strcmp(current_url, target) && back_count == 1 && !forward_count,
                  "deferred forward restores next page");

    LOADING_CHECK(load_page(root) == 0, "restore fixture for anchors");
    back_count = forward_count = 0;
    section_y = document_anchor_y(&document, "#section");
    LOADING_CHECK(section_y >= 0, "fixture section anchor exists");
    generation = document.generation;
    source = page_source.data;
    expected_y = section_y < max_scroll() ? section_y : max_scroll();
    loading_test_arm(LOADING_TEST_CANCEL);
    navigate_to(BROWSER_LOADING_TEST_BASE "/#section");
    LOADING_CHECK(!loading_test_ticks && !loading_test_injected &&
                  page_source.data == source && document.generation == generation &&
                  scroll_y == expected_y && back_count == 1,
                  "local anchor scrolls without fetching");
    loading_test_arm(LOADING_TEST_IDLE);
    navigate_back();
    LOADING_CHECK(!loading_test_ticks && page_source.data == source &&
                  document.generation == generation && !strcmp(current_url, root) &&
                  scroll_y == 0 && !back_count && forward_count == 1,
                  "back from section to base page stays local");
    loading_test_arm(LOADING_TEST_IDLE);
    navigate_forward();
    LOADING_CHECK(!loading_test_ticks && page_source.data == source &&
                  document.generation == generation &&
                  !strcmp(current_url, BROWSER_LOADING_TEST_BASE "/#section") &&
                  scroll_y == expected_y && back_count == 1 && !forward_count,
                  "forward to section stays local and restores scroll");
    loading_test_arm(LOADING_TEST_IDLE);
    handle_key(KBD_KEY_F5, none, 0);
    LOADING_CHECK(loading_test_ticks > 0 && document.generation != generation &&
                  !strcmp(current_url, BROWSER_LOADING_TEST_BASE "/#section") &&
                  scroll_y == expected_y && back_count == 1 && !forward_count,
                  "F5 on anchored URL fetches fresh page without adding history");
    generation = document.generation;
    navigate_to(BROWSER_LOADING_TEST_BASE "/#missing-section");
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
    source = page_source.data;
    loading_test_arm(LOADING_TEST_DRAFT);
    load_page_images();
    LOADING_CHECK(loading_test_injected == 1 && loading_test_ticks >= 2 &&
                  editing && !strcmp(address, loading_test_draft),
                  "draft survives streaming image completion");
    LOADING_CHECK(document.images[0].loaded == 1 && document.images[1].loaded == 1 &&
                  document.images[0].width == 16 && document.images[0].height == 16 &&
                  page_source.data == source && !strcmp(current_url, image_page),
                  "images adopted without replacing page source");
    cancel_address_edit();

    LOADING_CHECK(load_page(image_page) == 0, "reset images for cancellation");
    source = page_source.data;
    loading_test_arm(LOADING_TEST_CANCEL);
    load_page_images();
    LOADING_CHECK(loading_test_injected == 1 && loading_cancelled &&
                  document.images[0].loaded == -1 && document.images[1].loaded == -1 &&
                  page_source.data == source && !strcmp(current_url, image_page) &&
                  !back_count && !forward_count, "image cancellation retains page and history");

    LOADING_CHECK(load_page(image_page) == 0 && document.reader_active,
                  "reset images for reader switch");
    source = page_source.data;
    loading_test_arm(LOADING_TEST_READER);
    load_page_images();
    LOADING_CHECK(loading_test_injected == 1 && pending.action == PENDING_READER &&
                  document.reader_active && !strcmp(current_url, image_page),
                  "reader switch waits for image worker to join");
    dispatch_pending();
    LOADING_CHECK(!document.reader_active && !images_requested &&
                  document.images[0].loaded == 0 && page_source.data == source &&
                  pending.action == PENDING_NONE && !back_count,
                  "reader switch safely reparses retained source");

    prefer_reader = 1;
    LOADING_CHECK(load_page(image_page) == 0, "reset images for replacement navigation");
    loading_test_arm(LOADING_TEST_REPLACE);
    load_page_images();
    LOADING_CHECK(loading_test_injected == 1 && pending.action == PENDING_NAVIGATE &&
                  !strcmp(current_url, image_page), "navigation defers during image transfer");
    dispatch_pending();
    LOADING_CHECK(!strcmp(current_url, target) && back_count == 1 &&
                  !strcmp(back_history[0].url, image_page) && document.image_count == 0,
                  "image transfer cannot mutate replacement document");

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
               "form policy, Ctrl+Enter)\n", checks);
#undef LOADING_CHECK
}
