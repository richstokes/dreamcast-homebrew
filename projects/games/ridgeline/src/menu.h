/* Ridgeline menus. */
#ifndef RL_MENU_H
#define RL_MENU_H

#include "rl.h"

void menu_init(void);
void menu_title_update(float dt);
void menu_title_draw(void);
void menu_select_update(float dt);
void menu_select_draw(void);
void menu_pause_update(float dt);
void menu_pause_draw(void);
/* The sound test (soundtest.c): every effect by name, A plays or stops. */
void soundtest_update(float dt);
void soundtest_draw(void);
void soundtest_stop(void);

#endif
