/* Ridgeline HUD and the shared text helpers. */
#ifndef RL_HUD_H
#define RL_HUD_H

#include "rl.h"
#include "engine/text.h"

void hud_init(void);
void hud_update(float dt);
void hud_draw(void);                /* TR list */
void hud_message(const char *text, uint32_t col, float seconds);
const EngFont *hud_font(void);
void hud_text(float x, float y, float scale, int align, uint32_t col, const char *s);
void hud_text_c(float x, float cy, float scale, int align, uint32_t col, const char *s);
void hud_panel(float x0, float y0, float x1, float y1, uint32_t col);
extern float hud_recover_k;         /* 0..1 while Y is held */

#endif
