/* DreamKarts 2D overlay: text, race HUD, countdown, results, pause. */
#ifndef DK_HUD_H
#define DK_HUD_H

#include "dk.h"

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

void hud_init(void);
void hud_reset(void);
void hud_update(float dt);
float hud_text_width(const char *s, float scale);
void hud_text(float x, float y, float scale, int align, uint32_t col, const char *s);
void hud_panel(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom, float z);
/* Big lettering from the countdown atlas: 0..2 = "3".."1", 3 = GO!, 4 = FINISH!. */
void hud_bigtext(int which, float cx, float cy, float scale, uint32_t col);
void hud_format_time(char *buf, float t);
const char *hud_ordinal(int place);
uint32_t hud_place_color(int place);

/* Race HUD for one player inside screen rows [y0, y1). */
void hud_draw_race(int player, float y0, float y1);
void hud_draw_countdown(float y0, float y1);
/* Two-player split: divider, shared race clock and minimap. */
void hud_draw_split_overlay(void);
void hud_draw_results(void);
void hud_draw_pause(void);
/* Pop-up messages ("FINAL LAP!", lap times). */
void hud_message(int player, const char *text, uint32_t col, float seconds);
void hud_lap_popup(int player, int lap, float time, bool best);

#endif
