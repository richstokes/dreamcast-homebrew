/* PLAY, SOUND, BEEP and DRAW: see av.c. */
#ifndef AV_H
#define AV_H

#include <stdbool.h>

enum { AV_OK, AV_ILLEGAL, AV_BREAK };

/* Polled while a foreground sound blocks; returns true to abandon it. */
typedef bool (*av_break_fn)(void);

void av_reset(void);
int av_sound(double hz, double ticks, av_break_fn should_break);
int av_beep(av_break_fn should_break);
int av_play(const char *text, int length, av_break_fn should_break);
int av_wait_sound(av_break_fn should_break);
int av_draw(const char *text, int length, int default_color);
/* The graphics cursor shared by DRAW and the STEP coordinate forms. */
void av_set_position(int x, int y);
void av_get_position(int *x, int *y);

#endif
