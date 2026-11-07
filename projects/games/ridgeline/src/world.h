/* Ridgeline world: the sky, the sun and its flare, distant ranges, the
   trees, rocks, logs and buildings, grass near the camera. */
#ifndef RL_WORLD_H
#define RL_WORLD_H

#include "rl.h"
#include "engine/scenery.h"

extern Scenery scenery;

/* A scene's palette and light. */
typedef struct {
    uint32_t sky_top, sky_horizon, haze;
    uint32_t sun_col;
    float fog_near, fog_far;
    float sun_az, sun_el;       /* radians: bearing from north, elevation */
    float sun_k[3], sky_k[3], amb_k[3];   /* light on moving models */
} Palette;
extern const Palette *pal;

void world_init(void);
void world_build(void);
void world_update(float dt);
/* Drawing, each in its list: */
void world_draw_sky(void);            /* OP: dome and distant ranges */
void world_draw_op(void);             /* OP: terrain, trees, rocks, props */
void world_draw_pt(void);             /* PT: grass tufts */
void world_draw_tr(void);             /* TR: water, clouds, sun */
void world_draw_flare(void);          /* TR, last: the sun's lens flare */
/* The top of boulder i (lvl_spheres) as drawn up close. */
float world_boulder_top(int i);

#endif
