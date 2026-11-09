/* Ridgeline: small things in the air about the camera. Leaves coming
   down from the larches and maples, dust motes in the forest light,
   fireflies over the meadow after dusk, snow blowing about the summit. */
#ifndef RL_AIR_H
#define RL_AIR_H

#include "rl.h"

void air_init(void);           /* after the world is built */
void air_update(float dt);
void air_draw_op(void);        /* the leaves */
void air_draw_tr(void);        /* motes, fireflies, snow */

#endif
