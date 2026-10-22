#include "input.h"

#include <kos.h>

#define REPEAT_MASK (CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_DPAD_LEFT | CONT_DPAD_RIGHT | BTN_LTRIG | BTN_RTRIG)
#define REPEAT_DELAY_MS 400
#define REPEAT_EVERY_MS 70

static uint32_t previous;
static uint64_t repeat_at;

void input_poll(input_t *in) {
    uint32_t held = 0;
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t *st = dev ? (cont_state_t *)maple_dev_status(dev) : NULL;
    in->connected = st != NULL;
    if(st) {
        held = st->buttons;
        if(st->ltrig > 128) held |= BTN_LTRIG;
        if(st->rtrig > 128) held |= BTN_RTRIG;
        if(st->joyy < -80) held |= CONT_DPAD_UP;
        if(st->joyy > 80) held |= CONT_DPAD_DOWN;
        if(st->joyx < -80) held |= CONT_DPAD_LEFT;
        if(st->joyx > 80) held |= CONT_DPAD_RIGHT;
    }

    uint64_t now = timer_ms_gettime64();
    uint32_t pressed = held & ~previous;
    if(pressed & REPEAT_MASK) {
        repeat_at = now + REPEAT_DELAY_MS;
    }
    else if((held & REPEAT_MASK) && now >= repeat_at) {
        pressed |= held & REPEAT_MASK;
        repeat_at = now + REPEAT_EVERY_MS;
    }
    previous = held;
    in->held = held;
    in->pressed = pressed;
}
