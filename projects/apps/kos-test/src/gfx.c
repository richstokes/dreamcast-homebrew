/* PowerVR drawing: per-list RAM buffers submitted in list order, two fonts
   made from the BIOS font, and the screen chrome every page shares. */
#include "gfx.h"
#include "kt.h"

#include <dc/biosfont.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LISTS 5

/* Buffer sizes per list: text-heavy pages put ~2000 glyphs (128 bytes each)
   in the translucent list. */
static const size_t list_cap[LISTS] = {
    256 * 1024,     /* PVR_LIST_OP_POLY */
    16 * 1024,      /* PVR_LIST_OP_MOD */
    320 * 1024,     /* PVR_LIST_TR_POLY */
    16 * 1024,      /* PVR_LIST_TR_MOD */
    64 * 1024,      /* PVR_LIST_PT_POLY */
};

static uint8_t *list_buf[LISTS];
static size_t list_len[LISTS];
static const void *list_last_hdr[LISTS];
static void (*list_direct[LISTS])(void *);
static void *list_direct_arg[LISTS];
static bool overflow_logged;

pvr_poly_hdr_t gfx_hdr_op_flat, gfx_hdr_tr_flat;
static pvr_poly_hdr_t hdr_small, hdr_big;
static pvr_ptr_t tex_small, tex_big;

#define SMALL_TW 128
#define SMALL_TH 256
#define SMALL_COLS (SMALL_TW / FONT_W)
#define BIG_TW 256
#define BIG_TH 512
#define BIG_COLS (BIG_TW / BIG_W)

/* The 12x24 BIOS glyph as 0/1 coverage. */
static void glyph_bits(int c, uint8_t out[BIG_H][BIG_W]) {
    uint16_t px[BIG_W * BIG_H];

    memset(px, 0, sizeof(px));
    bfont_draw_ex(px, BIG_W, 0xffff, 0x0000, 16, true, (uint32_t)c, false, false);
    for(int y = 0; y < BIG_H; y++)
        for(int x = 0; x < BIG_W; x++)
            out[y][x] = px[y * BIG_W + x] ? 1 : 0;
}

/* Area-weighted 2/3 downscale: each output pixel covers 1.5x1.5 source
   pixels, so even columns weigh (1, .5) and odd ones (.5, 1). A gamma lift
   keeps one-pixel strokes readable. */
static uint16_t small_texel(const uint8_t g[BIG_H][BIG_W], int ox, int oy) {
    float wx[2], wy[2];
    int sx = (ox / 2) * 3 + (ox & 1), sy = (oy / 2) * 3 + (oy & 1);
    float sum = 0.0f;

    wx[0] = (ox & 1) ? 0.5f : 1.0f;
    wx[1] = (ox & 1) ? 1.0f : 0.5f;
    wy[0] = (oy & 1) ? 0.5f : 1.0f;
    wy[1] = (oy & 1) ? 1.0f : 0.5f;
    for(int j = 0; j < 2; j++)
        for(int i = 0; i < 2; i++)
            if(sy + j < BIG_H && sx + i < BIG_W && g[sy + j][sx + i])
                sum += wx[i] * wy[j];
    sum /= 2.25f;
    if(sum <= 0.0f)
        return 0x0fff;
    sum = powf(sum, 0.6f);
    int a = (int)(sum * 15.0f + 0.5f);
    if(a > 15)
        a = 15;
    return (uint16_t)((a << 12) | 0x0fff);
}

static void build_fonts(void) {
    uint16_t *small = calloc(SMALL_TW * SMALL_TH, sizeof(uint16_t));
    uint16_t *big = calloc(BIG_TW * BIG_TH, sizeof(uint16_t));
    uint8_t g[BIG_H][BIG_W];

    bfont_set_encoding(BFONT_CODE_ISO8859_1);
    for(int c = 33; c < 256; c++) {
        int slot = c - 32;

        if(c >= 127 && c < 160)
            continue;
        glyph_bits(c, g);
        uint16_t *cell = big + (slot / BIG_COLS) * BIG_H * BIG_TW + (slot % BIG_COLS) * BIG_W;
        for(int y = 0; y < BIG_H; y++)
            for(int x = 0; x < BIG_W; x++)
                cell[y * BIG_TW + x] = g[y][x] ? 0xffff : 0x0fff;
        cell = small + (slot / SMALL_COLS) * FONT_H * SMALL_TW + (slot % SMALL_COLS) * FONT_W;
        for(int y = 0; y < FONT_H; y++)
            for(int x = 0; x < FONT_W; x++)
                cell[y * SMALL_TW + x] = small_texel((const uint8_t (*)[BIG_W])g, x, y);
    }
    tex_small = pvr_mem_malloc(SMALL_TW * SMALL_TH * 2);
    tex_big = pvr_mem_malloc(BIG_TW * BIG_TH * 2);
    pvr_txr_load(small, tex_small, SMALL_TW * SMALL_TH * 2);
    pvr_txr_load(big, tex_big, BIG_TW * BIG_TH * 2);
    free(small);
    free(big);
}

static void compile_font(pvr_poly_hdr_t *hdr, pvr_ptr_t tex, int w, int h) {
    pvr_poly_cxt_t cxt;

    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED, w, h, tex,
                     PVR_FILTER_NEAREST);
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = false;
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(hdr, &cxt);
}

void gfx_init(void) {
    pvr_init_params_t params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_16, PVR_BINSIZE_16, PVR_BINSIZE_16,
          PVR_BINSIZE_16 },
        GFX_VERTBUF, 0, 0, 1, 2, 0
    };
    pvr_poly_cxt_t cxt;

    pvr_init(&params);
    pvr_set_bg_color(0.027f, 0.078f, 0.149f);

    for(int i = 0; i < LISTS; i++)
        list_buf[i] = memalign(32, list_cap[i]);

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.shading = true;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&gfx_hdr_op_flat, &cxt);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    cxt.gen.shading = true;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = false;
    pvr_poly_compile(&gfx_hdr_tr_flat, &cxt);

    build_fonts();
    compile_font(&hdr_small, tex_small, SMALL_TW, SMALL_TH);
    compile_font(&hdr_big, tex_big, BIG_TW, BIG_TH);
}

void gfx_begin(void) {
    for(int i = 0; i < LISTS; i++) {
        list_len[i] = 0;
        list_last_hdr[i] = NULL;
        list_direct[i] = NULL;
    }
}

void gfx_end(void) {
    static const pvr_list_t order[LISTS] = {
        PVR_LIST_OP_POLY, PVR_LIST_OP_MOD, PVR_LIST_TR_POLY, PVR_LIST_TR_MOD,
        PVR_LIST_PT_POLY
    };

    pvr_wait_ready();
    pvr_scene_begin();
    for(int i = 0; i < LISTS; i++) {
        pvr_list_t l = order[i];

        if(!list_len[l] && !list_direct[l])
            continue;
        pvr_list_begin(l);
        if(list_direct[l])
            list_direct[l](list_direct_arg[l]);
        if(list_len[l])
            pvr_prim(list_buf[l], list_len[l]);
        pvr_list_finish();
    }
    pvr_scene_finish();
}

void *gfx_alloc(pvr_list_t list, size_t size) {
    if(list_len[list] + size > list_cap[list]) {
        if(!overflow_logged) {
            KT_LOG("gfx: list %d buffer full\n", (int)list);
            overflow_logged = true;
        }
        return NULL;
    }
    void *p = list_buf[list] + list_len[list];
    list_len[list] += size;
    return p;
}

void gfx_push(pvr_list_t list, const void *data, size_t size) {
    void *p = gfx_alloc(list, size);

    if(p)
        memcpy(p, data, size);
    list_last_hdr[list] = NULL;
}

void gfx_hdr(pvr_list_t list, const pvr_poly_hdr_t *hdr) {
    if(list_last_hdr[list] == hdr)
        return;
    void *p = gfx_alloc(list, sizeof(*hdr));
    if(p) {
        memcpy(p, hdr, sizeof(*hdr));
        list_last_hdr[list] = hdr;
    }
}

void gfx_direct(pvr_list_t list, void (*fn)(void *), void *arg) {
    list_direct[list] = fn;
    list_direct_arg[list] = arg;
}

void gfx_quad(pvr_list_t list, const pvr_poly_hdr_t *hdr, float x0, float y0,
              float x1, float y1, float z, float u0, float v0, float u1,
              float v1, uint32_t c00, uint32_t c10, uint32_t c01, uint32_t c11) {
    gfx_hdr(list, hdr);
    pvr_vertex_t *v = gfx_alloc(list, 4 * sizeof(pvr_vertex_t));
    if(!v)
        return;
    gfx_vert(&v[0], PVR_CMD_VERTEX, x0, y0, z, u0, v0, c00, 0);
    gfx_vert(&v[1], PVR_CMD_VERTEX, x1, y0, z, u1, v0, c10, 0);
    gfx_vert(&v[2], PVR_CMD_VERTEX, x0, y1, z, u0, v1, c01, 0);
    gfx_vert(&v[3], PVR_CMD_VERTEX_EOL, x1, y1, z, u1, v1, c11, 0);
}

/* ---- 2D ---- */

#define UI_Z 10.0f

void gfx_rect(float x, float y, float w, float h, uint32_t argb) {
    gfx_quad(PVR_LIST_TR_POLY, &gfx_hdr_tr_flat, x, y, x + w, y + h, UI_Z,
             0, 0, 0, 0, argb, argb, argb, argb);
}

void gfx_grad(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    gfx_quad(PVR_LIST_TR_POLY, &gfx_hdr_tr_flat, x, y, x + w, y + h, UI_Z,
             0, 0, 0, 0, top, top, bottom, bottom);
}

void gfx_grad4(float x, float y, float w, float h, uint32_t c00, uint32_t c10,
               uint32_t c01, uint32_t c11) {
    gfx_quad(PVR_LIST_TR_POLY, &gfx_hdr_tr_flat, x, y, x + w, y + h, UI_Z,
             0, 0, 0, 0, c00, c10, c01, c11);
}

void gfx_box(float x, float y, float w, float h, float t, uint32_t argb) {
    gfx_rect(x, y, w, t, argb);
    gfx_rect(x, y + h - t, w, t, argb);
    gfx_rect(x, y + t, t, h - 2 * t, argb);
    gfx_rect(x + w - t, y + t, t, h - 2 * t, argb);
}

void gfx_line(float x0, float y0, float x1, float y1, float t, uint32_t argb) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);

    if(len < 0.001f)
        return;
    float nx = -dy / len * t * 0.5f, ny = dx / len * t * 0.5f;
    gfx_hdr(PVR_LIST_TR_POLY, &gfx_hdr_tr_flat);
    pvr_vertex_t *v = gfx_alloc(PVR_LIST_TR_POLY, 4 * sizeof(pvr_vertex_t));
    if(!v)
        return;
    gfx_vert(&v[0], PVR_CMD_VERTEX, x0 + nx, y0 + ny, UI_Z, 0, 0, argb, 0);
    gfx_vert(&v[1], PVR_CMD_VERTEX, x1 + nx, y1 + ny, UI_Z, 0, 0, argb, 0);
    gfx_vert(&v[2], PVR_CMD_VERTEX, x0 - nx, y0 - ny, UI_Z, 0, 0, argb, 0);
    gfx_vert(&v[3], PVR_CMD_VERTEX_EOL, x1 - nx, y1 - ny, UI_Z, 0, 0, argb, 0);
}

static float draw_glyphs(float x, float y, uint32_t argb, const char *s,
                         int max, bool big) {
    const pvr_poly_hdr_t *hdr = big ? &hdr_big : &hdr_small;
    int cw = big ? BIG_W : FONT_W, ch = big ? BIG_H : FONT_H;
    int cols = big ? BIG_COLS : SMALL_COLS;
    float tw = big ? BIG_TW : SMALL_TW, th = big ? BIG_TH : SMALL_TH;
    float du = cw / tw, dv = ch / th;

    gfx_hdr(PVR_LIST_TR_POLY, hdr);
    for(const unsigned char *p = (const unsigned char *)s; *p && max; p++, max--) {
        unsigned c = *p;

        if(c > 32 && !(c >= 127 && c < 160)) {
            int slot = (int)c - 32;
            float u = (slot % cols) * du, v = (slot / cols) * dv;
            pvr_vertex_t *vt = gfx_alloc(PVR_LIST_TR_POLY, 4 * sizeof(pvr_vertex_t));
            if(!vt)
                break;
            gfx_vert(&vt[0], PVR_CMD_VERTEX, x, y, UI_Z, u, v, argb, 0);
            gfx_vert(&vt[1], PVR_CMD_VERTEX, x + cw, y, UI_Z, u + du, v, argb, 0);
            gfx_vert(&vt[2], PVR_CMD_VERTEX, x, y + ch, UI_Z, u, v + dv, argb, 0);
            gfx_vert(&vt[3], PVR_CMD_VERTEX_EOL, x + cw, y + ch, UI_Z, u + du, v + dv, argb, 0);
        }
        x += cw;
    }
    return x;
}

float gfx_text(float x, float y, uint32_t argb, const char *s) {
    return draw_glyphs(x, y, argb, s, -1, false);
}

float gfx_text_big(float x, float y, uint32_t argb, const char *s) {
    return draw_glyphs(x, y, argb, s, -1, true);
}

float gfx_textf(float x, float y, uint32_t argb, const char *fmt, ...) {
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return gfx_text(x, y, argb, buf);
}

void gfx_text_right(float right, float y, uint32_t argb, const char *s) {
    gfx_text(right - strlen(s) * FONT_W, y, argb, s);
}

void gfx_text_center(float cx, float y, uint32_t argb, const char *s, bool big) {
    float w = strlen(s) * (big ? BIG_W : FONT_W);

    draw_glyphs(floorf(cx - w / 2), y, argb, s, -1, big);
}

float gfx_text_clip(float x, float y, uint32_t argb, const char *s, int max_chars) {
    int len = (int)strlen(s);

    if(len <= max_chars || max_chars < 4)
        return draw_glyphs(x, y, argb, s, max_chars, false);
    x = draw_glyphs(x, y, argb, s, max_chars - 3, false);
    return draw_glyphs(x, y, argb, "...", -1, false);
}

/* ---- Opaque ---- */

void gfx_op_rect(float x, float y, float w, float h, float z, uint32_t argb) {
    gfx_quad(PVR_LIST_OP_POLY, &gfx_hdr_op_flat, x, y, x + w, y + h, z,
             0, 0, 0, 0, argb, argb, argb, argb);
}

void gfx_op_grad4(float x, float y, float w, float h, float z, uint32_t c00,
                  uint32_t c10, uint32_t c01, uint32_t c11) {
    gfx_quad(PVR_LIST_OP_POLY, &gfx_hdr_op_flat, x, y, x + w, y + h, z,
             0, 0, 0, 0, c00, c10, c01, c11);
}

/* ---- Chrome ---- */

void ui_background(void) {
    gfx_op_grad4(0, 0, SCREEN_W, SCREEN_H, 1.0f, 0xff0a1c36, 0xff0a1c36,
                 0xff040a16, 0xff040a16);
}

void ui_header(const char *title, const char *right) {
    gfx_grad(0, 0, SCREEN_W, 36, 0xff1d4a8a, 0xff10284c);
    gfx_rect(0, 36, SCREEN_W, 2, C_HI);
    gfx_text_big(12, 6, C_TEXT, title);
    if(right)
        gfx_text_right(SCREEN_W - 12, 10, C_DIM, right);
}

void ui_footer(const char *hints) {
    gfx_rect(0, UI_BOTTOM + 4, SCREEN_W, 1, C_LINE);
    gfx_rect(0, UI_BOTTOM + 5, SCREEN_W, SCREEN_H - UI_BOTTOM - 5, 0xe0081428);
    gfx_text(12, UI_BOTTOM + 10, C_DIM, hints);
}

void ui_panel(float x, float y, float w, float h, const char *title) {
    gfx_rect(x, y, w, h, 0xd00e2442);
    gfx_box(x, y, w, h, 1, C_LINE);
    if(title) {
        gfx_rect(x + 1, y + 1, w - 2, 18, 0xff163564);
        gfx_text(x + 6, y + 2, C_HI, title);
    }
}

void ui_menu(float x, float y, float w, const char *const *items, int count,
             int selected, int first, int visible) {
    for(int i = first; i < count && i < first + visible; i++) {
        float ry = y + (i - first) * 28;

        if(i == selected) {
            gfx_grad(x, ry, w, 26, 0xff2d66b8, 0xff1c4888);
            gfx_box(x, ry, w, 26, 1, C_HI);
        }
        gfx_text_big(x + 14, ry + 1, i == selected ? C_HI : C_TEXT, items[i]);
    }
    if(first > 0)
        gfx_text(x + w - 20, y - 18, C_DIM, "^");
    if(first + visible < count)
        gfx_text(x + w - 20, y + visible * 28, C_DIM, "v");
}
