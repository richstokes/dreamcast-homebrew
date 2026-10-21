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
/* A night track (Neon Skyway, Cyberstone): moonlight, neon and a starry sky. */
extern bool world_night;
/* Spiral Canyon: a spring valley with rock towers, hoops and a lake. */
extern bool world_canyon;
/* Kart light tint per channel, 256 = full (lower and bluer at night); set
   by world_build, baked into the kart paint by models_paint_kart. */
extern uint16_t world_light[3];
/* The road's light colour across it at lateral offset d (Neon Skyway: cyan
   to magenta; Spiral Canyon: pink to sky blue). */
uint32_t world_lane_color(float d);
/* Terrain height under (x, z), ignoring the road and props (the sky
   track has none: a large negative value). */
float world_ground_y(float x, float z);
bool world_leak_point(float s, float d, V3 *out);
void world_diag_mode(int mode);   /* -DDK_ROCK_TINT builds only */
float world_rock_mesh_top(float x, float z, int *count, float *lowest);
/* Neon gates over the road (Neon Skyway): their lap distances. */
#define WORLD_MAX_GATES 12
extern int world_gates;
extern float world_gate_s[WORLD_MAX_GATES];

#endif
