/* Engine: controllers and the keyboard.

   Two local players: the first controller found is pad, the second pad2.
   Triggers count as buttons past a threshold, the stick has a dead zone,
   and menu directions repeat while held. A Dreamcast keyboard on any port
   reports this frame's key presses.

   The game's engine_config.h may define (test builds):
     ENG_INPUT_TAPS         input_film_tap(): scripted presses
     ENG_INPUT_FAKE_KEYS    input_fake_keys(): scripted typing
     ENG_INPUT_IGNORE_HOST  ignore the real controllers (Flycast hands every
                            instance the host's gamepad, whichever window
                            is in front) */
#ifndef ENG_INPUT_H
#define ENG_INPUT_H

#include "engine_config.h"
#include "vecmath.h"

#define ENG_PADS 2

typedef struct {
    bool connected;
    int port;                 /* maple port of the controller, -1 if none */
    uint32_t held, pressed, released;
    float stick_x, stick_y;   /* -1..1 with dead zone */
    float ltrig, rtrig;       /* 0..1 */
} Pad;

enum {
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3,
    BTN_START = 1 << 4, BTN_UP = 1 << 5, BTN_DOWN = 1 << 6,
    BTN_LEFT = 1 << 7, BTN_RIGHT = 1 << 8, BTN_L = 1 << 9, BTN_R = 1 << 10
};

/* pad is player 1 (first controller), pad2 player 2 (second controller). */
extern Pad pad, pad2;
Pad *input_pad(int player);
void input_init(void);
/* Once a displayed frame. */
void input_update(void);
/* Menu helpers with key repeat for held directions (player 1). */
bool input_menu_up(void);
bool input_menu_down(void);
bool input_menu_left(void);
bool input_menu_right(void);
/* Directions with repeat for any player. */
bool input_up(int player);
bool input_down(int player);
bool input_left(int player);
bool input_right(int player);
#ifdef ENG_INPUT_TAPS
/* Hold these buttons for the next read, as a quick tap. */
void input_film_tap(int player, uint32_t btns);
#endif
/* A Dreamcast keyboard on any port: this frame's key presses, as the
   capital letters and symbols they type or one of the keys below. */
enum { KEY_ENTER = 1, KEY_BACKSPACE, KEY_ESCAPE, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT };
#define MAX_KEYS 16
bool input_keyboard(void);
int input_keys(const uint8_t **keys);
/* After an update step, like the pads' pressed bits: a frame that runs two
   steps must not type each key twice, nor step a menu cursor twice. */
void input_step_taken(void);
#ifdef ENG_INPUT_FAKE_KEYS
void input_fake_keys(const uint8_t *keys, int n);
#endif

#endif
