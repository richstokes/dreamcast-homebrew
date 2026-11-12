/* Ridgeline: what belongs to the dunes alone. The powerline's wires,
   hung between its H-frames (the level's PROP_PYLONs, world.c); the sand
   the wind takes off the crests when it gusts, and the sand a truck
   sets sliding down a slip face; the whips, a sprung one on every truck
   and the ones stood about the camp, lit after dark. */
#ifndef RL_DUNES_H
#define RL_DUNES_H

#include "rl.h"

void dunes_init(void);          /* after the world is built; idle in the other areas */
void dunes_update(float dt);
void dunes_draw_op(void);       /* the wires */
void dunes_draw_tr(void);       /* blown and sliding sand, the whips' light */
/* The whip a prop carries (a flag stuck in the sand, a quad's, a sand
   rail's): its foot and its tip. False for a prop with none. */
bool dunes_whip_of(const lvl_prop_t *p, V3 *base, V3 *top);
/* How fast the wind is wearing tyre prints away, against a second of
   the clock: 1 in the other areas, less here in a lull, more in a gust. */
float dunes_wear(void);
/* How hard it is blowing, 0..1 (nothing in the other areas): the wind is
   most of what there is to hear out there. */
float dunes_wind(void);

#endif
