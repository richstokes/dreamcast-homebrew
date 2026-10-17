/* DreamKarts controller input: first controller found, with edge detection
   and menu key repeat. Triggers count as buttons past a threshold. */
#include "dk.h"

Pad pad;
static float repeat_t[4];
static bool repeat_fire[4];

void input_init(void) {
    memset(&pad, 0, sizeof(pad));
}

static float dead_zone(int raw) {
    float v = (float)raw / 127.0f;
    float a = fabsf(v);
    if(a < 0.12f) return 0.0f;
    v = signf(v) * (a - 0.12f) / 0.88f;
    return clampf(v, -1.0f, 1.0f);
}

void input_update(void) {
    uint32_t held = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    bool was_connected = pad.connected;
    pad.connected = st != NULL;
    if(was_connected != pad.connected)
        printf("DreamKarts input: controller %s\n", pad.connected ? "connected" : "missing");
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
        pad.ltrig = clampf(st->ltrig / 255.0f, 0, 1);
        pad.rtrig = clampf(st->rtrig / 255.0f, 0, 1);
        if(pad.ltrig > 0.35f) held |= BTN_L;
        if(pad.rtrig > 0.35f) held |= BTN_R;
        pad.stick_x = dead_zone(st->joyx);
        pad.stick_y = dead_zone(st->joyy);
    }
    else {
        pad.stick_x = pad.stick_y = pad.ltrig = pad.rtrig = 0;
    }
    pad.pressed = held & ~pad.held;
    pad.released = pad.held & ~held;
    pad.held = held;

    /* Menu directions: dpad or stick, with a 0.32 s delay then 0.09 s repeat. */
    bool dir[4] = {
        (held & BTN_UP) || pad.stick_y < -0.6f,
        (held & BTN_DOWN) || pad.stick_y > 0.6f,
        (held & BTN_LEFT) || pad.stick_x < -0.6f,
        (held & BTN_RIGHT) || pad.stick_x > 0.6f
    };
    for(int i = 0; i < 4; i++) {
        repeat_fire[i] = false;
        if(!dir[i]) {
            repeat_t[i] = 0;
            continue;
        }
        if(repeat_t[i] == 0) repeat_fire[i] = true;
        repeat_t[i] += DK_DT;
        if(repeat_t[i] > 0.32f) {
            repeat_fire[i] = true;
            repeat_t[i] -= 0.09f;
        }
    }
}

bool input_menu_up(void) { return repeat_fire[0]; }
bool input_menu_down(void) { return repeat_fire[1]; }
bool input_menu_left(void) { return repeat_fire[2]; }
bool input_menu_right(void) { return repeat_fire[3]; }
