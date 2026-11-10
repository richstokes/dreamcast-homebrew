/* Ridgeline: the fire burning in the ring at the high camp. Flames,
   sparks and a little smoke. Its light on what stands round it is
   glow.c's and lamps.c's, from the level this sets each frame. */
#ifndef RL_CAMPFIRE_H
#define RL_CAMPFIRE_H

#include "rl.h"

/* The middle of the fire ring, on the ground (in front of the tent). */
V3 campfire_pos(void);

void campfire_init(void);          /* after the terrain is up */
void campfire_update(float dt);
void campfire_draw_tr(void);

#endif
