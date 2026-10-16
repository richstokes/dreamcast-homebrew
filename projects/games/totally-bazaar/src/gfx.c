/* Totally Bazaar PowerVR 2D layer. */
#include "gfx.h"
#include "fastmath.h"

#include <kos.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static pvr_ptr_t tex_vram[JP_TEXTURE_COUNT];
static pvr_poly_hdr_t hdr_tr[JP_TEXTURE_COUNT];
static pvr_poly_hdr_t hdr_add[JP_TEXTURE_COUNT];
static pvr_poly_hdr_t hdr_op[JP_TEXTURE_COUNT];
static pvr_poly_hdr_t hdr_pt[JP_TEXTURE_COUNT];

/* Punch-through quads (cards) are buffered while the translucent list is
 * open and submitted as their own list at the end of the frame. Every
 * primitive takes the next depth value, so painter's order still holds
 * across both lists, and the ISP drops card pixels hidden by later cards. */
#define MAX_PT_QUADS 160
typedef struct {
    int16_t tex;
    pvr_vertex_t v[4];
} pt_quad_t;
static pt_quad_t pt_quads[MAX_PT_QUADS] __attribute__((aligned(32)));
static int pt_count;
static pvr_poly_hdr_t hdr_col;
static pvr_poly_hdr_t hdr_col_add;
static const pvr_poly_hdr_t *current_hdr;
static bool additive;
static float zcount;

#define TEX_NONE (-1)

static uint32_t pvr_format(jp_tex_format_t f) {
    switch(f) {
    case JP_TEX_RGB565: return PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED;
    case JP_TEX_ARGB1555: return PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_TWIDDLED;
    default: return PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_TWIDDLED;
    }
}

int gfx_init(void) {
    pvr_init_params_t params = pvr_default_params;
    pvr_poly_cxt_t cxt;
    int i;

    vid_set_enabled(0);
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_set_dithering(true);
    params.opb_sizes[PVR_LIST_OP_POLY] = PVR_BINSIZE_8;
    params.opb_sizes[PVR_LIST_TR_POLY] = PVR_BINSIZE_32;
    params.opb_sizes[PVR_LIST_PT_POLY] = PVR_BINSIZE_16;
    params.vertex_buf_size = 768 * 1024;
    params.autosort_disabled = 1;
    params.opb_overflow_count = 4;
    if(pvr_init(&params) < 0) {
        printf("Totally Bazaar: PowerVR initialization failed.\n");
        vid_set_enabled(1);
        return -1;
    }

    for(i = 0; i < JP_TEXTURE_COUNT; ++i) {
        const jp_texture_asset_t *a = &jp_texture_assets[i];
        size_t bytes = (size_t)a->width * a->height * 2u;
        uint32_t fmt = pvr_format(a->format);
        tex_vram[i] = pvr_mem_malloc(bytes);
        if(!tex_vram[i]) {
            printf("Totally Bazaar: out of texture memory loading texture %d.\n", i);
            vid_set_enabled(1);
            return -1;
        }
        pvr_txr_load_ex(jp_textures_pak + a->offset, tex_vram[i], a->width,
                        a->height, PVR_TXRLOAD_16BPP);

        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, fmt, a->width, a->height,
                         tex_vram[i], PVR_FILTER_BILINEAR);
        cxt.gen.alpha = true;
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.shading = PVR_SHADE_GOURAUD;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.txr.alpha = (a->format == JP_TEX_RGB565) ? PVR_TXRALPHA_DISABLE
                                                     : PVR_TXRALPHA_ENABLE;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_tr[i], &cxt);
        cxt.blend.dst = PVR_BLEND_ONE;
        pvr_poly_compile(&hdr_add[i], &cxt);

        pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, fmt, a->width, a->height,
                         tex_vram[i], PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.gen.shading = PVR_SHADE_GOURAUD;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        cxt.depth.write = PVR_DEPTHWRITE_ENABLE;
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_pt[i], &cxt);

        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt, a->width, a->height,
                         tex_vram[i], PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.txr.env = PVR_TXRENV_MODULATE;
        cxt.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&hdr_op[i], &cxt);
    }

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.alpha = true;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.shading = PVR_SHADE_GOURAUD;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
    cxt.blend.src = PVR_BLEND_SRCALPHA;
    cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&hdr_col, &cxt);
    cxt.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&hdr_col_add, &cxt);

    /* Card edges are filtered 1555 alpha; keep pixels at least half opaque. */
    PVR_SET(PVR_PT_ALPHA_REF, 0x80);
    printf("Totally Bazaar: %d textures loaded, %lu KiB PVR memory free.\n",
           JP_TEXTURE_COUNT, (unsigned long)(pvr_mem_available() / 1024u));
    vid_set_enabled(1);
    return 0;
}

void gfx_shutdown(void) {
    int i;
    for(i = 0; i < JP_TEXTURE_COUNT; ++i)
        if(tex_vram[i])
            pvr_mem_free(tex_vram[i]);
    pvr_shutdown();
}

void gfx_frame_begin(void) {
    pvr_wait_ready();
    pvr_scene_begin();
    current_hdr = NULL;
    additive = false;
    zcount = 1.0f;
    pt_count = 0;
}

static void put_header(const pvr_poly_hdr_t *h) {
    if(h != current_hdr) {
        pvr_prim(h, sizeof(*h));
        current_hdr = h;
    }
}

void gfx_background(int tex, uint32_t tint) {
    pvr_vertex_t v[4];
    int i;
    pvr_list_begin(PVR_LIST_OP_POLY);
    pvr_prim(&hdr_op[tex], sizeof(pvr_poly_hdr_t));
    for(i = 0; i < 4; ++i) {
        v[i].flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v[i].x = (i & 1) ? SCREEN_W : 0.0f;
        v[i].y = (i & 2) ? SCREEN_H : 0.0f;
        v[i].z = 0.5f;
        v[i].u = (i & 1) ? 1.0f : 0.0f;
        v[i].v = (i & 2) ? 1.0f : 0.0f;
        v[i].argb = tint;
        v[i].oargb = 0;
    }
    pvr_prim(v, sizeof(v));
    pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);
    current_hdr = NULL;
}

void gfx_frame_end(void) {
    int i, tex = -1;
    pvr_list_finish();
    if(pt_count) {
        pvr_list_begin(PVR_LIST_PT_POLY);
        for(i = 0; i < pt_count; ++i) {
            if(pt_quads[i].tex != tex) {
                tex = pt_quads[i].tex;
                pvr_prim(&hdr_pt[tex], sizeof(pvr_poly_hdr_t));
            }
            pvr_prim(pt_quads[i].v, sizeof(pt_quads[i].v));
        }
        pvr_list_finish();
    }
    pvr_scene_finish();
}

void gfx_quad_solid(int tex, const float xy[8], const float uv[8], uint32_t color) {
    pt_quad_t *q;
    int i;
    zcount += 0.0001f;
    if(pt_count >= MAX_PT_QUADS)
        return;
    q = &pt_quads[pt_count++];
    q->tex = (int16_t)tex;
    for(i = 0; i < 4; ++i) {
        q->v[i].flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        q->v[i].x = xy[i * 2];
        q->v[i].y = xy[i * 2 + 1];
        q->v[i].z = zcount;
        q->v[i].u = uv[i * 2];
        q->v[i].v = uv[i * 2 + 1];
        q->v[i].argb = color;
        q->v[i].oargb = 0;
    }
}

void gfx_set_additive(bool on) {
    additive = on;
}

static void emit(const pvr_poly_hdr_t *h, const float xy[8], const float uv[8],
                 const uint32_t c[4]) {
    pvr_vertex_t v[4];
    int i;
    put_header(h);
    zcount += 0.0001f;
    for(i = 0; i < 4; ++i) {
        v[i].flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        v[i].x = xy[i * 2];
        v[i].y = xy[i * 2 + 1];
        v[i].z = zcount;
        v[i].u = uv ? uv[i * 2] : 0.0f;
        v[i].v = uv ? uv[i * 2 + 1] : 0.0f;
        v[i].argb = c[i];
        v[i].oargb = 0;
    }
    pvr_prim(v, sizeof(v));
}

/* Vertex order for strips: top-left, top-right, bottom-left, bottom-right. */
void gfx_quad(int tex, const float xy[8], const float uv[8],
              const uint32_t colors[4]) {
    const pvr_poly_hdr_t *h;
    if(tex == TEX_NONE)
        h = additive ? &hdr_col_add : &hdr_col;
    else
        h = additive ? &hdr_add[tex] : &hdr_tr[tex];
    emit(h, xy, tex == TEX_NONE ? NULL : uv, colors);
}

void gfx_sprite(int tex, float x, float y, float w, float h, float u0,
                float v0, float u1, float v1, uint32_t color) {
    const float xy[8] = { x, y, x + w, y, x, y + h, x + w, y + h };
    const float uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    const uint32_t c[4] = { color, color, color, color };
    if(color >> 24 == 0)
        return;
    gfx_quad(tex, xy, uv, c);
}

void gfx_sprite_rot(int tex, float cx, float cy, float w, float h,
                    float angle, float u0, float v0, float u1, float v1,
                    uint32_t color) {
    float s = 0.0f, c = 1.0f;
    float hw = w * 0.5f, hh = h * 0.5f;
    const float lx[4] = { -hw, hw, -hw, hw };
    const float ly[4] = { -hh, -hh, hh, hh };
    float xy[8];
    const float uv[8] = { u0, v0, u1, v0, u0, v1, u1, v1 };
    const uint32_t col[4] = { color, color, color, color };
    int i;
    if(color >> 24 == 0)
        return;
    if(angle != 0.0f)
        fm_sincos(angle, &s, &c);
    for(i = 0; i < 4; ++i) {
        xy[i * 2] = cx + lx[i] * c - ly[i] * s;
        xy[i * 2 + 1] = cy + lx[i] * s + ly[i] * c;
    }
    gfx_quad(tex, xy, uv, col);
}

void gfx_rect(float x, float y, float w, float h, uint32_t color) {
    gfx_rect_grad(x, y, w, h, color, color);
}

void gfx_rect_grad(float x, float y, float w, float h, uint32_t top,
                   uint32_t bottom) {
    const float xy[8] = { x, y, x + w, y, x, y + h, x + w, y + h };
    const uint32_t c[4] = { top, top, bottom, bottom };
    gfx_quad(TEX_NONE, xy, NULL, c);
}

void gfx_atlas_rect(int px, int py, int pw, int ph, float x, float y,
                    float w, float h, uint32_t color) {
    const float inv = 1.0f / 256.0f;
    gfx_sprite(JP_TEXID_ATLAS, x, y, w, h, px * inv, py * inv,
               (px + pw) * inv, (py + ph) * inv, color);
}

void gfx_cell(int cell, float cx, float cy, float size, float angle,
              uint32_t color) {
    const float inv = 1.0f / 256.0f;
    int px = (cell % 4) * 64, py = (cell / 4) * 64;
    gfx_sprite_rot(JP_TEXID_ATLAS, cx, cy, size, size, angle,
                   (px + 0.5f) * inv, (py + 0.5f) * inv,
                   (px + 63.5f) * inv, (py + 63.5f) * inv, color);
}

void gfx_panel(float x, float y, float w, float h, float r, uint32_t color) {
    /* Panel cell at (64,192); corners use 20px of the 64px tile. */
    const float c = 20.0f;
    const int px = 64, py = 192;
    float xs[4], ys[4];
    int us[4] = { px, px + 20, px + 44, px + 64 };
    int vs[4] = { py, py + 20, py + 44, py + 64 };
    int i, j;
    if(r * 2 > w)
        r = w * 0.5f;
    if(r * 2 > h)
        r = h * 0.5f;
    (void)c;
    xs[0] = x;
    xs[1] = x + r;
    xs[2] = x + w - r;
    xs[3] = x + w;
    ys[0] = y;
    ys[1] = y + r;
    ys[2] = y + h - r;
    ys[3] = y + h;
    for(j = 0; j < 3; ++j)
        for(i = 0; i < 3; ++i)
            gfx_sprite(JP_TEXID_ATLAS, xs[i], ys[j], xs[i + 1] - xs[i],
                       ys[j + 1] - ys[j], us[i] / 256.0f, vs[j] / 256.0f,
                       us[i + 1] / 256.0f, vs[j + 1] / 256.0f, color);
}

static const jp_font_asset_t *font_of(int font) {
    return font == FONT_BIG ? &jp_font_big : &jp_font_small;
}

float gfx_text_width(int font, float scale, const char *text) {
    const jp_font_asset_t *f = font_of(font);
    float w = 0.0f;
    for(; *text; ++text) {
        int ch = (unsigned char)*text;
        if(ch < 32 || ch > 126)
            ch = '?';
        w += f->glyphs[ch - 32].advance * scale;
    }
    return w;
}

float gfx_line_height(int font, float scale) {
    return font_of(font)->line_height * scale;
}

float gfx_text(int font, float x, float y, float scale, uint32_t color,
               int align, const char *text) {
    const jp_font_asset_t *f = font_of(font);
    const jp_texture_asset_t *t = &jp_texture_assets[f->tex];
    const float iu = 1.0f / t->width, iv = 1.0f / t->height;
    float w = gfx_text_width(font, scale, text);
    if(align == ALIGN_CENTER)
        x -= w * 0.5f;
    else if(align == ALIGN_RIGHT)
        x -= w;
    for(; *text; ++text) {
        int ch = (unsigned char)*text;
        const jp_glyph_t *g;
        if(ch < 32 || ch > 126)
            ch = '?';
        g = &f->glyphs[ch - 32];
        if(g->w)
            gfx_sprite(f->tex, x + g->ox * scale, y + g->oy * scale,
                       g->w * scale, g->h * scale, g->x * iu, g->y * iv,
                       (g->x + g->w) * iu, (g->y + g->h) * iv, color);
        x += g->advance * scale;
    }
    return w;
}

float gfx_textf(int font, float x, float y, float scale, uint32_t color,
                int align, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return gfx_text(font, x, y, scale, color, align, buf);
}

uint32_t color_lerp(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0;
    int s;
    if(t <= 0.0f)
        return a;
    if(t >= 1.0f)
        return b;
    for(s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 0xff);
        float cb = (float)((b >> s) & 0xff);
        out |= ((uint32_t)(ca + (cb - ca) * t) & 0xffu) << s;
    }
    return out;
}

uint32_t color_alpha(uint32_t c, float a) {
    float base = (float)(c >> 24);
    if(a < 0.0f)
        a = 0.0f;
    if(a > 1.0f)
        a = 1.0f;
    return (c & 0x00ffffffu) | ((uint32_t)(base * a) << 24);
}
