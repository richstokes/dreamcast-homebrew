/* Ridgeline's profiling sections and counts (engine/prof.h). */
#ifndef RL_PROF_IDS_H
#define RL_PROF_IDS_H

enum {
    PS_MXFORM,      /* r_mesh: transform, outcode and projection */
    PS_MSUBMIT,     /* r_mesh: cull and store-queue submit */
    PS_PHYS,        /* the trucks' physics, all substeps */
    PS_TERRAIN,     /* terrain chunks */
    PS_TREES,       /* trees, rocks and props */
    PS_TRUCKS,      /* truck meshes and lighting */
    PS_TR,          /* water, effects, sky sprites, HUD */
    PS_COUNT
};
enum {
    PN_SVERTS, PN_SEMIT, PN_SCULL, PN_SCLIP, PN_MVERTS, PN_MTRIS, PN_MBACK, PN_SIN, PN_SOUT, PN_STRIPS, PN_HDRS,
    PN_COUNT
};
#define PROF_SECTION_NAMES {"mxform", "msubmit", "phys", "terrain", "trees", "trucks", "tr"}
#define PROF_COUNT_NAMES {"sv", "se", "scull", "sclip", "mv", "mt", "mback", "sin", "sout", "strips", "hdrs"}

#endif
