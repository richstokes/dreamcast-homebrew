/* Ridgeline: the land beyond the map. */
#ifndef RL_OUTLAND_H
#define RL_OUTLAND_H

#include "rl.h"
#include "world.h"

/* Once the area's ground is loaded and lit. */
void outland_init(void);
/* OP: the ground; the vertices sent. */
int outland_draw(void);
/* TR: its rivers, before the map's water. */
void outland_draw_tr(void);
/* Relit with the rest of the world (tod.c), after the map's ground: its
   edge takes the map's own colours. */
void outland_relight_setup(const Palette *p, V3 light);
int outland_relight_n(void);
void outland_relight(int from, int to);
/* The land's height past the map's edge (the map's own is terrain.c's);
   far below everything where there is none. */
float outland_height(float x, float z);

#endif
