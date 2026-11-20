/* Film builds (make film): scripted scenes for trailer footage. Each puts
   the kaiju somewhere with something to do and a camera to watch it by:
   a cinematic one with no HUD, or the game's own with it. The serial log
   says "KL FILM go <scene>" and "KL FILM cut <scene>" for the edit. */
#ifndef KL_FILM_H
#define KL_FILM_H

#include "kl.h"

#ifdef KL_FILM
void film_start(void);
void film_update(float dt);
/* The scene wants the HUD (it is on the gameplay camera). */
bool film_hud(void);
/* main.c's: a new city, and the gameplay camera. */
void game_world_reset(uint32_t seed);
void game_camera(float dt, float turn);
void game_camera_face(float yaw);
/* life.c's: a traffic jam of n vehicles round c, and the news helicopter
   hovering at p. */
void life_film_jam(V3 c, int n);
void life_film_heli(V3 p);
#endif

#endif
