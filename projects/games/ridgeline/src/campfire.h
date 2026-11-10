/* Ridgeline: the fire burning in the ring at the high camp. Flames,
   sparks and a little smoke, and after dark its light on the ground
   round it, the tent, the trees (lamps.c) and any truck parked by it. */
#ifndef RL_CAMPFIRE_H
#define RL_CAMPFIRE_H

#include "rl.h"

/* How far the fire's light reaches, metres. */
#define CAMPFIRE_REACH 20.0f

/* The middle of the fire ring, on the ground (in front of the tent). */
V3 campfire_pos(void);

void campfire_init(void);          /* after the terrain is up */
void campfire_update(float dt);
void campfire_draw_tr(void);

/* This frame's light: nothing by day, about 1 at night, flickering. */
float campfire_level(void);
/* How much of the fire's light gets d metres from it (1 beside it). */
static inline float campfire_falloff(float d2) {
    float w = 1.0f - d2 * (1.0f / (CAMPFIRE_REACH * CAMPFIRE_REACH));
    return w <= 0 ? 0 : w / (1.0f + d2 * (1.0f / 25.0f));
}
/* The fire's light at p this frame, and the direction it comes from. */
float campfire_light(V3 p, V3 *from);

#endif
