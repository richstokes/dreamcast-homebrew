/* DreamKarts: gulls on the beaches that lift off as karts come by, and
   Temple Hill's parrots bursting from the canopy at the first hairpin. */
#ifndef DK_BIRDS_H
#define DK_BIRDS_H

#include "dk.h"

void birds_init(void);
/* Place the birds for the track just built (world_build). */
void birds_build(void);
void birds_update(float dt);
/* Opaque list, current view; leaves the view-projection loaded. */
void birds_draw(void);
/* Seconds since the canopy flock burst, or -1 while it roosts. */
float birds_flock_time(void);

#endif
