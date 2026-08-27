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

static browser_document_t document;
static char address[MAX_URL] = "https://appsbyrich.com/";
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

typedef struct {
    char url[MAX_URL];
    int scroll_y;
} history_entry_t;

static history_entry_t history[MAX_HISTORY];
static int history_count;

static int show_transfer_progress(uint64_t received, uint64_t total,
                                  void *userdata) {
    static const char spinner[] = "|/-\\";
    uint64_t now = timer_ms_gettime64();
    (void)userdata;

    if(last_progress_draw && now - last_progress_draw < 200) return 0;
    last_progress_draw = now;
    if(total)
        snprintf(status_text, sizeof(status_text),
                 "Loading %s %lu/%lu KiB %c",
                 loading_label ? loading_label : "data",
                 (unsigned long)(received / 1024),
                 (unsigned long)((total + 1023) / 1024),
                 spinner[progress_frame++ & 3]);
    else
        snprintf(status_text, sizeof(status_text),
                 "Connecting %s %c",
                 loading_label ? loading_label : "",
                 spinner[progress_frame++ & 3]);
    render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                   address, editing, history_count > 0, status_text);
    return 0;
}

static void begin_loading(const char *label) {
    loading_label = label;
    last_progress_draw = 0;
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

static void history_push_current(void) {
    history_entry_t *entry;

    if(!current_url[0]) return;
    if(history_count && !strcmp(history[history_count - 1].url, current_url)) {
        history[history_count - 1].scroll_y = scroll_y;
        return;
    }
    if(history_count == MAX_HISTORY) {
        memmove(&history[0], &history[1],
                sizeof(history[0]) * (MAX_HISTORY - 1));
        history_count--;
    }
    entry = &history[history_count++];
    snprintf(entry->url, sizeof(entry->url), "%s", current_url);
    entry->scroll_y = scroll_y;
    printf("browser: history saved %s (%d/%d)\n",
           entry->url, history_count, MAX_HISTORY);
}

static int load_page(const char *requested) {
    fetch_result_t result;
    char target[MAX_URL];
    char message[256];
    long response_status;

    snprintf(target, sizeof(target), "%s", requested);
    normalize_address(target, sizeof(target));
    snprintf(current_url, sizeof(current_url), "%s", target);
    snprintf(address, sizeof(address), "%s", target);
    snprintf(status_text, sizeof(status_text), "Connecting page...");
    render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                   address, 0, history_count > 0, status_text);
    redraw_needed = 0;
    begin_loading("page");

    if(network_fetch(target, MAX_DOCUMENT_BYTES, &result) < 0) {
        end_loading();
        snprintf(message, sizeof(message), "Could not load this address: %s", result.error);
        document_free(&document);
        document_make_error(&document, "Page load failed", message);
        snprintf(status_text, sizeof(status_text), "Network error");
        redraw_needed = 1;
        return -1;
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
        return -1;
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
        return -1;
    }

    document_free(&document);
    end_loading();
    document_init(&document, result.effective_url);
    document_parse_html(&document, (const char *)result.data, result.size);
    if(result.truncated) document.truncated = 1;
    snprintf(address, sizeof(address), "%s", result.effective_url);
    snprintf(current_url, sizeof(current_url), "%s", result.effective_url);
    fetch_result_free(&result);
    scroll_y = 0;
    focused_link = -1;
    snprintf(status_text, sizeof(status_text), "Page ready; loading images...");
    render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                   address, 0, history_count > 0, status_text);
    redraw_needed = 0;

    begin_loading("image");
    document_load_images(&document);
    end_loading();
    snprintf(status_text, sizeof(status_text), "%.82s%s", document.title,
             document.truncated ? " [shortened]" : "");
    redraw_needed = 1;
    return 0;
}

static void navigate_to(const char *requested) {
    char target[MAX_URL];

    snprintf(target, sizeof(target), "%s", requested);
    history_push_current();
    load_page(target);
}

static void navigate_back(void) {
    history_entry_t entry;

    if(!history_count) {
        snprintf(status_text, sizeof(status_text), "No previous page");
        redraw_needed = 1;
        return;
    }
    entry = history[--history_count];
    printf("browser: back -> %s (%d remaining)\n", entry.url, history_count);
    if(load_page(entry.url) == 0) {
        scroll_y = entry.scroll_y;
        clamp_scroll();
        redraw_needed = 1;
    }
}

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
        else if(key == KBD_KEY_BACKSPACE ||
                (key == KBD_KEY_LEFT && (mods.raw & KBD_MOD_ALT)))
            navigate_back();
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
    if(pressed & CONT_B) navigate_back();
    if(pressed & CONT_A) {
        if(editing) { editing = 0; navigate_to(address); }
        else if(focused_link >= 0) follow_link(focused_link);
    }
    if(pressed & CONT_Y) focus_next_link();
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
    render_browser(&document, 0, mouse_x, mouse_y, -1, address, 0, status_text);

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
        load_page(address);
    }

    while(!quit) {
        keyboard = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
        mouse = maple_enum_type(0, MAPLE_FUNC_MOUSE);
        controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        quit |= process_keyboard(keyboard);
        process_mouse(mouse);
        quit |= process_controller(controller);
        network_idle_poll();
        if(redraw_needed) {
            render_browser(&document, scroll_y, mouse_x, mouse_y, focused_link,
                           address, editing, history_count > 0, status_text);
            redraw_needed = 0;
        }
        thd_sleep(16);
    }

    document_free(&document);
    network_shutdown();
    printf("browser: clean shutdown\n");
    return 0;
}
