#include "browser.h"

#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>
#include <dc/video.h>
#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

#ifndef BROWSER_HOME_URL
#define BROWSER_HOME_URL "https://appsbyrich.com/"
#endif

static browser_document_t document;
static char address[MAX_URL] = BROWSER_HOME_URL;
static char current_url[MAX_URL];
static char status_text[96];
static int scroll_y;
static int mouse_x = SCREEN_W / 2;
static int mouse_y = SCREEN_H / 2;
static int focused_link = -1;
static int editing;
static int redraw_needed = 1;
static const char *loading_label;
static uint64_t last_progress_draw;
static unsigned progress_frame;
static uint32_t loading_controller_buttons;
static uint32_t loading_mouse_buttons;
static int loading_cancelled;

#ifdef BROWSER_HISTORY_SELF_TEST
static int self_test_cancel_mode;
#endif

enum {
    LOAD_FAILED = -1,
    LOAD_CANCELED = -2
};

typedef struct {
    char url[MAX_URL];
    int scroll_y;
} history_entry_t;

static history_entry_t back_history[MAX_HISTORY];
static history_entry_t forward_history[MAX_HISTORY];
static int back_count;
static int forward_count;

static int show_transfer_progress(uint64_t received, uint64_t total,
                                  void *userdata) {
    static const char spinner[] = "|/-\\";
    maple_device_t *device;
    cont_state_t *controller_state;
    mouse_state_t *mouse_state;
    uint32_t pressed;
    uint64_t now = timer_ms_gettime64();
    (void)userdata;

#ifdef BROWSER_HISTORY_SELF_TEST
    if(self_test_cancel_mode == 1 ||
       (self_test_cancel_mode == 2 && loading_label &&
        !strcmp(loading_label, "image"))) {
        self_test_cancel_mode = 0;
        loading_cancelled = 1;
    }
#endif
    device = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if(device) {
        int raw;
        while((raw = kbd_queue_pop(device, 0)) != KBD_QUEUE_END) {
            if((raw & 0xff) == KBD_KEY_ESCAPE) loading_cancelled = 1;
        }
    }
    device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    controller_state = device ? maple_dev_status(device) : NULL;
    if(controller_state) {
        pressed = controller_state->buttons & ~loading_controller_buttons;
        loading_controller_buttons = controller_state->buttons;
        if(pressed & (CONT_B | CONT_START)) loading_cancelled = 1;
    }
    device = maple_enum_type(0, MAPLE_FUNC_MOUSE);
    mouse_state = device ? maple_dev_status(device) : NULL;
    if(mouse_state) {
        pressed = mouse_state->buttons & ~loading_mouse_buttons;
        loading_mouse_buttons = mouse_state->buttons;
        if(pressed & MOUSE_RIGHTBUTTON) loading_cancelled = 1;
    }
    if(loading_cancelled) {
        snprintf(status_text, sizeof(status_text), "Canceling %s...",
                 loading_label ? loading_label : "request");
        render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                       address, editing, back_count > 0, forward_count > 0,
                       status_text);
        return 1;
    }

    if(last_progress_draw && now - last_progress_draw < 200) return 0;
    last_progress_draw = now;
    if(total)
        snprintf(status_text, sizeof(status_text),
                 "Loading %s %lu/%luK %c | Esc/B cancel",
                 loading_label ? loading_label : "data",
                 (unsigned long)(received / 1024),
                 (unsigned long)((total + 1023) / 1024),
                 spinner[progress_frame++ & 3]);
    else
        snprintf(status_text, sizeof(status_text),
                 "Connecting %s %c | Esc/B cancel",
                 loading_label ? loading_label : "",
                 spinner[progress_frame++ & 3]);
    render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                   address, editing, back_count > 0, forward_count > 0,
                   status_text);
    return 0;
}

static void begin_loading(const char *label) {
    maple_device_t *device;
    cont_state_t *controller_state;
    mouse_state_t *mouse_state;

    loading_label = label;
    last_progress_draw = 0;
    loading_cancelled = 0;
    device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    controller_state = device ? maple_dev_status(device) : NULL;
    loading_controller_buttons = controller_state ? controller_state->buttons : 0;
    device = maple_enum_type(0, MAPLE_FUNC_MOUSE);
    mouse_state = device ? maple_dev_status(device) : NULL;
    loading_mouse_buttons = mouse_state ? mouse_state->buttons : 0;
    network_set_progress_callback(show_transfer_progress, NULL);
}

static void end_loading(void) {
    network_set_progress_callback(NULL, NULL);
    loading_label = NULL;
}

static void init_video(void) {
    vid_mode_t double_buffered;

    /* Let KOS select the correct VGA/NTSC/PAL timing, then reuse that mode
       with exactly two framebuffers instead of DM_MULTIBUFFER's 13. */
    vid_set_mode(DM_640x480, PM_RGB565);
    double_buffered = *vid_mode;
    double_buffered.fb_count = 2;
    vid_set_mode_ex(&double_buffered);
}

static int max_scroll(void) {
    int visible = SCREEN_H - PAGE_TOP;
    return document.height > visible ? document.height - visible : 0;
}

static void clamp_scroll(void) {
    int maximum = max_scroll();
    if(scroll_y < 0) scroll_y = 0;
    if(scroll_y > maximum) scroll_y = maximum;
}

static int link_at(int x, int screen_y) {
    int i;
    int page_y = screen_y - PAGE_TOP + scroll_y;
    if(screen_y < PAGE_TOP) return -1;
    for(i = 0; i < document.item_count; ++i) {
        const document_item_t *item = &document.items[i];
        if(item->link_id >= 0 && x >= item->x && x <= item->x + item->width &&
           page_y >= item->y && page_y <= item->y + item->height)
            return item->link_id;
    }
    return -1;
}

static void begin_address_edit(void) {
    editing = 1;
    focused_link = -1;
    snprintf(status_text, sizeof(status_text), "Type URL, Enter to open, Esc to cancel");
    redraw_needed = 1;
}

static void normalize_address(char *url, size_t size) {
    char temp[MAX_URL];
    if(strstr(url, "://")) return;
    snprintf(temp, sizeof(temp), "https://%.*s", (int)sizeof(temp) - 9, url);
    snprintf(url, size, "%s", temp);
}

static void history_push(history_entry_t *history, int *count,
                         const char *url, int saved_scroll,
                         const char *name) {
    history_entry_t *entry;

    if(!url[0]) return;
    if(*count && !strcmp(history[*count - 1].url, url)) {
        history[*count - 1].scroll_y = saved_scroll;
        return;
    }
    if(*count == MAX_HISTORY) {
        memmove(&history[0], &history[1],
                sizeof(history[0]) * (MAX_HISTORY - 1));
        (*count)--;
    }
    entry = &history[(*count)++];
    snprintf(entry->url, sizeof(entry->url), "%s", url);
    entry->scroll_y = saved_scroll;
    printf("browser: %s history saved %s (%d/%d)\n",
           name, entry->url, *count, MAX_HISTORY);
}

static int load_page(const char *requested) {
    fetch_result_t result;
    char target[MAX_URL];
    char previous_url[MAX_URL];
    char previous_address[MAX_URL];
    char message[256];
    int images_cancelled;
    long response_status;

    snprintf(previous_url, sizeof(previous_url), "%s", current_url);
    snprintf(previous_address, sizeof(previous_address), "%s", address);
    snprintf(target, sizeof(target), "%s", requested);
    normalize_address(target, sizeof(target));
    snprintf(current_url, sizeof(current_url), "%s", target);
    snprintf(address, sizeof(address), "%s", target);
    snprintf(status_text, sizeof(status_text), "Connecting page...");
    render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                   address, 0, back_count > 0, forward_count > 0,
                   status_text);
    redraw_needed = 0;
    begin_loading("page");

    if(network_fetch(target, MAX_DOCUMENT_BYTES, &result) < 0) {
        end_loading();
        if(result.cancelled) {
            snprintf(current_url, sizeof(current_url), "%s", previous_url);
            snprintf(address, sizeof(address), "%s",
                     previous_url[0] ? previous_url : previous_address);
            snprintf(status_text, sizeof(status_text), "Canceled; page unchanged");
            printf("browser: page load canceled; keeping %s\n",
                   current_url[0] ? current_url : "startup page");
            fetch_result_free(&result);
            redraw_needed = 1;
            return LOAD_CANCELED;
        }
        snprintf(message, sizeof(message), "Could not load this address: %s", result.error);
        document_free(&document);
        document_make_error(&document, "Page load failed", message);
        snprintf(status_text, sizeof(status_text), "Network error");
        redraw_needed = 1;
        return LOAD_FAILED;
    }

    if(result.status < 200 || result.status >= 400) {
        end_loading();
        response_status = result.status;
        snprintf(message, sizeof(message), "The server returned HTTP status %ld.", result.status);
        fetch_result_free(&result);
        document_free(&document);
        document_make_error(&document, "Server error", message);
        snprintf(status_text, sizeof(status_text), "HTTP %ld", response_status);
        redraw_needed = 1;
        return LOAD_FAILED;
    }

    if(result.content_type[0] && !strstr(result.content_type, "text/html") &&
       !strstr(result.content_type, "text/plain") &&
       !strstr(result.content_type, "application/xhtml")) {
        end_loading();
        snprintf(message, sizeof(message), "Unsupported page type: %.90s", result.content_type);
        fetch_result_free(&result);
        document_free(&document);
        document_make_error(&document, "Unsupported content", message);
        snprintf(status_text, sizeof(status_text), "Unsupported content");
        redraw_needed = 1;
        return LOAD_FAILED;
    }

    document_free(&document);
    end_loading();
    document_init(&document, result.effective_url);
    document_parse_html(&document, (const char *)result.data, result.size);
    if(result.truncated)
        document_mark_shortened(&document,
            "[Page shortened: HTML exceeded the 512 KiB safety limit]");
    snprintf(address, sizeof(address), "%s", result.effective_url);
    snprintf(current_url, sizeof(current_url), "%s", result.effective_url);
    fetch_result_free(&result);
    scroll_y = 0;
    focused_link = -1;
    snprintf(status_text, sizeof(status_text), "Page ready; loading images...");
    render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                   address, 0, back_count > 0, forward_count > 0,
                   status_text);
    redraw_needed = 0;

    begin_loading("image");
    document_load_images(&document);
    images_cancelled = loading_cancelled;
    end_loading();
    if(images_cancelled)
        snprintf(status_text, sizeof(status_text), "Images canceled; page ready");
    else
        snprintf(status_text, sizeof(status_text), "%.82s%s", document.title,
                 document.truncated ? " [shortened]" : "");
    redraw_needed = 1;
    return 0;
}

static void navigate_to(const char *requested) {
    history_entry_t current;
    char target[MAX_URL];
    int result;

    snprintf(target, sizeof(target), "%s", requested);
    snprintf(current.url, sizeof(current.url), "%s", current_url);
    current.scroll_y = scroll_y;
    result = load_page(target);
    if(result != LOAD_CANCELED) {
        history_push(back_history, &back_count, current.url,
                     current.scroll_y, "back");
        forward_count = 0;
    }
}

static void navigate_back(void) {
    history_entry_t entry;
    history_entry_t current;

    if(!back_count) {
        snprintf(status_text, sizeof(status_text), "No previous page");
        redraw_needed = 1;
        return;
    }
    entry = back_history[back_count - 1];
    snprintf(current.url, sizeof(current.url), "%s", current_url);
    current.scroll_y = scroll_y;
    printf("browser: back -> %s (%d remaining)\n", entry.url, back_count - 1);
    if(load_page(entry.url) == 0) {
        back_count--;
        history_push(forward_history, &forward_count, current.url,
                     current.scroll_y, "forward");
        scroll_y = entry.scroll_y;
        clamp_scroll();
        redraw_needed = 1;
    }
}

static void navigate_forward(void) {
    history_entry_t entry;
    history_entry_t current;

    if(!forward_count) {
        snprintf(status_text, sizeof(status_text), "No next page");
        redraw_needed = 1;
        return;
    }
    entry = forward_history[forward_count - 1];
    snprintf(current.url, sizeof(current.url), "%s", current_url);
    current.scroll_y = scroll_y;
    printf("browser: forward -> %s (%d remaining)\n",
           entry.url, forward_count - 1);
    if(load_page(entry.url) == 0) {
        forward_count--;
        history_push(back_history, &back_count, current.url,
                     current.scroll_y, "back");
        scroll_y = entry.scroll_y;
        clamp_scroll();
        redraw_needed = 1;
    }
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
        "<p>After image</p><ul><li><div>First item</div></li><li>Second item</li></ul>"
        "<table><tr><td>Cell A</td><td>Cell B</td></tr>"
        "<tr><td>Cell C</td></tr></table>"
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
    document_parse_html(test, inline_html, sizeof(inline_html) - 1);
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

    bullet = find_layout_item(test, "* ", TEXT_NORMAL);
    first_item = find_layout_item(test, "First item", TEXT_NORMAL);
    LAYOUT_CHECK(bullet && first_item && bullet->y == first_item->y &&
                 bullet->x < first_item->x,
                 "list bullet flow");
    pre_a = find_layout_item(test, "A  B", TEXT_CODE);
    pre_c = find_layout_item(test, "C", TEXT_CODE);
    LAYOUT_CHECK(pre_a && pre_c && pre_c->y > pre_a->y,
                 "preformatted spacing");
    cell_a = find_layout_item(test, "Cell A", TEXT_NORMAL);
    cell_b = find_layout_item(test, "Cell B", TEXT_NORMAL);
    cell_c = find_layout_item(test, "Cell C", TEXT_NORMAL);
    LAYOUT_CHECK(cell_a && cell_b && cell_c && cell_a->y == cell_b->y &&
                 cell_a->x < cell_b->x && cell_c->y > cell_a->y,
                 "table row/cell fallback");

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
    document_parse_html(test, wrapping_html, sizeof(wrapping_html) - 1);
    LAYOUT_CHECK(test->item_count >= 3 &&
                 test->items[0].y < test->items[test->item_count - 1].y,
                 "word wrapping rows");
    for(i = 0; i < test->item_count; ++i) {
        LAYOUT_CHECK(test->items[i].x + test->items[i].width <=
                     PAGE_MARGIN + PAGE_WIDTH,
                     "item crossed page edge");
    }
    document_free(test);
    free(test);
    printf("browser: LAYOUT SELF-TEST PASSED (inline flow/styles/reflow)\n");
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

    if(network_fetch("https://httpbin.org/image/png", MAX_IMAGE_BYTES,
                     &asset) < 0 || asset.status != 200 || !asset.size ||
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
    if(strcmp(current_url, "https://example.com/")) {
        printf("browser: HISTORY SELF-TEST FAILED (forward navigation)\n");
        return;
    }
    navigate_back();
    if(strcmp(current_url, original) || scroll_y != original_scroll ||
       back_count || forward_count != 1) {
        printf("browser: HISTORY SELF-TEST FAILED (back/scroll restore)\n");
        return;
    }
    scroll_y = original_scroll / 2;
    clamp_scroll();
    updated_scroll = scroll_y;
    navigate_forward();
    if(strcmp(current_url, "https://example.com/") || scroll_y ||
       back_count != 1 || forward_count) {
        printf("browser: HISTORY SELF-TEST FAILED (forward/scroll restore)\n");
        return;
    }
    navigate_back();
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
                 "Pg%3D%3D") != 0 ||
       self_test_cancel_mode || !loading_cancelled ||
       strcmp(status_text, "Images canceled; page ready") ||
       document.image_count != 1 || document.images[0].loaded != -1) {
        printf("browser: CANCEL SELF-TEST FAILED (image cancellation)\n");
        return;
    }
    printf("browser: IMAGE CANCEL SELF-TEST PASSED (page kept)\n");
}
#endif

static void follow_link(int link_id) {
    if(link_id < 0 || link_id >= document.link_count) return;
    navigate_to(document.links[link_id]);
}

static void focus_next_link(void) {
    int start = focused_link;
    int i;
    if(!document.link_count) return;
    focused_link = (focused_link + 1) % document.link_count;
    for(i = 0; i < document.item_count; ++i) {
        if(document.items[i].link_id == focused_link) {
            scroll_y = document.items[i].y - 100;
            clamp_scroll();
            break;
        }
    }
    if(start != focused_link)
        snprintf(status_text, sizeof(status_text), "Link: %.80s", document.links[focused_link]);
    redraw_needed = 1;
}

static int process_keyboard(maple_device_t *keyboard) {
    int raw;
    int old_scroll = scroll_y;
    while(keyboard && (raw = kbd_queue_pop(keyboard, 0)) != KBD_QUEUE_END) {
        kbd_key_t key = (kbd_key_t)(raw & 0xff);
        kbd_mods_t mods = { .raw = (raw >> 8) & 0xff };
        kbd_leds_t leds = { .raw = (raw >> 16) & 0xff };
        kbd_state_t *state = maple_dev_status(keyboard);
        char ascii = state ? kbd_key_to_ascii(key, state->region, mods, leds) : 0;

        redraw_needed = 1;

        if(editing) {
            size_t len = strlen(address);
            if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER) {
                editing = 0;
                navigate_to(address);
            } else if(key == KBD_KEY_ESCAPE) {
                editing = 0;
                snprintf(address, sizeof(address), "%s", current_url);
                snprintf(status_text, sizeof(status_text), "%s", document.title);
            } else if(key == KBD_KEY_BACKSPACE && len) {
                address[len - 1] = 0;
            } else if(ascii >= 32 && ascii <= 126 && len + 1 < sizeof(address)) {
                address[len] = ascii;
                address[len + 1] = 0;
            }
            continue;
        }

        if(key == KBD_KEY_F6 || (key == KBD_KEY_L && (mods.raw & KBD_MOD_CTRL)))
            begin_address_edit();
        else if(key == KBD_KEY_F5) load_page(address);
        else if(key == KBD_KEY_TAB) focus_next_link();
        else if((key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER) && focused_link >= 0)
            follow_link(focused_link);
        else if((key == KBD_KEY_BACKSPACE && !(mods.raw & KBD_MOD_SHIFT)) ||
                (key == KBD_KEY_LEFT && (mods.raw & KBD_MOD_ALT)))
            navigate_back();
        else if((key == KBD_KEY_BACKSPACE && (mods.raw & KBD_MOD_SHIFT)) ||
                (key == KBD_KEY_RIGHT && (mods.raw & KBD_MOD_ALT)))
            navigate_forward();
        else if(key == KBD_KEY_PGDOWN || key == KBD_KEY_SPACE) scroll_y += 350;
        else if(key == KBD_KEY_PGUP) scroll_y -= 350;
        else if(key == KBD_KEY_HOME) scroll_y = 0;
        else if(key == KBD_KEY_END) scroll_y = max_scroll();
        else if(key == KBD_KEY_ESCAPE) return 1;
        clamp_scroll();
    }

    /* Poll arrow state directly so Flycast navigation keys work reliably and
       holding a key scrolls smoothly instead of depending on key-repeat events. */
    if(keyboard && !editing) {
        kbd_state_t *state = kbd_get_state(keyboard);
        if(state) {
            if(state->key_states[KBD_KEY_DOWN].is_down) scroll_y += 14;
            if(state->key_states[KBD_KEY_UP].is_down) scroll_y -= 14;
            clamp_scroll();
        }
    }
    if(scroll_y != old_scroll) redraw_needed = 1;
    return 0;
}

static int process_mouse(maple_device_t *mouse) {
    static uint32_t previous_buttons;
    mouse_state_t *state;
    uint32_t pressed;
    int old_x = mouse_x;
    int old_y = mouse_y;
    int old_scroll = scroll_y;
    int old_focus = focused_link;
    if(!mouse || !(state = maple_dev_status(mouse))) return 0;
    mouse_x += state->dx;
    mouse_y += state->dy;
    if(mouse_x < 0) mouse_x = 0;
    if(mouse_x >= SCREEN_W) mouse_x = SCREEN_W - 1;
    if(mouse_y < 0) mouse_y = 0;
    if(mouse_y >= SCREEN_H) mouse_y = SCREEN_H - 1;
    if(state->dz) {
        scroll_y -= state->dz * 48;
        clamp_scroll();
    }
    focused_link = link_at(mouse_x, mouse_y);
    pressed = state->buttons & ~previous_buttons;
    previous_buttons = state->buttons;
    if(pressed & MOUSE_LEFTBUTTON) {
        if(mouse_y >= 8 && mouse_y < 40 && mouse_x < 62) navigate_back();
        else if(mouse_y >= 8 && mouse_y < 40 && mouse_x < 120) navigate_forward();
        else if(mouse_y >= 8 && mouse_y < 40 && mouse_x < 566) begin_address_edit();
        else if(mouse_y >= 8 && mouse_y < 40 && mouse_x >= 566) {
            if(editing) { editing = 0; navigate_to(address); }
            else load_page(address);
        }
        else if(focused_link >= 0) follow_link(focused_link);
    }
    if(mouse_x != old_x || mouse_y != old_y || scroll_y != old_scroll ||
       focused_link != old_focus || pressed)
        redraw_needed = 1;
    return 0;
}

static int process_controller(maple_device_t *controller) {
    static uint32_t previous_buttons;
    static int previous_ltrig;
    static int previous_rtrig;
    cont_state_t *state;
    uint32_t pressed;
    if(!controller || !(state = maple_dev_status(controller))) return 0;
    pressed = state->buttons & ~previous_buttons;
    previous_buttons = state->buttons;
    if(pressed || (state->buttons & (CONT_DPAD_DOWN | CONT_DPAD_UP |
                                    CONT_DPAD_RIGHT | CONT_DPAD_LEFT)))
        redraw_needed = 1;
    if(pressed & CONT_START) return 1;
    if(pressed & CONT_X) begin_address_edit();
    if(pressed & CONT_B) {
        if(editing) {
            editing = 0;
            snprintf(address, sizeof(address), "%s", current_url);
            snprintf(status_text, sizeof(status_text), "%s", document.title);
        } else {
            navigate_back();
        }
    }
    if(pressed & CONT_A) {
        if(editing) { editing = 0; navigate_to(address); }
        else if(focused_link >= 0) follow_link(focused_link);
    }
    if(pressed & CONT_Y) focus_next_link();
    if(state->ltrig > 64 && previous_ltrig <= 64 && !editing) navigate_back();
    if(state->rtrig > 64 && previous_rtrig <= 64 && !editing) navigate_forward();
    previous_ltrig = state->ltrig;
    previous_rtrig = state->rtrig;
    if(state->buttons & CONT_DPAD_DOWN) scroll_y += 14;
    if(state->buttons & CONT_DPAD_UP) scroll_y -= 14;
    if(state->buttons & CONT_DPAD_RIGHT) scroll_y += 48;
    if(state->buttons & CONT_DPAD_LEFT) scroll_y -= 48;
    clamp_scroll();
    return 0;
}

int main(int argc, char **argv) {
    maple_device_t *keyboard;
    maple_device_t *mouse;
    maple_device_t *controller;
    int quit = 0;
    (void)argc;
    (void)argv;

    init_video();
    document_make_error(&document, "Dreamcast Browser",
        "Starting network. F6 or Ctrl+L opens the address bar. Mouse, keyboard, and controller are supported.");
    snprintf(status_text, sizeof(status_text), "Starting network...");
    render_browser(&document, 0, mouse_x, mouse_y, -1, address, 0, 0, 0,
                   status_text);

    if(!net_default_dev) {
        document_free(&document);
        document_make_error(&document, "No network device",
            "No modem, LAN adapter, or BBA was initialized. Check the hardware or Flycast network settings.");
        snprintf(status_text, sizeof(status_text), "Offline");
    } else if(network_init() < 0) {
        document_free(&document);
        document_make_error(&document, "Network startup failed",
                            "The secure HTTP client could not be initialized.");
        snprintf(status_text, sizeof(status_text), "Offline");
    } else {
        const uint8_t *ip = net_default_dev->ip_addr;
        printf("browser: network %s, IP %u.%u.%u.%u\n", net_default_dev->name,
               ip[0], ip[1], ip[2], ip[3]);
        if(load_page(address) == 0) {
#ifdef BROWSER_HISTORY_SELF_TEST
            run_history_self_test();
#endif
        }
    }

    while(!quit) {
        keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
        mouse = maple_enum_type(0, MAPLE_FUNC_MOUSE);
        controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        quit |= process_keyboard(keyboard);
        process_mouse(mouse);
        quit |= process_controller(controller);
        if(redraw_needed) {
            render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                           address, editing, back_count > 0, forward_count > 0,
                           status_text);
            redraw_needed = 0;
        }
        thd_sleep(16);
    }

    document_free(&document);
    network_shutdown();
    printf("browser: clean shutdown\n");
    return 0;
}
