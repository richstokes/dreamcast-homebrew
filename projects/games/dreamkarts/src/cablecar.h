/* Bay City Hills' cable car: a hazard running up and down Hyde Street. */
#ifndef DK_CABLECAR_H
#define DK_CABLECAR_H

#include "kart.h"

/* In this race at all (Bay City Hills). */
bool cablecar_on(void);
/* A new race (or a restart): which of Hyde Street's two tracks it runs
   on, at random offline and always the first online and in a time
   trial. */
void cablecar_reset(void);
/* Where it is now: lap distance of its middle, the lateral offset of its
   track, and +1 climbing, -1 coming down, 0 waiting at an end. */
void cablecar_pose(float *s, float *d, int *dir);
/* A kart driving into it bounces off as off a wall, or spins out if it
   runs into an end; after the walls, in the kart's own physics. */
void cablecar_collide(Kart *k);
/* The CPUs' lane round it: td, the lateral offset they aim for at lap
   distance s, moved clear of the car when it is in the way. */
float cablecar_avoid(const Kart *k, float s, float td);

#endif
