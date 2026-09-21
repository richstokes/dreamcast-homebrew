/*
 * Services the portable editor and interpreter need from their host: the
 * Dreamcast front end, or the scripted harness used by the host tests.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stddef.h>

/* Keys are ASCII (Enter 13, Backspace 8, Tab 9, Escape 27, Ctrl+A..Z 1..26)
   or one of these. */
enum {
    K_UP = 0x100, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
    K_INS, K_DEL, K_CTRL_HOME, K_CTRL_END,
    K_F1, K_F2, K_F3, K_F4, K_F5, K_F6, K_F7, K_F8, K_F9, K_F10, K_F11, K_F12
};

/* Pumps input and shows the screen, at most once per display frame. Cheap
   enough to call from the interpreter's statement loop. */
void plat_poll(void);
/* Next queued key, or 0. */
int plat_key(void);
/* Discards typed-ahead keys and any pending break. */
void plat_flush_keys(void);
/* True once after Escape or Ctrl+C was pressed; those keys stay queued. */
bool plat_break_pressed(void);
double plat_seconds(void);          /* monotonic */
double plat_clock_seconds(void);    /* seconds since midnight, for TIMER */
void plat_sleep_ms(int ms);

/* Square-wave voice. Tones queue up and play in order; 0 Hz is a rest. */
void plat_tone(double hz, int ms);
int plat_tone_queued_ms(void);
void plat_tone_stop(void);

/* Controller port A: axes are -127..127, buttons index A B X Y Start. */
int plat_stick(int axis);
bool plat_button(int button);

/* Program storage: one VMU file per program. Names are at most
   PLAT_NAME_MAX characters, upper case, without the .BAS extension. */
#define PLAT_NAME_MAX 8
#define PLAT_LIST_MAX 64
int plat_storage_list(char names[][PLAT_NAME_MAX + 1], int max);
/* Both return NULL on success or a short reason for the failure. */
const char *plat_storage_save(const char *name, const char *text, size_t size);
const char *plat_storage_load(const char *name, char **text, size_t *size);

/* Bundled examples. */
int plat_examples_list(char names[][PLAT_NAME_MAX + 1], int max);
const char *plat_example_load(const char *name, char **text, size_t *size);

/* Mirrors program output to a debug console (the serial port). */
void plat_debug_output(const char *text, size_t size);

#endif
