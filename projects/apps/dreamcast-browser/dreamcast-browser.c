#include "browser.h"

#include <ctype.h>
#include <curl/curl.h>
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
static int editing_field = -1;
static char field_backup[MAX_FIELD_VALUE];
static const char *pending_post;
static int redraw_needed = 1;
static const char *loading_label;
static uint64_t last_progress_draw;
static unsigned progress_frame;
static uint32_t loading_controller_buttons;
static uint32_t loading_mouse_buttons;
static int loading_cancelled;
static int address_caret;
static int address_selected;
static int show_help;
static int exit_armed;
static int escape_released;

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

static void redraw(void) {
    browser_view_t view = {
        .scroll_y = scroll_y,
        .mouse_x = mouse_x,
        .mouse_y = mouse_y,
        .focused_link = focused_link,
        .address = address,
        .editing = editing,
        .address_caret = address_caret,
        .address_selected = address_selected,
        .can_go_back = back_count > 0,
        .can_go_forward = forward_count > 0,
        .show_help = show_help,
        .status = status_text
    };
    render_browser(&document, &view);
}

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
        redraw();
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
    redraw();
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

static int page_step(void) {
    return SCREEN_H - PAGE_TOP - 48;
}

/* Returns 0 when the link has no laid-out text or image to focus. */
static int link_bounds(int link, int *top, int *bottom) {
    int i;
    int found = 0;
    for(i = 0; i < document.item_count; ++i) {
        const document_item_t *item = &document.items[i];
        if(item->link_id != link) continue;
        if(!found || item->y < *top) *top = item->y;
        if(!found || item->y + item->height > *bottom)
            *bottom = item->y + item->height;
        found = 1;
    }
    return found;
}

static int link_on_screen(int link) {
    int top, bottom;
    return link_bounds(link, &top, &bottom) && bottom > scroll_y &&
           top < scroll_y + SCREEN_H - PAGE_TOP;
}

static void scroll_link_into_view(int link) {
    int top, bottom;
    int visible = SCREEN_H - PAGE_TOP;
    if(!link_bounds(link, &top, &bottom)) return;
    if(top < scroll_y + 8) {
        scroll_y = top - 48;
    } else if(bottom > scroll_y + visible - 8) {
        scroll_y = bottom - visible + 48;
        if(scroll_y > top - 8) scroll_y = top - 8;
    }
    clamp_scroll();
}

static void describe_focus(void) {
    const char *target;
    if(focused_link < 0 || focused_link >= document.link_count) return;
    target = document.links[focused_link];
    if(!strncmp(target, "form:", 5)) {
        int index = atoi(target + 5);
        const browser_field_t *field;
        if(index < 0 || index >= document.field_count) return;
        field = &document.fields[index];
        if(!strcmp(field->type, "submit"))
            snprintf(status_text, sizeof(status_text), "Button %.20s: Enter submits",
                     field->value[0] ? field->value : "Submit");
        else if(!strcmp(field->type, "checkbox"))
            snprintf(status_text, sizeof(status_text), "Checkbox %.18s: Enter toggles",
                     field->name);
        else
            snprintf(status_text, sizeof(status_text), "Field %.21s: Enter edits",
                     field->name);
        return;
    }
    /* HTTPS is the default, so spend the narrow footer on the rest. */
    if(!strncmp(target, "https://", 8)) target += 8;
    snprintf(status_text, sizeof(status_text), "Link: %.80s", target);
}

/* Moves keyboard focus through links and form controls in document order.
   When the focused link has been scrolled away, start from the viewport. */
static void focus_step(int direction) {
    int next = -1;
    int i;
    int visible = SCREEN_H - PAGE_TOP;

    if(focused_link >= 0 && focused_link < document.link_count &&
       link_on_screen(focused_link)) {
        for(i = 1; i <= document.link_count; ++i) {
            int candidate = ((focused_link + direction * i) % document.link_count +
                             document.link_count) % document.link_count;
            int top, bottom;
            if(link_bounds(candidate, &top, &bottom)) {
                next = candidate;
                break;
            }
        }
    } else if(direction > 0) {
        for(i = 0; i < document.item_count && next < 0; ++i) {
            const document_item_t *item = &document.items[i];
            if(item->link_id >= 0 && item->y + item->height > scroll_y)
                next = item->link_id;
        }
        for(i = 0; i < document.item_count && next < 0; ++i)
            if(document.items[i].link_id >= 0) next = document.items[i].link_id;
    } else {
        for(i = document.item_count - 1; i >= 0 && next < 0; --i) {
            const document_item_t *item = &document.items[i];
            if(item->link_id >= 0 && item->y < scroll_y + visible)
                next = item->link_id;
        }
        for(i = document.item_count - 1; i >= 0 && next < 0; --i)
            if(document.items[i].link_id >= 0) next = document.items[i].link_id;
    }

    redraw_needed = 1;
    if(next < 0) {
        snprintf(status_text, sizeof(status_text), "No links on this page");
        return;
    }
    focused_link = next;
    scroll_link_into_view(next);
    describe_focus();
}

static int is_word_char(char c) {
    return isalnum((unsigned char)c);
}

/* Shared single-line editing for the address bar and text fields. When
   selected is non-NULL and set, the whole text is selected: typing replaces
   it and deletion clears it. */
static void edit_line(char *text, size_t size, size_t limit, int *caret,
                      int *selected, kbd_key_t key, kbd_mods_t mods,
                      char ascii) {
    size_t len = strlen(text);
    size_t pos = *caret < 0 ? len : (size_t)*caret;
    int ctrl = (mods.raw & KBD_MOD_CTRL) != 0;
    int all = selected && *selected;

    if(pos > len) pos = len;
    if(selected) *selected = 0;

    if(key == KBD_KEY_A && ctrl) {
        if(selected) *selected = 1;
        pos = len;
    } else if(all && (key == KBD_KEY_BACKSPACE || key == KBD_KEY_DEL ||
                      (ctrl && (key == KBD_KEY_U || key == KBD_KEY_W)))) {
        text[0] = 0;
        pos = 0;
    } else if(key == KBD_KEY_LEFT) {
        if(all) pos = 0;
        else if(ctrl) {
            while(pos && !is_word_char(text[pos - 1])) pos--;
            while(pos && is_word_char(text[pos - 1])) pos--;
        } else if(pos) pos--;
    } else if(key == KBD_KEY_RIGHT) {
        if(all) pos = len;
        else if(ctrl) {
            while(pos < len && !is_word_char(text[pos])) pos++;
            while(pos < len && is_word_char(text[pos])) pos++;
        } else if(pos < len) pos++;
    } else if(key == KBD_KEY_HOME) {
        pos = 0;
    } else if(key == KBD_KEY_END) {
        pos = len;
    } else if(ctrl && (key == KBD_KEY_BACKSPACE || key == KBD_KEY_W)) {
        size_t from = pos;
        while(from && !is_word_char(text[from - 1])) from--;
        while(from && is_word_char(text[from - 1])) from--;
        memmove(text + from, text + pos, len - pos + 1);
        pos = from;
    } else if(ctrl && key == KBD_KEY_U) {
        memmove(text, text + pos, len - pos + 1);
        pos = 0;
    } else if(key == KBD_KEY_BACKSPACE) {
        if(pos) {
            memmove(text + pos - 1, text + pos, len - pos + 1);
            pos--;
        }
    } else if(key == KBD_KEY_DEL) {
        if(pos < len) memmove(text + pos, text + pos + 1, len - pos);
    } else if(!ctrl && !(mods.raw & KBD_MOD_ALT) && ascii >= 32 && ascii <= 126) {
        if(all) {
            text[0] = 0;
            len = pos = 0;
        }
        if(len < limit && len + 1 < size) {
            memmove(text + pos + 1, text + pos, len - pos + 1);
            text[pos++] = ascii;
        }
    } else if(all) {
        *selected = 1; /* Unrelated keys keep the selection. */
    }
    *caret = (int)pos;
}

static void begin_address_edit(void) {
    editing = 1;
    focused_link = -1;
    show_help = 0;
    address_caret = (int)strlen(address);
    address_selected = 1;
    snprintf(status_text, sizeof(status_text), "Type a URL: Enter opens, Esc cancels");
    redraw_needed = 1;
}

static void cancel_address_edit(void) {
    editing = 0;
    address_selected = 0;
    snprintf(address, sizeof(address), "%s", current_url);
    snprintf(status_text, sizeof(status_text), "%s", document.title);
    redraw_needed = 1;
}

static void finish_field_edit(int restore) {
    browser_field_t *field;
    if(editing_field < 0 || editing_field >= document.field_count) {
        editing_field = -1;
        return;
    }
    field = &document.fields[editing_field];
    if(restore) snprintf(field->value, sizeof(field->value), "%s", field_backup);
    field->caret = -1;
    document_refresh_field(&document, editing_field);
    editing_field = -1;
    memset(field_backup, 0, sizeof(field_backup));
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

    finish_field_edit(0);
    editing = 0;
    address_selected = 0;
    show_help = 0;
    snprintf(previous_url, sizeof(previous_url), "%s", current_url);
    snprintf(previous_address, sizeof(previous_address), "%s", address);
    snprintf(target, sizeof(target), "%s", requested);
    normalize_address(target, sizeof(target));
    snprintf(current_url, sizeof(current_url), "%s", target);
    snprintf(address, sizeof(address), "%s", target);
    snprintf(status_text, sizeof(status_text), "Connecting page...");
    redraw();
    redraw_needed = 0;
    begin_loading("page");

    int fetch_code = pending_post ? network_post(target,pending_post,MAX_DOCUMENT_BYTES,&result)
                                  : network_fetch(target,MAX_DOCUMENT_BYTES,&result);
    pending_post=NULL;
    if(fetch_code < 0) {
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

    if((result.status < 200 || result.status >= 400) && !strstr(result.content_type,"text/html")) {
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
    redraw();
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

static void submit_form(int field_index) {
    browser_field_t *clicked=&document.fields[field_index];
    browser_form_t *form=&document.forms[clicked->form];
    char body[32768]={0}, target[MAX_URL];
    size_t used=0;
    if(!form->valid || !network_same_origin(current_url,form->action) || strncmp(form->action,"https://",8)) {
        snprintf(status_text,sizeof(status_text),"Form blocked: requires supported fields and same-origin HTTPS");
        redraw_needed=1;return;
    }
    for(int i=0;i<document.field_count;++i) {
        browser_field_t *field=&document.fields[i];
        char *name,*value;
        size_t needed;
        if(field->form!=clicked->form || !field->name[0] ||
           (!strcmp(field->type,"checkbox")&&!field->checked) ||
           (!strcmp(field->type,"submit")&&i!=field_index))continue;
        if(!form->post&&!strcmp(field->type,"password")) {
            snprintf(status_text,sizeof(status_text),"Password forms require POST");redraw_needed=1;return;
        }
        name=curl_easy_escape(NULL,field->name,0);value=curl_easy_escape(NULL,field->value,0);
        if(!name||!value) {curl_free(name);curl_free(value);return;}
        needed=strlen(name)+strlen(value)+2;
        if(used+needed>=sizeof(body)) {curl_free(name);curl_free(value);return;}
        used+=(size_t)snprintf(body+used,sizeof(body)-used,"%s%s=%s",used?"&":"",name,value);
        curl_free(name);memset(value,0,strlen(value));curl_free(value);
    }
    snprintf(target,sizeof(target),"%s",form->action);
    if(form->post)pending_post=body;
    else {
        char *query=strchr(target,'?');if(query)*query=0;
        if(strlen(target)+used+2>=sizeof(target)) {
            snprintf(status_text,sizeof(status_text),"Form query too long");redraw_needed=1;return;
        }
        strcat(target,"?");strcat(target,body);
    }
    navigate_to(target);
    memset(body,0,sizeof(body));
}

static void follow_link(int link_id) {
    if(link_id < 0 || link_id >= document.link_count) return;
    if(!strncmp(document.links[link_id],"form:",5)) {
        int index=atoi(document.links[link_id]+5);
        browser_field_t *field;
        if(index<0||index>=document.field_count)return;
        field=&document.fields[index];
        if(!strcmp(field->type,"submit"))submit_form(index);
        else if(!strcmp(field->type,"checkbox")) {
            field->checked=!field->checked;document_refresh_field(&document,index);
        } else {
            editing_field=index;
            snprintf(field_backup,sizeof(field_backup),"%s",field->value);
            field->caret=(int)strlen(field->value);
            document_refresh_field(&document,index);
            snprintf(status_text,sizeof(status_text),"Edit %.14s: Enter done, Esc undo",field->name);
        }
        redraw_needed=1;return;
    }
    navigate_to(document.links[link_id]);
}

static void reload_page(void) {
    int saved_scroll = scroll_y;
    if(load_page(address) == 0) {
        scroll_y = saved_scroll;
        clamp_scroll();
    }
}

static void disarm_exit(void) {
    if(!exit_armed) return;
    exit_armed = 0;
    snprintf(status_text, sizeof(status_text), "%s", document.title);
}

/* Esc first clears link focus, then needs a second, separate press to exit
   so a stray or held Esc cannot close the browser. */
static int handle_escape(void) {
    if(focused_link >= 0) {
        focused_link = -1;
        snprintf(status_text, sizeof(status_text), "%s", document.title);
        return 0;
    }
    if(exit_armed) return escape_released;
    exit_armed = 1;
    escape_released = 0;
    snprintf(status_text, sizeof(status_text), "Press Esc again to exit");
    return 0;
}

static void process_field_key(kbd_key_t key, kbd_mods_t mods, char ascii) {
    browser_field_t *field = &document.fields[editing_field];
    if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER ||
       key == KBD_KEY_TAB || key == KBD_KEY_ESCAPE) {
        finish_field_edit(key == KBD_KEY_ESCAPE);
        snprintf(status_text, sizeof(status_text),
                 key == KBD_KEY_ESCAPE ? "Edit undone" : "Field saved; Tab moves on");
        if(key == KBD_KEY_TAB) focus_step(mods.raw & KBD_MOD_SHIFT ? -1 : 1);
        return;
    }
    edit_line(field->value, sizeof(field->value), (size_t)field->maxlength,
              &field->caret, NULL, key, mods, ascii);
    document_refresh_field(&document, editing_field);
}

static void process_address_key(kbd_key_t key, kbd_mods_t mods, char ascii) {
    if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER) {
        if(!address[0]) cancel_address_edit();
        else navigate_to(address);
    } else if(key == KBD_KEY_ESCAPE) {
        cancel_address_edit();
    } else if(key == KBD_KEY_F6 || (key == KBD_KEY_L && (mods.raw & KBD_MOD_CTRL))) {
        address_caret = (int)strlen(address);
        address_selected = 1;
    } else {
        edit_line(address, sizeof(address), sizeof(address) - 1, &address_caret,
                  &address_selected, key, mods, ascii);
    }
}

/* Handles one queued key press; returns 1 when the browser should exit. */
static int handle_key(kbd_key_t key, kbd_mods_t mods, char ascii) {
    int ctrl = (mods.raw & KBD_MOD_CTRL) != 0;
    int alt = (mods.raw & KBD_MOD_ALT) != 0;
    int shift = (mods.raw & KBD_MOD_SHIFT) != 0;

    redraw_needed = 1;

    if(editing_field >= 0) {
        process_field_key(key, mods, ascii);
        return 0;
    }
    if(editing) {
        process_address_key(key, mods, ascii);
        return 0;
    }
    if(show_help) {
        show_help = 0; /* Any key dismisses help without acting. */
        return 0;
    }
    if(key != KBD_KEY_ESCAPE) disarm_exit();

    if(key == KBD_KEY_F1 || ascii == '?')
        show_help = 1;
    else if(key == KBD_KEY_F6 || (key == KBD_KEY_L && ctrl))
        begin_address_edit();
    else if(key == KBD_KEY_F5 || (key == KBD_KEY_R && ctrl))
        reload_page();
    else if(key == KBD_KEY_TAB)
        focus_step(shift ? -1 : 1);
    else if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER) {
        if(focused_link >= 0) follow_link(focused_link);
        else snprintf(status_text, sizeof(status_text), "Tab selects a link; F1 for help");
    }
    else if((key == KBD_KEY_BACKSPACE && !shift) || (key == KBD_KEY_LEFT && alt))
        navigate_back();
    else if((key == KBD_KEY_BACKSPACE && shift) || (key == KBD_KEY_RIGHT && alt))
        navigate_forward();
    else if(key == KBD_KEY_HOME && alt)
        navigate_to(BROWSER_HOME_URL);
    else if(key == KBD_KEY_PGDOWN || (key == KBD_KEY_SPACE && !shift))
        scroll_y += page_step();
    else if(key == KBD_KEY_PGUP || (key == KBD_KEY_SPACE && shift))
        scroll_y -= page_step();
    else if(key == KBD_KEY_HOME) scroll_y = 0;
    else if(key == KBD_KEY_END) scroll_y = max_scroll();
    else if(key == KBD_KEY_ESCAPE && handle_escape()) return 1;
    clamp_scroll();
    return 0;
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
        if(handle_key(key, mods, ascii)) return 1;
    }

    /* Poll arrow state directly so Flycast navigation keys work reliably and
       holding a key scrolls smoothly instead of depending on key-repeat events. */
    if(keyboard && !editing && editing_field<0) {
        kbd_state_t *state = kbd_get_state(keyboard);
        if(state) {
            if(exit_armed && !state->key_states[KBD_KEY_ESCAPE].is_down)
                escape_released = 1;
            if(!show_help) {
                if(state->key_states[KBD_KEY_DOWN].is_down) scroll_y += 14;
                if(state->key_states[KBD_KEY_UP].is_down) scroll_y -= 14;
            }
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
    if(mouse_x != old_x || mouse_y != old_y || state->dz)
        focused_link = link_at(mouse_x, mouse_y);
    pressed = state->buttons & ~previous_buttons;
    previous_buttons = state->buttons;
    if(pressed) disarm_exit();
    if(show_help && pressed) {
        show_help = 0;
        redraw_needed = 1;
        return 0;
    }
    if(pressed & MOUSE_LEFTBUTTON) {
        if(mouse_y >= 8 && mouse_y < 40 && mouse_x < 62) navigate_back();
        else if(mouse_y >= 8 && mouse_y < 40 && mouse_x < 120) navigate_forward();
        else if(mouse_y >= 8 && mouse_y < 40 && mouse_x < 566) begin_address_edit();
        else if(mouse_y >= 8 && mouse_y < 40 && mouse_x >= 566) {
            if(editing) navigate_to(address);
            else reload_page();
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
    if(pressed) disarm_exit();
    if(show_help) {
        if(pressed) show_help = 0;
        previous_ltrig = state->ltrig;
        previous_rtrig = state->rtrig;
        return 0;
    }
    if(pressed & CONT_START) return 1;
    if(pressed & CONT_X) begin_address_edit();
    if(pressed & CONT_B) {
        if(editing) cancel_address_edit();
        else navigate_back();
    }
    if(pressed & CONT_A) {
        if(editing) navigate_to(address);
        else if(focused_link >= 0) follow_link(focused_link);
    }
    if(pressed & CONT_Y) focus_step(1);
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

#ifdef BROWSER_HISTORY_SELF_TEST
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
    document_parse_html(&document, page_html, sizeof(page_html) - 1);
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
        if(!strcmp(document.fields[i].type,"submit")) { submit_form(i);return 0; }
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

int main(int argc, char **argv) {
    maple_device_t *keyboard;
    maple_device_t *mouse;
    maple_device_t *controller;
    int quit = 0;
    (void)argc;
    (void)argv;

    init_video();
    document_make_error(&document, "Dreamcast Browser",
        "Starting network. F6 or Ctrl+L opens the address bar, Tab moves between links, and F1 lists every keyboard shortcut. Mouse and controller are also supported.");
    snprintf(status_text, sizeof(status_text), "Starting network...");
    redraw();

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

#ifdef BROWSER_HISTORY_SELF_TEST
    run_keyboard_self_test();
#endif

#ifdef BROWSER_FORM_SELF_TEST
    run_form_self_test();
#endif

    while(!quit) {
        keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
        mouse = maple_enum_type(0, MAPLE_FUNC_MOUSE);
        controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        quit |= process_keyboard(keyboard);
        process_mouse(mouse);
        quit |= process_controller(controller);
        if(redraw_needed) {
            redraw();
            redraw_needed = 0;
        }
        thd_sleep(16);
    }

    document_free(&document);
    network_shutdown();
    printf("browser: clean shutdown\n");
    return 0;
}
