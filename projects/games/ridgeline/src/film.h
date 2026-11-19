/* Ridgeline film builds (make VARIANT=film): boot straight into a list of
   scripted scenes for a trailer, each an area, an hour of the day, trucks
   on the mountain (up to eight: MAX_TRUCKS is 8 in these builds) and one
   camera shot, with no HUD. A scene can have a stunt too (film.c's gags:
   trucks dropped, launched, spun or floated, for a comedy reel). Each scene's footage starts
   at its "RL FILM go" serial line and ends at "RL FILM cut"; the second or
   two before "go" let the trucks get going and the light settle, and are
   cut away in the edit. */
#ifndef RL_FILM_H
#define RL_FILM_H

#include "rl.h"

#ifdef RL_FILM
void film_start(void);
/* The drive step while game.film: drivers, physics, the camera. */
void film_update(float dt);
#endif

#endif
