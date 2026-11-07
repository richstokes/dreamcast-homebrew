/* Engine: fine-grained frame profiling for benchmark builds.

   Sections accumulate CPU cycles from the SH-4's performance counter 0
   (KOS runs it in elapsed-time mode, one count per 5 ns cycle) and events
   from counter 1, whose event changes every frame: pipeline stalls on an
   operand-cache miss, stalls on the FPU, and operand-cache misses. Each
   log window then reports every section's time per frame and the share of
   it spent in each kind of stall. Builds without ENG_PROF compile it all
   away. Flycast does not emulate the counters (they read 0): console only.

   The game's prof_ids.h names its sections and counts: enums PS_* ending
   PS_COUNT and PN_* ending PN_COUNT, with their log names in
   PROF_SECTION_NAMES and PROF_COUNT_NAMES (initializer lists). The
   renderer uses PS_MXFORM, PS_MSUBMIT, PN_SVERTS, PN_SEMIT, PN_SCULL,
   PN_SCLIP, PN_MVERTS, PN_MTRIS, PN_MBACK, PN_SIN, PN_SOUT, PN_STRIPS and
   PN_HDRS, so every game's list has those. ENG_PROF_TAG starts the line. */
#ifndef ENG_PROF_H
#define ENG_PROF_H

#include "engine_config.h"
#include "prof_ids.h"
#include <stdint.h>

#define PROF_EVENTS 3   /* counter 1 modes, one per frame in turn */

#ifdef ENG_PROF
typedef struct { uint32_t c, e; } ProfMark;
extern uint32_t prof_cyc[PS_COUNT], prof_ev[PS_COUNT], prof_n[PN_COUNT];
void prof_start(void);       /* counter 1 on its first event */
void prof_frame_end(void);   /* bank the frame, counter 1 on to the next event */
void prof_print(void);       /* the window's ENG_PROF_TAG line */
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
