/* Ridgeline: the trucks' headlights on the trees, rocks and props. */
#ifndef RL_LAMPS_H
#define RL_LAMPS_H

#include "rl.h"

/* Building (world_build): forget the objects, add each near tree, rock
   and prop as its range of scenery pool vertices, then index them. */
void lamps_clear(void);
void lamps_add(uint32_t first, uint32_t end);
void lamps_index(void);

/* Drawing: light what the beams reach before the scenery is drawn
   (trucks: whether any are on screen), and put its colours back after. */
void lamps_begin(bool trucks);
void lamps_end(void);

#endif
