/* Ridgeline water: built with the terrain, relit with it, drawn in the
   translucent list with ripples, sky and glints. */
#ifndef RL_WATER_H
#define RL_WATER_H

#include "rl.h"
#include "engine/scenery.h"

/* Add the water's chunks to sc, marking each vertex with mark(sc, tag | its
   water vertex) for the relight. */
void water_build(Scenery *sc, void (*mark)(const Scenery *, uint32_t), uint32_t tag);
/* A water vertex's colour for the hour's water brightness: worked out
   once in a relight pass (water_relight_begin starts one). */
void water_relight_begin(void);
uint32_t water_relit(uint32_t id, float water_k);
int water_wet_n(void);          /* ids run below this */
void water_draw(const Scenery *sc);

/* How much of the sky's colour water at p takes from where the camera
   is (0..1), past the reach of the water drawn near; foam takes none. */
float water_sheen(V3 p, float foam);

#endif
