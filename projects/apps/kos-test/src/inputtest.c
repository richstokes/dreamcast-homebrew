/* Input test: one panel per maple port showing whatever is plugged in, live.
   Controllers draw their buttons, triggers and sticks; keyboards their keys,
   modifiers and typed text; mice their buttons and movement, plus a cursor
   on screen. Sub-units (VMUs, jump packs, microphones) are listed too. */
#include "gfx.h"
#include "kt.h"

#include <dc/maple/keyboard.h>
#include <dc/maple/mouse.h>
#include <dc/maple/purupuru.h>
#include <dc/maple/vmu.h>
#include <stdio.h>
#include <string.h>

/* KOS before KallistiOS#1533 had no name for it (and masked it out) */
#ifndef MOUSE_MIDDLEBUTTON
#define MOUSE_MIDDLEBUTTON (1 << 0)
#endif

#define TEXT_MAX 34

typedef struct {
    char text[TEXT_MAX + 1];
    int mouse_x, mouse_y, wheel;
    uint32_t presses;       /* controller button presses counted */
    uint32_t last_buttons;
    uint64_t beep_until;
} port_state_t;

static port_state_t ports[MAPLE_PORT_COUNT];
static int cursor_x = SCREEN_W / 2, cursor_y = SCREEN_H / 2;
static bool any_mouse;

static const char *key_name(int k) {
    static char buf[8];
    static const char *const named[] = {
        [0x28] = "ENTER", [0x29] = "ESC", [0x2a] = "BKSP", [0x2b] = "TAB",
        [0x2c] = "SPACE", [0x2d] = "-", [0x2e] = "=", [0x2f] = "[", [0x30] = "]",
        [0x31] = "\\", [0x32] = "#", [0x33] = ";", [0x34] = "'", [0x35] = "`",
        [0x36] = ",", [0x37] = ".", [0x38] = "/", [0x39] = "CAPS",
        [0x46] = "PRTSC", [0x47] = "SCRLK", [0x48] = "PAUSE", [0x49] = "INS",
        [0x4a] = "HOME", [0x4b] = "PGUP", [0x4c] = "DEL", [0x4d] = "END",
        [0x4e] = "PGDN", [0x4f] = "RIGHT", [0x50] = "LEFT", [0x51] = "DOWN",
        [0x52] = "UP", [0x53] = "NUMLK", [0x54] = "KP/", [0x55] = "KP*",
        [0x56] = "KP-", [0x57] = "KP+", [0x58] = "KPENT", [0x63] = "KP.",
        [0x64] = "\\|", [0x65] = "S3",
    };
    if(k >= KBD_KEY_A && k <= KBD_KEY_Z)
        snprintf(buf, sizeof(buf), "%c", 'A' + k - KBD_KEY_A);
    else if(k >= 0x1e && k <= 0x27)
        snprintf(buf, sizeof(buf), "%c", k == 0x27 ? '0' : '1' + k - 0x1e);
    else if(k >= 0x3a && k <= 0x45)
        snprintf(buf, sizeof(buf), "F%d", k - 0x3a + 1);
    else if(k >= 0x59 && k <= 0x62)
        snprintf(buf, sizeof(buf), "KP%d", k == 0x62 ? 0 : k - 0x59 + 1);
    else if(k < (int)(sizeof(named) / sizeof(named[0])) && named[k])
        return named[k];
    else
        snprintf(buf, sizeof(buf), "%02X", k);
    return buf;
}

static void button(float x, float y, float w, float h, const char *label, bool on,
                   uint32_t color) {
    gfx_rect(x, y, w, h, on ? color : 0xff1a3050);
    gfx_box(x, y, w, h, 1, on ? 0xffffffff : C_LINE);
    gfx_text_center(x + w / 2, y + (h - FONT_H) / 2, on ? 0xff000000 : C_DIM, label, false);
}

static void stick(float cx, float cy, float r, int jx, int jy, const char *label) {
    gfx_box(cx - r, cy - r, 2 * r, 2 * r, 1, C_LINE);
    gfx_rect(cx - r, cy, 2 * r, 1, 0x60ffffff);
    gfx_rect(cx, cy - r, 1, 2 * r, 0x60ffffff);
    float dx = cx + jx / 128.0f * r, dy = cy + jy / 128.0f * r;
    gfx_rect(dx - 3, dy - 3, 7, 7, C_HI);
    char s[24];
    snprintf(s, sizeof(s), "%s %+4d %+4d", label, jx, jy);
    gfx_text_center(cx, cy + r + 4, C_DIM, s, false);
}

static void trigger_bar(float x, float y, float w, int v, const char *label) {
    gfx_rect(x, y, w, 10, 0xff1a3050);
    gfx_rect(x, y, w * v / 255.0f, 10, C_PASS);
    gfx_box(x, y, w, 10, 1, C_LINE);
    gfx_textf(x, y + 11, C_DIM, "%s %3d", label, v);
}

static void draw_controller(float x, float y, maple_device_t *dev, cont_state_t *st) {
    uint32_t b = st->buttons;

    trigger_bar(x + 4, y, 120, st->ltrig, "L");
    trigger_bar(x + 176, y, 120, st->rtrig, "R");
    /* D-pad */
    float dx = x + 30, dy = y + 56;
    button(dx, dy - 22, 20, 20, "", b & CONT_DPAD_UP, C_HI);
    button(dx, dy + 22, 20, 20, "", b & CONT_DPAD_DOWN, C_HI);
    button(dx - 22, dy, 20, 20, "", b & CONT_DPAD_LEFT, C_HI);
    button(dx + 22, dy, 20, 20, "", b & CONT_DPAD_RIGHT, C_HI);
    /* Face buttons, Dreamcast layout: Y top, X left, B right, A bottom */
    float fx = x + 248, fy = y + 56;
    button(fx, fy - 22, 22, 20, "Y", b & CONT_Y, 0xff40d040);
    button(fx - 24, fy, 22, 20, "X", b & CONT_X, 0xffffd040);
    button(fx + 24, fy, 22, 20, "B", b & CONT_B, 0xff4080ff);
    button(fx, fy + 22, 22, 20, "A", b & CONT_A, 0xffff4040);
    stick(x + 150, y + 64, 22, st->joyx, st->joyy, "J1");
    button(x + 128, y + 110, 44, 18, "START", b & CONT_START, C_HI);
    /* Extended buttons and the second D-pad and stick, when present */
    if(cont_has_capabilities(dev, CONT_CAPABILITIES_EXTENDED_BUTTONS)) {
        button(x + 206, y + 110, 22, 18, "C", b & CONT_C, C_HI);
        button(x + 230, y + 110, 22, 18, "Z", b & CONT_Z, C_HI);
        button(x + 254, y + 110, 22, 18, "D", b & CONT_D, C_HI);
    }
    if(cont_has_capabilities(dev, CONT_CAPABILITIES_SECONDARY_DPAD)) {
        button(x + 30, y + 104, 14, 12, "", b & CONT_DPAD2_UP, C_HI);
        button(x + 30, y + 128, 14, 12, "", b & CONT_DPAD2_DOWN, C_HI);
        button(x + 14, y + 116, 14, 12, "", b & CONT_DPAD2_LEFT, C_HI);
        button(x + 46, y + 116, 14, 12, "", b & CONT_DPAD2_RIGHT, C_HI);
    }
    if(cont_has_capabilities(dev, CONT_CAPABILITIES_SECONDARY_ANALOG))
        gfx_textf(x + 96, y + 134, C_DIM, "J2 %+4d %+4d", st->joy2x, st->joy2y);
    gfx_textf(x + 206, y + 134, C_DIM, "BTN %04lX", (unsigned long)(b & 0xffff));
}

static void draw_keyboard(float x, float y, maple_device_t *dev, port_state_t *ps) {
    kbd_state_t *k = kbd_get_state(dev);
    static const char *const mods[] = { "LCTRL", "LSHFT", "LALT", "S1",
                                        "RCTRL", "RSHFT", "RALT", "S2" };
    static const char *const regions[] = { "?", "JP", "US", "UK", "DE", "FR", "IT", "ES" };

    if(!k)
        return;
    for(int i = 0; i < 8; i++)
        button(x + 4 + (i % 4) * 58, y + (i / 4) * 20, 56, 18, mods[i],
               k->cond.modifiers.raw & (1 << i), C_HI);
    float px = x + 4;
    gfx_text(px, y + 46, C_DIM, "KEYS");
    px += 40;
    for(int i = 0; i < KBD_MAX_PRESSED_KEYS; i++) {
        int code = k->cond.keys[i];
        if(code <= KBD_KEY_ERROR)
            continue;
        px = gfx_text(px, y + 46, C_HI, key_name(code)) + 8;
    }
    gfx_textf(x + 4, y + 66, C_DIM, "LEDS %s%s%s  REGION %s",
              k->cond.leds.num_lock ? "NUM " : "", k->cond.leds.caps_lock ? "CAPS " : "",
              k->cond.leds.scroll_lock ? "SCRL" : "",
              k->region < 8 ? regions[k->region] : "?");
    gfx_rect(x + 4, y + 88, 292, 20, 0xff000000);
    gfx_box(x + 4, y + 88, 292, 20, 1, C_LINE);
    float tx = gfx_text(x + 8, y + 90, C_TEXT, ps->text);
    if((kt_frame() / 20) & 1)
        gfx_rect(tx, y + 92, 7, 13, C_HI);
    gfx_text(x + 4, y + 112, C_DIM, "TYPE TO TEST. ESC EXITS.");
}

static void draw_mouse(float x, float y, maple_device_t *dev, port_state_t *ps) {
    mouse_state_t *m = maple_dev_status(dev);

    if(!m)
        return;
    button(x + 4, y, 60, 22, "LEFT", m->buttons & MOUSE_LEFTBUTTON, C_HI);
    button(x + 68, y, 60, 22, "MID", m->buttons & MOUSE_MIDDLEBUTTON, C_HI);
    button(x + 132, y, 60, 22, "RIGHT", m->buttons & MOUSE_RIGHTBUTTON, C_HI);
    button(x + 196, y, 60, 22, "SIDE", m->buttons & MOUSE_SIDEBUTTON, C_HI);
    gfx_textf(x + 4, y + 32, C_TEXT, "DELTA  X %+4d  Y %+4d  Z %+3d", m->dx, m->dy, m->dz);
    gfx_textf(x + 4, y + 52, C_TEXT, "TOTAL  X %+6d  Y %+6d", ps->mouse_x, ps->mouse_y);
    gfx_textf(x + 4, y + 72, C_TEXT, "WHEEL  %+d", ps->wheel);
    gfx_text(x + 4, y + 96, C_DIM, "THE CURSOR FOLLOWS THE MOUSE");
}

static void draw_port(int p, float x, float y, float w, float h) {
    port_state_t *ps = &ports[p];
    maple_device_t *dev = maple_enum_dev(p, 0);
    char title[48], name[31];

    if(!dev) {
        snprintf(title, sizeof(title), "PORT %c", 'A' + p);
        ui_panel(x, y, w, h, title);
        gfx_text_center(x + w / 2, y + h / 2 - 8, C_DIM, "NO DEVICE", false);
        return;
    }
    memcpy(name, dev->info.product_name, 30);
    name[30] = 0;
    for(int i = 29; i >= 0 && name[i] == ' '; i--)
        name[i] = 0;
    snprintf(title, sizeof(title), "PORT %c  %.28s", 'A' + p, name);
    ui_panel(x, y, w, h, title);

    float cy = y + 24;
    uint32_t f = dev->info.functions;
    if(f & MAPLE_FUNC_CONTROLLER) {
        cont_state_t *st = maple_dev_status(dev);
        if(st)
            draw_controller(x + 4, cy, dev, st);
    }
    else if(f & MAPLE_FUNC_KEYBOARD)
        draw_keyboard(x + 4, cy, dev, ps);
    else if(f & MAPLE_FUNC_MOUSE)
        draw_mouse(x + 4, cy, dev, ps);
    else
        gfx_text_clip(x + 8, cy, C_TEXT, maple_pcaps(f), 36);

    /* Sub-units */
    float sy = y + h - 20;
    float sx = x + 6;
    for(int u = 1; u < MAPLE_UNIT_COUNT; u++) {
        maple_device_t *sub = maple_enum_dev(p, u);
        if(!sub)
            continue;
        uint32_t sf = sub->info.functions;
        const char *what = sf & MAPLE_FUNC_MEMCARD ? "VMU" :
                           sf & MAPLE_FUNC_PURUPURU ? "JUMP PACK" :
                           sf & MAPLE_FUNC_MICROPHONE ? "MIC" :
                           sf & MAPLE_FUNC_CAMERA ? "CAMERA" :
                           sf & MAPLE_FUNC_LCD ? "LCD" : "UNIT";
        sx = gfx_textf(sx, sy, C_INFO, "U%d %s", u, what) + 12;
    }
    if(ps->presses)
        gfx_textf(x + w - 96, sy, C_DIM, "%5lu PRESSES", (unsigned long)ps->presses);
}

static void input_enter(void) {
    maple_device_t *dev;

    memset(ports, 0, sizeof(ports));
    /* Buttons already held (the press that opened this screen) are not new. */
    for(int p = 0; p < MAPLE_PORT_COUNT; p++) {
        dev = maple_enum_dev(p, 0);
        cont_state_t *st = dev && (dev->info.functions & MAPLE_FUNC_CONTROLLER) ?
                           maple_dev_status(dev) : NULL;
        if(st)
            ports[p].last_buttons = st->buttons;
    }
    for(int n = 0; (dev = maple_enum_type(n, MAPLE_FUNC_KEYBOARD)) != NULL; n++)
        while(kbd_queue_pop(dev, true) != KBD_QUEUE_END)
            ;
}

static void rumble_port(int p) {
    maple_device_t *pack = NULL;

    for(int u = 1; u < MAPLE_UNIT_COUNT && !pack; u++) {
        maple_device_t *d = maple_enum_dev(p, u);
        if(d && (d->info.functions & MAPLE_FUNC_PURUPURU))
            pack = d;
    }
    if(pack) {
        purupuru_effect_t e = { .cont = false, .motor = 1, .fpow = 7, .freq = 26, .inc = 1 };
        purupuru_rumble(pack, &e);
    }
}

static void beep_port(int p) {
    for(int u = 1; u < MAPLE_UNIT_COUNT; u++) {
        maple_device_t *d = maple_enum_dev(p, u);
        if(d && (d->info.functions & MAPLE_FUNC_CLOCK)) {
            vmu_beep_waveform(d, 0xf0, 0x78, 0, 0);
            ports[p].beep_until = timer_ms_gettime64() + 250;
        }
    }
}

static void input_update(const kt_input_t *in) {
    bool leave = false;
    (void)in;

    any_mouse = false;
    for(int p = 0; p < MAPLE_PORT_COUNT; p++) {
        port_state_t *ps = &ports[p];
        maple_device_t *dev = maple_enum_dev(p, 0);

        if(ps->beep_until && timer_ms_gettime64() > ps->beep_until) {
            for(int u = 1; u < MAPLE_UNIT_COUNT; u++) {
                maple_device_t *d = maple_enum_dev(p, u);
                if(d && (d->info.functions & MAPLE_FUNC_CLOCK))
                    vmu_beep_waveform(d, 0, 0, 0, 0);
            }
            ps->beep_until = 0;
        }
        if(!dev)
            continue;
        uint32_t f = dev->info.functions;
        if(f & MAPLE_FUNC_CONTROLLER) {
            cont_state_t *st = maple_dev_status(dev);
            if(!st)
                continue;
            const uint32_t exit = CONT_START | CONT_B;
            uint32_t down = st->buttons & ~ps->last_buttons;
            for(uint32_t m = down; m; m &= m - 1)
                ps->presses++;
            /* START + B leaves; START + X rumbles; START + Y beeps. */
            if((st->buttons & exit) == exit && (ps->last_buttons & exit) != exit)
                leave = true;
            if((down & CONT_X) && (st->buttons & CONT_START))
                rumble_port(p);
            if((down & CONT_Y) && (st->buttons & CONT_START))
                beep_port(p);
            ps->last_buttons = st->buttons;
        }
        else if(f & MAPLE_FUNC_KEYBOARD) {
            int k;
            while((k = kbd_queue_pop(dev, true)) != KBD_QUEUE_END) {
                size_t len = strlen(ps->text);
                if(k == 27 || k == (KBD_KEY_ESCAPE << 8))
                    leave = true;
                else if(k == '\b' || k == (KBD_KEY_BACKSPACE << 8)) {
                    if(len)
                        ps->text[len - 1] = 0;
                }
                else if(k == '\n' || k == '\r')
                    ps->text[0] = 0;
                else if(k >= 32 && k < 256 && k != 127) {
                    if(len >= TEXT_MAX) {
                        memmove(ps->text, ps->text + 1, len);
                        len--;
                    }
                    ps->text[len] = (char)k;
                    ps->text[len + 1] = 0;
                }
            }
        }
        else if(f & MAPLE_FUNC_MOUSE) {
            mouse_state_t *m = maple_dev_status(dev);
            if(!m)
                continue;
            any_mouse = true;
            ps->mouse_x += m->dx;
            ps->mouse_y += m->dy;
            ps->wheel += m->dz;
            cursor_x += m->dx;
            cursor_y += m->dy;
            if(cursor_x < 0) cursor_x = 0;
            if(cursor_y < 0) cursor_y = 0;
            if(cursor_x > SCREEN_W - 1) cursor_x = SCREEN_W - 1;
            if(cursor_y > SCREEN_H - 1) cursor_y = SCREEN_H - 1;
        }
    }
#ifdef KT_AUTOTEST
    leave = false;
#endif
    if(leave)
        kt_pop();
}

static void input_draw(void) {
    ui_background();
    ui_header("INPUT TEST", "MAPLE BUS");
    draw_port(0, 6, 44, 312, 200);
    draw_port(1, 322, 44, 312, 200);
    draw_port(2, 6, 248, 312, 200);
    draw_port(3, 322, 248, 312, 200);
    if(any_mouse) {
        gfx_rect(cursor_x - 8, cursor_y, 17, 1, C_HI);
        gfx_rect(cursor_x, cursor_y - 8, 1, 17, C_HI);
    }
    ui_footer("START+B EXIT   START+X RUMBLE   START+Y VMU BEEP   ESC EXIT");
}

const kt_screen_t input_screen = {
    "Input test", input_enter, input_update, input_draw, NULL
};
