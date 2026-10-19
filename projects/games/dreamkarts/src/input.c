/* DreamKarts controller input: first controller found, with edge detection
   and menu key repeat. Triggers count as buttons past a threshold. */
#include "dk.h"

Pad pad, pad2;
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

void input_update(void) {
    read_pad(&pad, 0, 0);
    read_pad(&pad2, 1, 1);
}

bool input_up(int player) { return repeat_fire[player & 1][0]; }
bool input_down(int player) { return repeat_fire[player & 1][1]; }
bool input_left(int player) { return repeat_fire[player & 1][2]; }
bool input_right(int player) { return repeat_fire[player & 1][3]; }

bool input_menu_up(void) { return repeat_fire[0][0]; }
bool input_menu_down(void) { return repeat_fire[0][1]; }
bool input_menu_left(void) { return repeat_fire[0][2]; }
bool input_menu_right(void) { return repeat_fire[0][3]; }
