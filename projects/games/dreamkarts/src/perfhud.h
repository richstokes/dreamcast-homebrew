/* Performance overlay and frame rate counter, picked from the pause menu
   (or the overlay from boot with -DDK_PERF_HUD). */
#ifndef DK_PERFHUD_H
#define DK_PERFHUD_H

#include "dk.h"

/* The pause menu's PERF setting, in the order it steps through. Not
   saved: it starts off every boot. */
enum { PERF_OFF, PERF_OVERLAY, PERF_FPS, PERF_MODES };
extern int perfhud_mode;
/* Switching to the overlay starts its figures afresh. */
void perfhud_set_mode(int mode);
/* Once per displayed frame, after render(): the frame's wall-clock length,
   its CPU time, the five profiled phases (update, world OP, karts, PT, TR)
   in ms, and how many vblanks it missed. */
void perfhud_frame(float dt_real, float cpu_ms, const float phase_ms[5], int missed);
/* At boot, straight after the game's textures: takes the overlay's
   128 KiB texture from VRAM up front, so textures that leave too little
   room fail where everyone sees it, not the first time someone switches
   the overlay on. */
void perfhud_reserve(void);
/* One opaque quad at the end of the OP list, after the karts profile mark. */
void perfhud_draw(bool split);
/* The FPS setting's counter, in the TR list after the HUD. */
void perfhud_draw_fps(bool split);

#endif
