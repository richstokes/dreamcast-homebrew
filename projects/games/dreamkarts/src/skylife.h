/* DreamKarts: Neon Skyway's flying traffic under the deck, and the UFO
   that rises out of the clouds on the final lap. */
#ifndef DK_SKYLIFE_H
#define DK_SKYLIFE_H

#include "dk.h"

/* Lay out the lanes for the track just built (world_build). */
void skylife_build(void);
/* A new race: the UFO back under the clouds. */
void skylife_reset(void);
void skylife_update(float dt);
/* Opaque list, current view; leaves the view-projection loaded. */
void skylife_draw_op(void);
/* Translucent list: lights, glows and the beam. */
void skylife_draw_tr(void);
/* Where the UFO is, and how far (0-1) the searchlights turn to follow it. */
float skylife_ufo(V3 *pos);

#endif
