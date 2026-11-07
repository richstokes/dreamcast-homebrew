/* Ridgeline birds: jays, juncos and ravens perched on logs, rocks and
   props that fly off as a truck comes near, hawks circling overhead and
   the odd skein of geese crossing the sky. */
#ifndef RL_BIRDS_H
#define RL_BIRDS_H

#include "rl.h"

/* Place the perched birds on the level (after world_build). */
void birds_init(void);
/* Everyone back on their perches, no geese up (a new drive). */
void birds_reset(void);
void birds_update(float dt);
/* Opaque list, current view; leaves the view-projection loaded. */
void birds_draw(void);

#endif
