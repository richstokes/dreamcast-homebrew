/* Ridgeline deer: a few in the meadows that graze, look up as a truck
   comes near and bolt when it gets close or sounds its horn; in the
   Highlands, cattle and sheep as well. Cosmetic: nothing collides with
   them, and they use their own random numbers so the game's stay as they
   were. */
#ifndef RL_DEER_H
#define RL_DEER_H

#include "rl.h"

void deer_reset(void);
void deer_update_all(float dt);
void deer_draw_all(void);    /* OP list */

#endif
