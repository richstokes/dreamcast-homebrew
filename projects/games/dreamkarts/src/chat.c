/* DreamKarts lobby chat.

   Closed, the chat is a strip along the foot of the lobby room showing the
   newest line, with a pulsing NEW badge counting the lines that arrived
   since the box was last open, so nothing is missed while the race is
   being set up. X opens the box: the last few lines, the line being typed
   and rows of on-screen keys (letters, digits, a little punctuation and
   some quick phrases) worked with the d-pad and A. A Dreamcast keyboard on
   any port types straight in as well, and a key pressed while the box is
   closed opens it. Enter or Start sends; the
   line goes out through net_say() and shows once the server echoes it. A
   watcher gets the strip and the box, but no typing. */
#include "chat.h"
#include "audio.h"
#include "hud.h"
#include "kart.h"
#include "net.h"
#include "render.h"
#include <string.h>

/* The box over the lobby room. */
#define BOX_X0 24
#define BOX_X1 616
#define BOX_Y0 176
#define BOX_Y1 436
#define LINE_H 20
#define LINE_SCALE 0.42f
/* The strip below the lobby panels. */
#define STRIP_Y0 404
#define STRIP_Y1 432

typedef struct {
    const char *label, *text;   /* text: what pressing it types ("" for DEL/SEND) */
    float x, w;
} Key;

#define KEY_ROWS 4
#define KEYS_PER_ROW 13
static Key keys[KEY_ROWS][KEYS_PER_ROW];
static int key_count[KEY_ROWS];
static const char *const letter_rows[3] = {"ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "0123456789!?'"};
static const Key action_row[] = {
    {"SPACE", " ", 0, 72}, {"DEL", "", 0, 56}, {"SEND", "", 0, 64},
    {"GG", "GG", 0, 44}, {"HI!", "HI!", 0, 48}, {"READY", "READY", 0, 72}, {"NICE", "NICE", 0, 56},
    {"LOL", "LOL", 0, 48}, {"WAIT", "WAIT", 0, 56}
};
static char letter_labels[3][KEYS_PER_ROW][2];

static bool open_box;
static int row, col;
static char line[NET_CHAT_LEN];
static int seen;            /* net_chat_count() when the box was last open */
static int heard;           /* net_chat_count() last frame, for the new-line sound */
static float fail_t;        /* seconds left showing "not sent" */
static float t_open;        /* seconds since the box opened */
static const pvr_poly_hdr_t *h_flat;
#ifdef DK_AUTOTEST
static uint32_t at_press;
#endif

static void build_keys(void) {
    for(int r = 0; r < 3; r++) {
        for(int c = 0; c < KEYS_PER_ROW; c++) {
            letter_labels[r][c][0] = letter_rows[r][c];
            letter_labels[r][c][1] = '\0';
            keys[r][c] = (Key){letter_labels[r][c], letter_labels[r][c], 36 + c * 44, 40};
        }
        key_count[r] = KEYS_PER_ROW;
    }
    float x = 36;
    int n = (int)(sizeof(action_row) / sizeof(action_row[0]));
    for(int c = 0; c < n; c++) {
        keys[3][c] = action_row[c];
        keys[3][c].x = x;
        x += action_row[c].w + 4;
    }
    key_count[3] = n;
}

void chat_enter_lobby(void) {
    if(!h_flat) {
        h_flat = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
        build_keys();
    }
    open_box = false;
    row = col = 0;
    line[0] = '\0';
    seen = heard = 0;
    fail_t = 0;
}

bool chat_open(void) { return open_box; }

static void set_open(bool on) {
    open_box = on;
    t_open = 0;
    if(on) seen = net_chat_count();
    audio_sfx(on ? DK_SFX_MENU_SELECT : DK_SFX_MENU_BACK, 170, 128);
}

static void type_text(const char *t) {
    int len = (int)strlen(line);
    /* A phrase after a word gets a space first; no space to start or twice. */
    if(t[0] == ' ' && (len == 0 || line[len - 1] == ' ')) return;
    if(t[1] && t[0] != ' ' && len > 0 && line[len - 1] != ' ' && len < NET_CHAT_LEN - 1) line[len++] = ' ';
    for(const char *c = t; *c && len < NET_CHAT_LEN - 1; c++) line[len++] = *c;
    line[len] = '\0';
    audio_sfx(DK_SFX_MENU_MOVE, 200, 128);
}

static void delete_char(void) {
    int len = (int)strlen(line);
    if(len > 0) {
        line[len - 1] = '\0';
        audio_sfx(DK_SFX_MENU_BACK, 160, 128);
    }
}

static void send_line(void) {
    /* Trailing spaces go; an empty line is nothing to say. */
    int len = (int)strlen(line);
    while(len > 0 && line[len - 1] == ' ') line[--len] = '\0';
    if(len == 0) return;
    if(net_say(line)) {
        line[0] = '\0';
        audio_sfx(DK_SFX_MENU_SELECT, 230, 128);
    }
    else {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);   /* the last one is still on its way */
    }
}

/* Up or down a row lands on the key under this one. */
static void move_row(int dir) {
    float cx = keys[row][col].x + keys[row][col].w * 0.5f;
    row = (row + dir + KEY_ROWS) % KEY_ROWS;
    int best = 0;
    float best_d = 1e9f;
    for(int c = 0; c < key_count[row]; c++) {
        float d = fabsf(keys[row][c].x + keys[row][c].w * 0.5f - cx);
        if(d < best_d) {
            best_d = d;
            best = c;
        }
    }
    col = best;
}

static void press_key(void) {
    const Key *k = &keys[row][col];
    if(strcmp(k->label, "DEL") == 0) delete_char();
    else if(strcmp(k->label, "SEND") == 0) send_line();
    else type_text(k->text);
}

static void keyboard_input(bool can_say) {
    const uint8_t *k;
    int n = input_keys(&k);
    for(int i = 0; i < n; i++) {
        if(k[i] == KEY_ESCAPE) {
            if(open_box) set_open(false);
        }
        else if(!can_say) {
            continue;
        }
        else if(k[i] == KEY_ENTER) {
            if(open_box) send_line();
        }
        else if(k[i] == KEY_BACKSPACE) {
            if(open_box) delete_char();
        }
        else if(k[i] >= 32) {
            if(!open_box) set_open(true);
            char t[2] = {(char)k[i], '\0'};
            type_text(t);
        }
    }
}

bool chat_update(float dt, bool can_say) {
#ifdef DK_AUTOTEST
    pad.pressed |= at_press;
    at_press = 0;
#endif
    bool was_open = open_box;
    t_open += dt;
    if(fail_t > 0) fail_t -= dt;
    /* The sound of a new line: the sender's Chao, or a click for our own. */
    int count = net_chat_count();
    if(count > heard) {
        const NetChat *c = net_chat(count - 1);
        if(c && c->slot == net_lobby()->you) audio_sfx(DK_SFX_MENU_SELECT, 120, 128);
        else if(c) audio_chao(CHAO_HI, c->colour % COLOUR_COUNT, 150, 128);
        heard = count;
    }
    if(net_say_state() == NET_SAY_FAILED && fail_t <= 0 && !line[0]) fail_t = 2.5f;
    keyboard_input(can_say);
    if(pad.pressed & BTN_X) {
        set_open(!open_box);
        return true;
    }
    if(!open_box) return was_open;
    seen = count;
    if(!can_say) {
        if(pad.pressed & BTN_B) set_open(false);
        return true;
    }
    bool moved = false;
    if(input_menu_up()) { move_row(-1); moved = true; }
    if(input_menu_down()) { move_row(1); moved = true; }
    if(input_menu_left()) { col = (col + key_count[row] - 1) % key_count[row]; moved = true; }
    if(input_menu_right()) { col = (col + 1) % key_count[row]; moved = true; }
    if(moved) audio_sfx(DK_SFX_MENU_MOVE, 150, 128);
    if(pad.pressed & BTN_A) press_key();
    if(pad.pressed & BTN_START) send_line();
    if(pad.pressed & BTN_B) {
        if(line[0]) delete_char();
        else set_open(false);
    }
    return true;
}

const char *chat_hint(void) {
    if(!open_box) return NULL;
    if(input_keyboard()) return "TYPE, OR A: KEY   ENTER/START: SEND   ESC/X: CLOSE";
    return "A: TYPE   B: DELETE   START: SEND   X: CLOSE";
}

/* "NAME: TEXT", the name in the sender's colour, shrunk to fit max_w. */
static void draw_line(const NetChat *c, float x, float y, float scale, float max_w, bool me) {
    char name[NET_NAME_LEN + 2];
    snprintf(name, sizeof(name), "%s:", c->name);
    float w = hud_text_width(name, scale) + hud_text_width(" ", scale) + hud_text_width(c->text, scale);
    if(w > max_w) {
        float fit = scale * max_w / w;
        y += (scale - fit) * 21.0f;
        scale = fit;
    }
    uint32_t col = me ? argb(255, 255, 255, 140) : c->slot == NET_SPECTATOR ? argb(255, 200, 225, 255)
                                                                             : (kart_colours[c->colour % COLOUR_COUNT].ui | 0xff000000);
    hud_text(x, y, scale, ALIGN_LEFT, col, name);
    hud_text(x + hud_text_width(name, scale) + hud_text_width(" ", scale), y, scale, ALIGN_LEFT, 0xffffffff, c->text);
}

static void draw_strip(float t) {
    hud_panel(BOX_X0, STRIP_Y0, BOX_X1, STRIP_Y1, argb(225, 30, 100, 220), argb(225, 12, 36, 120), 15);
    int count = net_chat_count();
    hud_text(BOX_X0 + 12, STRIP_Y0 + 5, 0.4f, ALIGN_LEFT, argb(255, 255, 220, 60), "CHAT");
    float x = BOX_X0 + 12 + hud_text_width("CHAT", 0.4f) + 14;
    int unread = count - seen;
    float right = BOX_X1 - 12;
    if(unread > 0) {
        char badge[16];
        snprintf(badge, sizeof(badge), "%d NEW", unread);
        /* White lettering (the font's own outline keeps it crisp) on a red
           pill that breathes. */
        float bw = hud_text_width(badge, 0.46f) + 18;
        float pulse = 0.85f + 0.15f * sinf(t * 5.0f);
        r_hdr(h_flat);
        r_quad2d(right - bw, STRIP_Y0 + 3, right, STRIP_Y1 - 3, 0, 0, 1, 1, color_alpha(argb(255, 232, 64, 40), pulse), 16.0f);
        hud_text(right - bw * 0.5f, STRIP_Y0 + 4, 0.46f, ALIGN_CENTER, 0xffffffff, badge);
        right -= bw + 12;
    }
    const NetChat *c = net_chat(count - 1);
    if(c) draw_line(c, x, STRIP_Y0 + 5, 0.42f, right - x, c->slot == net_lobby()->you);
}

static void draw_key(const Key *k, float y, bool sel) {
    r_hdr(h_flat);
    r_quad2d(k->x, y, k->x + k->w, y + 22, 0, 0, 1, 1, sel ? argb(255, 255, 210, 60) : argb(200, 20, 50, 130), 16.1f);
    const char *label = k->label[0] == ' ' ? "_" : k->label;
    hud_text(k->x + k->w * 0.5f, y + 2, 0.42f, ALIGN_CENTER, sel ? argb(255, 20, 30, 80) : 0xffffffff, label);
}

static void draw_box(float t, bool can_say) {
    hud_panel(BOX_X0, BOX_Y0, BOX_X1, BOX_Y1, argb(255, 24, 70, 170), argb(255, 8, 22, 76), 15.2f);
    hud_text(BOX_X0 + 12, BOX_Y0 + 4, 0.45f, ALIGN_LEFT, argb(255, 255, 220, 60), "LOBBY CHAT");
    hud_text(BOX_X1 - 12, BOX_Y0 + 6, 0.4f, ALIGN_RIGHT, argb(220, 200, 225, 255), "X: CLOSE");
    int lines = can_say ? 5 : 11;
    float y0 = BOX_Y0 + 28;
    int count = net_chat_count(), me = net_lobby()->you;
    int first = count - lines;
    if(first < count - NET_CHAT_KEEP) first = count - NET_CHAT_KEEP;
    if(first < 0) first = 0;
    /* Newest at the bottom, just above the typing line. */
    float y = y0 + (lines - (count - first)) * LINE_H;
    for(int i = first; i < count; i++, y += LINE_H) {
        const NetChat *c = net_chat(i);
        if(c) draw_line(c, BOX_X0 + 16, y, LINE_SCALE, BOX_X1 - BOX_X0 - 32, c->slot == me);
    }
    if(!can_say) return;
    float iy = y0 + lines * LINE_H + 4;
    r_hdr(h_flat);
    r_quad2d(BOX_X0 + 8, iy, BOX_X1 - 8, iy + 26, 0, 0, 1, 1, argb(230, 8, 20, 60), 16.0f);
    char shown[NET_CHAT_LEN + 3];
    bool cursor = ((int)(t * 3) & 1) && strlen(line) < NET_CHAT_LEN - 1;
    snprintf(shown, sizeof(shown), "> %s%s", line, cursor ? "_" : "");
    hud_text(BOX_X0 + 16, iy + 3, 0.5f, ALIGN_LEFT, 0xffffffff, shown);
    int say = net_say_state();
    if(say == NET_SAY_SENDING)
        hud_text(BOX_X1 - 16, iy + 5, 0.4f, ALIGN_RIGHT, argb(220, 200, 225, 255), "SENDING...");
    else if(fail_t > 0)
        hud_text(BOX_X1 - 16, iy + 5, 0.4f, ALIGN_RIGHT, argb(255, 255, 160, 120), "NOT SENT - TRY AGAIN");
    for(int r = 0; r < KEY_ROWS; r++)
        for(int c = 0; c < key_count[r]; c++) draw_key(&keys[r][c], iy + 34 + r * 24, r == row && c == col);
}

void chat_draw(float t) {
    if(!h_flat) return;
    if(open_box) draw_box(t, !net_spectating());
    else draw_strip(t);
}

#ifdef DK_AUTOTEST
void chat_at_press(uint32_t btns) {
    at_press |= btns;
}

bool chat_at_pick(const char *label) {
    for(int r = 0; r < KEY_ROWS; r++)
        for(int c = 0; c < key_count[r]; c++)
            if(strcmp(keys[r][c].label, label) == 0) {
                row = r;
                col = c;
                return true;
            }
    return false;
}
#endif
