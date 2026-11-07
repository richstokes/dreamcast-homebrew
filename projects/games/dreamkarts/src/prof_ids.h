/* DreamKarts' profiling sections and counts for the engine profiler
   (projects/common/engine/prof.h): what the hwbench's DK PC lines report. */
#ifndef DK_PROF_IDS_H
#define DK_PROF_IDS_H

#include "engine/prof_engine.h"
#include <stdint.h>

enum {
    PS_MATS,        /* update_kart_mats: part matrices and part-space sun */
    PS_LIGHT,       /* per-vertex kart lighting */
    ENG_PROF_SECTIONS,
    PS_DOLPHINS,    /* dolphins_draw and biplane_draw, all of them */
    PS_CHUNKS,      /* draw_chunks (OP), culling and r_strip */
    PS_SKY,
    PS_SEA,
    PS_FOAM,
    PS_PHASE_UPD,   /* the whole phases, for their stall shares */
    PS_PHASE_WORLD,
    PS_PHASE_KARTS,
    PS_PHASE_PT,
    PS_SHADOWS,     /* models_draw_shadow_volumes, all views */
    PS_PHASE_TR,
    PS_U_INPUT,     /* inside upd: controllers (and online, the probe) */
    PS_U_STEP,      /* the simulation steps */
    PS_U_CAMERA,
    PS_U_AUDIO,     /* music, sound and engine loops */
    PS_U_OTHER,     /* rumble and VMU screens */
    PS_COUNT
};
/* Work counts a frame: the renderer's, then the kart vertices lit. */
enum {
    ENG_PROF_COUNTS,
    PN_LVERTS,
    PN_COUNT
};

#define PROF_SECTION_NAMES {"mats", "light", ENG_PROF_SECTION_NAMES, "dolphins", "chunks", "sky", "sea", "foam", "upd", \
    "world", "karts", "pt", "shadows", "tr", "u-input", "u-step", "u-camera", "u-audio", "u-other"}
#define PROF_COUNT_NAMES {ENG_PROF_COUNT_NAMES, "lv"}

#endif
