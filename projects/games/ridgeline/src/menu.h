/* Ridgeline menus. */
#ifndef RL_MENU_H
#define RL_MENU_H

#include "rl.h"

void menu_init(void);
void menu_title_update(float dt);
const char *menu_truck_name(int model);
void menu_title_draw(void);
/* The truck select opens on its first page (the area's, offline). */
void menu_select_begin(void);
void menu_select_update(float dt);
void menu_select_draw(void);
/* An area's card (its map, what it is, its ways up), its top at y0: the
   area page's, and the loading screen's. */
void menu_area_card(int area, float y0);
void menu_pause_update(float dt);
void menu_pause_draw(void);
/* The sound test (soundtest.c): every effect by name, A plays or stops. */
void soundtest_update(float dt);
void soundtest_draw(void);
void soundtest_stop(void);

#endif
