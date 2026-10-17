/* DreamKarts world: track scenery, rebuilt whenever the track changes. */
#ifndef DK_WORLD_H
#define DK_WORLD_H

#include "dk.h"

void world_init(void);
/* (Re)build the static scenery for the current track. */
void world_build(void);
void world_update(float dt);
/* Fog for one full view or a split screen; call before a scene begins. */
void world_set_fog(bool split);
void world_draw_op(void);
void world_draw_pt(void);
void world_draw_tr(void);
/* Sun direction (towards the sun) for lens flare and model lighting. */
extern V3 world_sun_dir;
extern float world_time;
/* Two-player split: shorter draw distance and lighter far scenery. */
extern bool world_split;

#endif
