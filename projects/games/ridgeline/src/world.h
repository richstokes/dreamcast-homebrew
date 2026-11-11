/* Ridgeline world: the sky, the sun and its flare, distant ranges, the
   trees, rocks, logs and buildings, grass near the camera. */
#ifndef RL_WORLD_H
#define RL_WORLD_H

#include "rl.h"
#include "engine/scenery.h"

extern Scenery scenery;

/* The wind aloft: toward the north-east, 12 m/s (the clouds ride it). */
#define WIND_X 0.78f
#define WIND_Z 0.62f
#define WIND_SPEED 12.0f

/* The hour's palette and light (tod.c works it out from the clock). */
typedef struct {
    uint32_t sky_top, sky_horizon, haze;
    uint32_t sun_col;           /* the disc in the sky (the moon's at night) */
    uint32_t glow_col;          /* the sky low toward the sun */
    uint32_t cloud_col;
    float glow_k;               /* how much of that glow */
    float fog_near, fog_far;
    float sun_k[3], sky_k[3], amb_k[3];   /* the light: direct, from the sky, ambient */
    float stars;                /* 0 .. 1: how bright the stars */
    float dark;                 /* 0 day .. 1 night: headlights matter, dust is dim */
    float water_k;              /* the water's brightness */
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
/* An albedo lit by this scene's sun and sky, for a face with normal n. */
uint32_t world_lit(uint32_t albedo, V3 n);
/* How deep in the summit's cloud the camera is, 0..1. */
float world_mist(void);
/* Where the fog is solid this frame, metres. */
float world_fog_far(void);
/* The fog this frame, for what fogs itself: where it starts and is solid,
   and its colour. */
void world_fog(float *near_m, float *far_m, uint32_t *col);
/* How much of the haze the skyline's nearest ring is: land drawn in front
   of it should never be hazier. */
#define WORLD_SKYLINE_HAZE 0.70f

/* Integer lerp of two colours, t in 0..256 (the dome and ranges do a few
   hundred a frame). */
static inline uint32_t mix256(uint32_t a, uint32_t b, int t) {
    uint32_t rb = ((a & 0xff00ffu) * (uint32_t)(256 - t) + (b & 0xff00ffu) * (uint32_t)t) >> 8;
    uint32_t ag = (((a >> 8) & 0xff00ffu) * (uint32_t)(256 - t) + ((b >> 8) & 0xff00ffu) * (uint32_t)t) >> 8;
    return (rb & 0xff00ffu) | ((ag & 0xff00ffu) << 8);
}

/* Relighting the trees, rocks and props (tod.c): every scenery vertex
   keeps what it was lit from; a pass recolours [from, to) of them for
   the palette and light direction given to setup. */
void world_relight_setup(const Palette *p, V3 light);
int world_relight_n(void);
void world_relight(int from, int to);
/* What each of those vertices was lit from: its albedo (an index into
   world_alb; bit 7 marks a colour used as is, bit 6 a downward normal),
   the normal's x and z, and the occlusion (hundredths). Record k is pool
   vertex world_lrec_first + k. */
typedef struct { uint8_t alb; int8_t nx, nz; uint8_t ao; } LightRec;
extern LightRec *world_lrec;
extern uint32_t world_lrec_first;
extern uint32_t world_alb[64];

#endif
