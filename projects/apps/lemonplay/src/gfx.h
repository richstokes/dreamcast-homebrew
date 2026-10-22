/* LemonPlay drawing: flat rectangles, BIOS-font text and the video picture,
   all as PowerVR quads on a 640x480 screen. */
#ifndef LEMONPLAY_GFX_H
#define LEMONPLAY_GFX_H

#include <dc/pvr.h>
#include <stdint.h>

#define SCREEN_W 640
#define SCREEN_H 480
#define FONT_W 12
#define FONT_H 24

/* Colours are 0xAARRGGBB. */
#define COL_BG        0xff15130cu
#define COL_PANEL     0xff221f14u
#define COL_LEMON     0xfff7d33cu
#define COL_LEMON_DIM 0xff8f7a24u
#define COL_TEXT      0xffeeeadcu
#define COL_MUTED     0xff9a9686u
#define COL_ERROR     0xffff7a5cu
#define COL_SELECT    0xff3b3418u

void gfx_init(void);

/* One frame: begin, then any number of opaque/translucent draws (each call
   picks its list), then end. Opaque draws must all come before the first
   translucent one. */
void gfx_begin(void);
void gfx_end(void);
/* True when the PVR can take a new frame without blocking. */
int gfx_ready(void);

void gfx_rect(float x, float y, float w, float h, uint32_t argb);       /* opaque */
void gfx_rect_alpha(float x, float y, float w, float h, uint32_t argb); /* translucent */
/* Draws ISO-8859-1 text; returns the x after the last glyph. */
float gfx_text(float x, float y, uint32_t argb, float scale, const char *text);
/* Like gfx_text but stops at max_w pixels, ending with "..." if cut. */
void gfx_text_clip(float x, float y, uint32_t argb, float scale, const char *text, float max_w);
float gfx_text_width(const char *text, float scale);
/* A textured opaque quad, for the video picture. */
void gfx_textured(const pvr_poly_hdr_t *hdr, float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1);

/* UTF-8 to the BIOS font's ISO-8859-1, '?' for anything it can't show. */
void utf8_to_latin1(char *dst, size_t size, const char *src);

#endif
