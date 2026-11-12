/* Ridgeline: the warm lights that stay put - the campfires, the
   lanterns on the summit lookout and the ones at the start (on the
   cabin, the picnic table and a post by the grid), and the dunes' camp:
   a lantern under a trailer's awning and the light in its windows, for
   the trailers someone is in. Where they are, how far
   each reaches, their light on the ground and on a truck beside them,
   and the lanterns' own glow. Their light on the scenery is in lamps.c;
   the fire itself is campfire.c. */
#ifndef RL_GLOW_H
#define RL_GLOW_H

#include "rl.h"

/* Lights in a group flicker as one, and share a patch of lit ground: a
   group's lights stand close together. */
enum {
    GLOW_FIRE, GLOW_FIRE2,      /* campfire.c's, which sets their levels */
    GLOW_TOWER, GLOW_CABIN, GLOW_TABLE, GLOW_POST,
    GLOW_CAMP,                  /* a lit trailer each, GLOW_CAMPS of them */
    GLOW_GROUPS = GLOW_CAMP + 3
};
#define GLOW_CAMPS (GLOW_GROUPS - GLOW_CAMP)
#define GLOW_MAX 16
#define GLOW_REACH_MAX 20.0f

typedef struct {
    V3 p;
    float reach, core;   /* gone by reach metres; half as bright by about core */
    float k;             /* how bright beside it */
    uint8_t group;
    bool lantern;        /* a lantern: world.c builds it, its glow is drawn here */
    float size;          /* the lantern's half width */
    float hang;          /* the length of what it hangs by (0: it stands on something) */
    bool post;           /* on a post of its own, an arm out to it */
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

void glow_place(void);         /* from the level, each time the world is built */
void glow_init(void);          /* after the world is built */
void glow_update(float dt);
void glow_draw_tr(void);
/* The warm light on p this frame, and the direction it comes from. */
float glow_light(V3 p, V3 *from);
/* Whether any of it could reach within r metres of p (the grass asks
   block by block before asking tuft by tuft). */
bool glow_near(V3 p, float r);
/* col with the warm light at p added, as the lit ground has it. */
uint32_t glow_tint(uint32_t col, V3 p);

#endif
