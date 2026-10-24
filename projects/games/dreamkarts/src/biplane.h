/* DreamKarts: Spiral Canyon's red biplane, which weaves ahead of the karts
   on the final lap. */
#ifndef DK_BIPLANE_H
#define DK_BIPLANE_H

#include "dk.h"

void biplane_init(void);
/* Call after world_build (the plane is lit for the track's sun). */
void biplane_reset(void);
/* Watch the humans reach the long way home on the final lap and fly any
   show. Race states only. */
void biplane_update(float dt);
/* Its drone for player 1's camera, every frame (off when on is false:
   paused, or not racing). */
void biplane_audio(bool on);
/* Opaque list, current view. */
void biplane_draw(void);
/* Translucent list, current view: the plane as it fades out. */
void biplane_draw_tr(void);
/* Seconds into the newest show, or -1 when none is flying. */
float biplane_show_time(void);

#endif
