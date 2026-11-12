/* Ridgeline: the fire burning in the ring in front of the camp's tent,
   and another in a fire pit where an area has one. Flames, sparks and a
   little smoke. A fire's light on what stands round it is glow.c's and
   lamps.c's, from the level this sets each frame. */
#ifndef RL_CAMPFIRE_H
#define RL_CAMPFIRE_H

#include "rl.h"

#define CAMPFIRES 2       /* the tent's, the pit's */

/* Find the area's fires, each time the world is built. */
void campfire_place(void);
/* Whether the area has fire i, and where: the middle of its ring. */
bool campfire_lit(int i, V3 *at);
/* Where a prop's fire burns, on the ground: in front of a tent, in the
   middle of a fire pit. False for any other prop. */
bool campfire_of(const lvl_prop_t *p, V3 *at);

void campfire_init(void);          /* after the terrain is up */
void campfire_update(float dt);
void campfire_draw_tr(void);

#endif
