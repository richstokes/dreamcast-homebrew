/* DreamKarts world: Emerald Beach scenery built once at start-up. */
#ifndef DK_WORLD_H
#define DK_WORLD_H

#include "dk.h"

void world_init(void);
void world_update(float dt);
void world_draw_op(void);
void world_draw_pt(void);
void world_draw_tr(void);
/* Sun direction (towards the sun) for lens flare and model lighting. */
extern V3 world_sun_dir;
extern float world_time;

#endif
