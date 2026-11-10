/* Ridgeline's profiling sections and counts (engine/prof.h). */
#ifndef RL_PROF_IDS_H
#define RL_PROF_IDS_H

#include "engine/prof_engine.h"

enum {
    ENG_PROF_SECTIONS,
    PS_PHYS,        /* the trucks' physics, all substeps */
    PS_TERRAIN,     /* terrain chunks */
    PS_TREES,       /* trees, rocks and props */
    PS_TRUCKS,      /* truck meshes and lighting */
    PS_LAMPS,       /* headlights on the trees, rocks and props */
    PS_TR,          /* water, effects, sky sprites, HUD */
    PS_COUNT
};
enum { ENG_PROF_COUNTS, PN_COUNT };
#define PROF_SECTION_NAMES {ENG_PROF_SECTION_NAMES, "phys", "terrain", "trees", "trucks", "lamps", "tr"}
#define PROF_COUNT_NAMES {ENG_PROF_COUNT_NAMES}

#endif
