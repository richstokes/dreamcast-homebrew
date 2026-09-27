/* Self-tests, built only when a BROWSER_*_SELF_TEST flag asks for them. They
   drive the same functions the keyboard, mouse and controller do and report
   on the serial console. A release build compiles none of this. */

#include "app.h"

#ifdef BROWSER_SELF_TEST

#include <arch/timer.h>
#include <kos/thread.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef BROWSER_HISTORY_SELF_TEST
int self_test_cancel_mode;
#endif

static void wait_for_load(void);
static int load_request(const char *requested, const char *post_body);
static int load_page(const char *requested);

/* If frames stop while a test is waiting on them, say what every thread
   is doing: a test that hangs silently explains nothing. */
volatile unsigned frame_heartbeat;
static volatile int watchdog_armed;

static void *watchdog(void *unused) {
    unsigned seen = frame_heartbeat;
    int quiet = 0;
    (void)unused;
    for(;;) {
        thd_sleep(1000);
        if(!watchdog_armed || frame_heartbeat != seen) {
            seen = frame_heartbeat;
            quiet = 0;
        } else if(++quiet == 20) {
            printf("browser: SELF-TEST FAILED (no frame for 20 seconds, phase %d)\n",
                   (int)load.phase);
            network_report_stall();
        }
    }
    return NULL;
}

#ifdef BROWSER_HISTORY_SELF_TEST
static document_item_t *find_layout_item(browser_document_t *doc,
                                         const char *text,
                                         text_style_t style) {
    int i;
    for(i = 0; i < doc->item_count; ++i) {
        document_item_t *item = &doc->items[i];
        if(item->type == ITEM_TEXT && item->style == style &&
           strstr(item->text, text))
            return item;
    }
    return NULL;
}

static int run_layout_self_test(void) {
    static const char inline_html[] =
        "<h1>Layout test</h1>"
        "<p>Hello <strong>bold</strong> and <em>soft</em> with "
        "<code>code</code> and <a href='/next'>link</a>.</p>"
        "<p><a href='/one'>LinkOne</a><a href='/two'>LinkTwo</a></p>"
        "<p><a href='/tail'>Linked</a>tail <a href='/punct'>Punct</a>.</p>"
        "<!-- <a href='/hidden'>COMMENT_SHOULD_NOT_RENDER</a> -->"
        "<p>Before image</p><img src='/tiny.png' alt='tiny' width='100' height='72'>"
        "<img src='/decoration.png' alt=''>"
        "<p id='after-image'>After image</p><ul><li><div>First item</div></li><li>Second item</li></ul>"
        "<table><tr><td>Cell A</td><td>Cell B</td></tr>"
        "<tr><td>Cell C</td></tr></table>"
        "<table><tr><th>Head A</th><th>Head B</th></tr></table>"
        "<pre>A  B\nC</pre>";
    static const char wrapping_html[] =
        "<p>This intentionally long paragraph contains enough ordinary words "
        "to wrap across multiple display rows without crossing the page edge "
        "or splitting every inline fragment onto its own row.</p>";
    browser_document_t *test;
    document_item_t *normal;
    document_item_t *strong;
    document_item_t *emphasis;
    document_item_t *code;
    document_item_t *link;
    document_item_t *link_one;
    document_item_t *link_two;
    document_item_t *link_tail;
    document_item_t *punctuation;
    document_item_t *bullet;
    document_item_t *first_item;
    document_item_t *pre_a;
    document_item_t *pre_c;
    document_item_t *cell_a;
    document_item_t *cell_b;
    document_item_t *cell_c;
    document_item_t *image_item = NULL;
    document_item_t *after_image;
    int after_y;
    int i;

#define LAYOUT_CHECK(condition, label) do { \
    if(!(condition)) { \
        printf("browser: LAYOUT SELF-TEST FAILED (%s)\n", label); \
        document_free(test); \
        free(test); \
        return -1; \
    } \
} while(0)

    test = malloc(sizeof(*test));
    if(!test) {
        printf("browser: LAYOUT SELF-TEST FAILED (allocation)\n");
        return -1;
    }
    document_init(test, "https://example.com/");
    document_parse_html(test, inline_html, sizeof(inline_html) - 1, NULL);
    normal = find_layout_item(test, "Hello", TEXT_NORMAL);
    strong = find_layout_item(test, "bold", TEXT_STRONG);
    emphasis = find_layout_item(test, "soft", TEXT_EMPHASIS);
    code = find_layout_item(test, "code", TEXT_CODE);
    link = find_layout_item(test, "link", TEXT_LINK);
    LAYOUT_CHECK(normal && strong && emphasis && code && link,
                 "inline styles missing");
    for(i = 0; i < test->item_count; ++i)
        LAYOUT_CHECK(!strstr(test->items[i].text, "COMMENT_SHOULD_NOT_RENDER"),
                     "HTML comment leaked into layout");
    LAYOUT_CHECK(strong->text[0] == ' ' && emphasis->text[0] == ' ' &&
                 code->text[0] == ' ' && link->text[0] == ' ',
                 "inline whitespace lost");
    LAYOUT_CHECK(normal->y == strong->y && strong->y == emphasis->y &&
                 emphasis->y == code->y && code->y == link->y,
                 "inline fragments changed rows");
    LAYOUT_CHECK(normal->x < strong->x && strong->x < emphasis->x &&
                 emphasis->x < code->x && code->x < link->x &&
                 link->link_id >= 0,
                 "inline run order/link");
    link_one = find_layout_item(test, "LinkOne", TEXT_LINK);
    link_two = find_layout_item(test, "LinkTwo", TEXT_LINK);
    LAYOUT_CHECK(link_one && link_two && link_one->y == link_two->y &&
                 link_two->text[0] == ' ' &&
                 link_two->x == link_one->x + link_one->width,
                 "adjacent link separation");
    link_tail = find_layout_item(test, "tail", TEXT_NORMAL);
    punctuation = find_layout_item(test, ".", TEXT_NORMAL);
    LAYOUT_CHECK(link_tail && link_tail->text[0] == ' ' && punctuation &&
                 punctuation->text[0] == '.',
                 "link/prose punctuation spacing");

    /* A bullet and the text after it are one run of the same style. */
    bullet = find_layout_item(test, "* First item", TEXT_NORMAL);
    first_item = find_layout_item(test, "* Second item", TEXT_NORMAL);
    LAYOUT_CHECK(bullet && first_item && bullet->x == PAGE_MARGIN + 12 &&
                 first_item->x == bullet->x && first_item->y > bullet->y,
                 "list bullet flow");
    pre_a = find_layout_item(test, "A  B", TEXT_CODE);
    pre_c = find_layout_item(test, "C", TEXT_CODE);
    LAYOUT_CHECK(pre_a && pre_c && pre_c->y > pre_a->y,
                 "preformatted spacing");
    /* Cells of a table that only positions the page flow as one run. */
    cell_a = find_layout_item(test, "Cell A", TEXT_NORMAL);
    cell_b = find_layout_item(test, "Cell A Cell B", TEXT_NORMAL);
    cell_c = find_layout_item(test, "Cell C", TEXT_NORMAL);
    LAYOUT_CHECK(cell_a && cell_b == cell_a && cell_c && cell_c->y > cell_a->y,
                 "table row/cell fallback");

    /* Headings make it a data table, whose cells are set apart by a bar. */
    cell_a = find_layout_item(test, "Head A", TEXT_NORMAL);
    cell_b = find_layout_item(test, "Head B", TEXT_NORMAL);
    cell_c = find_layout_item(test, "|", TEXT_SEPARATOR);
    LAYOUT_CHECK(cell_a && cell_b && cell_c && cell_a->y == cell_b->y &&
                 cell_c->y == cell_a->y && cell_a->x < cell_c->x &&
                 cell_c->x < cell_b->x, "data table separators");

    after_image = find_layout_item(test, "After image", TEXT_NORMAL);
    for(i = 0; i < test->item_count; ++i) {
        if(test->items[i].type == ITEM_IMAGE) {
            image_item = &test->items[i];
            break;
        }
    }
    LAYOUT_CHECK(image_item && after_image && test->image_count == 1 &&
                 image_item->width == 100 && image_item->height == 72,
                 "declared image dimensions");
    after_y = after_image->y;
    test->images[image_item->image_id].loaded = 1;
    test->images[image_item->image_id].width = 100;
    test->images[image_item->image_id].height = 120;
    document_reflow(test);
    LAYOUT_CHECK(after_image->y == after_y + 48 && image_item->height == 120,
                 "image reflow shift");
    LAYOUT_CHECK(document_anchor_y(test, "#after-image") == after_image->y,
                 "section target follows image reflow");
    after_y = after_image->y;
    document_reflow(test);
    LAYOUT_CHECK(after_image->y == after_y, "image reflow idempotence");
    document_mark_shortened(test, "[Network document shortened]");
    LAYOUT_CHECK(test->truncated &&
                 test->items[test->item_count - 1].type == ITEM_NOTICE &&
                 strstr(test->items[test->item_count - 1].text,
                        "Network document shortened"),
                 "network truncation notice");

    document_free(test);
    document_init(test, "https://example.com/");
    document_parse_html(test, wrapping_html, sizeof(wrapping_html) - 1, NULL);
    LAYOUT_CHECK(test->item_count >= 3 &&
                 test->items[0].y < test->items[test->item_count - 1].y,
                 "word wrapping rows");
    for(i = 0; i < test->item_count; ++i) {
        LAYOUT_CHECK(test->items[i].x + test->items[i].width <=
                     PAGE_MARGIN + PAGE_WIDTH,
                     "item crossed page edge");
    }
    document_free(test);
    document_init(test, "https://example.com/");
    {
        static const char reader_html[] =
            "<title>Reader test</title><nav>MENU_SHOULD_NOT_RENDER</nav>"
            "<main id=content><header><h1>Article heading</h1></header>"
            "<div><div><p>First paragraph</p></div></div>"
            "<div hidden>HIDDEN_SHOULD_NOT_RENDER</div><p>Second paragraph</p>"
            "<form action=/search><input name=q><button>Search</button></form>"
            "</main><footer>FOOTER_SHOULD_NOT_RENDER</footer>";
        document_item_t *first, *second;
        document_parse_html_mode(test, reader_html, sizeof(reader_html) - 1, NULL, 1);
        LAYOUT_CHECK(test->reader_available && test->reader_active &&
                     !strcmp(test->title, "Reader test") && test->field_count == 2 &&
                     test->forms[0].valid, "reader title and form retained");
        LAYOUT_CHECK(document_anchor_y(test, "content") == test->items[0].y,
                     "reader main section target");
        for(i = 0; i < test->item_count; ++i)
            LAYOUT_CHECK(!strstr(test->items[i].text, "SHOULD_NOT_RENDER"),
                         "reader or hidden element leaked into layout");
        first = find_layout_item(test, "First paragraph", TEXT_NORMAL);
        second = find_layout_item(test, "Second paragraph", TEXT_NORMAL);
        LAYOUT_CHECK(first && second && second->y - first->y <= 32,
                     "nested block spacing compact");
    }
    document_free(test);
    free(test);
    printf("browser: LAYOUT SELF-TEST PASSED (inline/reader/anchors/reflow)\n");
#undef LAYOUT_CHECK
    return 0;
}

static void run_history_self_test(void) {
    fetch_result_t asset;
    char original[MAX_URL];
    char original_title[MAX_TITLE];
    int original_scroll;
    int updated_scroll;

    if(run_layout_self_test() < 0) return;

    if(network_fetch_wait("https://httpbin.org/image/png", NULL, FETCH_IMAGE,
                          MAX_IMAGE_BYTES, &asset) < 0 || asset.status != 200 || !asset.size ||
       asset.size > MAX_IMAGE_BYTES ||
       !strstr(asset.content_type, "image/png")) {
        printf("browser: ASSET PREFLIGHT SELF-TEST FAILED\n");
        fetch_result_free(&asset);
        return;
    }
    printf("browser: ASSET PREFLIGHT SELF-TEST PASSED (%lu bytes)\n",
           (unsigned long)asset.size);
    fetch_result_free(&asset);

    snprintf(original, sizeof(original), "%s", current_url);
    snprintf(original_title, sizeof(original_title), "%s", document.title);
    snprintf(address, sizeof(address), "https://example.com/");
    self_test_cancel_mode = 1;
    navigate_to(address);
    wait_for_load();
    if(strcmp(current_url, original) || strcmp(address, original) ||
       strcmp(document.title, original_title) ||
       back_count || forward_count || self_test_cancel_mode) {
        printf("browser: CANCEL SELF-TEST FAILED (page/history changed)\n");
        return;
    }
    printf("browser: CANCEL SELF-TEST PASSED (page/history preserved)\n");

    scroll_y = 64;
    clamp_scroll();
    original_scroll = scroll_y;
    navigate_to("https://example.com/");
    wait_for_load();
    if(strcmp(current_url, "https://example.com/")) {
        printf("browser: HISTORY SELF-TEST FAILED (forward navigation)\n");
        return;
    }
    navigate_back();
    wait_for_load();
    if(strcmp(current_url, original) || scroll_y != original_scroll ||
       back_count || forward_count != 1) {
        printf("browser: HISTORY SELF-TEST FAILED (back/scroll restore)\n");
        return;
    }
    scroll_y = original_scroll / 2;
    clamp_scroll();
    updated_scroll = scroll_y;
    navigate_forward();
    wait_for_load();
    if(strcmp(current_url, "https://example.com/") || scroll_y ||
       back_count != 1 || forward_count) {
        printf("browser: HISTORY SELF-TEST FAILED (forward/scroll restore)\n");
        return;
    }
    navigate_back();
    wait_for_load();
    if(strcmp(current_url, original) || scroll_y != updated_scroll ||
       back_count || forward_count != 1) {
        printf("browser: HISTORY SELF-TEST FAILED (history round trip)\n");
        return;
    }
    printf("browser: HISTORY SELF-TEST PASSED (back/forward, scroll %d)\n",
           scroll_y);

    self_test_cancel_mode = 2;
    if(load_page("https://httpbin.org/base64/"
                 "PGh0bWw%2BPHRpdGxlPkNhbmNlbCBJbWFnZSBUZXN0PC90aXRsZT48Ym9keT48"
                 "aW1nIHNyYz0iL2ltYWdlL3BuZyIgYWx0PSJ0ZXN0Ij48L2JvZHk%2BPC9odG1s"
                 "Pg%3D%3D") != 0) {
        printf("browser: CANCEL SELF-TEST FAILED (image page)\n");
        return;
    }
    load_page_images();
    wait_for_load();
    if(self_test_cancel_mode || !loading_cancelled ||
       strcmp(status_text, "Images canceled; page ready") ||
       document.image_count != 1 || document.images[0].loaded != -1) {
        printf("browser: CANCEL SELF-TEST FAILED (image cancellation)\n");
        return;
    }
    printf("browser: IMAGE CANCEL SELF-TEST PASSED (page kept)\n");
}
#endif

#if defined(BROWSER_HISTORY_SELF_TEST) || defined(BROWSER_PERF_SELF_TEST)
/* A repeated read of one Maple report must not repeat its relative motion
   or turn a held button into a fresh click. */
static void run_mouse_self_test(void) {
    mouse_state_t sample = { .dx = 7, .dy = -3, .dz = 1 };
    maple_driver_t driver = { 0 };
    maple_device_t mouse = { .valid = 1, .drv = &driver, .status = &sample };
    int saved_x = mouse_x, saved_y = mouse_y, saved_scroll = scroll_y;
    int saved_height = document.height, saved_focus = focused_link;
    int saved_help = show_help, saved_exit = exit_armed;
    int saved_released = escape_released;
    int ok;

    mouse_x = 100;
    mouse_y = 200;
    scroll_y = 200;
    document.height = SCREEN_H + 512;
    show_help = 0;
    process_mouse(&mouse);
    ok = mouse_x == 107 && mouse_y == 197 && scroll_y == 152;
    process_mouse(&mouse);
    ok &= mouse_x == 107 && mouse_y == 197 && scroll_y == 152;
    sample.dx = -2;
    sample.dy = 4;
    sample.dz = -1;
    process_mouse(&mouse);
    ok &= mouse_x == 105 && mouse_y == 201 && scroll_y == 200;

    show_help = 1;
    sample.buttons = MOUSE_LEFTBUTTON;
    process_mouse(&mouse);
    ok &= !show_help;
    show_help = 1;
    process_mouse(&mouse);
    ok &= show_help && sample.buttons == MOUSE_LEFTBUTTON;
    sample.buttons = 0;
    process_mouse(&mouse);

    mouse_x = saved_x;
    mouse_y = saved_y;
    scroll_y = saved_scroll;
    document.height = saved_height;
    focused_link = saved_focus;
    show_help = saved_help;
    exit_armed = saved_exit;
    escape_released = saved_released;
    redraw_needed = 1;
    printf("browser: MOUSE SELF-TEST %s (relative motion, skipped polls, held buttons)\n",
           ok ? "PASSED" : "FAILED");
}
#endif

#if defined(BROWSER_HISTORY_SELF_TEST) || defined(BROWSER_PERF_SELF_TEST)
/* The stick as a pointer: dead zone, fine and fast movement, scrolling at
   the page edges, and A pressing what the pointer is on. */
static void run_controller_self_test(void) {
    static const char page_html[] =
        "<form action='https://example.com/f' method='post'>"
        "<input type=checkbox name=c value=1> Tick</form>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>"
        "<p>line</p><p>line</p><p>line</p><p>line</p><p>line</p><p>line</p>";
    cont_state_t sample = { 0 };
    maple_driver_t driver = { 0 };
    maple_device_t pad = { .valid = 1, .drv = &driver, .status = &sample };
    browser_document_t *saved = malloc(sizeof(*saved));
    const document_item_t *box;
    int saved_x = mouse_x, saved_y = mouse_y, saved_toolbar = toolbar_hidden;
    int i, ok, before;
    const char *failed = NULL;

#define PAD_CHECK(condition, label) do { \
    if(!failed && !(condition)) failed = label; \
} while(0)

    if(!saved) {
        printf("browser: CONTROLLER SELF-TEST FAILED (allocation)\n");
        return;
    }
    memcpy(saved, &document, sizeof(document));
    document_init(&document, "https://example.com/");
    document_parse_html(&document, page_html, sizeof(page_html) - 1, NULL);
    toolbar_hidden = 0;
    scroll_y = 0;
    focused_link = -1;
    editing = 0;
    editing_field = -1;
    show_help = 0;
    process_controller(&pad); /* settle the button edge detection */
    PAD_CHECK(document.field_count == 1 && document.fields[0].item >= 0 &&
              max_scroll() > 400, "test page shape");
    box = &document.items[document.fields[0].item];

    mouse_x = 300;
    mouse_y = 300;
    sample.joyx = STICK_DEAD_ZONE - 1;
    sample.joyy = -(STICK_DEAD_ZONE - 1);
    for(i = 0; i < 30; ++i) process_controller(&pad);
    PAD_CHECK(mouse_x == 300 && mouse_y == 300, "dead zone holds the pointer still");

    sample.joyx = STICK_DEAD_ZONE;
    sample.joyy = 0;
    for(i = 0; i < 64; ++i) process_controller(&pad);
    PAD_CHECK(mouse_x == 300 + 64 * 96 / 256 && mouse_y == 300,
              "a nudge moves a fraction of a pixel per frame");

    mouse_x = 100;
    sample.joyx = 127;
    for(i = 0; i < 30; ++i) process_controller(&pad);
    PAD_CHECK(mouse_x > 100 + 200 && mouse_x < 100 + 300, "a full push is fast");
    for(i = 0; i < 120; ++i) process_controller(&pad);
    PAD_CHECK(mouse_x == SCREEN_W - 1, "the pointer stops at the screen edge");

    sample.joyx = 0;
    sample.joyy = 127;
    for(i = 0; i < 40; ++i) process_controller(&pad);
    PAD_CHECK(mouse_y == SCREEN_H - 1 - STICK_EDGE && scroll_y > 100,
              "pushing past the bottom scrolls the page");
    before = scroll_y;
    sample.joyy = -128;
    for(i = 0; i < 300 && (scroll_y || mouse_y > page_top() + STICK_EDGE); ++i)
        process_controller(&pad);
    PAD_CHECK(scroll_y == 0 && before > 0 && mouse_y == page_top() + STICK_EDGE,
              "pushing past the top scrolls back");
    for(i = 0; i < 60; ++i) process_controller(&pad);
    PAD_CHECK(mouse_y == 0 && scroll_y == 0, "the pointer can reach the toolbar");

    /* Steer onto the checkbox and press A. */
    mouse_x = box->x + 40;
    mouse_y = page_top() + box->y + 60;
    sample.joyx = -60;
    sample.joyy = -60;
    for(i = 0; i < 200 && focused_link != document.fields[0].link; ++i)
        process_controller(&pad);
    PAD_CHECK(focused_link == document.fields[0].link && pointer_active,
              "the pointer focuses the control under it");
    sample.joyx = sample.joyy = 0;
    sample.buttons = CONT_A;
    process_controller(&pad);
    PAD_CHECK(document.fields[0].checked, "A presses the control under the pointer");
    process_controller(&pad);
    PAD_CHECK(document.fields[0].checked, "holding A presses once");
    sample.buttons = 0;
    process_controller(&pad);

    /* After Y has chosen a link, A opens it wherever the pointer rests. */
    mouse_x = 500;
    mouse_y = 400;
    sample.buttons = CONT_Y;
    process_controller(&pad);
    sample.buttons = 0;
    process_controller(&pad);
    PAD_CHECK(!pointer_active && focused_link == document.fields[0].link,
              "Y chooses a link without the pointer");
    sample.buttons = CONT_A;
    process_controller(&pad);
    sample.buttons = 0;
    process_controller(&pad);
    PAD_CHECK(!document.fields[0].checked, "A opens the link Y chose");

    /* On the address bar, A opens the on-screen keyboard; pointing at a key
       chooses it and A types it. */
    mouse_x = 300;
    mouse_y = 24;
    pointer_active = 1;
    sample.buttons = CONT_A;
    process_controller(&pad);
    sample.buttons = 0;
    process_controller(&pad);
    PAD_CHECK(editing && osk_open, "A on the address bar opens the keyboard");
    {
        int x, y, w, h;
        osk_key_t key;
        osk_key_rect(2, 3, &x, &y, &w, &h);
        osk_key(2, 3, 0, &key);
        mouse_x = x + w / 2 - 2;
        mouse_y = y + h / 2;
        sample.joyx = STICK_DEAD_ZONE + 20;
        for(i = 0; i < 4; ++i) process_controller(&pad);
        sample.joyx = 0;
        PAD_CHECK(osk_row == 2 && osk_column == 3, "pointing at a key chooses it");
        sample.buttons = CONT_A;
        process_controller(&pad);
        sample.buttons = 0;
        process_controller(&pad);
        PAD_CHECK(key.action == OSK_CHAR && address[0] == key.ch && !address[1],
                  "A types the chosen key");
    }
    ok = !failed;
    if(editing) cancel_address_edit();
    close_osk();
    process_controller(&pad);
    document_free(&document);
    memcpy(&document, saved, sizeof(document));
    free(saved);
    mouse_x = saved_x;
    mouse_y = saved_y;
    toolbar_hidden = saved_toolbar;
    scroll_y = 0;
    focused_link = -1;
    pointer_active = 0;
    exit_armed = escape_released = 0;
    snprintf(address, sizeof(address), "%s", current_url);
    snprintf(status_text, sizeof(status_text), "%s", document.title);
    clamp_scroll();
    redraw_needed = 1;
    if(ok)
        printf("browser: CONTROLLER SELF-TEST PASSED (stick pointer, edge scrolling, "
               "A press, keyboard keys)\n");
    else
        printf("browser: CONTROLLER SELF-TEST FAILED (%s) pointer=%d,%d scroll=%d "
               "focus=%d\n", failed, mouse_x, mouse_y, scroll_y, focused_link);
#undef PAD_CHECK
}
#endif

#if defined(BROWSER_HISTORY_SELF_TEST) || defined(BROWSER_PERF_SELF_TEST)
static int press(kbd_key_t key, uint8_t modifiers, char ascii) {
    kbd_mods_t mods = { .raw = modifiers };
    return handle_key(key, mods, ascii);
}

/* Exercises the keyboard paths that have no other automated coverage:
   focus stepping, the Esc exit guard, help, and line editing. */
static void run_keyboard_self_test(void) {
#define FILLER10 "<p>filler line</p><p>filler line</p><p>filler line</p>" \
                 "<p>filler line</p><p>filler line</p><p>filler line</p>" \
                 "<p>filler line</p><p>filler line</p><p>filler line</p>" \
                 "<p>filler line</p>"
    static const char page_html[] =
        "<h1>Keyboard test</h1><p><a href='/first'>First link</a></p>"
        FILLER10 FILLER10
        "<p><a href='/middle'>Middle link</a></p>"
        FILLER10 FILLER10
        "<p><a href='/last'>Last link</a></p>"
        "<form action='https://example.com/f' method='post'>"
        "<input name='q' value='abc'></form>";
    browser_document_t *saved;
    int field_link = -1;
    int field_index;
    int bottom_focus;
    int i;

#define KEY_CHECK(condition, label) do { \
    if(!(condition)) { \
        printf("browser: KEYBOARD SELF-TEST FAILED (%s) " \
               "focus=%d scroll=%d/%d links=%d\n", label, focused_link, \
               scroll_y, max_scroll(), document.link_count); \
        goto restore; \
    } \
} while(0)

    saved = malloc(sizeof(*saved));
    if(!saved) {
        printf("browser: KEYBOARD SELF-TEST FAILED (allocation)\n");
        return;
    }
    memcpy(saved, &document, sizeof(document));
    document_init(&document, "https://example.com/");
    document_parse_html(&document, page_html, sizeof(page_html) - 1, NULL);
    scroll_y = 0;
    focused_link = -1;
    editing = 0;
    editing_field = -1;
    show_help = 0;
    exit_armed = 0;
    escape_released = 0;
    for(i = 0; i < document.link_count; ++i)
        if(!strncmp(document.links[i], "form:", 5)) field_link = i;
    KEY_CHECK(document.link_count >= 4 && field_link >= 0 &&
              document.height > SCREEN_H - PAGE_TOP, "test page shape");

    KEY_CHECK(!press(KBD_KEY_TAB, 0, 0) && focused_link == 0, "Tab focuses first link");
    KEY_CHECK(!press(KBD_KEY_TAB, KBD_MOD_LSHIFT, 0) &&
              focused_link == document.link_count - 1 &&
              link_on_screen(focused_link), "Shift+Tab wraps and scrolls");
    KEY_CHECK(!press(KBD_KEY_TAB, 0, 0) && focused_link == 0 &&
              link_on_screen(0), "Tab wraps to the first link");

    press(KBD_KEY_END, 0, 0);
    KEY_CHECK(scroll_y == max_scroll() && !link_on_screen(0), "End scrolls to bottom");
    press(KBD_KEY_TAB, 0, 0);
    bottom_focus = focused_link;
    KEY_CHECK(bottom_focus > 0 && link_on_screen(bottom_focus),
              "Tab resumes from the viewport");
    press(KBD_KEY_HOME, 0, 0);
    press(KBD_KEY_TAB, 0, 0);
    KEY_CHECK(focused_link == 0 && link_on_screen(0),
              "Home then Tab returns to the top link");

    KEY_CHECK(!press(KBD_KEY_ESCAPE, 0, 0) && focused_link == -1 && !exit_armed,
              "Esc clears focus first");
    KEY_CHECK(!press(KBD_KEY_ESCAPE, 0, 0) && exit_armed, "Esc arms exit");
    KEY_CHECK(!press(KBD_KEY_ESCAPE, 0, 0), "held Esc does not exit");
    escape_released = 1;
    KEY_CHECK(press(KBD_KEY_ESCAPE, 0, 0), "second Esc exits");
    exit_armed = escape_released = 0;

    press(KBD_KEY_F1, 0, 0);
    KEY_CHECK(show_help, "F1 opens help");
    press(KBD_KEY_TAB, 0, 0);
    KEY_CHECK(!show_help && focused_link == -1, "any key closes help without acting");

    press(KBD_KEY_SPACE, 0, ' ');
    KEY_CHECK(scroll_y == page_step(), "Space scrolls a screen down");
    press(KBD_KEY_SPACE, KBD_MOD_LSHIFT, ' ');
    KEY_CHECK(!scroll_y, "Shift+Space scrolls back up");

    press(KBD_KEY_F6, 0, 0);
    KEY_CHECK(editing && address_selected &&
              address_caret == (int)strlen(address), "F6 selects the address");
    press(KBD_KEY_X, 0, 'x');
    KEY_CHECK(!address_selected && !strcmp(address, "x"), "typing replaces the selection");
    press(KBD_KEY_Y, 0, 'y');
    press(KBD_KEY_Z, 0, 'z');
    KEY_CHECK(!strcmp(address, "xyz") && address_caret == 3, "address insertion");
    press(KBD_KEY_LEFT, 0, 0);
    press(KBD_KEY_LEFT, 0, 0);
    press(KBD_KEY_BACKSPACE, 0, 0);
    KEY_CHECK(!strcmp(address, "yz") && address_caret == 0, "caret motion and backspace");
    press(KBD_KEY_DEL, 0, 0);
    KEY_CHECK(!strcmp(address, "z") && address_caret == 0, "delete at the caret");
    press(KBD_KEY_END, 0, 0);
    press(KBD_KEY_A, 0, 'a');
    press(KBD_KEY_B, 0, 'b');
    KEY_CHECK(!strcmp(address, "zab") && address_caret == 3, "End then insertion");
    press(KBD_KEY_W, KBD_MOD_LCTRL, 'w');
    KEY_CHECK(!address[0] && !address_caret, "Ctrl+W deletes the word");
    press(KBD_KEY_A, KBD_MOD_LCTRL, 'a');
    KEY_CHECK(address_selected, "Ctrl+A selects all");
    press(KBD_KEY_ESCAPE, 0, 0);
    KEY_CHECK(!editing && !strcmp(address, current_url), "Esc restores the address");

    focused_link = field_link;
    press(KBD_KEY_ENTER, 0, 0);
    KEY_CHECK(editing_field >= 0 &&
              document.fields[editing_field].caret == 3, "Enter edits a field");
    field_index = editing_field;
    press(KBD_KEY_D, 0, 'd');
    KEY_CHECK(!strcmp(document.fields[editing_field].value, "abcd"), "field insertion");
    press(KBD_KEY_HOME, 0, 0);
    press(KBD_KEY_X, KBD_MOD_LSHIFT, 'X');
    KEY_CHECK(!strcmp(document.fields[editing_field].value, "Xabcd") &&
              strstr(document.items[document.fields[editing_field].item].text,
                     "X|abcd"), "field caret rendering");
    press(KBD_KEY_ESCAPE, 0, 0);
    KEY_CHECK(editing_field == -1 &&
              !strcmp(document.fields[field_index].value, "abc") &&
              document.fields[field_index].caret == -1, "Esc undoes the field edit");
    printf("browser: KEYBOARD SELF-TEST PASSED (focus, help, editing, exit guard)\n");

restore:
    document_free(&document);
    memcpy(&document, saved, sizeof(document));
    free(saved);
    scroll_y = 0;
    focused_link = -1;
    editing = 0;
    editing_field = -1;
    show_help = exit_armed = escape_released = 0;
    address_selected = 0;
    snprintf(address, sizeof(address), "%s", current_url);
    clamp_scroll();
    redraw_needed = 1;
#undef KEY_CHECK
#undef FILLER10
}
#endif

#ifdef BROWSER_HISTORY_SELF_TEST
static int link_to(const char *target) {
    int i;
    for(i = 0; i < document.link_count; ++i)
        if(!strcmp(document.links[i], target)) return i;
    return -1;
}

static int field_link(const char *name) {
    int i;
    for(i = 0; i < document.field_count; ++i)
        if(!strcmp(document.fields[i].name, name)) return document.fields[i].link;
    return -1;
}

static void osk_press(int row, int column) {
    osk_row = row;
    osk_column = column;
    osk_activate();
}

#define UI_CHECK(condition, label) do { \
    if(!(condition)) { \
        printf("browser: UI SELF-TEST FAILED (%s) status='%s' url=%s\n", \
               label, status_text, current_url); \
        goto restore; \
    } \
} while(0)

/* On-screen keyboard and form controls through the same paths the
   controller and keyboard use. */
static void run_ui_self_test(void) {
    static const char page_html[] =
        "<form action='https://example.com/f' method='post'>"
        "<input name='q' value=''>"
        "<select name='s'><option value='a'>Apple<option value='b'>Banana"
        "<option value='c'>Cherry</select>"
        "<input type=radio name=r value=1 checked> One "
        "<input type=radio name=r value=2> Two"
        "<textarea name=t>x</textarea><button>Send</button></form>";
    browser_document_t *saved = malloc(sizeof(*saved));
    char encoded[32];
    int q, sel, r1, r2, t;

    if(!saved) {
        printf("browser: UI SELF-TEST FAILED (allocation)\n");
        return;
    }
    memcpy(saved, &document, sizeof(document));
    document_init(&document, "https://example.com/");
    document_parse_html(&document, page_html, sizeof(page_html) - 1, NULL);
    q = field_link("q");
    sel = field_link("s");
    r1 = document.fields[2].link;
    r2 = document.fields[3].link;
    t = field_link("t");
    UI_CHECK(q >= 0 && sel >= 0 && r1 >= 0 && r2 >= 0 && t >= 0 &&
             document.field_count == 6, "test form shape");

    begin_address_edit(1);
    UI_CHECK(editing && osk_open && address_selected, "controller opens the keyboard");
    osk_press(1, 0);
    UI_CHECK(!strcmp(address, "q"), "OSK key replaces the selected URL");
    osk_shift = 1;
    osk_press(1, 1);
    UI_CHECK(!strcmp(address, "qW") && !osk_shift, "shift is one-shot");
    osk_press(OSK_ROWS - 1, 2);
    osk_press(OSK_ROWS - 1, 3);
    UI_CHECK(!strcmp(address, "q.com"), "OSK delete and .com");
    osk_press(OSK_ROWS - 1, 4);
    UI_CHECK(!editing && !osk_open && !strcmp(address, current_url), "OSK cancel");

    follow_link(q, 1);
    UI_CHECK(editing_field >= 0 && osk_open, "controller edits a field with the OSK");
    osk_type(KBD_KEY_NONE, 'a');
    osk_press(OSK_ROWS - 1, 5);
    UI_CHECK(editing_field < 0 && !osk_open && !strcmp(document.fields[0].value, "a"),
             "OSK done keeps the field");
    follow_link(q, 1);
    osk_type(KBD_KEY_NONE, 'b');
    osk_press(OSK_ROWS - 1, 4);
    UI_CHECK(!strcmp(document.fields[0].value, "a"), "OSK cancel restores the field");

    follow_link(sel, 0);
    UI_CHECK(editing_field == 1 && !osk_open, "select edits without the OSK");
    press(KBD_KEY_DOWN, 0, 0);
    UI_CHECK(!strcmp(document.fields[1].value, "b"), "Down chooses the next option");
    press(KBD_KEY_C, 0, 'c');
    UI_CHECK(!strcmp(document.fields[1].value, "c"), "typing jumps to an option");
    press(KBD_KEY_ESCAPE, 0, 0);
    UI_CHECK(editing_field < 0 && !strcmp(document.fields[1].value, "a"),
             "Esc restores the choice");
    follow_link(sel, 0);
    press(KBD_KEY_DOWN, 0, 0);
    press(KBD_KEY_ENTER, 0, 0);
    UI_CHECK(!strcmp(document.fields[1].value, "b") &&
             strstr(document.items[document.fields[1].item].text, "Banana v"),
             "Enter keeps the choice");

    follow_link(r2, 0);
    UI_CHECK(document.fields[3].checked && !document.fields[2].checked,
             "radio buttons are exclusive");

    follow_link(t, 0);
    press(KBD_KEY_ENTER, KBD_MOD_LSHIFT, 0);
    press(KBD_KEY_Y, 0, 'y');
    press(KBD_KEY_ENTER, 0, 0);
    UI_CHECK(editing_field < 0 && !strcmp(document.fields[4].value, "x\ny"),
             "Shift+Enter adds a textarea line");

    form_encoding("caf\xe9\nx", encoded, sizeof(encoded));
    UI_CHECK(!strcmp(encoded, "caf\xc3\xa9\r\nx"), "forms submit UTF-8 and CRLF");
    printf("browser: UI SELF-TEST PASSED (OSK, select, radio, textarea, encoding)\n");

restore:
    finish_field_edit(0);
    if(editing) cancel_address_edit();
    close_osk();
    document_free(&document);
    memcpy(&document, saved, sizeof(document));
    free(saved);
    focused_link = -1;
    scroll_y = 0;
    redraw_needed = 1;
}

/* Bookmark actions on the live page, with VMU writes disabled. */
static void run_bookmark_self_test(void) {
    static bookmark_list_t saved_list;
    char original[MAX_URL];
    int link;

    saved_list = bookmarks;
    memset(&bookmarks, 0, sizeof(bookmarks));
    storage_enabled = 0;
    snprintf(original, sizeof(original), "%s", current_url);

    navigate_to(BOOKMARKS_URL);
    UI_CHECK(!strcmp(current_url, BOOKMARKS_URL) &&
             !strcmp(bookmark_candidate_url, original), "bookmarks page opens");
    link = link_to("about:bookmark-add");
    UI_CHECK(link >= 0, "page offers to bookmark the previous page");
    follow_link(link, 0);
    UI_CHECK(bookmarks.count == 1 && !strcmp(bookmarks.items[0].url, original) &&
             link_to(original) >= 0 && link_to("about:bookmark-add") < 0,
             "bookmark added and listed");

    snprintf(current_url, sizeof(current_url), "https://evil.test/");
    handle_internal_link("about:exit");
    handle_internal_link("about:bookmark-remove?0");
    snprintf(current_url, sizeof(current_url), "%s", BOOKMARKS_URL);
    UI_CHECK(!quit_requested && bookmarks.count == 1, "web pages cannot run actions");

    follow_link(link_to("about:exit"), 0);
    UI_CHECK(quit_requested, "exit link works on the internal page");
    quit_requested = 0;
    follow_link(link_to("about:bookmark-remove?0"), 0);
    UI_CHECK(!bookmarks.count, "bookmark removed");

    press(KBD_KEY_D, KBD_MOD_LCTRL, 'd');
    UI_CHECK(!bookmarks.count, "Ctrl+D ignores internal pages");
    press(KBD_KEY_B, KBD_MOD_LCTRL | KBD_MOD_LSHIFT, 'B');
    wait_for_load();
    UI_CHECK(!strcmp(current_url, original), "Ctrl+Shift+B returns to the page");
    press(KBD_KEY_B, KBD_MOD_LCTRL, 'b');
    UI_CHECK(toolbar_hidden && !toolbar_shown() && page_top() == 0, "Ctrl+B hides the toolbar");
    press(KBD_KEY_B, KBD_MOD_LCTRL, 'b');
    UI_CHECK(!toolbar_hidden && page_top() == PAGE_TOP, "Ctrl+B shows the toolbar");
    press(KBD_KEY_D, KBD_MOD_LCTRL, 'd');
    UI_CHECK(bookmarks.count == 1 && !strcmp(bookmarks.items[0].url, original),
             "Ctrl+D bookmarks the page");
    printf("browser: BOOKMARK SELF-TEST PASSED (page, actions, isolation, keys)\n");

restore:
    bookmarks = saved_list;
    storage_enabled = 1;
    quit_requested = 0;
    redraw_needed = 1;
}

static int field_index_where(const char *name, const char *value) {
    int i;
    for(i = 0; i < document.field_count; ++i)
        if(!strcmp(document.fields[i].name, name) &&
           (!value || !strcmp(document.fields[i].value, value)))
            return i;
    return -1;
}

/* Fills a real form and checks what the server received. httpbin echoes the
   decoded fields as JSON, which proves the encoding end to end. */
static void run_live_form_self_test(void) {
    static char body[4096];
    char original[MAX_URL];
    fetch_result_t result;
    size_t used = 0;
    int name, large, cheese, delivery, comments, button;

    snprintf(original, sizeof(original), "%s", current_url);
    if(load_page("https://httpbin.org/forms/post") != 0) {
        printf("browser: LIVE FORM SELF-TEST SKIPPED (form page unavailable)\n");
        return;
    }
    name = field_index_where("custname", NULL);
    large = field_index_where("size", "large");
    cheese = field_index_where("topping", "cheese");
    delivery = field_index_where("delivery", NULL);
    comments = field_index_where("comments", NULL);
    button = document.field_count - 1;
    if(name < 0 || large < 0 || cheese < 0 || delivery < 0 || comments < 0 ||
       !document.forms[0].valid || !document_field_is_submit(&document.fields[button]) ||
       strcmp(document.fields[button].label, "Submit order") ||
       strcmp(document.fields[delivery].type, "time") ||
       strcmp(document.fields[comments].type, "textarea")) {
        printf("browser: LIVE FORM SELF-TEST FAILED (form parsing)\n");
        load_page(original);
        return;
    }
    snprintf(document.fields[name].value, MAX_FIELD_VALUE, "Caf\xe9 Tester");
    snprintf(document.fields[delivery].value, MAX_FIELD_VALUE, "12:30");
    snprintf(document.fields[comments].value, MAX_FIELD_VALUE, "line1\nline2");
    document_toggle_field(&document, large);
    document_toggle_field(&document, cheese);
    if(build_form_body(button, body, sizeof(body), &used) < 0 ||
       network_fetch_wait("https://httpbin.org/post", body, FETCH_PAGE,
                          MAX_DOCUMENT_BYTES, &result) < 0) {
        printf("browser: LIVE FORM SELF-TEST SKIPPED (post failed: %s)\n", status_text);
        load_page(original);
        return;
    }
    if(result.status == 200 && result.data &&
       strstr((char *)result.data, "\"custname\": \"Caf\\u00e9 Tester\"") &&
       strstr((char *)result.data, "\"comments\": \"line1\\r\\nline2\"") &&
       strstr((char *)result.data, "\"size\": \"large\"") &&
       strstr((char *)result.data, "\"topping\": \"cheese\"") &&
       strstr((char *)result.data, "\"delivery\": \"12:30\""))
        printf("browser: LIVE FORM SELF-TEST PASSED (radio, checkbox, time, textarea, "
               "button, UTF-8)\n");
    else
        printf("browser: LIVE FORM SELF-TEST FAILED (HTTP %ld, body %.300s)\n",
               result.status, result.data ? (char *)result.data : "");
    fetch_result_free(&result);
    load_page(original);
}

/* Round-trips a record through the VMU, then puts back what was there. */
static void run_vmu_self_test(void) {
    static bookmark_list_t original, sample, loaded;
    int had = storage_load_bookmarks(&original);

    if(had == STORAGE_ERROR) {
        printf("browser: VMU SELF-TEST SKIPPED (existing bookmark file is invalid)\n");
        return;
    }
    memset(&sample, 0, sizeof(sample));
    bookmarks_add(&sample, "https://example.com/", "Example \xe9");
    bookmarks_add(&sample, "http://old.test/?a=1&b=2", "Old");
    if(storage_save_bookmarks(&sample) == STORAGE_NO_VMU) {
        printf("browser: VMU SELF-TEST SKIPPED (no VMU)\n");
        return;
    }
    memset(&loaded, 0, sizeof(loaded));
    if(storage_load_bookmarks(&loaded) != STORAGE_OK || loaded.count != 2 ||
       strcmp(loaded.items[0].title, "Example \xe9") ||
       strcmp(loaded.items[1].url, "http://old.test/?a=1&b=2"))
        printf("browser: VMU SELF-TEST FAILED (round trip)\n");
    else
        printf("browser: VMU SELF-TEST PASSED (bookmarks saved and reloaded)\n");
    if(had == STORAGE_OK) storage_save_bookmarks(&original);
    else storage_remove();
    memset(&loaded, 0, sizeof(loaded));
    if(had == STORAGE_OK ? storage_load_bookmarks(&loaded) != STORAGE_OK ||
                           loaded.count != original.count
                         : storage_load_bookmarks(&loaded) != STORAGE_NO_VMU)
        printf("browser: VMU SELF-TEST FAILED (could not restore the previous file)\n");
    else
        printf("browser: VMU SELF-TEST restored the previous bookmark file\n");
}

#undef UI_CHECK
#endif

#if defined(BROWSER_HISTORY_SELF_TEST) || defined(BROWSER_PERF_SELF_TEST)
/* Every incremental frame must match a full redraw of the same state, or
   the dirty-row tracking has missed something. */
static int render_check(const char *step, browser_document_t *page,
                        const browser_view_t *view, uint16_t *snapshot) {
    const uint16_t *pixels = render_frame_pixels();
    int y;
    render_frame(page, view);
    memcpy(snapshot, pixels, (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));
    render_invalidate();
    render_frame(page, view);
    for(y = 0; y < SCREEN_H; ++y) {
        if(memcmp(snapshot + y * SCREEN_W, pixels + y * SCREEN_W,
                  SCREEN_W * sizeof(uint16_t))) {
            printf("browser: RENDER SELF-TEST FAILED (%s: row %d differs)\n", step, y);
            return 0;
        }
    }
    return 1;
}

static void run_render_self_test(void) {
    static const int scroll_steps[] = { 14, 14, -14, 48, -48, 200, -3, 1, 400, -400, 0 };
    static const int mouse_steps[][2] = {
        { 0, 0 }, { 639, 479 }, { 300, 65 }, { 300, 75 }, { 636, 300 }, { 12, 316 },
        { 320, 240 }, { 320, 241 }, { 5, 479 }
    };
    browser_document_t *page = malloc(sizeof(*page));
    uint16_t *snapshot = malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));
    static char html[16384];
    char typed[MAX_URL];
    browser_view_t view;
    size_t n = 0;
    size_t i;
    int ok = 1;

    if(!page || !snapshot) {
        free(page);
        free(snapshot);
        printf("browser: RENDER SELF-TEST SKIPPED (no memory)\n");
        return;
    }
    memset(page, 0, sizeof(*page));
    for(i = 0; i < 30 && n < sizeof(html) - 256; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n,
                              "<h2>Section %u</h2><p>Paragraph %u has <a href='/%u'>a link"
                              "</a>, <b>bold</b> and <code>code</code> text that wraps "
                              "across the full width of the page.</p><hr>",
                              (unsigned)i, (unsigned)i, (unsigned)i);
    document_init(page, "https://example.com/");
    document_parse_html(page, html, n, NULL);
    snprintf(typed, sizeof(typed), "https://example.com/");
    current_view(&view);
    view.address = typed;
    view.status = "Render self-test";
    view.scroll_y = 0;
    view.mouse_x = 320;
    view.mouse_y = 240;
    view.focused_link = -1;
    view.editing = 0;
    view.osk_open = 0;
    view.show_help = 0;
    render_invalidate();
    ok &= render_check("baseline", page, &view, snapshot);

    /* Address bar editing. */
    view.editing = 1;
    view.address_selected = 1;
    view.address_caret = (int)strlen(typed);
    ok &= render_check("select all", page, &view, snapshot);
    view.address_selected = 0;
    typed[0] = 0;
    view.address_caret = 0;
    ok &= render_check("clear", page, &view, snapshot);
    for(i = 0; i < 40 && ok; ++i) {
        typed[i] = (char)('a' + (int)(i % 26));
        typed[i + 1] = 0;
        view.address_caret = (int)i + 1;
        ok &= render_check("typing", page, &view, snapshot);
    }
    view.address_caret = 3;
    ok &= render_check("caret move", page, &view, snapshot);
    view.status = "Type a URL or search: Enter opens";
    ok &= render_check("status", page, &view, snapshot);
    view.editing = 0;
    view.can_go_back = !view.can_go_back;
    ok &= render_check("toolbar buttons", page, &view, snapshot);

    /* Scrolling by small and large amounts, with the cursor in the page. */
    for(i = 0; scroll_steps[i] && ok; ++i) {
        view.scroll_y += scroll_steps[i];
        if(view.scroll_y < 0) view.scroll_y = 0;
        ok &= render_check("scroll", page, &view, snapshot);
    }
    view.scroll_y = 100;
    view.focused_link = 2;
    ok &= render_check("focus", page, &view, snapshot);
    view.scroll_y = 114;
    view.mouse_y = 260;
    ok &= render_check("scroll and mouse", page, &view, snapshot);
    for(i = 0; i < sizeof(mouse_steps) / sizeof(mouse_steps[0]) && ok; ++i) {
        view.mouse_x = mouse_steps[i][0];
        view.mouse_y = mouse_steps[i][1];
        ok &= render_check("mouse", page, &view, snapshot);
    }
    view.mouse_y = 400;
    view.scroll_y += 20;
    ok &= render_check("scroll under cursor", page, &view, snapshot);
    view.scroll_y -= 33;
    ok &= render_check("scroll back", page, &view, snapshot);

    /* On-screen keyboard, help, and page changes. */
    view.osk_open = 1;
    ok &= render_check("osk open", page, &view, snapshot);
    view.osk_column = 3;
    ok &= render_check("osk key", page, &view, snapshot);
    view.osk_shift = 1;
    ok &= render_check("osk shift", page, &view, snapshot);
    view.scroll_y += 14;
    ok &= render_check("scroll with osk", page, &view, snapshot);
    view.osk_open = 0;
    ok &= render_check("osk close", page, &view, snapshot);
    view.show_help = 1;
    ok &= render_check("help", page, &view, snapshot);
    view.scroll_y += 14;
    ok &= render_check("scroll with help", page, &view, snapshot);
    view.show_help = 0;
    ok &= render_check("help off", page, &view, snapshot);
    /* Item text is sized to its contents: change it in place. */
    if(page->items[3].text[0]) page->items[3].text[0] = '#';
    page->items[0].text[0] = 'Z';
    document_touch(page);
    ok &= render_check("page change", page, &view, snapshot);
    view.toolbar = 0;
    ok &= render_check("toolbar hidden", page, &view, snapshot);
    view.scroll_y += 14;
    ok &= render_check("scroll without toolbar", page, &view, snapshot);
    view.mouse_y = 30;
    ok &= render_check("mouse without toolbar", page, &view, snapshot);
    view.toolbar = 1;
    ok &= render_check("toolbar shown", page, &view, snapshot);
    view.scroll_y = 0;
    view.mouse_x = 100;
    view.mouse_y = 20;
    typed[2] = 'Z';
    ok &= render_check("everything", page, &view, snapshot);

    if(ok) printf("browser: RENDER SELF-TEST PASSED (incremental frames match full redraws)\n");
    document_free(page);
    free(page);
    free(snapshot);
    render_invalidate();
}

/* Times drawing a fixed, text-heavy page so results compare across builds. */
static void run_render_benchmark(void) {
    static char html[16384];
    browser_document_t *page = malloc(sizeof(*page));
    browser_view_t view;
    uint64_t start;
    size_t n = 0;
    int i;

    if(!page) return;
    for(i = 0; i < 40 && n < sizeof(html) - 256; ++i)
        n += (size_t)snprintf(html + n, sizeof(html) - n,
                              "<p>Paragraph %d has <a href='/%d'>a link</a>, "
                              "<b>bold</b> and <code>code</code> text that wraps "
                              "across the full width of the page.</p>", i, i);
    document_init(page, "https://example.com/");
    document_parse_html(page, html, n, NULL);
    current_view(&view);
    view.scroll_y = 200;
    view.focused_link = 3;
    start = timer_us_gettime64();
    for(i = 0; i < 60; ++i) {
        render_invalidate();
        render_frame(page, &view);
    }
    printf("browser: RENDER BENCHMARK page %lu us/frame",
           (unsigned long)((timer_us_gettime64() - start) / 60));
    view.osk_open = 1;
    start = timer_us_gettime64();
    for(i = 0; i < 60; ++i) {
        render_invalidate();
        render_frame(page, &view);
    }
    printf(", with keyboard %lu us/frame (compose only, %d items)\n",
           (unsigned long)((timer_us_gettime64() - start) / 60), page->item_count);
    /* Incremental frames: only the rows that changed are drawn. */
    view.osk_open = 0;
    view.editing = 1;
    view.address = "https://example.com/";
    render_draw(page, &view);
    start = timer_us_gettime64();
    for(i = 0; i < 60; ++i) {
        view.address_caret = i & 7;
        render_frame(page, &view);
    }
    printf("browser: RENDER BENCHMARK incremental: address caret %lu us/frame",
           (unsigned long)((timer_us_gettime64() - start) / 60));
    start = timer_us_gettime64();
    for(i = 0; i < 60; ++i) {
        view.scroll_y = 200 + (i & 1 ? 14 : 0);
        render_frame(page, &view);
    }
    printf(", scroll 14px %lu us/frame",
           (unsigned long)((timer_us_gettime64() - start) / 60));
    start = timer_us_gettime64();
    for(i = 0; i < 60; ++i) {
        view.mouse_x = 300 + i;
        view.mouse_y = 300 + (i & 1);
        render_frame(page, &view);
    }
    printf(", mouse move %lu us/frame\n",
           (unsigned long)((timer_us_gettime64() - start) / 60));
    view.address = address;
    view.editing = 0;
    view.scroll_y = 200;
    view.mouse_x = mouse_x;
    view.mouse_y = mouse_y;
#ifdef BROWSER_FRAME_DUMP
    osk_row = 2;
    osk_column = 3;
    view.osk_row = osk_row;
    view.osk_column = osk_column;
    view.editing = 1;
    view.address = "https://example.com/caf\xe9?q=1";
    view.address_caret = 12;
    view.address_selected = 0;
    view.status = "A types, B deletes, Start finishes";
    render_draw(page, &view);
    render_dump_frame("keyboard");
#endif
    document_free(page);
    free(page);
}
#undef UI_CHECK
#endif

#ifdef BROWSER_FORM_SELF_TEST
static int test_field(const char *name, const char *value) {
    for(int i=0;i<document.field_count;++i) {
        if(!strcmp(document.fields[i].name,name)) {
            snprintf(document.fields[i].value,sizeof(document.fields[i].value),"%s",value);
            document_refresh_field(&document,i); return i;
        }
    }
    return -1;
}
static int test_submit(void) {
    for(int i=0;i<document.field_count;++i)
        if(!strcmp(document.fields[i].type,"submit")) { submit_form(i);wait_for_load();return 0; }
    return -1;
}
static void run_form_self_test(void) {
    char test_user[64],test_password[129];
    FILE *credentials=fopen("/rd/test-credentials.txt","r");
    if(!credentials||!fgets(test_user,sizeof(test_user),credentials)||
       !fgets(test_password,sizeof(test_password),credentials)) {
        printf("browser: FORM SELF-TEST FAILED credentials missing\n");
        if(credentials) fclose(credentials);
        return;
    }
    fclose(credentials);
    test_user[strcspn(test_user,"\r\n")]=0;test_password[strcspn(test_password,"\r\n")]=0;
    if(load_page("https://dcvmu.com/register")<0||test_field("username",test_user)<0||
       test_field("email","dcvmu-qa@example.com")<0||test_field("password",test_password)<0||test_submit()<0||
       strcmp(current_url,"https://dcvmu.com/account"))goto fail;
    printf("browser: REGISTER FORM PASS (HTTPS, CSRF, cookie, redirect)\n");
    if(test_submit()<0||strcmp(current_url,"https://dcvmu.com/"))goto fail;
    printf("browser: LOGOUT FORM PASS\n");
    if(load_page("https://dcvmu.com/login")<0||test_field("username",test_user)<0||
       test_field("password",test_password)<0||test_submit()<0||strcmp(current_url,"https://dcvmu.com/account"))goto fail;
    printf("browser: LOGIN FORM PASS (authenticated account)\n");
    if(load_page("https://dcvmu.com/")<0||test_field("game","DCVMU Test Game")<0||test_submit()<0||
       !strstr(current_url,"game=DCVMU%20Test%20Game"))goto fail;
    printf("browser: FILTER GET FORM PASS\n");
    load_page("https://dcvmu.com/account");
    memset(test_password,0,sizeof(test_password));
    printf("browser: FORM SELF-TEST PASSED\n");return;
fail:
    memset(test_password,0,sizeof(test_password));
    printf("browser: FORM SELF-TEST FAILED at %s\n",current_url);
}
#endif

/* Self-tests read as a script: they run frames until a load has ended.
   Intents queued meanwhile are left for the test to inspect or dispatch. */
static void wait_for_load(void) {
    uint64_t started = timer_ms_gettime64();
    int reported = 0;
    watchdog_armed = 1;
    while(load.phase != PHASE_IDLE) {
        frame_step(0);
        /* Every transfer has a timeout well inside this. */
        if(!reported && timer_ms_gettime64() - started > 120000 &&
           !network_slow_link()) {
            reported = 1;
            printf("browser: SELF-TEST FAILED (a load has stalled in phase %d)\n",
                   (int)load.phase);
            network_report_stall();
        }
    }
    watchdog_armed = 0;
}

/* Runs until nothing is loading and nothing is waiting to. */
__attribute__((unused)) static void settle(void) {
    do {
        wait_for_load();
        dispatch_pending();
    } while(load.phase != PHASE_IDLE || pending.action != PENDING_NONE);
}

static int load_request(const char *requested, const char *post_body) {
    start_load(NAV_PLAIN, requested, post_body);
    wait_for_load();
    return load.result;
}

__attribute__((unused)) static int load_page(const char *requested) {
    return load_request(requested, NULL);
}

#ifdef BROWSER_LOADING_SELF_TEST
#include "tests/loading_self_test.inc"
#endif
#ifdef BROWSER_SITE_SELF_TEST
#include "tests/site_self_test.inc"
#endif

void run_self_tests(void) {
    kthread_t *dog = thd_create(1, watchdog, NULL);
    if(dog) thd_set_prio(dog, PRIO_DEFAULT - 1);
    wait_for_load();
#ifdef BROWSER_HISTORY_SELF_TEST
    if(load.result == LOAD_OK) run_history_self_test();
    run_keyboard_self_test();
    run_mouse_self_test();
    run_controller_self_test();
    run_ui_self_test();
    run_bookmark_self_test();
    run_live_form_self_test();
    run_vmu_self_test();
#endif
#ifdef BROWSER_PERF_SELF_TEST
    run_keyboard_self_test();
    run_mouse_self_test();
    run_controller_self_test();
#endif
#if defined(BROWSER_HISTORY_SELF_TEST) || defined(BROWSER_PERF_SELF_TEST)
    run_render_benchmark();
    run_render_self_test();
#endif
#ifdef BROWSER_FORM_SELF_TEST
    run_form_self_test();
#endif
#ifdef BROWSER_LOADING_SELF_TEST
    run_loading_self_test();
#endif
#ifdef BROWSER_SITE_SELF_TEST
    run_site_self_test();
#endif
}

#endif /* BROWSER_SELF_TEST */
