/* Engine: bitmap text (text.h). */
#include "text.h"

#define TEXT_Z 20.0f

static inline const eng_glyph_t *glyph(const EngFont *f, unsigned c) {
    if(c < 32 || c > 127) c = '?';
    return &f->glyphs[c - 32];
}

float text_width(const EngFont *f, const char *s, float scale) {
    float w = 0;
    for(; *s; s++) w += glyph(f, (unsigned char)*s)->adv * scale;
    return w;
}

void text_draw(const EngFont *f, float x, float y, float scale, int align, uint32_t col, const char *s) {
    if(align != ALIGN_LEFT) {
        float w = text_width(f, s, scale);
        x -= align == ALIGN_CENTER ? w * 0.5f : w;
    }
    r_hdr(f->hdr);
    const float du = 1.0f / f->tex_w, dv = (float)f->row_h / f->tex_h;
    for(; *s; s++) {
        const eng_glyph_t *g = glyph(f, (unsigned char)*s);
        if(g->w) {
            float x0 = x + g->ox * scale, u0 = g->x * du, v0 = g->row * dv;
            r_quad2d(x0, y, x0 + g->w * scale, y + f->row_h * scale, u0, v0, u0 + g->w * du, v0 + dv, col, TEXT_Z);
        }
        x += g->adv * scale;
    }
}

void text_draw_c(const EngFont *f, float x, float cy, float scale, int align, uint32_t col, const char *s) {
    text_draw(f, x, cy - (f->cap_top + f->cap_bottom) * 0.5f * scale, scale, align, col, s);
}

void text_letter_c(const EngFont *f, float cx, float cy, float scale, uint32_t col, char c) {
    const eng_glyph_t *g = glyph(f, (unsigned char)c);
    char s[2] = {c, 0};
    text_draw_c(f, cx - (g->ox + g->w * 0.5f) * scale, cy, scale, ALIGN_LEFT, col, s);
}
