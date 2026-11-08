/* The 48x32 screens of the VMUs in the controllers: a 1-bit screen to draw
   into, a 3x5 font, and sending each VMU its screen only when it changed.

   A game calls vmulcd_update() once a frame with two functions of its own:
   one says how long a VMU on a controller port waits between screens, the
   other draws that port's screen (vmulcd_update clears it first). Every
   screen is a 200-byte maple write that vmu_draw_lcd only queues, never
   waits on; a card that is busy (a save) just gets it next frame.

   DreamKarts found that 20 screens a second on every VMU crowded the bus
   until controllers dropped out, so keep idle animations to ~4 a second
   and gameplay to ~10. */
#ifndef DC_VMULCD_H
#define DC_VMULCD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMULCD_W 48
#define VMULCD_H 32

void vmulcd_pset(int x, int y);
/* Set or clear the pixels x0 <= x < x1, y0 <= y < y1. */
void vmulcd_fill(int x0, int y0, int x1, int y1);
void vmulcd_blank(int x0, int y0, int x1, int y1);
/* A 1-pixel line, both ends included. */
void vmulcd_line(int x0, int y0, int x1, int y1);
/* A 1-bit sprite of h rows, bit 63 of each the leftmost pixel. _cut first
   clears a pixel round it so it stands out from what is behind. */
void vmulcd_bits(const uint64_t *rows, int h, int x, int y);
void vmulcd_bits_cut(const uint64_t *rows, int h, int x, int y);
/* 3x5 capitals, digits and a little punctuation; 1 pixel between letters. */
int vmulcd_text_w(const char *s);
void vmulcd_text(int x, int y, const char *s);
void vmulcd_text_c(int y, const char *s);   /* centred across the screen */

/* Once a frame: for each VMU in a controller whose gap has passed, draw its
   screen and send it if it changed. */
void vmulcd_update(float dt, float (*gap)(int port), void (*draw)(int port));

#ifdef __cplusplus
}
#endif

#endif
