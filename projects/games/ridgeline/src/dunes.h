/* Ridgeline: what belongs to the dunes alone. The powerline's wires,
   hung between its H-frames (the level's PROP_PYLONs, world.c), and the
   sand the wind takes off the crests when it gusts. */
#ifndef RL_DUNES_H
#define RL_DUNES_H

#include "rl.h"

void dunes_init(void);          /* after the world is built; idle in the other areas */
void dunes_update(float dt);
void dunes_draw_op(void);       /* the wires */
void dunes_draw_tr(void);       /* blown sand */
/* How hard it is blowing, 0..1 (nothing in the other areas): the wind is
   most of what there is to hear out there. */
float dunes_wind(void);

#endif
