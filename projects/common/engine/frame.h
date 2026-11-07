/* Engine: frame pacing.

   Physics runs one step per displayed frame. pvr_wait_ready() paces the
   loop to the display, whose period is not 1/60 s (16.68 ms on an NTSC
   console, whatever the host gives in Flycast), so a fixed 1/60 step would
   owe the clock a whole step every few seconds and run twice in one frame:
   a one-frame hitch of everything. The step is the measured vblank period
   instead, nudged by up to 1% toward the time still owed so game time stays
   on the clock; only a frame that missed vblanks takes more than one step,
   and a stall forgets its time rather than replaying it. */
#ifndef ENG_FRAME_H
#define ENG_FRAME_H

#include "engine_config.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint64_t t0;      /* when the frame began (us), after the wait for the display */
    float dt_real;    /* seconds since the last frame began, at most 0.1 */
    float dt;         /* the step each of this frame's updates takes */
    int steps;        /* updates to run: 1, more after missed vblanks, at most 4 */
    float vbl_dt;     /* the display's frame period as measured, smoothed */
    float fps;        /* frames over the last second */
    float cpu_ms;     /* the last frame's work, frame_begin to frame_end */
} EngFrame;
extern EngFrame eng_frame;

/* Before the first frame: starts the clock. */
void frame_init(void);
/* Wait for the display, then set this frame's step, count and timings. A
   test build may overwrite dt, dt_real and steps before stepping (a frame
   held for a screenshot takes 0 steps). */
void frame_begin(void);
/* After rendering: cpu_ms, and the FPS counter. True on the frame the
   counter rolls over, once a second. */
bool frame_end(void);
/* Vblanks a frame of dt_real seconds missed. */
static inline int frame_missed(float dt_real) {
    return dt_real > 1.5f * eng_frame.vbl_dt ? (int)(dt_real / eng_frame.vbl_dt + 0.5f) - 1 : 0;
}

#endif
