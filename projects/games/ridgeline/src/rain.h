/* Ridgeline: showers (an area that has them: LvlLook.rain). The clock
   brings them, so everyone in a lobby is rained on together; while one
   passes the light goes grey and flat, the haze closes in, the ground
   darkens, and there is rain in the air about the camera and rings on
   the water. */
#ifndef RL_RAIN_H
#define RL_RAIN_H

#include "rl.h"
#include "world.h"

/* The shower over the area at an hour, 0 .. 1; and how wet it has left
   the ground (that outlasts it). */
float rain_shower(float hour);
float rain_wet(float hour);
/* The hour's palette under its weather (tod.c, each step). */
void rain_palette(Palette *p, float hour);
/* How much of the sun, the moon and the stars the overcast hides now. */
float rain_overcast(void);
/* The rain falling at the camera, 0 .. 1: the shower, heavier with
   height, and a drizzle inside the cloud on the tops. */
float rain_now(void);
void rain_update(float dt);
void rain_draw_tr(void);

#endif
