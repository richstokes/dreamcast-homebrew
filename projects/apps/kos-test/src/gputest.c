/* GPU feature test: one page per PowerVR feature, LEFT/RIGHT to change page.
   Textures are made on first use and kept, as are the compiled headers. */
#include "gfx.h"
#include "gpu.h"
#include "kt.h"

#include <dc/pvr.h>
#include <dc/video.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *title;
    const char *hint;           /* what A/X do on this page */
    void (*enter)(void);
    void (*update)(const kt_input_t *in);
    void (*draw)(void);
    void (*leave)(void);
} page_t;

static int page;
static uint32_t page_frames;
static char status[80];

static uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | ((b & 0xff) >> 3));
}

static uint32_t argb(int a, int r, int g, int b) {
    return (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
}

static uint32_t hue(float h) {
    h = fmodf(h, 1.0f) * 6.0f;
    int i = (int)h;
    float f = h - i;
    int up = (int)(f * 255), down = 255 - up;
    switch(i) {
        case 0: return argb(255, 255, up, 0);
        case 1: return argb(255, down, 255, 0);
        case 2: return argb(255, 0, 255, up);
        case 3: return argb(255, 0, down, 255);
        case 4: return argb(255, up, 0, 255);
        default: return argb(255, 255, 0, down);
    }
}

/* Texel (x, y) of a square twiddled texture. */
static uint32_t twid(uint32_t x, uint32_t y) {
    uint32_t r = 0;
    for(int b = 0; b < 10; b++)
        r |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1);
    return r;
}

static void compile_txr(pvr_poly_hdr_t *h, pvr_list_t list, int fmt, int w, int ht,
                        pvr_ptr_t t, pvr_filter_mode_t filter) {
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_txr(&cxt, list, fmt, w, ht, t, filter);
    cxt.gen.culling = PVR_CULLING_NONE;
    if(list == PVR_LIST_TR_POLY) {
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = false;
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    }
    else
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(h, &cxt);
}

static void label(float cx, float y, const char *s) {
    gfx_text_center(cx, y, C_TEXT, s, false);
}

/* ---- Colour spectrum ---- */

static bool dither = true;

static void color_enter(void) {
    vid_set_dithering(dither);
}

static void color_update(const kt_input_t *in) {
    if(in->pressed & CONT_A) {
        dither = !dither;
        vid_set_dithering(dither);
    }
}

static void color_draw(void) {
    ui_background();
    for(int i = 0; i < 6; i++) {
        float x = 20 + i * 100;
        uint32_t a = hue(i / 6.0f), b = hue((i + 1) / 6.0f);
        gfx_op_grad4(x, 50, 100, 60, 2, 0xffffffff, 0xffffffff, a, b);
        gfx_op_grad4(x, 110, 100, 60, 2, a, b, 0xff000000, 0xff000000);
    }
    static const uint32_t ramp[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
    static const char *const names[4] = { "R", "G", "B", "W" };
    for(int k = 0; k < 4; k++) {
        float y = 182 + k * 46;
        gfx_op_grad4(40, y, 580, 18, 2, 0xff000000, ramp[k], 0xff000000, ramp[k]);
        for(int s = 0; s < 32; s++) {
            int v = s * 255 / 31;
            uint32_t c = 0xff000000 | (ramp[k] & 0xff0000 ? (uint32_t)v << 16 : 0) |
                         (ramp[k] & 0xff00 ? (uint32_t)v << 8 : 0) | (ramp[k] & 0xff ? (uint32_t)v : 0);
            gfx_op_rect(40 + s * 580.0f / 32, y + 20, 580.0f / 32 + 0.5f, 16, 2, c);
        }
        gfx_text(20, y + 10, C_TEXT, names[k]);
    }
    static const uint32_t sw[8] = { 0xff000000, 0xffff0000, 0xff00ff00, 0xffffff00,
                                    0xff0000ff, 0xffff00ff, 0xff00ffff, 0xffffffff };
    static const char *const swn[8] = { "BLACK", "RED", "GREEN", "YELLOW", "BLUE", "MAGENTA",
                                        "CYAN", "WHITE" };
    for(int i = 0; i < 8; i++) {
        gfx_op_rect(24 + i * 76, 372, 68, 44, 2, sw[i]);
        label(58 + i * 76, 420, swn[i]);
    }
    snprintf(status, sizeof(status), "DITHERING %s (16-BIT FRAMEBUFFER)", dither ? "ON" : "OFF");
}

/* ---- Texture formats ---- */

#define TS 64
static pvr_ptr_t fmt_tex[9];
static pvr_poly_hdr_t fmt_hdr[9];
static const char *const fmt_names[9] = {
    "ARGB1555", "RGB565", "ARGB4444", "YUV422", "PAL 4BPP", "PAL 8BPP", "VQ 565",
    "565 LINEAR", "STRIDE 96",
};

static void src_px(int x, int y, int w, int *r, int *g, int *b, int *a) {
    *r = x * 255 / (w - 1);
    *g = y * 4 > 255 ? 255 : y * 4;
    *b = 255 - (x * 128 / w + y * 2);
    if(((x / 8) ^ (y / 8)) & 1) { *r = *r * 3 / 4; *g = *g * 3 / 4; *b = *b * 3 / 4; }
    float dx = x - w / 2.0f + 0.5f, dy = y - 31.5f, d = sqrtf(dx * dx + dy * dy);
    *a = d < 22 ? 255 : d > 31 ? 0 : (int)(255 * (31 - d) / 9);
}

static void formats_build(void) {
    uint16_t *px = malloc(TS * TS * 2);
    uint8_t *ix = malloc(TS * TS);
    int r, g, b, a;

    pvr_set_pal_format(PVR_PAL_ARGB8888);
    for(int f = 0; f < 4; f++) {
        for(int y = 0; y < TS; y++)
            for(int x = 0; x < TS; x++) {
                src_px(x, y, TS, &r, &g, &b, &a);
                uint16_t v;
                if(f == 0) v = (uint16_t)((a >= 128) << 15 | (r >> 3) << 10 | (g >> 3) << 5 | b >> 3);
                else if(f == 1) v = rgb565(r, g, b);
                else if(f == 2) v = (uint16_t)((a >> 4) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | b >> 4);
                else {
                    /* YUV422: texel pairs share U (even) and V (odd) */
                    int x0 = x & ~1, r2, g2, b2, a2, rr, gg, bb;
                    src_px(x0, y, TS, &rr, &gg, &bb, &a2);
                    src_px(x0 + 1, y, TS, &r2, &g2, &b2, &a2);
                    int Y = (int)(0.299f * r + 0.587f * g + 0.114f * b);
                    int U = (int)(-0.169f * (rr + r2) / 2 - 0.331f * (gg + g2) / 2 + 0.5f * (bb + b2) / 2 + 128);
                    int V = (int)(0.5f * (rr + r2) / 2 - 0.419f * (gg + g2) / 2 - 0.081f * (bb + b2) / 2 + 128);
                    v = (uint16_t)(Y << 8 | ((x & 1) ? V : U));
                }
                px[y * TS + x] = v;
            }
        fmt_tex[f] = pvr_mem_malloc(TS * TS * 2);
        if(f == 3)
            pvr_txr_load(px, fmt_tex[f], TS * TS * 2);
        else
            pvr_txr_load_ex(px, fmt_tex[f], TS, TS, PVR_TXRLOAD_16BPP);
    }
    static const int fmts[4] = { PVR_TXRFMT_ARGB1555, PVR_TXRFMT_RGB565, PVR_TXRFMT_ARGB4444,
                                 PVR_TXRFMT_YUV422 | PVR_TXRFMT_NONTWIDDLED };
    for(int f = 0; f < 4; f++)
        compile_txr(&fmt_hdr[f], PVR_LIST_TR_POLY, fmts[f], TS, TS, fmt_tex[f], PVR_FILTER_BILINEAR);

    /* 4bpp: 16 colours in palette entries 0-15, low nibble first */
    for(int i = 0; i < 16; i++)
        pvr_set_pal_entry(i, hue(i / 16.0f) & (i == 0 ? 0x00ffffff : 0xffffffff));
    memset(ix, 0, TS * TS);
    for(int y = 0; y < TS; y++)
        for(int x = 0; x < TS; x++) {
            int v = ((x / 4) + (y / 4)) & 15;
            ix[(y * TS + x) >> 1] |= (uint8_t)(x & 1 ? v << 4 : v);
        }
    fmt_tex[4] = pvr_mem_malloc(TS * TS / 2);
    pvr_txr_load_ex(ix, fmt_tex[4], TS, TS, PVR_TXRLOAD_4BPP);
    compile_txr(&fmt_hdr[4], PVR_LIST_TR_POLY, PVR_TXRFMT_PAL4BPP | PVR_TXRFMT_4BPP_PAL(0), TS,
                TS, fmt_tex[4], PVR_FILTER_BILINEAR);

    /* 8bpp: bank 1 (entries 256-511) */
    for(int i = 0; i < 256; i++)
        pvr_set_pal_entry(256 + i, hue(i / 256.0f));
    for(int y = 0; y < TS; y++)
        for(int x = 0; x < TS; x++)
            ix[y * TS + x] = (uint8_t)((int)(128 + 64 * sinf(x * 0.2f) + 64 * sinf(y * 0.15f + x * 0.05f)));
    fmt_tex[5] = pvr_mem_malloc(TS * TS);
    pvr_txr_load_ex(ix, fmt_tex[5], TS, TS, PVR_TXRLOAD_8BPP);
    compile_txr(&fmt_hdr[5], PVR_LIST_TR_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(1), TS,
                TS, fmt_tex[5], PVR_FILTER_BILINEAR);

    /* VQ: a 256-entry codebook of 2x2 blocks picked from the image, then
       every block mapped to its nearest entry. Entries hold their texels in
       twiddled order: top-left, bottom-left, top-right, bottom-right. */
    uint16_t book[256][4];
    int blk[32 * 32][4][3];
    for(int by = 0; by < 32; by++)
        for(int bx = 0; bx < 32; bx++)
            for(int k = 0; k < 4; k++) {
                int x = bx * 2 + (k >> 1), y = by * 2 + (k & 1);
                src_px(x, y, TS, &r, &g, &b, &a);
                blk[by * 32 + bx][k][0] = r;
                blk[by * 32 + bx][k][1] = g;
                blk[by * 32 + bx][k][2] = b;
            }
    for(int e = 0; e < 256; e++)
        for(int k = 0; k < 4; k++) {
            int *c = blk[e * 4 + (e & 3)][k];
            book[e][k] = rgb565(c[0], c[1], c[2]);
        }
    uint8_t idx[32 * 32];
    for(int i = 0; i < 32 * 32; i++) {
        int best = 0, best_d = 1 << 30;
        for(int e = 0; e < 256; e++) {
            int d = 0;
            int *ref = blk[e * 4 + (e & 3)][0];
            for(int k = 0; k < 4; k++)
                for(int c = 0; c < 3; c++) {
                    int t = blk[i][k][c] - ref[k * 3 + c];
                    d += t * t;
                }
            if(d < best_d) { best_d = d; best = e; }
        }
        idx[i] = (uint8_t)best;
    }
    uint8_t tw[32 * 32];
    for(int y = 0; y < 32; y++)
        for(int x = 0; x < 32; x++)
            tw[twid((uint32_t)x, (uint32_t)y)] = idx[y * 32 + x];
    fmt_tex[6] = pvr_mem_malloc(2048 + 1024);
    pvr_txr_load(book, fmt_tex[6], 2048);
    pvr_txr_load(tw, (uint8_t *)fmt_tex[6] + 2048, 1024);
    compile_txr(&fmt_hdr[6], PVR_LIST_TR_POLY, PVR_TXRFMT_VQ_ENABLE | PVR_TXRFMT_RGB565, TS, TS,
                fmt_tex[6], PVR_FILTER_BILINEAR);

    /* RGB565 left linear (not twiddled) */
    for(int y = 0; y < TS; y++)
        for(int x = 0; x < TS; x++) {
            src_px(x, y, TS, &r, &g, &b, &a);
            px[y * TS + x] = rgb565(r, g, b);
        }
    fmt_tex[7] = pvr_mem_malloc(TS * TS * 2);
    pvr_txr_load(px, fmt_tex[7], TS * TS * 2);
    compile_txr(&fmt_hdr[7], PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, TS,
                TS, fmt_tex[7], PVR_FILTER_BILINEAR);

    /* 96 wide: a multiple of 32 that is not a power of two */
    uint16_t *wide = malloc(96 * TS * 2);
    for(int y = 0; y < TS; y++)
        for(int x = 0; x < 96; x++) {
            src_px(x, y, 96, &r, &g, &b, &a);
            wide[y * 96 + x] = rgb565(r, g, b);
        }
    fmt_tex[8] = pvr_mem_malloc(96 * TS * 2);
    pvr_txr_load(wide, fmt_tex[8], 96 * TS * 2);
    compile_txr(&fmt_hdr[8], PVR_LIST_TR_POLY,
                PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED | PVR_TXRFMT_X32_STRIDE, 128, TS,
                fmt_tex[8], PVR_FILTER_BILINEAR);
    free(wide);
    free(px);
    free(ix);
}

static void formats_enter(void) {
    if(!fmt_tex[0])
        formats_build();
    pvr_txr_set_stride(96);
}

static void checker_bg(void) {
    for(int y = 40; y < 448; y += 16)
        for(int x = 0; x < SCREEN_W; x += 16)
            gfx_op_rect(x, y, 16, 16, 2, ((x ^ y) & 16) ? 0xff505050 : 0xff303030);
}

static void formats_draw(void) {
    ui_background();
    checker_bg();
    for(int i = 0; i < 9; i++) {
        float x = 14 + (i % 5) * 124, y = 62 + (i / 5) * 184;
        float u1 = i == 8 ? 96.0f / 128.0f : 1.0f;
        gfx_quad(PVR_LIST_TR_POLY, &fmt_hdr[i], x, y, x + 112, y + 112, 5, 0, 0, u1, 1,
                 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
        gfx_rect(x - 2, y + 116, 116, 20, 0xc0000000);
        label(x + 56, y + 118, fmt_names[i]);
    }
    gfx_text(512, 260, C_DIM, "Alpha formats");
    gfx_text(512, 278, C_DIM, "show the");
    gfx_text(512, 296, C_DIM, "checkerboard");
    gfx_text(512, 314, C_DIM, "through a disc.");
    snprintf(status, sizeof(status), "TWIDDLED UNLESS MARKED. PALETTES IN ARGB8888.");
}

/* ---- Filtering and mipmaps ---- */

static pvr_ptr_t mip_tex[2];
static pvr_poly_hdr_t filt_hdr[3];
static bool mip_colored;
static int mip_bias = PVR_MIPBIAS_NORMAL;

/* Offsets of each level in a 16-bit twiddled mipmap chain, smallest first. */
static uint32_t mip_offset(int size) {
    uint32_t off = 6;
    for(int s = 1; s < size; s <<= 1)
        off += (uint32_t)(s * s * 2);
    return off;
}

static void mips_build(void) {
    static const uint32_t tint[9] = { 0xffffff, 0xff4040, 0x40ff40, 0x4040ff, 0xffff40,
                                      0xff40ff, 0x40ffff, 0xff8020, 0x8080ff };
    for(int t = 0; t < 2; t++) {
        mip_tex[t] = pvr_mem_malloc(0x2aab0);
        volatile uint16_t *base = (volatile uint16_t *)mip_tex[t];
        int level = 0;
        for(int size = 256; size >= 1; size >>= 1, level++) {
            volatile uint16_t *dst = (volatile uint16_t *)((uint8_t *)base + mip_offset(size));
            uint32_t ti = tint[level];
            for(int y = 0; y < size; y++)
                for(int x = 0; x < size; x++) {
                    /* the 32-texel checker at full size, averaged when smaller */
                    int cell = 256 / size;
                    int on = cell <= 32 ? (((x * cell) / 32 ^ (y * cell) / 32) & 1) : -1;
                    int v = on < 0 ? 160 : on ? 235 : 70;
                    int r = v, g = v, b = v;
                    if(t) {
                        r = r * (int)(ti >> 16 & 255) / 255;
                        g = g * (int)(ti >> 8 & 255) / 255;
                        b = b * (int)(ti & 255) / 255;
                    }
                    dst[twid((uint32_t)x, (uint32_t)y)] = rgb565(r, g, b);
                }
        }
    }
}

static void filt_compile(void) {
    pvr_poly_cxt_t cxt;
    pvr_ptr_t t = mip_tex[mip_colored];

    for(int i = 0; i < 3; i++) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256,
                         t, i == 0 ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        if(i == 2) {
            cxt.txr.mipmap = true;
            cxt.txr.mipmap_bias = (pvr_mip_bias_t)mip_bias;
        }
        else {
            /* without mipmaps the top level starts at the chain's end */
            cxt.txr.base = (pvr_ptr_t)((uint8_t *)t + mip_offset(256));
        }
        pvr_poly_compile(&filt_hdr[i], &cxt);
    }
}

static void filt_enter(void) {
    if(!mip_tex[0])
        mips_build();
    filt_compile();
}

static void filt_update(const kt_input_t *in) {
    if(in->pressed & CONT_A) {
        mip_colored = !mip_colored;
        filt_compile();
    }
    if(in->pressed & CONT_X) {
        mip_bias = mip_bias % 15 + 1;
        filt_compile();
    }
}

static void floor_strip(const pvr_poly_hdr_t *h, float cx) {
    const float near_w = 100, far_w = 6, near_y = 440, far_y = 176, depth = 24;

    gfx_hdr(PVR_LIST_OP_POLY, h);
    pvr_vertex_t *v = gfx_alloc(PVR_LIST_OP_POLY, 4 * sizeof(pvr_vertex_t));
    if(!v)
        return;
    gfx_vert(&v[0], PVR_CMD_VERTEX, cx - far_w, far_y, 1.0f / depth, 0, depth, 0xffffffff, 0);
    gfx_vert(&v[1], PVR_CMD_VERTEX, cx + far_w, far_y, 1.0f / depth, 2, depth, 0xffffffff, 0);
    gfx_vert(&v[2], PVR_CMD_VERTEX, cx - near_w, near_y, 1.0f, 0, 0, 0xffffffff, 0);
    gfx_vert(&v[3], PVR_CMD_VERTEX_EOL, cx + near_w, near_y, 1.0f, 2, 0, 0xffffffff, 0);
}

static void filt_draw(void) {
    gfx_op_grad4(0, 40, SCREEN_W, 140, 0.01f, 0xff2050a0, 0xff2050a0, 0xff90b0e0, 0xff90b0e0);
    gfx_op_rect(0, 176, SCREEN_W, 272, 0.01f, 0xff101010);
    floor_strip(&filt_hdr[0], 110);
    floor_strip(&filt_hdr[1], 320);
    floor_strip(&filt_hdr[2], 530);
    label(110, 56, "NEAREST");
    label(320, 56, "BILINEAR");
    label(530, 56, "BILINEAR + MIPMAPS");
    snprintf(status, sizeof(status), "A: %s MIP LEVELS   X: MIP BIAS %.2f",
             mip_colored ? "COLOURED" : "PLAIN", mip_bias * 0.25);
}

/* ---- Blend modes ---- */

static pvr_poly_hdr_t blend_hdr[8][8];
static const char *const blend_names[8] = { "0", "1", "DC", "1-DC", "SA", "1-SA", "DA", "1-DA" };

static void blend_enter(void) {
    static bool done;
    pvr_poly_cxt_t cxt;

    if(done)
        return;
    done = true;
    for(int s = 0; s < 8; s++)
        for(int d = 0; d < 8; d++) {
            pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
            cxt.blend.src = (pvr_blend_mode_t)s;
            cxt.blend.dst = (pvr_blend_mode_t)d;
            cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
            cxt.depth.write = false;
            cxt.gen.culling = PVR_CULLING_NONE;
            pvr_poly_compile(&blend_hdr[s][d], &cxt);
        }
}

static void blend_draw(void) {
    ui_background();
    for(int s = 0; s < 8; s++)
        for(int d = 0; d < 8; d++) {
            float x = 92 + d * 66, y = 64 + s * 46;
            gfx_op_grad4(x, y, 62, 42, 2, 0xff2040ff, 0xffffff40, 0xff2040ff, 0xffffff40);
            gfx_quad(PVR_LIST_TR_POLY, &blend_hdr[s][d], x + 6, y + 6, x + 56, y + 36, 5, 0, 0,
                     0, 0, 0x80ff6030, 0x80ff6030, 0x20ff6030, 0x20ff6030);
        }
    for(int i = 0; i < 8; i++) {
        gfx_text_center(92 + i * 66 + 31, 46, C_HI, blend_names[i], false);
        gfx_text_right(84, 64 + i * 46 + 13, C_HI, blend_names[i]);
    }
    snprintf(status, sizeof(status), "ROWS: SOURCE FACTOR   COLUMNS: DESTINATION FACTOR");
}

/* ---- Texture environment and UV modes ---- */

static pvr_poly_hdr_t env_hdr[4], uv_hdr[4];
static pvr_ptr_t alpha_tex;

static void env_enter(void) {
    static const pvr_txr_shading_mode_t envs[4] = {
        PVR_TXRENV_REPLACE, PVR_TXRENV_MODULATE, PVR_TXRENV_DECAL, PVR_TXRENV_MODULATEALPHA
    };
    static const pvr_uv_flip_t flips[4] = { PVR_UVFLIP_NONE, PVR_UVFLIP_U, PVR_UVFLIP_UV, PVR_UVFLIP_NONE };
    pvr_poly_cxt_t cxt;

    if(alpha_tex)
        return;
    uint16_t *px = malloc(TS * TS * 2);
    for(int y = 0; y < TS; y++)
        for(int x = 0; x < TS; x++) {
            int a = x * 15 / (TS - 1), v = ((x / 8 + y / 8) & 1) ? 15 : 8;
            px[y * TS + x] = (uint16_t)(a << 12 | v << 8 | (y / 4) << 4 | (15 - y / 4));
        }
    alpha_tex = pvr_mem_malloc(TS * TS * 2);
    pvr_txr_load_ex(px, alpha_tex, TS, TS, PVR_TXRLOAD_16BPP);
    free(px);
    for(int i = 0; i < 4; i++) {
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444, TS, TS, alpha_tex,
                         PVR_FILTER_BILINEAR);
        cxt.txr.env = envs[i];
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = false;
        cxt.gen.culling = PVR_CULLING_NONE;
        pvr_poly_compile(&env_hdr[i], &cxt);
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565, 256, 256, gpu_test_texture(),
                         PVR_FILTER_BILINEAR);
        cxt.txr.uv_flip = flips[i];
        cxt.txr.uv_clamp = i == 3 ? PVR_UVCLAMP_UV : PVR_UVCLAMP_NONE;
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        pvr_poly_compile(&uv_hdr[i], &cxt);
    }
}

static void env_draw(void) {
    static const char *const en[4] = { "REPLACE", "MODULATE", "DECAL", "MODULATE ALPHA" };
    static const char *const un[4] = { "REPEAT", "FLIP U", "FLIP U+V", "CLAMP U+V" };

    ui_background();
    checker_bg();
    for(int i = 0; i < 4; i++) {
        float x = 20 + i * 156;
        gfx_quad(PVR_LIST_TR_POLY, &env_hdr[i], x, 70, x + 140, 200, 5, 0, 0, 1, 1,
                 0xc0ff4040, 0xc040ff40, 0xc04040ff, 0xc0ffffff);
        gfx_rect(x, 204, 140, 20, 0xc0000000);
        label(x + 70, 206, en[i]);
        gfx_quad(PVR_LIST_OP_POLY, &uv_hdr[i], x, 250, x + 140, 390, 5, -1, -1, 2, 2,
                 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
        gfx_rect(x, 394, 140, 20, 0xc0000000);
        label(x + 70, 396, un[i]);
    }
    gfx_text(20, 50, C_DIM, "ARGB4444 texture (alpha left to right) with RGBW vertex colours");
    gfx_text(20, 232, C_DIM, "UV from -1 to 2 on the 256x256 test texture");
    snprintf(status, sizeof(status), "TEXTURE ENVIRONMENT AND UV MODES");
}

/* ---- Shading and specular ---- */

static pvr_poly_hdr_t shade_flat, shade_gouraud, shade_spec;

static void shade_enter(void) {
    static bool done;
    pvr_poly_cxt_t cxt;

    if(done)
        return;
    done = true;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.shading = false;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&shade_flat, &cxt);
    cxt.gen.shading = true;
    pvr_poly_compile(&shade_gouraud, &cxt);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565, 256, 256, gpu_test_texture(),
                     PVR_FILTER_BILINEAR);
    cxt.gen.specular = true;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&shade_spec, &cxt);
}

static void shade_tri(const pvr_poly_hdr_t *h, float cx) {
    gfx_hdr(PVR_LIST_OP_POLY, h);
    pvr_vertex_t *v = gfx_alloc(PVR_LIST_OP_POLY, 3 * sizeof(pvr_vertex_t));
    if(!v)
        return;
    gfx_vert(&v[0], PVR_CMD_VERTEX, cx, 80, 5, 0, 0, 0xffff0000, 0);
    gfx_vert(&v[1], PVR_CMD_VERTEX, cx + 95, 300, 5, 0, 0, 0xff00ff00, 0);
    gfx_vert(&v[2], PVR_CMD_VERTEX_EOL, cx - 95, 300, 5, 0, 0, 0xff0000ff, 0);
}

static void shade_draw(void) {
    float t = page_frames * 0.03f;

    ui_background();
    shade_tri(&shade_flat, 110);
    shade_tri(&shade_gouraud, 320);
    /* Offset colour: a moving highlight added on top of the texture */
    gfx_hdr(PVR_LIST_OP_POLY, &shade_spec);
    pvr_vertex_t *v = gfx_alloc(PVR_LIST_OP_POLY, 4 * sizeof(pvr_vertex_t));
    if(v) {
        int h0 = (int)(127 + 127 * sinf(t)), h1 = (int)(127 + 127 * sinf(t + 1.5f));
        uint32_t s0 = argb(0, h0, h0, h0), s1 = argb(0, h1, h1, h1);
        gfx_vert(&v[0], PVR_CMD_VERTEX, 440, 80, 5, 0, 0, 0xffa0a0a0, s0);
        gfx_vert(&v[1], PVR_CMD_VERTEX, 620, 80, 5, 1, 0, 0xffa0a0a0, s1);
        gfx_vert(&v[2], PVR_CMD_VERTEX, 440, 300, 5, 0, 1, 0xffa0a0a0, s1);
        gfx_vert(&v[3], PVR_CMD_VERTEX_EOL, 620, 300, 5, 1, 1, 0xffa0a0a0, s0);
    }
    label(110, 320, "FLAT (LAST VERTEX)");
    label(320, 320, "GOURAUD");
    label(530, 320, "OFFSET COLOUR");
    gfx_text(40, 360, C_DIM, "Flat shading takes the colour of each triangle's last vertex.");
    gfx_text(40, 378, C_DIM, "The offset (specular) colour is added after texturing.");
    snprintf(status, sizeof(status), "SHADING AND SPECULAR");
}

/* ---- Fog ---- */

static pvr_poly_hdr_t fog_hdr;
static int fog_mode;
#define FOG_MODES 6
static const char *const fog_names[FOG_MODES] = { "TABLE LINEAR", "TABLE CUSTOM", "TABLE EXP",
                                                  "TABLE EXP2", "VERTEX", "OFF" };

/* Linear fog from w = 2 to 20 written entry by entry: entry i covers
   scaled 1/w = 2^(i/16) * (16 + i%16) / 16, and the scale is the far
   depth, so entry i sits at w = 20 / that. Entry 0 is the farthest. */
static void fog_custom_linear(void) {
    static float table[129];
    const float start = 2.0f, end = 20.0f;

    for(int i = 0; i < 129; i++) {
        float s = (float)(1 << (i >> 4)) * (float)((i & 15) + 16) / 16.0f;
        float w = end / s, f = (w - start) / (end - start);
        table[i] = f < 0 ? 0 : f > 1 ? 1 : f;
    }
    pvr_fog_table_custom(table);
    pvr_fog_far_depth(end);
}

static void fog_setup(void) {
    pvr_poly_cxt_t cxt;

    pvr_fog_table_color(1.0f, 0.55f, 0.6f, 0.7f);
    pvr_fog_vertex_color(0.55f, 0.6f, 0.7f);
    if(fog_mode == 0) pvr_fog_table_linear(2.0f, 20.0f);
    if(fog_mode == 1) fog_custom_linear();
    if(fog_mode == 2) pvr_fog_table_exp(0.15f);
    if(fog_mode == 3) pvr_fog_table_exp2(0.12f);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565, 256, 256, gpu_test_texture(),
                     PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.fog_type = fog_mode < 4 ? PVR_FOG_TABLE : fog_mode == 4 ? PVR_FOG_VERTEX : PVR_FOG_DISABLE;
    /* vertex fog reads its amount from the offset colour's alpha */
    cxt.gen.specular = fog_mode == 4;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&fog_hdr, &cxt);
}

static void fog_update(const kt_input_t *in) {
    if(in->pressed & CONT_A) {
        fog_mode = (fog_mode + 1) % FOG_MODES;
        fog_setup();
        KT_LOG("GPU fog mode %s\n", fog_names[fog_mode]);

    }
}

/* A quad in camera space (x, y, w), projected here. */
static void fog_quad(float x0, float y0, float x1, float y1, float w0, float w1, bool wall) {
    pvr_vertex_t *v;
    float f = 300;
    float px[4], py[4], w[4];
    float u[4] = { 0, 1, 0, 1 }, vv[4] = { 0, 0, 1, 1 };

    if(wall) {
        /* vertical, at constant depth w0 */
        px[0] = x0; py[0] = y0; px[1] = x1; py[1] = y0; px[2] = x0; py[2] = y1; px[3] = x1; py[3] = y1;
        w[0] = w[1] = w[2] = w[3] = w0;
    }
    else {
        /* floor at height y0, depths w0 (far) to w1 (near) */
        px[0] = x0; px[1] = x1; px[2] = x0; px[3] = x1;
        py[0] = py[1] = py[2] = py[3] = y0;
        w[0] = w[1] = w0;
        w[2] = w[3] = w1;
        vv[0] = vv[1] = w0;
        vv[2] = vv[3] = w1;
        u[1] = u[3] = (x1 - x0);
    }
    gfx_hdr(PVR_LIST_OP_POLY, &fog_hdr);
    v = gfx_alloc(PVR_LIST_OP_POLY, 4 * sizeof(pvr_vertex_t));
    if(!v)
        return;
    for(int i = 0; i < 4; i++) {
        float d = (w[i] - 2.0f) / 18.0f;
        int fog = d < 0 ? 0 : d > 1 ? 255 : (int)(d * 255);
        gfx_vert(&v[i], i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, 320 + px[i] * f / w[i],
                 240 - py[i] * f / w[i], 1.0f / w[i], u[i], vv[i], 0xffffffff, argb(fog, 0, 0, 0));
    }
}

static void fog_draw(void) {
    /* Written again once a frame has gone through: Flycast ignored a table
       set before the page's first frame. */
    if(page_frames == 2)
        fog_setup();
    pvr_set_bg_color(0.55f, 0.6f, 0.7f);
    for(int z = 0; z < 24; z++)
        fog_quad(-3, -1, 3, -1, 2 + z + 1, 2 + z, false);
    for(int k = 0; k < 6; k++) {
        float w = 3 + k * 3.5f;
        fog_quad(-2.4f, 1.2f, -1.6f, -1, w, w, true);
        fog_quad(1.6f, 1.2f, 2.4f, -1, w, w, true);
    }
    snprintf(status, sizeof(status), "A: FOG MODE  %s", fog_names[fog_mode]);
}

static void fog_leave(void) {
    pvr_set_bg_color(0.027f, 0.078f, 0.149f);
}

/* ---- Depth and punch-through ---- */

static pvr_poly_hdr_t depth_red, depth_blue[8], pt_hdr;
static int depth_mode = PVR_DEPTHCMP_GREATER;
static uint32_t pt_ref = 0x80, pt_saved;
static const char *const depth_names[8] = { "NEVER", "LESS", "EQUAL", "LEQUAL", "GREATER",
                                            "NOTEQUAL", "GEQUAL", "ALWAYS" };

static void depth_enter(void) {
    static bool done;
    pvr_poly_cxt_t cxt;

    env_enter();    /* the alpha texture */
    pt_saved = PVR_GET(PVR_PT_ALPHA_REF);
    PVR_SET(PVR_PT_ALPHA_REF, pt_ref);
    if(done)
        return;
    done = true;
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&depth_red, &cxt);
    for(int i = 0; i < 8; i++) {
        cxt.depth.comparison = (pvr_depthcmp_mode_t)i;
        pvr_poly_compile(&depth_blue[i], &cxt);
    }
    pvr_poly_cxt_txr(&cxt, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB4444, TS, TS, alpha_tex,
                     PVR_FILTER_NEAREST);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&pt_hdr, &cxt);
}

static void depth_leave(void) {
    PVR_SET(PVR_PT_ALPHA_REF, pt_saved);
}

static void depth_update(const kt_input_t *in) {
    if(in->pressed & CONT_A)
        depth_mode = (depth_mode + 1) % 8;
    if(in->pressed & CONT_X) {
        pt_ref = (pt_ref + 0x20) & 0xff;
        PVR_SET(PVR_PT_ALPHA_REF, pt_ref);
    }
}

static void depth_draw(void) {
    ui_background();
    /* red at z 5; blue slopes from 2 (left) to 8 (right) through it */
    gfx_quad(PVR_LIST_OP_POLY, &depth_red, 40, 90, 300, 330, 5, 0, 0, 0, 0, 0xffe03030,
             0xffe03030, 0xffe03030, 0xffe03030);
    gfx_hdr(PVR_LIST_OP_POLY, &depth_blue[depth_mode]);
    pvr_vertex_t *v = gfx_alloc(PVR_LIST_OP_POLY, 4 * sizeof(pvr_vertex_t));
    if(v) {
        gfx_vert(&v[0], PVR_CMD_VERTEX, 60, 130, 2, 0, 0, 0xff3060ff, 0);
        gfx_vert(&v[1], PVR_CMD_VERTEX, 320, 130, 8, 0, 0, 0xff3060ff, 0);
        gfx_vert(&v[2], PVR_CMD_VERTEX, 60, 290, 2, 0, 0, 0xff3060ff, 0);
        gfx_vert(&v[3], PVR_CMD_VERTEX_EOL, 320, 290, 8, 0, 0, 0xff3060ff, 0);
    }
    label(180, 340, "BLUE DEPTH TEST:");
    gfx_text_center(180, 358, C_HI, depth_names[depth_mode], false);

    gfx_op_grad4(360, 90, 240, 240, 2, 0xff204020, 0xff204020, 0xff60a060, 0xff60a060);
    gfx_quad(PVR_LIST_PT_POLY, &pt_hdr, 380, 110, 580, 310, 5, 0, 0, 3, 3, 0xffffffff,
             0xffffffff, 0xffffffff, 0xffffffff);
    label(480, 340, "PUNCH-THROUGH ALPHA >=");
    gfx_textf(458, 358, C_HI, "0x%02lX", (unsigned long)pt_ref);
    snprintf(status, sizeof(status), "A: DEPTH COMPARE   X: PUNCH-THROUGH REFERENCE");
}

/* ---- Modifier volumes ---- */

static pvr_poly_hdr_t mod_poly, mod_cheap;
static pvr_mod_hdr_t mod_first, mod_last;
static bool mod_cheap_mode;

static void mod_enter(void) {
    static bool done;
    pvr_poly_cxt_t cxt;

    if(done)
        return;
    done = true;
    pvr_poly_cxt_col_mod(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_mod_compile(&mod_poly, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    cxt.fmt.modifier = true;
    cxt.gen.modifier_mode = false;      /* cheap shadow */
    pvr_poly_compile(&mod_cheap, &cxt);
    pvr_mod_compile(&mod_first, PVR_LIST_OP_MOD, PVR_MODIFIER_OTHER_POLY, PVR_CULLING_NONE);
    pvr_mod_compile(&mod_last, PVR_LIST_OP_MOD, PVR_MODIFIER_INCLUDE_LAST_POLY, PVR_CULLING_NONE);
}

static void mod_update(const kt_input_t *in) {
    if(in->pressed & CONT_A)
        mod_cheap_mode = !mod_cheap_mode;
}

static void mod_tri(const pvr_mod_hdr_t *h, float ax, float ay, float bx, float by, float cx, float cy) {
    gfx_push(PVR_LIST_OP_MOD, h, sizeof(*h));
    pvr_modifier_vol_t *m = gfx_alloc(PVR_LIST_OP_MOD, sizeof(*m));
    if(!m)
        return;
    memset(m, 0, sizeof(*m));
    m->flags = PVR_CMD_VERTEX_EOL;
    m->ax = ax; m->ay = ay; m->az = 50;
    m->bx = bx; m->by = by; m->bz = 50;
    m->cx = cx; m->cy = cy; m->cz = 50;
}

static void mod_draw(void) {
    float t = page_frames * 0.02f;
    float mx = 320 + 170 * cosf(t), my = 250 + 110 * sinf(t * 1.3f);

    pvr_set_shadow_scale(mod_cheap_mode, 0.35f);
    for(int y = 0; y < 6; y++)
        for(int x = 0; x < 8; x++) {
            float x0 = 16 + x * 78, y0 = 48 + y * 66;
            uint32_t out = hue((x + y * 8) / 48.0f);
            uint32_t in = 0xffffffff ^ (out & 0x00ffffff);
            if(mod_cheap_mode) {
                gfx_quad(PVR_LIST_OP_POLY, &mod_cheap, x0, y0, x0 + 72, y0 + 60, 2, 0, 0, 0, 0,
                         out, out, out, out);
                continue;
            }
            gfx_hdr(PVR_LIST_OP_POLY, &mod_poly);
            pvr_vertex_pcm_t *v = gfx_alloc(PVR_LIST_OP_POLY, 4 * sizeof(pvr_vertex_pcm_t));
            if(!v)
                continue;
            float vx[4] = { x0, x0 + 72, x0, x0 + 72 }, vy[4] = { y0, y0, y0 + 60, y0 + 60 };
            for(int i = 0; i < 4; i++) {
                v[i].flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
                v[i].x = vx[i];
                v[i].y = vy[i];
                v[i].z = 2;
                v[i].argb0 = out;
                v[i].argb1 = in;
                v[i].d1 = v[i].d2 = 0;
            }
        }
    /* The volume: a square made of two triangles in front of everything */
    mod_tri(&mod_first, mx - 90, my - 90, mx + 90, my - 90, mx - 90, my + 90);
    mod_tri(&mod_last, mx + 90, my - 90, mx + 90, my + 90, mx - 90, my + 90);
    snprintf(status, sizeof(status), "A: %s", mod_cheap_mode ? "CHEAP SHADOW (DARKEN INSIDE)" :
             "NORMAL MODIFIER (SECOND COLOUR INSIDE)");
}

static void mod_leave(void) {
    pvr_set_shadow_scale(false, 1.0f);
}

/* ---- 3D scene ---- */

static bool scene_tex = true;

static void scene_update(const kt_input_t *in) {
    if(in->pressed & CONT_A)
        scene_tex = !scene_tex;
}

static void scene_draw(void) {
    pvr_stats_t st;

    pvr_set_bg_color(0.05f, 0.05f, 0.1f);
    torus_view(SCREEN_W / 2, SCREEN_H / 2);
    torus_set(page_frames * 0.02f, 0, 0, 3.4f, scene_tex);
    gfx_direct(PVR_LIST_OP_POLY, torus_submit, NULL);
    pvr_get_stats(&st);
    snprintf(status, sizeof(status), "A: TEXTURE   %d TRIS  %.1f FPS  RENDER %.2f MS",
             torus_triangles(), (double)st.frame_rate, st.rnd_last_time / 1e6);
}

static void scene_leave(void) {
    pvr_set_bg_color(0.027f, 0.078f, 0.149f);
}

/* ---- Palette animation ---- */

static pvr_ptr_t plasma_tex;
static pvr_poly_hdr_t plasma_hdr;

static void pal_enter(void) {
    if(plasma_tex)
        return;
    pvr_set_pal_format(PVR_PAL_ARGB8888);
    uint8_t *ix = malloc(128 * 128);
    for(int y = 0; y < 128; y++)
        for(int x = 0; x < 128; x++) {
            float dx = x - 64.0f, dy = y - 64.0f;
            float v = sinf(x * 0.09f) + sinf(y * 0.07f) + sinf(sqrtf(dx * dx + dy * dy) * 0.12f) +
                      sinf((x + y) * 0.05f);
            ix[y * 128 + x] = (uint8_t)((v + 4) * 31.9f);
        }
    plasma_tex = pvr_mem_malloc(128 * 128);
    pvr_txr_load_ex(ix, plasma_tex, 128, 128, PVR_TXRLOAD_8BPP);
    free(ix);
    compile_txr(&plasma_hdr, PVR_LIST_OP_POLY, PVR_TXRFMT_PAL8BPP | PVR_TXRFMT_8BPP_PAL(2), 128,
                128, plasma_tex, PVR_FILTER_BILINEAR);
}

static void pal_draw(void) {
    float shift = page_frames / 256.0f;

    /* Rewriting bank 2 each frame animates the texture without touching it. */
    for(int i = 0; i < 256; i++)
        pvr_set_pal_entry(512 + i, hue(i / 256.0f + shift) & (((i >> 4) & 1) ? 0xffffffff : 0xffc0c0c0));
    ui_background();
    gfx_quad(PVR_LIST_OP_POLY, &plasma_hdr, 128, 52, 512, 436, 5, 0, 0, 1, 1, 0xffffffff,
             0xffffffff, 0xffffffff, 0xffffffff);
    snprintf(status, sizeof(status), "8BPP TEXTURE, 256 PALETTE ENTRIES REWRITTEN EVERY FRAME");
}

/* ---- Sprites ---- */

#define SPRITE_MAX 2000
static struct { float x, y, dx, dy; uint32_t c; } spr[SPRITE_MAX];
static int spr_count = 200;
static pvr_sprite_hdr_t spr_hdr;

static void spr_enter(void) {
    static bool done;
    pvr_sprite_cxt_t sc;
    uint32_t seed = 5;

    env_enter();
    if(done)
        return;
    done = true;
    for(int i = 0; i < SPRITE_MAX; i++) {
        spr[i].x = (float)(rng_next(&seed) % 600);
        spr[i].y = (float)(48 + rng_next(&seed) % 380);
        spr[i].dx = ((int)(rng_next(&seed) % 400) - 200) / 100.0f;
        spr[i].dy = ((int)(rng_next(&seed) % 400) - 200) / 100.0f;
    }
    pvr_sprite_cxt_txr(&sc, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444, TS, TS, alpha_tex,
                       PVR_FILTER_BILINEAR);
    sc.txr.uv_clamp = PVR_UVCLAMP_UV;   /* no bleed from the opposite edge */
    sc.gen.culling = PVR_CULLING_NONE;
    sc.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    sc.depth.write = false;
    pvr_sprite_compile(&spr_hdr, &sc);
    spr_hdr.argb = 0xffffffff;
    spr_hdr.oargb = 0;
}

static void spr_update(const kt_input_t *in) {
    if(in->repeat & CONT_A) spr_count = spr_count + 100 > SPRITE_MAX ? SPRITE_MAX : spr_count + 100;
    if(in->repeat & CONT_X) spr_count = spr_count > 100 ? spr_count - 100 : 0;
    for(int i = 0; i < spr_count; i++) {
        spr[i].x += spr[i].dx;
        spr[i].y += spr[i].dy;
        if(spr[i].x < 0 || spr[i].x > SCREEN_W - 32) spr[i].dx = -spr[i].dx;
        if(spr[i].y < 40 || spr[i].y > 448 - 32) spr[i].dy = -spr[i].dy;
    }
}

static void spr_draw(void) {
    pvr_stats_t st;
    uint32_t uv0 = PVR_PACK_16BIT_UV(0, 0), uv1 = PVR_PACK_16BIT_UV(1, 0), uv2 = PVR_PACK_16BIT_UV(1, 1);

    ui_background();
    gfx_push(PVR_LIST_TR_POLY, &spr_hdr, sizeof(spr_hdr));
    for(int i = 0; i < spr_count; i++) {
        pvr_sprite_txr_t *s = gfx_alloc(PVR_LIST_TR_POLY, sizeof(*s));
        if(!s)
            break;
        float x = spr[i].x, y = spr[i].y;
        s->flags = PVR_CMD_VERTEX_EOL;
        s->ax = x; s->ay = y; s->az = 5;
        s->bx = x + 32; s->by = y; s->bz = 5;
        s->cx = x + 32; s->cy = y + 32; s->cz = 5;
        s->dx = x; s->dy = y + 32;
        s->dummy = 0;
        s->auv = uv0;
        s->buv = uv1;
        s->cuv = uv2;
    }
    pvr_get_stats(&st);
    snprintf(status, sizeof(status), "A: MORE  X: FEWER   %d SPRITES  %.1f FPS", spr_count,
             (double)st.frame_rate);
}

/* ---- Render to texture ---- */

static pvr_ptr_t rtt_tex;
static pvr_poly_hdr_t rtt_hdr;

static void rtt_enter(void) {
    if(rtt_tex)
        return;
    rtt_tex = pvr_mem_malloc(256 * 256 * 2);
    compile_txr(&rtt_hdr, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, 256, 256,
                rtt_tex, PVR_FILTER_BILINEAR);
}

/* The first scene of the frame renders the torus into rtt_tex. */
static void rtt_update(const kt_input_t *in) {
    pvr_wait_ready();
    if(pvr_scene_begin_rtt(rtt_tex, 256, 256, 256) < 0)
        return;
    pvr_list_begin(PVR_LIST_OP_POLY);
    torus_view(128, 128);
    torus_set(page_frames * 0.03f, 0, 0, 3.0f, true);
    torus_submit(NULL);
    pvr_list_finish();
    pvr_scene_finish();
    torus_view(SCREEN_W / 2, SCREEN_H / 2);
}

static void rtt_draw(void) {
    float t = page_frames * 0.015f;

    ui_background();
    for(int k = 0; k < 4; k++) {
        float cx = 160 + (k % 2) * 320, cy = 140 + (k / 2) * 200, a = t + k * 0.8f;
        float s = 80 + 10 * sinf(t * 2 + k);
        gfx_hdr(PVR_LIST_OP_POLY, &rtt_hdr);
        pvr_vertex_t *v = gfx_alloc(PVR_LIST_OP_POLY, 4 * sizeof(pvr_vertex_t));
        if(!v)
            continue;
        float c = cosf(a) * s, d = sinf(a) * s;
        gfx_vert(&v[0], PVR_CMD_VERTEX, cx - c + d, cy - d - c, 5, 0, 0, 0xffffffff, 0);
        gfx_vert(&v[1], PVR_CMD_VERTEX, cx + c + d, cy + d - c, 5, 1, 0, 0xffffffff, 0);
        gfx_vert(&v[2], PVR_CMD_VERTEX, cx - c - d, cy - d + c, 5, 0, 1, 0xffffffff, 0);
        gfx_vert(&v[3], PVR_CMD_VERTEX_EOL, cx + c - d, cy + d + c, 5, 1, 1, 0xffffffff, 0);
    }
    snprintf(status, sizeof(status), "TORUS RENDERED TO A 256X256 TEXTURE, THEN DRAWN FOUR TIMES");
}

/* ---- Page table ---- */

static const page_t pages[] = {
    { "COLOUR SPECTRUM", "A DITHER", color_enter, color_update, color_draw, NULL },
    { "TEXTURE FORMATS", NULL, formats_enter, NULL, formats_draw, NULL },
    { "FILTERING & MIPMAPS", "A TINT  X BIAS", filt_enter, filt_update, filt_draw, NULL },
    { "BLEND MODES", NULL, blend_enter, NULL, blend_draw, NULL },
    { "TEXTURE ENV & UV", NULL, env_enter, NULL, env_draw, NULL },
    { "SHADING & SPECULAR", NULL, shade_enter, NULL, shade_draw, NULL },
    { "FOG", "A MODE", fog_setup, fog_update, fog_draw, fog_leave },
    { "DEPTH & PUNCH-THROUGH", "A DEPTH  X ALPHA", depth_enter, depth_update, depth_draw, depth_leave },
    { "MODIFIER VOLUMES", "A MODE", mod_enter, mod_update, mod_draw, mod_leave },
    { "3D SCENE", "A TEXTURE", NULL, scene_update, scene_draw, scene_leave },
    { "PALETTE ANIMATION", NULL, pal_enter, NULL, pal_draw, NULL },
    { "SPRITES", "A MORE  X FEWER", spr_enter, spr_update, spr_draw, NULL },
    { "RENDER TO TEXTURE", NULL, rtt_enter, rtt_update, rtt_draw, NULL },
};
#define PAGES ((int)(sizeof(pages) / sizeof(pages[0])))

static void enter_page(int p) {
    if(pages[page].leave)
        pages[page].leave();
    page = p;
    page_frames = 0;
    status[0] = 0;
    if(pages[page].enter)
        pages[page].enter();
    KT_LOG("GPU page %d: %s\n", page + 1, pages[page].title);
}

static void gpu_enter(void) {
    page_frames = 0;
    if(pages[page].enter)
        pages[page].enter();
}

static void gpu_leave(void) {
    if(pages[page].leave)
        pages[page].leave();
    vid_set_dithering(true);
}

static void gpu_update(const kt_input_t *in) {
#ifdef KT_AUTOTEST
    /* Scripted runs press A (and X) now and then to try each page's modes. */
    kt_input_t fake = *in;
    if(autotest_active() && page_frames > 90 && page_frames % 40 == 39)
        fake.pressed |= CONT_A | (page_frames % 80 == 79 ? CONT_X : 0);
    in = &fake;
#endif
    if(in->pressed & (CONT_B | CONT_START)) {
        kt_pop();
        return;
    }
    if(in->pressed & CONT_DPAD_RIGHT) enter_page((page + 1) % PAGES);
    else if(in->pressed & CONT_DPAD_LEFT) enter_page((page + PAGES - 1) % PAGES);
    if(pages[page].update)
        pages[page].update(in);
    page_frames++;
}

static void gpu_draw(void) {
    char title[48], hints[96];

    pages[page].draw();
    snprintf(title, sizeof(title), "GPU %d/%d: %s", page + 1, PAGES, pages[page].title);
    gfx_grad(0, 0, SCREEN_W, 36, 0xe01d4a8a, 0xe010284c);
    gfx_rect(0, 36, SCREEN_W, 2, C_HI);
    gfx_text_big(12, 6, C_TEXT, title);
    gfx_rect(0, UI_BOTTOM + 4, SCREEN_W, SCREEN_H - UI_BOTTOM - 4, 0xe0081428);
    gfx_text(12, UI_BOTTOM + 4, C_HI, status);
    snprintf(hints, sizeof(hints), "LEFT/RIGHT PAGE   %s%sB BACK", pages[page].hint ? pages[page].hint : "",
             pages[page].hint ? "   " : "");
    gfx_text(12, UI_BOTTOM + 18, C_DIM, hints);
}

const kt_screen_t gpu_screen = {
    "GPU test", gpu_enter, gpu_update, gpu_draw, gpu_leave
};

#ifdef KT_AUTOTEST
int gpu_page_count(void) {
    return PAGES;
}

void gpu_autotest_page(int p) {
    if(kt_current() != &gpu_screen) {
        page = 0;
        kt_push(&gpu_screen);
    }
    if(p != page)
        enter_page(p);
}
#endif
