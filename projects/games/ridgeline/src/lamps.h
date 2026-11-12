/* Ridgeline: the trucks' headlights, and the campfire's and lanterns' light, on the
   trees, rocks and props. */
#ifndef RL_LAMPS_H
#define RL_LAMPS_H

#include "rl.h"

/* Building (world_build): forget the objects, add each near tree, rock
   and prop as its range of scenery pool vertices, then index them. */
void lamps_clear(void);
void lamps_add(uint32_t first, uint32_t end);
/* The chunk the next objects are in: where it measures the camera's
   distance from and how far it is drawn (0: always), for the campfire and lanterns. */
void lamps_lod(V3 p, float far);
void lamps_index(void);

/* Drawing: light what the beams reach before the scenery is drawn
   (trucks: whether any are on screen), and put its colours back after. */
void lamps_begin(bool trucks);
void lamps_end(void);

/* This frame's beams for what is lit as it is drawn (the grass, the
   trucks), between lamps_begin and the next one. Which beams could reach
   the ground within r of (x, z), as a mask; and their light at p from
   the beams in mask (LAMPS_ALL: every one) bar truck skip's own (-1:
   none), about 0..1.3, with the way back to them in *from if given. */
#define LAMPS_ALL 0xff
int lamps_ground_mask(float x, float z, float r);
float lamps_light(V3 p, int mask, int skip, V3 *from);
/* And the beams themselves, for what falls through them (rain.c): how
   many, and beam i's cone: from o along f, r across and u up it, its
   spread each way a tangent, its reach in metres. */
int lamps_beams(void);
void lamps_beam(int i, V3 *o, V3 *f, V3 *r, V3 *u, float *th, float *tv, float *range);

#endif
