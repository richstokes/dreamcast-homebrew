/* Engine: the test builds' shared pieces. A shot line tells
   tools/capture.py to grab the Flycast window; a QaWindow gathers the
   frame statistics a log window reports (frames, dropped vblanks, steps,
   CPU and PVR render time, the vertex buffer's high-water mark). The
   game's engine_config.h names the log prefix in ENG_QA_TAG ("DK"). */
#ifndef ENG_QA_H
#define ENG_QA_H

#include "engine_config.h"
#include <stdint.h>

/* "<ENG_QA_TAG> SHOT <name>", name printf-formatted. */
void qa_shot(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

typedef struct {
    float t;                  /* seconds in the window */
    int frames;
    int missed;               /* displayed frames over 1.5 vblanks */
    int multi, zero;          /* frames that ran 2+ or 0 steps */
    float cpu_sum, cpu_max;   /* the game's ms a frame */
    float rnd_sum, rnd_max;   /* the PVR's render ms (pvr_get_stats) */
    uint32_t vtx_max;         /* the vertex buffer's peak, bytes */
} QaWindow;

/* Add a frame; returns its PVR render time in ms (0 before the first). */
float qa_window_frame(QaWindow *w, float dt_real, int steps, float cpu_ms);
void qa_window_reset(QaWindow *w);

#endif
