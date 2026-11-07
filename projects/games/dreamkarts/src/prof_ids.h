/* DreamKarts' profiling sections and counts for the engine profiler
   (projects/common/engine/prof.h): what the hwbench's DK PC lines report. */
#ifndef DK_PROF_IDS_H
#define DK_PROF_IDS_H

#include <stdint.h>

enum {
    PS_MATS,        /* update_kart_mats: part matrices and part-space sun */
    PS_LIGHT,       /* per-vertex kart lighting */
    PS_MXFORM,      /* r_mesh: transform, outcode and projection */
    PS_MSUBMIT,     /* r_mesh: per-triangle cull and store-queue submit */
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
/* Work counts a frame. */
enum {
    PN_SVERTS,      /* r_strip vertices transformed */
    PN_SEMIT,       /* r_strip vertices emitted unclipped */
    PN_SCULL,       /* r_strip calls culled whole */
    PN_SCLIP,       /* r_strip calls that took the clipping path */
    PN_MVERTS,      /* r_mesh vertices transformed */
    PN_MTRIS,       /* r_mesh triangles submitted */
    PN_MBACK,       /* r_mesh triangles culled as back faces */
    PN_LVERTS,      /* kart vertices lit */
    PN_SIN,         /* bounded strips sent whole on their sphere */
    PN_SOUT,        /* bounded strips culled on their sphere */
    PN_STRIPS,      /* r_strip_bounded calls */
    PN_HDRS,        /* polygon headers sent */
    PN_COUNT
};

/* Every build: accumulated microseconds per phase (update, world OP,
   karts, PT, TR), timed by main.c for the performance overlay; the
   autotest's log line reads and zeroes them. */
extern uint64_t prof_us[6];
extern int prof_frames;

#define PROF_SECTION_NAMES {"mats", "light", "mxform", "msubmit", "dolphins", "chunks", "sky", "sea", "foam", "upd", \
    "world", "karts", "pt", "shadows", "tr", "u-input", "u-step", "u-camera", "u-audio", "u-other"}
#define PROF_COUNT_NAMES {"sv", "se", "scull", "sclip", "mv", "mt", "mback", "lv", "sin", "sout", "strips", "hdrs"}

#endif
