#include "browser.h"
#include "storage.h"

#include <ctype.h>
#include <curl/curl.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>
#include <dc/vblank.h>
#include <dc/video.h>
#include <kos.h>
#include <kos/sem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT | INIT_NET);

#ifndef BROWSER_HOME_URL
#ifdef BROWSER_PERF_SELF_TEST
#define BROWSER_HOME_URL "about:bookmarks"
#else
#define BROWSER_HOME_URL "https://appsbyrich.com/"
#endif
#endif

/* Address-bar text that is not a location is searched here. */
#ifndef BROWSER_SEARCH_URL
#define BROWSER_SEARCH_URL "https://lite.duckduckgo.com/lite/?q="
#endif

#define BOOKMARKS_URL "about:bookmarks"

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
static int select_backup;
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
static int toolbar_hidden;
static int exit_armed;
static int escape_released;
static int quit_requested;
static int osk_open;
static int osk_row = 1;
static int osk_column;
static int osk_shift;
static bookmark_list_t bookmarks;
static char bookmark_candidate_url[MAX_URL];
static char bookmark_candidate_title[BOOKMARK_TITLE];
static int storage_enabled = 1;

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

/* Ctrl+B hides the toolbar for a full-height page; it comes back while
   the address is being edited or a page is loading. */
static int toolbar_shown(void) {
    return !toolbar_hidden || editing || loading_label != NULL;
}

static int page_top(void) {
    return toolbar_shown() ? PAGE_TOP : 0;
}

static void current_view(browser_view_t *out) {
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
        .toolbar = toolbar_shown(),
        .show_help = show_help,
        .osk_open = osk_open,
        .osk_row = osk_row,
        .osk_column = osk_column,
        .osk_shift = osk_shift,
        .status = status_text
    };
    *out = view;
}

/* Frame clock. The main loop sleeps until the vertical blank interrupt
   rather than polling on a timer, so input is sampled and frames are
   presented once per display refresh with no scheduler-tick rounding. */
static semaphore_t frame_signal;
static int vblank_handle = -1;
#ifdef BROWSER_PROFILE
static volatile uint32_t prof_vblank_at;
#endif

static void on_vblank(uint32_t code, void *data) {
    kthread_t *frame_thread = data;
    int waiting = sem_count(&frame_signal) < 0;
    (void)code;
#ifdef BROWSER_PROFILE
    prof_vblank_at = (uint32_t)timer_us_gettime64();
#endif
    sem_signal(&frame_signal);
    /* Signaling makes the thread runnable, but KOS otherwise waits for its
       next 10 ms scheduler tick. Resume a frame waiter as this IRQ returns. */
    if(waiting) thd_schedule_next(frame_thread);
}

static void frame_clock_init(void) {
    sem_init(&frame_signal, 0);
    vblank_handle = vblank_handler_add(on_vblank, thd_get_current());
    if(vblank_handle < 0)
        printf("browser: no vblank handler; falling back to timed frames\n");
}

static void frame_clock_shutdown(void) {
    if(vblank_handle >= 0) vblank_handler_remove(vblank_handle);
    vblank_handle = -1;
    sem_destroy(&frame_signal);
}

/* Blocks until the next vertical blank, coalescing any that passed while
   the loop was busy so a long page load is not followed by a burst. */
static void wait_for_vblank(void) {
    if(vblank_handle < 0) {
        vid_waitvbl();
        return;
    }
    sem_wait(&frame_signal);
    while(sem_trywait(&frame_signal) == 0)
        ;
}

/* KOS polls every Maple device at each vertical blank; its DMA completes
   about a millisecond later and only then queues new key presses. Waiting
   for it means this frame's input is this frame's, not the previous one's. */
static void wait_for_maple_poll(void) {
    uint64_t deadline = timer_us_gettime64() + 4000;
    while(maple_state.dma_in_progress && timer_us_gettime64() < deadline)
        thd_pass();
}

#ifdef BROWSER_PROFILE
static uint64_t prof_draw_us, prof_present_us, prof_wait_us, prof_input_us, prof_report_at;
static uint64_t prof_poll_us, prof_wake_us;
static unsigned prof_wake_max;
static unsigned prof_frames, prof_iters, prof_keys, prof_draw_max, prof_iter_max;
#define PROF_BEGIN uint64_t prof_t0 = timer_us_gettime64()
#define PROF_ADD(acc) (acc += timer_us_gettime64() - prof_t0)
#else
#define PROF_BEGIN (void)0
#define PROF_ADD(acc) (void)0
#endif

/* Composes the changed rows now; presenting waits for the display. */
static void compose(void) {
    browser_view_t view;
    current_view(&view);
#ifdef BROWSER_PROFILE
    {
        uint64_t t0 = timer_us_gettime64(), t1;
        render_frame(&document, &view);
        t1 = timer_us_gettime64();
        prof_draw_us += t1 - t0;
        if(t1 - t0 > prof_draw_max) prof_draw_max = (unsigned)(t1 - t0);
    }
#else
    render_frame(&document, &view);
#endif
}

static void present(void) {
    PROF_BEGIN;
    /* The buffer flipped away from last time is scanned out until the next
       vertical blank; sleep through that rather than spin. */
    while(!render_present_ready()) wait_for_vblank();
    render_present();
    PROF_ADD(prof_present_us);
#ifdef BROWSER_PROFILE
    prof_frames++;
#endif
}

static void redraw(void) {
    compose();
    present();
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
    /* Show the (blank) second buffer so the first frame is drawn off screen. */
    vid_flip(-1);
}

/* While the on-screen keyboard covers the bottom of the page, allow
   scrolling far enough to lift the last rows above it. */
static int max_scroll(void) {
    int visible = SCREEN_H - page_top() - (osk_open ? SCREEN_H - OSK_TOP : 0);
    return document.height > visible ? document.height - visible : 0;
}

static void clamp_scroll(void) {
    int maximum = max_scroll();
    if(scroll_y < 0) scroll_y = 0;
    if(scroll_y > maximum) scroll_y = maximum;
}

static int link_at(int x, int screen_y) {
    int i;
    int page_y = screen_y - page_top() + scroll_y;
    if(screen_y < page_top() || (osk_open && screen_y >= OSK_TOP)) return -1;
    for(i = 0; i < document.item_count; ++i) {
        const document_item_t *item = &document.items[i];
        if(item->link_id >= 0 && x >= item->x && x <= item->x + item->width &&
           page_y >= item->y && page_y <= item->y + item->height)
            return item->link_id;
    }
    return -1;
}

static int page_step(void) {
    return SCREEN_H - page_top() - 48;
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
           top < scroll_y + SCREEN_H - page_top();
}

static void scroll_link_into_view(int link) {
    int top, bottom;
    int visible = SCREEN_H - page_top();
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
    int visible = SCREEN_H - page_top();

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

static void insert_char(char *text, size_t size, size_t limit, int *caret, char c) {
    size_t len = strlen(text);
    size_t pos = *caret < 0 || (size_t)*caret > len ? len : (size_t)*caret;
    if(len >= limit || len + 1 >= size) return;
    memmove(text + pos + 1, text + pos, len - pos + 1);
    text[pos] = c;
    *caret = (int)pos + 1;
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
        int inserted = all ? 0 : (int)pos;
        if(all) text[0] = 0;
        insert_char(text, size, limit, &inserted, ascii);
        pos = (size_t)inserted;
    } else if(all) {
        *selected = 1; /* Unrelated keys keep the selection. */
    }
    *caret = (int)pos;
}

static int keyboard_attached(void) {
    return maple_enum_type(0, MAPLE_FUNC_KEYBOARD) != NULL;
}

static void close_osk(void) {
    if(!osk_open) return;
    osk_open = 0;
    osk_shift = 0;
    clamp_scroll();
    redraw_needed = 1;
}

/* Keeps the field being edited visible above the on-screen keyboard. */
static void reveal_editing_field(void) {
    int top, bottom;
    int visible = (osk_open ? OSK_TOP : SCREEN_H) - page_top();
    const browser_field_t *field;
    if(editing_field < 0) return;
    field = &document.fields[editing_field];
    if(field->link < 0 || !link_bounds(field->link, &top, &bottom)) return;
    if(bottom > scroll_y + visible - 8) scroll_y = bottom - visible + 24;
    if(top < scroll_y + 8) scroll_y = top - 24;
    clamp_scroll();
}

static void open_osk(void) {
    osk_open = 1;
    osk_shift = 0;
    snprintf(status_text, sizeof(status_text), "A types, B deletes, Start finishes");
    reveal_editing_field();
    redraw_needed = 1;
}

static void begin_address_edit(int want_osk) {
    editing = 1;
    focused_link = -1;
    show_help = 0;
    address_caret = (int)strlen(address);
    address_selected = 1;
    snprintf(status_text, sizeof(status_text), "Type a URL or search: Enter opens");
    if(want_osk) open_osk();
    redraw_needed = 1;
}

static void cancel_address_edit(void) {
    editing = 0;
    address_selected = 0;
    close_osk();
    snprintf(address, sizeof(address), "%s", current_url);
    snprintf(status_text, sizeof(status_text), "%s", document.title);
    redraw_needed = 1;
}

static void finish_field_edit(int restore) {
    browser_field_t *field;
    if(editing_field < 0 || editing_field >= document.field_count) {
        editing_field = -1;
        close_osk();
        return;
    }
    field = &document.fields[editing_field];
    if(!strcmp(field->type, "select")) {
        if(restore) document_select_option(&document, editing_field, select_backup);
    } else if(restore) {
        snprintf(field->value, sizeof(field->value), "%s", field_backup);
    }
    field->caret = -1;
    document_refresh_field(&document, editing_field);
    editing_field = -1;
    memset(field_backup, 0, sizeof(field_backup));
    close_osk();
    redraw_needed = 1;
}

static void normalize_address(char *url, size_t size) {
    char temp[MAX_URL];
    if(strstr(url, "://") || !strncmp(url, "about:", 6)) return;
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

static int is_web_url(const char *url) {
    return !strncmp(url, "https://", 8) || !strncmp(url, "http://", 7);
}

/* Builds an internal page in place of a network response. */
static int load_internal(const char *url, const char *previous_url) {
    static char html[65536];
    char note[96];
    int found = !strcmp(url, BOOKMARKS_URL);

    /* The page being left is what "Bookmark this page" offers. */
    if(found && is_web_url(previous_url)) {
        snprintf(bookmark_candidate_url, sizeof(bookmark_candidate_url), "%s",
                 previous_url);
        snprintf(bookmark_candidate_title, sizeof(bookmark_candidate_title), "%.*s",
                 (int)sizeof(bookmark_candidate_title) - 1, document.title);
    }
    document_free(&document);
    if(found) {
        storage_describe(note, sizeof(note));
        bookmarks_page_html(&bookmarks, bookmark_candidate_url, bookmark_candidate_title,
                            BROWSER_HOME_URL, note, html, sizeof(html));
        document_init(&document, url);
        document_parse_html(&document, html, strlen(html),
                            "text/html; charset=windows-1252");
    } else {
        document_make_error(&document, "Unknown internal page",
                            "This browser has no internal page at that address.");
    }
    snprintf(current_url, sizeof(current_url), "%s", url);
    snprintf(address, sizeof(address), "%s", url);
    snprintf(status_text, sizeof(status_text), "%s", document.title);
    scroll_y = 0;
    focused_link = -1;
    redraw_needed = 1;
    return found ? 0 : LOAD_FAILED;
}

static int load_request(const char *requested, const char *post_body) {
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
    close_osk();
    snprintf(previous_url, sizeof(previous_url), "%s", current_url);
    snprintf(previous_address, sizeof(previous_address), "%s", address);
    snprintf(target, sizeof(target), "%s", requested);
    normalize_address(target, sizeof(target));
    if(!strncmp(target, "about:", 6)) return load_internal(target, previous_url);
    snprintf(current_url, sizeof(current_url), "%s", target);
    snprintf(address, sizeof(address), "%s", target);
    snprintf(status_text, sizeof(status_text), "Connecting page...");
    redraw();
    redraw_needed = 0;
    begin_loading("page");

    int fetch_code = post_body ? network_post(target, post_body, MAX_DOCUMENT_BYTES, &result)
                               : network_fetch(target, MAX_DOCUMENT_BYTES, &result);
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
    document_parse_html(&document, (const char *)result.data, result.size,
                        result.content_type);
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

static int load_page(const char *requested) {
    return load_request(requested, NULL);
}

static void navigate_request(const char *requested, const char *post_body) {
    history_entry_t current;
    char target[MAX_URL];
    int result;

    snprintf(target, sizeof(target), "%s", requested);
    snprintf(current.url, sizeof(current.url), "%s", current_url);
    current.scroll_y = scroll_y;
    result = load_request(target, post_body);
    if(result != LOAD_CANCELED) {
        history_push(back_history, &back_count, current.url,
                     current.scroll_y, "back");
        forward_count = 0;
    }
}

static void navigate_to(const char *requested) {
    navigate_request(requested, NULL);
}

/* Opens address-bar text as a location or, failing that, as a search. */
static void open_typed_address(void) {
    char target[MAX_URL];
    if(address_resolve(address, BROWSER_SEARCH_URL, target, sizeof(target)) < 0) {
        cancel_address_edit();
        return;
    }
    navigate_to(target);
}

static void navigate_back(void);

static void toggle_toolbar(void) {
    toolbar_hidden = !toolbar_hidden;
    clamp_scroll();
    snprintf(status_text, sizeof(status_text), "%s",
             toolbar_hidden ? "Address bar hidden; Ctrl+B shows it" : document.title);
    redraw_needed = 1;
}

static void toggle_bookmarks_page(void) {
    if(!strcmp(current_url, BOOKMARKS_URL) && back_count) navigate_back();
    else if(strcmp(current_url, BOOKMARKS_URL)) navigate_to(BOOKMARKS_URL);
}

static void persist_bookmarks(const char *done) {
    int result = storage_enabled ? storage_save_bookmarks(&bookmarks) : STORAGE_OK;
    if(result == STORAGE_OK)
        snprintf(status_text, sizeof(status_text), "%s", done);
    else if(result == STORAGE_NO_VMU)
        snprintf(status_text, sizeof(status_text), "%s (no VMU to save to)", done);
    else
        snprintf(status_text, sizeof(status_text), "Could not write bookmarks to VMU");
    redraw_needed = 1;
}

static void bookmark_current_page(void) {
    int result;
    if(!is_web_url(current_url)) {
        snprintf(status_text, sizeof(status_text), "Open a web page to bookmark it");
        redraw_needed = 1;
        return;
    }
    if(bookmarks_find(&bookmarks, current_url) >= 0) {
        snprintf(status_text, sizeof(status_text), "Already bookmarked");
        redraw_needed = 1;
        return;
    }
    result = bookmarks_add(&bookmarks, current_url, document.title);
    if(result == -1) {
        snprintf(status_text, sizeof(status_text), "Bookmarks full: remove one first");
        redraw_needed = 1;
        return;
    }
    persist_bookmarks("Bookmarked");
}

/* Refreshes the bookmarks page after a change without adding history. */
static void refresh_internal_page(void) {
    int saved_scroll = scroll_y;
    char saved_status[sizeof(status_text)];
    snprintf(saved_status, sizeof(saved_status), "%s", status_text);
    load_internal(current_url, current_url);
    scroll_y = saved_scroll;
    clamp_scroll();
    snprintf(status_text, sizeof(status_text), "%s", saved_status);
}

/* Actions exist only on internal pages, so a web page cannot exit the
   browser or change bookmarks by linking to them. */
static void handle_internal_link(const char *link) {
    int action = !strcmp(link, "about:bookmark-add") || !strcmp(link, "about:exit") ||
                 !strncmp(link, "about:bookmark-remove?", 22);
    if(!action) {
        navigate_to(link);
        return;
    }
    if(strncmp(current_url, "about:", 6)) {
        snprintf(status_text, sizeof(status_text), "Blocked a web page's internal link");
        redraw_needed = 1;
        return;
    }
    if(!strcmp(link, "about:exit")) {
        quit_requested = 1;
    } else if(!strcmp(link, "about:bookmark-add")) {
        int result = bookmarks_add(&bookmarks, bookmark_candidate_url,
                                   bookmark_candidate_title);
        if(result == -1)
            snprintf(status_text, sizeof(status_text), "Bookmarks full: remove one first");
        else if(result >= 0)
            persist_bookmarks("Bookmarked");
        refresh_internal_page();
    } else if(!bookmarks_remove(&bookmarks, atoi(link + 22))) {
        persist_bookmarks("Bookmark removed");
        refresh_internal_page();
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

/* Field text is ISO-8859-1 for the BIOS font. Servers expect UTF-8, and
   HTML submits line breaks as CRLF. */
static void form_encoding(const char *text, char *out, size_t size) {
    size_t n = 0;
    for(; *text && n + 3 < size; ++text) {
        unsigned char c = (unsigned char)*text;
        if(c == '\n') {
            out[n++] = '\r';
            out[n++] = '\n';
        } else if(c >= 0x80) {
            out[n++] = (char)(0xc0 | (c >> 6));
            out[n++] = (char)(0x80 | (c & 0x3f));
        } else {
            out[n++] = (char)c;
        }
    }
    out[n] = 0;
}

static int append_form_pair(char *body, size_t size, size_t *used,
                            const char *name, const char *value) {
    static char name_utf8[160];
    static char value_utf8[MAX_FIELD_VALUE * 2 + 1];
    char *escaped_name;
    char *escaped_value;
    int result = -1;

    form_encoding(name, name_utf8, sizeof(name_utf8));
    form_encoding(value, value_utf8, sizeof(value_utf8));
    escaped_name = curl_easy_escape(NULL, name_utf8, 0);
    escaped_value = curl_easy_escape(NULL, value_utf8, 0);
    if(escaped_name && escaped_value &&
       *used + strlen(escaped_name) + strlen(escaped_value) + 2 < size) {
        *used += (size_t)snprintf(body + *used, size - *used, "%s%s=%s",
                                  *used ? "&" : "", escaped_name, escaped_value);
        result = 0;
    }
    /* Values may be passwords: leave no copies behind. */
    memset(value_utf8, 0, sizeof(value_utf8));
    if(escaped_value) memset(escaped_value, 0, strlen(escaped_value));
    curl_free(escaped_name);
    curl_free(escaped_value);
    return result;
}

/* Encodes the form that field_index submits. Returns 0, or -1 after
   setting the status line to explain the refusal. */
static int build_form_body(int field_index, char *body, size_t size, size_t *out_used) {
    browser_field_t *clicked = &document.fields[field_index];
    browser_form_t *form = &document.forms[clicked->form];
    size_t used = 0;
    int i;

    if(!form->valid || !network_same_origin(current_url, form->action) ||
       strncmp(form->action, "https://", 8)) {
        snprintf(status_text, sizeof(status_text),
                 "Form blocked: requires supported fields and same-origin HTTPS");
        redraw_needed = 1;
        return -1;
    }
    body[0] = 0;
    for(i = 0; i < document.field_count; ++i) {
        browser_field_t *field = &document.fields[i];
        int ok = 0;
        if(field->form != clicked->form || field->disabled ||
           ((!strcmp(field->type, "checkbox") || !strcmp(field->type, "radio")) &&
            !field->checked) ||
           (document_field_is_submit(field) && i != field_index) ||
           (!strcmp(field->type, "select") && !field->option_count))
            continue;
        if(!form->post && !strcmp(field->type, "password")) {
            snprintf(status_text, sizeof(status_text), "Password forms require POST");
            redraw_needed = 1;
            memset(body, 0, size);
            return -1;
        }
        if(!strcmp(field->type, "image")) {
            /* Image buttons submit the click position; there is none here. */
            char x[80], y[80];
            snprintf(x, sizeof(x), "%s%sx", field->name, field->name[0] ? "." : "");
            snprintf(y, sizeof(y), "%s%sy", field->name, field->name[0] ? "." : "");
            ok = append_form_pair(body, size, &used, x, "0") == 0 &&
                 append_form_pair(body, size, &used, y, "0") == 0;
        } else if(!field->name[0]) {
            continue;
        } else {
            ok = append_form_pair(body, size, &used, field->name, field->value) == 0;
        }
        if(!ok) {
            snprintf(status_text, sizeof(status_text), "Form data too large to send");
            redraw_needed = 1;
            memset(body, 0, size);
            return -1;
        }
    }
    *out_used = used;
    return 0;
}

static void submit_form(int field_index) {
    /* Static so a large body does not sit on the stack through TLS. */
    static char body[32768];
    browser_form_t *form = &document.forms[document.fields[field_index].form];
    char target[MAX_URL];
    size_t used;

    if(build_form_body(field_index, body, sizeof(body), &used) < 0) return;
    snprintf(target, sizeof(target), "%s", form->action);
    if(form->post) {
        navigate_request(target, body);
    } else {
        char *query = strpbrk(target, "?#");
        if(query) *query = 0;
        if(strlen(target) + used + 2 >= sizeof(target)) {
            snprintf(status_text, sizeof(status_text), "Form query too long");
            redraw_needed = 1;
        } else {
            strcat(target, "?");
            strcat(target, body);
            navigate_to(target);
        }
    }
    memset(body, 0, sizeof(body));
}

static void begin_field_edit(int index, int want_osk) {
    browser_field_t *field = &document.fields[index];
    editing_field = index;
    if(!strcmp(field->type, "select")) {
        select_backup = field->selected;
        field->caret = 0;
        snprintf(status_text, sizeof(status_text), "Arrows choose, Enter done, Esc undo");
    } else {
        snprintf(field_backup, sizeof(field_backup), "%s", field->value);
        field->caret = (int)strlen(field->value);
        snprintf(status_text, sizeof(status_text), "Edit %.14s: Enter done, Esc undo",
                 field->name);
        if(want_osk) open_osk();
    }
    document_refresh_field(&document, index);
    redraw_needed = 1;
}

static void activate_field(int index, int want_osk) {
    browser_field_t *field;
    if(index < 0 || index >= document.field_count) return;
    field = &document.fields[index];
    if(document_field_is_submit(field)) submit_form(index);
    else if(!strcmp(field->type, "checkbox") || !strcmp(field->type, "radio"))
        document_toggle_field(&document, index);
    else begin_field_edit(index, want_osk);
    redraw_needed = 1;
}

static void follow_link(int link_id, int want_osk) {
    const char *link;
    if(link_id < 0 || link_id >= document.link_count) return;
    link = document.links[link_id];
    if(!strncmp(link, "form:", 5)) activate_field(atoi(link + 5), want_osk);
    else if(!strncmp(link, "about:", 6)) handle_internal_link(link);
    else navigate_to(link);
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

/* Moves a select list's choice; typing a letter jumps to the next option
   whose label starts with it. */
static void process_select_key(browser_field_t *field, kbd_key_t key, char ascii) {
    int selected = field->selected;
    int i;
    if(key == KBD_KEY_UP || key == KBD_KEY_LEFT) selected--;
    else if(key == KBD_KEY_DOWN || key == KBD_KEY_RIGHT) selected++;
    else if(key == KBD_KEY_PGUP) selected -= 5;
    else if(key == KBD_KEY_PGDOWN) selected += 5;
    else if(key == KBD_KEY_HOME) selected = 0;
    else if(key == KBD_KEY_END) selected = field->option_count - 1;
    else if(isalnum((unsigned char)ascii)) {
        for(i = 1; i <= field->option_count; ++i) {
            int candidate = (field->selected + i) % field->option_count;
            const char *label = document.options[field->option_first + candidate].label;
            if(tolower((unsigned char)label[0]) == tolower((unsigned char)ascii)) {
                selected = candidate;
                break;
            }
        }
    }
    document_select_option(&document, editing_field, selected);
    field->caret = 0;
    document_refresh_field(&document, editing_field);
}

static void process_field_key(kbd_key_t key, kbd_mods_t mods, char ascii) {
    browser_field_t *field = &document.fields[editing_field];
    int shift = (mods.raw & KBD_MOD_SHIFT) != 0;
    if(!strcmp(field->type, "textarea") && shift &&
       (key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER)) {
        insert_char(field->value, sizeof(field->value), (size_t)field->maxlength,
                    &field->caret, '\n');
        document_refresh_field(&document, editing_field);
        return;
    }
    if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER ||
       key == KBD_KEY_TAB || key == KBD_KEY_ESCAPE) {
        finish_field_edit(key == KBD_KEY_ESCAPE);
        snprintf(status_text, sizeof(status_text),
                 key == KBD_KEY_ESCAPE ? "Edit undone" : "Field saved; Tab moves on");
        if(key == KBD_KEY_TAB) focus_step(mods.raw & KBD_MOD_SHIFT ? -1 : 1);
        return;
    }
    if(!strcmp(field->type, "select")) {
        process_select_key(field, key, ascii);
        return;
    }
    edit_line(field->value, sizeof(field->value), (size_t)field->maxlength,
              &field->caret, NULL, key, mods, ascii);
    document_refresh_field(&document, editing_field);
}

static void process_address_key(kbd_key_t key, kbd_mods_t mods, char ascii) {
    if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER) {
        open_typed_address();
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
        begin_address_edit(0);
    else if(key == KBD_KEY_D && ctrl)
        bookmark_current_page();
    else if(key == KBD_KEY_B && ctrl && shift)
        toggle_bookmarks_page();
    else if(key == KBD_KEY_B && ctrl)
        toggle_toolbar();
    else if(key == KBD_KEY_F5 || (key == KBD_KEY_R && ctrl))
        reload_page();
    else if(key == KBD_KEY_TAB)
        focus_step(shift ? -1 : 1);
    else if(key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER) {
        if(focused_link >= 0) follow_link(focused_link, 0);
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
#ifdef BROWSER_PROFILE
        prof_keys++;
#endif
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

/* Applies an on-screen keyboard key to whichever text is being edited. */
static void osk_type(kbd_key_t key, char ascii) {
    kbd_mods_t none = { .raw = 0 };
    if(editing_field >= 0) {
        browser_field_t *field = &document.fields[editing_field];
        edit_line(field->value, sizeof(field->value), (size_t)field->maxlength,
                  &field->caret, NULL, key, none, ascii);
        document_refresh_field(&document, editing_field);
    } else if(editing) {
        edit_line(address, sizeof(address), sizeof(address) - 1, &address_caret,
                  &address_selected, key, none, ascii);
    }
    redraw_needed = 1;
}

static void osk_finish(int accept) {
    if(editing_field >= 0) {
        finish_field_edit(!accept);
        snprintf(status_text, sizeof(status_text),
                 accept ? "Field saved; Y moves on" : "Edit undone");
    } else if(editing) {
        if(accept) open_typed_address();
        else cancel_address_edit();
    }
    close_osk();
}

static void osk_activate(void) {
    osk_key_t key;
    const char *p;
    osk_key(osk_row, osk_column, osk_shift, &key);
    switch(key.action) {
    case OSK_CHAR:
        osk_type(KBD_KEY_NONE, key.ch);
        osk_shift = 0; /* Shift applies to one character, as on phones. */
        break;
    case OSK_SPACE: osk_type(KBD_KEY_NONE, ' '); break;
    case OSK_BACKSPACE: osk_type(KBD_KEY_BACKSPACE, 0); break;
    case OSK_TEXT: for(p = key.text; *p; ++p) osk_type(KBD_KEY_NONE, *p); break;
    case OSK_SHIFT: osk_shift = !osk_shift; break;
    case OSK_CANCEL: osk_finish(0); break;
    case OSK_DONE: osk_finish(1); break;
    }
    redraw_needed = 1;
}

static int process_mouse(maple_device_t *mouse) {
    static uint32_t previous_buttons;
    mouse_state_t sample, *shared;
    const mouse_state_t *state = &sample;
    irq_mask_t irqs;
    uint32_t pressed;
    int old_x = mouse_x;
    int old_y = mouse_y;
    int old_scroll = scroll_y;
    int old_focus = focused_link;
    if(!mouse) return 0;
    /* Autodetection can skip a device's poll for a frame. Consume relative
       motion once, keeping button state until the next Maple reply. */
    irqs = irq_disable();
    shared = maple_dev_status(mouse);
    if(shared) {
        sample = *shared;
        shared->dx = shared->dy = shared->dz = 0;
    }
    irq_restore(irqs);
    if(!shared) return 0;
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
        /* Without a keyboard, text entry needs the on-screen keyboard. */
        int want_osk = !keyboard_attached();
        int row, column;
        if(osk_open && mouse_y >= OSK_TOP) {
            if(osk_hit(mouse_x, mouse_y, &row, &column)) {
                osk_row = row;
                osk_column = column;
                osk_activate();
            }
        }
        else if(!toolbar_shown() || mouse_y < 8 || mouse_y >= 40) {
            if(focused_link >= 0) follow_link(focused_link, want_osk);
        }
        else if(mouse_x < 62) navigate_back();
        else if(mouse_x < 120) navigate_forward();
        else if(mouse_x < 566) begin_address_edit(want_osk);
        else {
            if(editing) open_typed_address();
            else reload_page();
        }
    }
    if(mouse_x != old_x || mouse_y != old_y || scroll_y != old_scroll ||
       focused_link != old_focus || pressed)
        redraw_needed = 1;
    return 0;
}

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

#define DPAD_MASK (CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_DPAD_LEFT | CONT_DPAD_RIGHT)

/* D-pad directions to act on this frame: a new press at once, then
   keyboard-style auto-repeat while it is held. */
static uint32_t dpad_moves(uint32_t buttons, uint32_t pressed) {
    static uint64_t next_repeat;
    uint64_t now = timer_ms_gettime64();
    if(pressed & DPAD_MASK) {
        next_repeat = now + 350;
        return pressed & DPAD_MASK;
    }
    if((buttons & DPAD_MASK) && now >= next_repeat) {
        next_repeat = now + 90;
        return buttons & DPAD_MASK;
    }
    return 0;
}

static void process_osk_controller(uint32_t pressed, uint32_t moves,
                                   int left_trigger, int right_trigger) {
    if(moves & CONT_DPAD_UP) osk_move(&osk_row, &osk_column, -1, 0);
    if(moves & CONT_DPAD_DOWN) osk_move(&osk_row, &osk_column, 1, 0);
    if(moves & CONT_DPAD_LEFT) osk_move(&osk_row, &osk_column, 0, -1);
    if(moves & CONT_DPAD_RIGHT) osk_move(&osk_row, &osk_column, 0, 1);
    if(pressed & CONT_A) osk_activate();
    if(!osk_open) return;
    if(pressed & CONT_B) osk_type(KBD_KEY_BACKSPACE, 0);
    if(pressed & CONT_X) osk_type(KBD_KEY_NONE, ' ');
    if(pressed & CONT_Y) osk_shift = !osk_shift;
    if(left_trigger) osk_type(KBD_KEY_LEFT, 0);
    if(right_trigger) osk_type(KBD_KEY_RIGHT, 0);
    if(pressed & CONT_START) osk_finish(1);
}

static void process_select_controller(uint32_t pressed, uint32_t moves) {
    browser_field_t *field = &document.fields[editing_field];
    if(moves & (CONT_DPAD_UP | CONT_DPAD_LEFT)) process_select_key(field, KBD_KEY_UP, 0);
    if(moves & (CONT_DPAD_DOWN | CONT_DPAD_RIGHT)) process_select_key(field, KBD_KEY_DOWN, 0);
    if(pressed & (CONT_A | CONT_START)) {
        finish_field_edit(0);
        snprintf(status_text, sizeof(status_text), "Choice saved");
    } else if(pressed & CONT_B) {
        finish_field_edit(1);
        snprintf(status_text, sizeof(status_text), "Choice undone");
    }
}

static void process_controller(maple_device_t *controller) {
    static uint32_t previous_buttons;
    static int previous_ltrig;
    static int previous_rtrig;
    cont_state_t *state;
    uint32_t pressed;
    uint32_t moves;
    int left_trigger;
    int right_trigger;

    if(!controller || !(state = maple_dev_status(controller))) return;
    pressed = state->buttons & ~previous_buttons;
    previous_buttons = state->buttons;
    moves = dpad_moves(state->buttons, pressed);
    left_trigger = state->ltrig > 64 && previous_ltrig <= 64;
    right_trigger = state->rtrig > 64 && previous_rtrig <= 64;
    previous_ltrig = state->ltrig;
    previous_rtrig = state->rtrig;
    if(pressed || left_trigger || right_trigger || (state->buttons & DPAD_MASK))
        redraw_needed = 1;
    if(pressed || left_trigger || right_trigger) disarm_exit();

    if(show_help) {
        if(pressed) show_help = 0;
        return;
    }
    if(osk_open) {
        process_osk_controller(pressed, moves, left_trigger, right_trigger);
        return;
    }
    if(editing_field >= 0 && !strcmp(document.fields[editing_field].type, "select")) {
        process_select_controller(pressed, moves);
        return;
    }
    if(editing || editing_field >= 0) {
        /* Editing began from the keyboard or mouse: any button brings up
           the on-screen keyboard instead of acting on the page. */
        if(pressed || left_trigger || right_trigger) open_osk();
        return;
    }
    if(pressed & CONT_START) toggle_bookmarks_page();
    if(pressed & CONT_X) begin_address_edit(1);
    if(pressed & CONT_B) navigate_back();
    if((pressed & CONT_A) && focused_link >= 0) follow_link(focused_link, 1);
    if(pressed & CONT_Y) focus_step(1);
    if(left_trigger) navigate_back();
    if(right_trigger) navigate_forward();
    if(state->buttons & CONT_DPAD_DOWN) scroll_y += 14;
    if(state->buttons & CONT_DPAD_UP) scroll_y -= 14;
    if(state->buttons & CONT_DPAD_RIGHT) scroll_y += 48;
    if(state->buttons & CONT_DPAD_LEFT) scroll_y -= 48;
    clamp_scroll();
}

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
       network_post("https://httpbin.org/post", body, MAX_DOCUMENT_BYTES, &result) < 0) {
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
    snprintf(page->items[3].text, sizeof(page->items[3].text), "Changed text");
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
    frame_clock_init();
    storage_load_bookmarks(&bookmarks);
    document_make_error(&document, "Dreamcast Browser",
        "Starting network. With a keyboard, F6 opens the address bar and F1 lists "
        "every shortcut. With a controller, X opens the address bar and Start "
        "opens bookmarks and the menu.");
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
    run_mouse_self_test();
    run_ui_self_test();
    run_bookmark_self_test();
    run_live_form_self_test();
    run_vmu_self_test();
#endif

#ifdef BROWSER_PERF_SELF_TEST
    run_keyboard_self_test();
    run_mouse_self_test();
#endif
#if defined(BROWSER_HISTORY_SELF_TEST) || defined(BROWSER_PERF_SELF_TEST)
    run_render_benchmark();
    run_render_self_test();
#endif

#ifdef BROWSER_FORM_SELF_TEST
    run_form_self_test();
#endif

#ifdef BROWSER_PROFILE
    /* Startup, network loads, and the synthetic tests aren't interactive frames. */
    prof_iters = prof_frames = prof_keys = prof_draw_max = prof_iter_max = 0;
    prof_input_us = prof_draw_us = prof_present_us = prof_wait_us = 0;
    prof_poll_us = prof_wake_us = prof_wake_max = 0;
    prof_report_at = 0;
#endif
    while(!quit) {
#ifdef BROWSER_PROFILE
        uint64_t iter_start = timer_us_gettime64();
#endif
        {
            PROF_BEGIN;
            wait_for_vblank();
            PROF_ADD(prof_wait_us);
#ifdef BROWSER_PROFILE
            {
                unsigned wake = (unsigned)((uint32_t)timer_us_gettime64() - prof_vblank_at);
                prof_wake_us += wake;
                if(wake > prof_wake_max) prof_wake_max = wake;
            }
#endif
        }
        {
            PROF_BEGIN;
            wait_for_maple_poll();
            PROF_ADD(prof_poll_us);
        }
        {
            PROF_BEGIN;
            keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
            mouse = maple_enum_type(0, MAPLE_FUNC_MOUSE);
            controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
            quit |= process_keyboard(keyboard);
            process_mouse(mouse);
            process_controller(controller);
            PROF_ADD(prof_input_us);
        }
        quit |= quit_requested;
        if(redraw_needed) {
            redraw_needed = 0;
            compose();
            present();
        }
#ifdef BROWSER_PROFILE
        {
            uint64_t now = timer_us_gettime64();
            unsigned iter = (unsigned)(now - iter_start);
            prof_iters++;
            if(iter > prof_iter_max) prof_iter_max = iter;
            if(!prof_report_at) prof_report_at = now + 2000000;
            if(now >= prof_report_at) {
                printf("PROF iters %u frames %u keys %u | wait %lu us/it poll %lu us/it wake %lu us/it (max %u) input %lu us/it draw %lu us/frame (max %u) present %lu us/frame | iter max %u us\n",
                       prof_iters, prof_frames, prof_keys,
                       (unsigned long)(prof_wait_us / (prof_iters ? prof_iters : 1)),
                       (unsigned long)(prof_poll_us / (prof_iters ? prof_iters : 1)),
                       (unsigned long)(prof_wake_us / (prof_iters ? prof_iters : 1)), prof_wake_max,
                       (unsigned long)(prof_input_us / (prof_iters ? prof_iters : 1)),
                       (unsigned long)(prof_draw_us / (prof_frames ? prof_frames : 1)), prof_draw_max,
                       (unsigned long)(prof_present_us / (prof_frames ? prof_frames : 1)),
                       prof_iter_max);
                printf("PROF state editing %d address_length %u\n", editing, (unsigned)strlen(address));
                prof_iters = prof_frames = prof_keys = prof_draw_max = prof_iter_max = 0;
                prof_input_us = prof_draw_us = prof_present_us = prof_wait_us = 0;
                prof_poll_us = prof_wake_us = prof_wake_max = 0;
                prof_report_at = now + 2000000;
            }
        }
#endif
    }

    frame_clock_shutdown();
    document_free(&document);
    network_shutdown();
    printf("browser: clean shutdown\n");
    return 0;
}
