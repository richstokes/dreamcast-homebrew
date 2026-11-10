/* Ridgeline: the warm lights that stay put - the campfire at the high
   camp and the lanterns on the summit lookout. Where they are, how far
   each reaches, their light on the ground and on a truck beside them,
   and the lanterns' own glow. Their light on the scenery is in lamps.c;
   the fire itself is campfire.c. */
#ifndef RL_GLOW_H
#define RL_GLOW_H

#include "rl.h"

/* Lights in a group flicker as one. */
enum { GLOW_FIRE, GLOW_LANTERNS, GLOW_GROUPS };
#define GLOW_MAX 8
#define GLOW_REACH_MAX 20.0f

typedef struct {
    V3 p;
    float reach, core;   /* gone by reach metres; half as bright by about core */
    float k;             /* how bright beside it */
    uint8_t group;
    bool lantern;        /* drawn here as a lantern's glow (the fire draws itself) */
} Glow;
extern Glow glows[GLOW_MAX];
extern int glow_n;
/* This frame's light from each group: nothing by day, about 1 at night. */
extern float glow_level[GLOW_GROUPS];
/* A sphere round each group's reach. */
extern V3 glow_c[GLOW_GROUPS];
extern float glow_r[GLOW_GROUPS];

/* How much of g's light gets to a point whose distance squared is d2. */
static inline float glow_falloff(const Glow *g, float d2) {
    float w = 1.0f - d2 / (g->reach * g->reach);
    return w <= 0 ? 0 : g->k * w / (1.0f + d2 / (g->core * g->core));
}

void glow_place(void);         /* from the level, once the terrain is up */
void glow_init(void);          /* after the world is built */
void glow_update(float dt);
void glow_draw_tr(void);
/* The summit lookout's lanterns, about its base: n of them into out. */
int glow_tower_lanterns(const V3 **out);
/* The warm light on p this frame, and the direction it comes from. */
float glow_light(V3 p, V3 *from);

#endif
