/* FlappyKarts: the hidden mini game behind Sonic 1's level select code on
   the title screen. The player's kart, seen side on, flaps through an
   endless run of columns built from the background course's textures,
   under its sky. */
#ifndef DK_FLAPPY_H
#define DK_FLAPPY_H

#include "dk.h"

void flappy_init(void);
/* From the title screen (the code). Leaves through game_set_state. */
void flappy_enter(void);
void flappy_update(float dt);
void flappy_camera(Camera *cam);
/* Opaque list: ground, columns and the kart. */
void flappy_draw_op(void);
/* Translucent list: the kart's shadow. */
void flappy_draw_tr(void);
/* 2D overlay: score, prompts, the game-over panel. */
void flappy_draw_hud(void);
#ifdef DK_AT_FLAPPY
/* Capture build: enters from the title, flies a few columns, crashes. */
void flappy_autotest(void);
#endif

#endif
