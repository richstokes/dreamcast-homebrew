/* Totally Bazaar PowerVR 2D layer: one opaque background, then translucent
 * primitives drawn in painter's order (autosort disabled). */
#ifndef BAZAAR_GFX_H
#define BAZAAR_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "texture_assets.h"

#define SCREEN_W 640.0f
#define SCREEN_H 480.0f

enum { FONT_SMALL = 0, FONT_BIG = 1 };
enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

#define ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))

int gfx_init(void);
void gfx_shutdown(void);

/* Frame structure: begin, background, translucent primitives, end. */
void gfx_frame_begin(void);
void gfx_background(int tex, uint32_t tint);
void gfx_frame_end(void);

/* Additive blending for the next primitives (glows, sparkles). */
void gfx_set_additive(bool on);

void gfx_quad(int tex, const float xy[8], const float uv[8],
              const uint32_t colors[4]);
void gfx_sprite(int tex, float x, float y, float w, float h,
                float u0, float v0, float u1, float v1, uint32_t color);
/* Rotated, centred sprite. sx may be negative or tiny for flips. */
void gfx_sprite_rot(int tex, float cx, float cy, float w, float h,
                    float angle, float u0, float v0, float u1, float v1,
                    uint32_t color);
void gfx_rect(float x, float y, float w, float h, uint32_t color);
void gfx_rect_grad(float x, float y, float w, float h, uint32_t top,
                   uint32_t bottom);
/* Rounded 9-slice panel from the atlas panel cell. */
void gfx_panel(float x, float y, float w, float h, float r, uint32_t color);
/* Atlas helpers. cell: 64x64 cell index; sub: pixel rectangle. */
void gfx_cell(int cell, float cx, float cy, float size, float angle,
              uint32_t color);
void gfx_atlas_rect(int px, int py, int pw, int ph, float x, float y,
                    float w, float h, uint32_t color);

float gfx_text(int font, float x, float y, float scale, uint32_t color,
               int align, const char *text);
float gfx_textf(int font, float x, float y, float scale, uint32_t color,
                int align, const char *fmt, ...)
    __attribute__((format(printf, 7, 8)));
float gfx_text_width(int font, float scale, const char *text);
float gfx_line_height(int font, float scale);

uint32_t color_lerp(uint32_t a, uint32_t b, float t);
uint32_t color_alpha(uint32_t c, float a);

#endif
