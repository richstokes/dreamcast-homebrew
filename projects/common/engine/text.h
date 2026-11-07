/* Engine: bitmap text from a font atlas (one glyph row per FONT_ROW_H
   texels, ASCII 32-127), as the games' build_textures.py write it: per
   glyph its atlas x and row, rect width, rect left relative to the pen and
   the pen advance. Drawn as 2D quads in whichever list is open. */
#ifndef ENG_TEXT_H
#define ENG_TEXT_H

#include "render.h"

typedef struct { uint16_t x; uint8_t row, w; int8_t ox; uint8_t adv; } eng_glyph_t;

typedef struct {
    const pvr_poly_hdr_t *hdr;
    const eng_glyph_t *glyphs;     /* 96: ASCII 32..127 */
    int row_h, tex_w, tex_h;
    int cap_top, cap_bottom;       /* the capitals' rows within a glyph row */
} EngFont;

enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

float text_width(const EngFont *f, const char *s, float scale);
/* y is the top of the glyph row; z above the scene (2D overlay depth). */
void text_draw(const EngFont *f, float x, float y, float scale, int align, uint32_t col, const char *s);
/* The same with the capitals centred on cy. */
void text_draw_c(const EngFont *f, float x, float cy, float scale, int align, uint32_t col, const char *s);
/* One character centred on (cx, cy) by its drawn rect rather than its
   advance: a capital's rect (outline and all) runs wider than its pen
   advance, so advance centring lands it right of a button's middle. */
void text_letter_c(const EngFont *f, float cx, float cy, float scale, uint32_t col, char c);

#endif
