/* Engine: the profile sections and counts the renderer reports (prof.h).
   A game's prof_ids.h places ENG_PROF_SECTIONS in its PS_ enum and
   ENG_PROF_COUNTS in its PN_ enum, and the matching name lists at the same
   places in PROF_SECTION_NAMES and PROF_COUNT_NAMES:

       enum { PS_MINE, ENG_PROF_SECTIONS, PS_OTHER, PS_COUNT };
       #define PROF_SECTION_NAMES {"mine", ENG_PROF_SECTION_NAMES, "other"} */
#ifndef ENG_PROF_ENGINE_H
#define ENG_PROF_ENGINE_H

/* r_mesh: transform, outcode and projection; per-triangle cull and
   store-queue submit. */
#define ENG_PROF_SECTIONS PS_MXFORM, PS_MSUBMIT
#define ENG_PROF_SECTION_NAMES "mxform", "msubmit"
/* r_strip vertices transformed and emitted unclipped, strips culled whole
   and strips that took the clipping path; r_mesh vertices transformed,
   triangles submitted and back faces culled; bounded strips sent on their
   sphere and culled on it, r_strip_bounded calls, polygon headers sent. */
#define ENG_PROF_COUNTS PN_SVERTS, PN_SEMIT, PN_SCULL, PN_SCLIP, PN_MVERTS, PN_MTRIS, PN_MBACK, PN_SIN, PN_SOUT, \
    PN_STRIPS, PN_HDRS
#define ENG_PROF_COUNT_NAMES "sv", "se", "scull", "sclip", "mv", "mt", "mback", "sin", "sout", "strips", "hdrs"

#endif
