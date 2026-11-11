/* Ridgeline: what the wind moves in the desert. Tumbleweeds bowling
   across the flats, and now and then, in the heat of the day, a dust
   devil walking over them. Nothing here touches a truck. */
#ifndef RL_DESERT_H
#define RL_DESERT_H

#include "rl.h"

void desert_init(void);        /* after the world is built; idle where the area isn't desert */
void desert_update(float dt);
void desert_draw_pt(void);     /* the tumbleweeds */
void desert_draw_tr(void);     /* the dust devil */

#endif
