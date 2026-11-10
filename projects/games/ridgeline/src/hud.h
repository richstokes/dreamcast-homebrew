/* Ridgeline HUD and the shared text helpers. */
#ifndef RL_HUD_H
#define RL_HUD_H

#include "rl.h"
#include "engine/text.h"

void hud_init(void);
void hud_update(float dt);
void hud_draw(void);                /* TR list */
void hud_draw_map(void);            /* the map alone (demo mode) */
/* The colour of truck index `truck` on the map. */
uint32_t hud_map_color(int truck);
void hud_message(const char *text, uint32_t col, float seconds);
/* A smaller second line under the message just set, gone with it. */
void hud_message_sub(const char *text);
const EngFont *hud_font(void);
void hud_text(float x, float y, float scale, int align, uint32_t col, const char *s);
void hud_text_c(float x, float cy, float scale, int align, uint32_t col, const char *s);
void hud_panel(float x0, float y0, float x1, float y1, uint32_t col);
/* A small gold trophy (online: reached the summit), s pixels tall. */
void hud_trophy(float cx, float cy, float s, float alpha);
/* A held button's progress bar: 0..1, and what letting go too soon misses. */
extern float hud_hold_k;
extern const char *hud_hold_label;

#endif
