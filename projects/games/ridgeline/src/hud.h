/* Ridgeline HUD and the shared text helpers. */
#ifndef RL_HUD_H
#define RL_HUD_H

#include "rl.h"
#include "engine/text.h"

void hud_init(void);
void hud_area(void);                /* the loaded area changed: its map */
const pvr_poly_hdr_t *hud_map_hdr(int area);   /* an area's map picture, for a 2D quad */
void hud_update(float dt);
void hud_draw(void);                /* TR list */
void hud_draw_map(void);            /* the map alone (demo mode) */
/* The colour of truck index `truck` on the map. */
uint32_t hud_map_color(int truck);
/* Messages, of two kinds.

   A banner (hud_message) is something that happened: a viewpoint found,
   the summit, a trail driven, a truck unlocked. Banners wait their turn:
   each is shown alone, in the order they came, and one that has others
   waiting behind it keeps its time short. The lines and the sound set
   after hud_message belong to that banner and come up with it.

   A status (hud_status) is what the truck just did: the drive mode,
   a recover, a checkpoint. Only the latest is kept, it never waits, and
   while a banner is up it is a smaller line under it. */
void hud_message(const char *text, uint32_t col, float seconds);
void hud_message_sub(const char *text);      /* a smaller second line */
void hud_message_sub2(const char *text);     /* and a third */
void hud_message_sound(int sfx, int vol);    /* played as the banner comes up */
void hud_status(const char *text, uint32_t col, float seconds);
/* Banners showing or waiting, and room for how many more. */
int hud_message_count(void);
int hud_message_room(void);
/* Nothing showing, nothing waiting (a drive begins). */
void hud_message_clear(void);
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
