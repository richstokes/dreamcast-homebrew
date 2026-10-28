/* DreamKarts controller input: first controller found, with edge detection
   and menu key repeat. Triggers count as buttons past a threshold. A
   Dreamcast keyboard on any port feeds the lobby chat. */
#include "dk.h"
#include <ctype.h>
#include <dc/maple/keyboard.h>

Pad pad, pad2;
static bool kbd_connected;
static uint8_t key_buf[MAX_KEYS];
static int key_n;
#ifdef DK_AUTOTEST
static uint8_t fake_keys[MAX_KEYS];
static int fake_n;

/* Test builds: key presses the next read reports as typed. */
void input_fake_keys(const uint8_t *keys, int n) {
    for(int i = 0; i < n && fake_n < MAX_KEYS; i++) fake_keys[fake_n++] = keys[i];
}
#endif
static float repeat_t[MAX_PLAYERS][4];
static bool repeat_fire[MAX_PLAYERS][4];
#if defined(DK_FILM) || defined(DK_CAPTURE)
static uint32_t film_taps[MAX_PLAYERS];

/* Film and capture builds: hold these buttons for the next read, as a quick tap. */
void input_film_tap(int player, uint32_t btns) {
    film_taps[player & 1] |= btns;
}
#endif

Pad *input_pad(int player) {
    return player ? &pad2 : &pad;
}

void input_init(void) {
    memset(&pad, 0, sizeof(pad));
    memset(&pad2, 0, sizeof(pad2));
    pad.port = pad2.port = -1;
}

static float dead_zone(int raw) {
    float v = (float)raw / 127.0f;
    float a = fabsf(v);
    if(a < 0.12f) return 0.0f;
    v = signf(v) * (a - 0.12f) / 0.88f;
    return clampf(v, -1.0f, 1.0f);
}

static void read_pad(Pad *pd, int port_index, int player) {
    uint32_t held = 0;
    maple_device_t *dev = maple_enum_type(port_index, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    bool was_connected = pd->connected;
    pd->connected = st != NULL;
    pd->port = st ? dev->port : -1;
    if(was_connected != pd->connected)
        printf("DreamKarts input: controller %d %s\n", player + 1, pd->connected ? "connected" : "missing");
    if(st) {
        uint32_t b = st->buttons;
        if(b & CONT_A) held |= BTN_A;
        if(b & CONT_B) held |= BTN_B;
        if(b & CONT_X) held |= BTN_X;
        if(b & CONT_Y) held |= BTN_Y;
        if(b & CONT_START) held |= BTN_START;
        if(b & CONT_DPAD_UP) held |= BTN_UP;
        if(b & CONT_DPAD_DOWN) held |= BTN_DOWN;
        if(b & CONT_DPAD_LEFT) held |= BTN_LEFT;
        if(b & CONT_DPAD_RIGHT) held |= BTN_RIGHT;
        pd->ltrig = clampf(st->ltrig / 255.0f, 0, 1);
        pd->rtrig = clampf(st->rtrig / 255.0f, 0, 1);
        if(pd->ltrig > 0.35f) held |= BTN_L;
        if(pd->rtrig > 0.35f) held |= BTN_R;
        pd->stick_x = dead_zone(st->joyx);
        pd->stick_y = dead_zone(st->joyy);
    }
    else {
        pd->stick_x = pd->stick_y = pd->ltrig = pd->rtrig = 0;
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    /* Scripted runs in Flycast drive themselves, and Flycast hands every
       instance the host's gamepad whichever window is in front: a stray
       press from a game being played alongside would cycle the title's
       track or pick two players. The script's own taps still land. */
    held = 0;
    pd->stick_x = pd->stick_y = pd->ltrig = pd->rtrig = 0;
#endif
#if defined(DK_FILM) || defined(DK_CAPTURE)
    held |= film_taps[player];
    film_taps[player] = 0;
#endif
    pd->pressed = held & ~pd->held;
    pd->released = pd->held & ~held;
    pd->held = held;

    /* Menu directions: dpad or stick, with a 0.32 s delay then 0.09 s repeat. */
    bool dir[4] = {
        (held & BTN_UP) || pd->stick_y < -0.6f,
        (held & BTN_DOWN) || pd->stick_y > 0.6f,
        (held & BTN_LEFT) || pd->stick_x < -0.6f,
        (held & BTN_RIGHT) || pd->stick_x > 0.6f
    };
    float *rt = repeat_t[player];
    bool *rf = repeat_fire[player];
    for(int i = 0; i < 4; i++) {
        rf[i] = false;
        if(!dir[i]) {
            rt[i] = 0;
            continue;
        }
        if(rt[i] == 0) rf[i] = true;
        rt[i] += DK_DT;
        if(rt[i] > 0.32f) {
            rf[i] = true;
            rt[i] -= 0.09f;
        }
    }
}

/* The keyboard's queue holds the keys pressed since the last read; each
   becomes the character it types in capitals (the chat's font has no
   lower case), or a KEY_* for the editing keys. */
static void read_keyboard(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_KEYBOARD);
    if((dev != NULL) != kbd_connected) {
        kbd_connected = dev != NULL;
        printf("DreamKarts input: keyboard %s\n", kbd_connected ? "connected" : "missing");
    }
    key_n = 0;
    if(dev) {
        kbd_state_t *ks = kbd_get_state(dev);
        int raw;
        while((raw = kbd_queue_pop(dev, false)) != KBD_QUEUE_END) {
            kbd_key_t key = (kbd_key_t)(raw & 0xff);
            kbd_mods_t mods = {.raw = (uint8_t)((raw >> 8) & 0xff)};
            kbd_leds_t leds = {.raw = (uint8_t)((raw >> 16) & 0xff)};
            uint8_t out = 0;
            switch(key) {
                case KBD_KEY_ENTER:
                case KBD_KEY_PAD_ENTER: out = KEY_ENTER; break;
                case KBD_KEY_BACKSPACE: out = KEY_BACKSPACE; break;
                case KBD_KEY_ESCAPE: out = KEY_ESCAPE; break;
                case KBD_KEY_UP: out = KEY_UP; break;
                case KBD_KEY_DOWN: out = KEY_DOWN; break;
                case KBD_KEY_LEFT: out = KEY_LEFT; break;
                case KBD_KEY_RIGHT: out = KEY_RIGHT; break;
                default: {
                    int c = ks ? (unsigned char)kbd_key_to_ascii(key, ks->region, mods, leds) : 0;
                    if(c >= 32 && c <= 126) out = (uint8_t)toupper(c);
                    break;
                }
            }
            if(out && key_n < MAX_KEYS) key_buf[key_n++] = out;
        }
    }
#ifdef DK_AUTOTEST
    for(int i = 0; i < fake_n && key_n < MAX_KEYS; i++) key_buf[key_n++] = fake_keys[i];
    fake_n = 0;
#endif
}

bool input_keyboard(void) { return kbd_connected; }

int input_keys(const uint8_t **keys) {
    *keys = key_buf;
    return key_n;
}

/* After an update step, like the pads' pressed bits: a frame that runs two
   steps must not type each key twice, nor step a menu cursor twice (it did,
   on the online name screen when its keyboard ran the console past a
   vblank: every press skipped a key). */
void input_step_taken(void) {
    key_n = 0;
    memset(repeat_fire, 0, sizeof(repeat_fire));
}

void input_update(void) {
    read_pad(&pad, 0, 0);
    read_pad(&pad2, 1, 1);
    read_keyboard();
}

bool input_up(int player) { return repeat_fire[player & 1][0]; }
bool input_down(int player) { return repeat_fire[player & 1][1]; }
bool input_left(int player) { return repeat_fire[player & 1][2]; }
bool input_right(int player) { return repeat_fire[player & 1][3]; }

bool input_menu_up(void) { return repeat_fire[0][0]; }
bool input_menu_down(void) { return repeat_fire[0][1]; }
bool input_menu_left(void) { return repeat_fire[0][2]; }
bool input_menu_right(void) { return repeat_fire[0][3]; }
