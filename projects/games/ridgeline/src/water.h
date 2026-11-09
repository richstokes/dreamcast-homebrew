/* Ridgeline water: built with the terrain, relit with it, drawn in the
   translucent list with ripples, sky and glints. */
#ifndef RL_WATER_H
#define RL_WATER_H

#include "rl.h"
#include "engine/scenery.h"

/* Add the water's chunks to sc, marking each vertex with mark(sc, tag | its
   water vertex) for the relight. */
void water_build(Scenery *sc, void (*mark)(const Scenery *, uint32_t), uint32_t tag);
/* A water vertex's colour for the hour's water brightness. */
uint32_t water_relit(uint32_t id, float water_k);
void water_draw(const Scenery *sc);

#endif
