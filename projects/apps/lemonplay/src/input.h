/* Controller input with key repeat on the d-pad and triggers. */
#ifndef LEMONPLAY_INPUT_H
#define LEMONPLAY_INPUT_H

#include <dc/maple/controller.h>
#include <stdint.h>

/* CONT_* buttons, plus the analog triggers as buttons. */
#define BTN_LTRIG (1u << 20)
#define BTN_RTRIG (1u << 21)

typedef struct {
    uint32_t held;
    uint32_t pressed;   /* newly pressed this poll, or repeating */
    int connected;
} input_t;

void input_poll(input_t *in);

#endif
