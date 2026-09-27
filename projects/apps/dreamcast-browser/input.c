/* Input: the keyboard, mouse and controller, the on-screen keyboard, text
   editing in the address bar and in form fields, and moving the page and
   the focus. */

#include "app.h"

#include <arch/timer.h>
#include <ctype.h>
#include <kos/irq.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* While the on-screen keyboard covers the bottom of the page, allow
   scrolling far enough to lift the last rows above it. */
int max_scroll(void) {
    int visible = SCREEN_H - page_top() - (osk_open ? SCREEN_H - OSK_TOP : 0);
    return document.height > visible ? document.height - visible : 0;
}

void clamp_scroll(void) {
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

int page_step(void) {
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

int link_on_screen(int link) {
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

void close_osk(void) {
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

void open_osk(void) {
    osk_open = 1;
    osk_shift = 0;
    snprintf(status_text, sizeof(status_text), "A types, B deletes, Start finishes");
    reveal_editing_field();
    redraw_needed = 1;
}

void begin_address_edit(int want_osk) {
    editing = 1;
    focused_link = -1;
    show_help = 0;
    address_caret = (int)strlen(address);
    address_selected = 1;
    snprintf(status_text, sizeof(status_text), "Type a URL or search: Enter opens");
    if(want_osk) open_osk();
    redraw_needed = 1;
}

void cancel_address_edit(void) {
    editing = 0;
    address_selected = 0;
    close_osk();
    snprintf(address, sizeof(address), "%s", current_url);
    snprintf(status_text, sizeof(status_text), "%s", document.title);
    redraw_needed = 1;
}

void finish_field_edit(int restore) {
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
    if((mods.raw & KBD_MOD_CTRL) &&
       (key == KBD_KEY_ENTER || key == KBD_KEY_PAD_ENTER)) {
        int submit = editing_field;
        for(int i = 0; i < document.field_count; ++i) {
            if(document.fields[i].form == field->form &&
               !document.fields[i].disabled && document_field_is_submit(&document.fields[i])) {
                submit = i;
                break;
            }
        }
        finish_field_edit(0);
        submit_form(submit);
        return;
    }
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
int handle_key(kbd_key_t key, kbd_mods_t mods, char ascii) {
    int ctrl = (mods.raw & KBD_MOD_CTRL) != 0;
    int alt = (mods.raw & KBD_MOD_ALT) != 0;
    int shift = (mods.raw & KBD_MOD_SHIFT) != 0;

    redraw_needed = 1;

    if(loading_label && key == KBD_KEY_ESCAPE && !editing && editing_field < 0) {
        loading_cancelled = 1;
        return 0;
    }
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
    else if(key == KBD_KEY_F7)
        toggle_reader();
    else if(key == KBD_KEY_F4)
        load_page_images();
    else if(key == KBD_KEY_F5 || (key == KBD_KEY_R && ctrl))
        reload_page();
    else if(key == KBD_KEY_TAB) {
        pointer_active = 0;
        focus_step(shift ? -1 : 1);
    }
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

int process_keyboard(maple_device_t *keyboard) {
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
void osk_type(kbd_key_t key, char ascii) {
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

void osk_activate(void) {
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

/* A press at the pointer, from the mouse button or the controller's A.
   Without a keyboard, text entry needs the on-screen keyboard. */
static void pointer_click(int want_osk) {
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

int process_mouse(maple_device_t *mouse) {
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
    if(mouse_x != old_x || mouse_y != old_y || state->dz) {
        focused_link = link_at(mouse_x, mouse_y);
        pointer_active = 1;
    }
    pressed = state->buttons & ~previous_buttons;
    previous_buttons = state->buttons;
    if(pressed) disarm_exit();
    if(show_help && pressed) {
        show_help = 0;
        redraw_needed = 1;
        return 0;
    }
    if(loading_label && (pressed & MOUSE_RIGHTBUTTON)) loading_cancelled = 1;
    if(pressed & MOUSE_LEFTBUTTON) {
        pointer_active = 1;
        pointer_click(!keyboard_attached());
    }
    if(mouse_x != old_x || mouse_y != old_y || scroll_y != old_scroll ||
       focused_link != old_focus || pressed)
        redraw_needed = 1;
    return 0;
}

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

/* The analog stick steers the pointer the mouse would. Speed grows with
   the square of the push: a nudge moves a pixel at a time for small links,
   a full push crosses the screen in about a second. */

static int stick_speed(int value) {
    int push = value < 0 ? -value : value;
    int speed;
    if(push < STICK_DEAD_ZONE) return 0;
    push -= STICK_DEAD_ZONE;
    speed = 96 + push * push / 5; /* 1/256 pixel per frame */
    return value < 0 ? -speed : speed;
}

/* Returns nonzero when the pointer or the page moved. Holding the pointer
   against the top or bottom of the page scrolls it. */
static int move_pointer_with_stick(const cont_state_t *state) {
    static int carry_x, carry_y;
    int speed_x = stick_speed(state->joyx);
    int speed_y = stick_speed(state->joyy);
    int old_x = mouse_x, old_y = mouse_y, old_scroll = scroll_y;
    int top = page_top();
    int bottom = (osk_open ? OSK_TOP : SCREEN_H) - 1;
    int step_x, step_y;

    if(!speed_x) carry_x = 0;
    if(!speed_y) carry_y = 0;
    if(!speed_x && !speed_y) return 0;
    carry_x += speed_x;
    carry_y += speed_y;
    step_x = carry_x / 256;
    step_y = carry_y / 256;
    carry_x -= step_x * 256;
    carry_y -= step_y * 256;

    mouse_x += step_x;
    if(mouse_x < 0) mouse_x = 0;
    if(mouse_x >= SCREEN_W) mouse_x = SCREEN_W - 1;
    if(step_y > 0 && mouse_y + step_y > bottom - STICK_EDGE && mouse_y <= bottom &&
       !editing && editing_field < 0 && scroll_y < max_scroll()) {
        /* The rest of the push moves the page instead of the pointer. */
        int room = bottom - STICK_EDGE - mouse_y;
        if(room < 0) room = 0;
        mouse_y += room;
        scroll_y += step_y - room;
    } else if(step_y < 0 && mouse_y + step_y < top + STICK_EDGE && mouse_y >= top &&
              !editing && editing_field < 0 && scroll_y > 0) {
        int room = mouse_y - (top + STICK_EDGE);
        if(room < 0) room = 0;
        mouse_y -= room;
        scroll_y += step_y + room;
    } else {
        mouse_y += step_y;
    }
    if(mouse_y < 0) mouse_y = 0;
    if(mouse_y >= SCREEN_H) mouse_y = SCREEN_H - 1;
    clamp_scroll();
    if(mouse_x == old_x && mouse_y == old_y && scroll_y == old_scroll) return 0;

    pointer_active = 1;
    if(osk_open && mouse_y >= OSK_TOP) {
        int row, column;
        /* Pointing at a key chooses it; A then types it. */
        if(osk_hit(mouse_x, mouse_y, &row, &column)) {
            osk_row = row;
            osk_column = column;
        }
    } else {
        focused_link = link_at(mouse_x, mouse_y);
    }
    return 1;
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

void process_controller(maple_device_t *controller) {
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
    if(move_pointer_with_stick(state)) redraw_needed = 1;
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
    if(loading_label && (pressed & CONT_B)) {
        loading_cancelled = 1;
        pressed &= ~CONT_B;
    }
    if(state->buttons & CONT_Y) {
        if(left_trigger) { toggle_reader(); return; }
        if(right_trigger) { load_page_images(); return; }
    }
    if(pressed & CONT_START) toggle_bookmarks_page();
    if(pressed & CONT_X) begin_address_edit(1);
    if(pressed & CONT_B) navigate_back();
    if(pressed & CONT_A) {
        /* A presses what the pointer is on, toolbar buttons included; after
           Y has chosen a link, it opens that link wherever the pointer is. */
        if(pointer_active) pointer_click(1);
        else if(focused_link >= 0) follow_link(focused_link, 1);
    }
    if(pressed & CONT_Y) {
        pointer_active = 0;
        focus_step(1);
    }
    if(left_trigger) navigate_back();
    if(right_trigger) navigate_forward();
    if(state->buttons & CONT_DPAD_DOWN) scroll_y += 14;
    if(state->buttons & CONT_DPAD_UP) scroll_y -= 14;
    if(state->buttons & CONT_DPAD_RIGHT) scroll_y += 48;
    if(state->buttons & CONT_DPAD_LEFT) scroll_y -= 48;
    clamp_scroll();
}
