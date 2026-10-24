/* Fine-grained frame profiling for the benchmark builds (DK_AUTOTEST).

   Sections accumulate CPU cycles from the SH-4's performance counter 0
   (KOS runs it in elapsed-time mode, one count per 5 ns cycle) and events
   from counter 1, whose event changes every frame: pipeline stalls on an
   operand-cache miss, stalls on the FPU, and operand-cache misses. Each
   10 s window then reports every section's time per frame and the share of
   it spent in each kind of stall. Release builds compile it all away. */
#ifndef DK_PROF_H
#define DK_PROF_H

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
    PS_PHASE_TR,
    PS_U_INPUT,     /* inside upd: controllers (and online, the probe) */
    PS_U_STEP,      /* the simulation steps */
    PS_U_CAMERA,
    PS_U_AUDIO,     /* music, sound and engine loops */
    PS_U_OTHER,     /* rumble and VMU screens */
    PS_COUNT
};
#define PROF_EVENTS 3   /* counter 1 modes, one per frame in turn */
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

#ifdef DK_AUTOTEST
#define DK_PROF 1
typedef struct { uint32_t c, e; } ProfMark;
extern uint32_t prof_cyc[PS_COUNT], prof_ev[PS_COUNT], prof_n[PN_COUNT];
static inline ProfMark prof_mark(void) {
    ProfMark m = {*(volatile uint32_t *)0xff100008, *(volatile uint32_t *)0xff100010};
    return m;
}
/* Add the time since *m to section id and restart *m from now. */
static inline void prof_lap(int id, ProfMark *m) {
    ProfMark n = prof_mark();
    prof_cyc[id] += n.c - m->c;
    prof_ev[id] += n.e - m->e;
    *m = n;
}
#define PROF_BEGIN(m) ProfMark m = prof_mark()
#define PROF_RESTART(m) (m = prof_mark())
#define PROF_LAP(id, m) prof_lap(id, &m)
#define PROF_COUNT(id, n) (prof_n[id] += (n))
#else
#define PROF_BEGIN(m) do {} while(0)
#define PROF_RESTART(m) do {} while(0)
#define PROF_LAP(id, m) do {} while(0)
#define PROF_COUNT(id, n) do {} while(0)
#endif

#endif
