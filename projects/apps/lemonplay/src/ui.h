/* Screen furniture shared by the menus and the player. Everything a viewer
   needs to read stays inside the title-safe area (SAFE_X, SAFE_Y). */
#ifndef LEMONPLAY_UI_H
#define LEMONPLAY_UI_H

#include <stdint.h>

#define HEADER_H 84
#define FOOTER_Y 420

/* Header bar with the logo; subtitle (may be NULL) beside the name. Draws
   opaque polygons, so call it before any translucent drawing. */
void ui_header(const char *subtitle);
/* Footer bar; then add button hints left to right with ui_hint. */
void ui_footer(void);
void ui_hint(char button, const char *label);
/* A hint at the right-hand end of the footer. */
void ui_hint_right(char button, const char *label);
/* Eight dots chasing round a circle centred on (cx, cy). */
void ui_spinner(float cx, float cy);
/* Progress bar from x to x + w; fraction 0-1. */
void ui_progress(float x, float y, float w, float fraction, int highlight);
/* A centred toast just above the footer. */
void ui_toast(const char *text, int error);
/* Seconds as m:ss or h:mm:ss. */
void ui_format_time(char *out, int size, double secs);

#endif
