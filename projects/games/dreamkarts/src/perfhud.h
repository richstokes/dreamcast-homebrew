/* Performance overlay, switched on from the pause menu (or from boot with
   -DDK_PERF_HUD). */
#ifndef DK_PERFHUD_H
#define DK_PERFHUD_H

#include "dk.h"

/* The pause menu's switch. Not saved: it starts off every boot. */
extern bool perfhud_on;
/* Turn the overlay on or off; on starts its figures afresh. */
void perfhud_enable(bool on);
/* Once per displayed frame, after render(): the frame's wall-clock length,
   its CPU time, the five profiled phases (update, world OP, karts, PT, TR)
   in ms, and how many vblanks it missed. */
void perfhud_frame(float dt_real, float cpu_ms, const float phase_ms[5], int missed);
/* One opaque quad at the end of the OP list, after the karts profile mark. */
void perfhud_draw(bool split);

#endif
