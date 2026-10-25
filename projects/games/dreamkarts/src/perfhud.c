/* Performance overlay (pause menu, or -DDK_PERF_HUD from boot): frame CPU time and its phases, PVR
   render time, the overlay's own cost, vertex buffer use, triangles and
   missed vblanks. Times are averaged over half a second, with the maximum
   beside them, and drawn as bars against one NTSC frame.

   The PVR's cost goes with the number of polygons in a tile, not their
   size: the ~150 glyph quads of this box cost the console 1.2 to 1.8 ms of
   render time a frame, translucent or punch-through. So the CPU draws the
   whole box (background, text and bars) into one RGB565 texture whenever
   the figures change, twice a second, one 16-row band per frame, and the
   overlay is a single opaque quad. Being opaque it also hides the world
   behind it, which saves the PVR more than the quad costs. The CPU and PT
   figures leave out the overlay's own work, which has its own row. */
#include "perfhud.h"
#include "hud.h"
#include "render.h"
#include "texture_assets.h"
#include "font_metrics.h"

extern const uint8_t dk_textures_pak[];

#define FRAME_MS 16.68f          /* one NTSC vblank */
#define VTX_BUF_KIB 1024.0f      /* render_init's vertex_buf_size */
#define WIN_SECS 0.5f

enum { R_CPU, R_UPD, R_WORLD, R_KARTS, R_PT, R_TR, R_PVR, R_HUD, R_TIMES };
static const char *const row_names[R_TIMES] = {"CPU", "upd", "world", "karts", "pt", "tr", "PVR", "hud"};

static struct {
    float sum[R_TIMES], max[R_TIMES];
    float vtx_sum, vtx_max;
    int tris_sum, tris_max, n;
    float t;
} acc;
static struct {
    int missed;
    float t;
} acc_sec;

static struct {
    float avg[R_TIMES], max[R_TIMES];
    float vtx, vtx_peak;         /* KiB */
    int missed;                  /* vblanks missed in the last second */
    char avg_s[R_TIMES][8], max_s[R_TIMES][8];
    char vtx_s[8], vtx_peak_s[8], tris_s[8], tris_max_s[8], missed_s[16];
} shown;

static int band_next;            /* next band of the box to redraw (below) */
static void refresh_band(void);

static float draw_ms, frame_ms;  /* the overlay's own cost: drawing, bookkeeping */
static int draw_tris;

#ifdef DK_PERF_HUD
int perfhud_mode = PERF_OVERLAY;
#else
int perfhud_mode;
#endif

void perfhud_set_mode(int mode) {
    if(mode == PERF_OVERLAY && perfhud_mode != PERF_OVERLAY) {
        memset(&acc, 0, sizeof(acc));
        memset(&acc_sec, 0, sizeof(acc_sec));
        draw_ms = frame_ms = 0;
        draw_tris = 0;
    }
    perfhud_mode = mode;
}

static char *put_int(char *p, int v) {
    char t[12];
    int n = 0;
    if(v < 0) v = 0;
    do t[n++] = (char)('0' + v % 10); while(v /= 10);
    while(n) *p++ = t[--n];
    *p = 0;
    return p;
}

static void put_ms(char *p, float ms) {
    int c = (int)(ms * 100.0f + 0.5f);
    if(c > 99999) c = 99999;
    p = put_int(p, c / 100);
    *p++ = '.';
    *p++ = (char)('0' + c / 10 % 10);
    *p++ = (char)('0' + c % 10);
    *p = 0;
}

static void put_kib(char *p, float k) {
    p = put_int(p, (int)(k + 0.5f));
    *p++ = 'K';
    *p = 0;
}

void perfhud_frame(float dt_real, float cpu_ms, const float phase_ms[5], int missed) {
    uint64_t t0 = timer_us_gettime64();
    float rnd = 0, vtx = 0;
    pvr_stats_t ps;
    if(pvr_get_stats(&ps) == 0) {
        if(ps.rnd_last_time < 100000000ull) rnd = (float)ps.rnd_last_time * 1e-6f;
        vtx = (float)ps.vtx_buffer_used / 1024.0f;
    }
    float v[R_TIMES] = {cpu_ms - draw_ms, phase_ms[0], phase_ms[1], phase_ms[2], phase_ms[3] - draw_ms,
                        phase_ms[4], rnd, draw_ms + frame_ms};
    for(int i = 0; i < R_TIMES; i++) {
        acc.sum[i] += v[i];
        if(v[i] > acc.max[i]) acc.max[i] = v[i];
    }
    int tris = rstats.tris - draw_tris;
    acc.tris_sum += tris;
    if(tris > acc.tris_max) acc.tris_max = tris;
    acc.vtx_sum += vtx;
    /* Our own peak per window, like the other rows: KOS's
       vtx_buffer_used_max is the highest since boot and never comes down. */
    if(vtx > acc.vtx_max) acc.vtx_max = vtx;
    acc.n++;
    acc.t += dt_real;
    if(acc.t >= WIN_SECS) {
        float inv = 1.0f / (float)acc.n;
        for(int i = 0; i < R_TIMES; i++) {
            shown.avg[i] = acc.sum[i] * inv;
            shown.max[i] = acc.max[i];
            put_ms(shown.avg_s[i], shown.avg[i]);
            put_ms(shown.max_s[i], shown.max[i]);
        }
        shown.vtx = acc.vtx_sum * inv;
        shown.vtx_peak = acc.vtx_max;
        /* Flycast leaves the TA's vertex pointer register, which KOS reads
           for these, at zero; the console reports them. */
        if(shown.vtx_peak > 0) {
            put_kib(shown.vtx_s, shown.vtx);
            put_kib(shown.vtx_peak_s, shown.vtx_peak);
        }
        else {
            strcpy(shown.vtx_s, "--");
            strcpy(shown.vtx_peak_s, "--");
        }
        put_int(shown.tris_s, (int)((float)acc.tris_sum * inv + 0.5f));
        put_int(shown.tris_max_s, acc.tris_max);
        memset(&acc, 0, sizeof(acc));
        band_next = 0;
    }
    acc_sec.missed += missed;
    acc_sec.t += dt_real;
    if(acc_sec.t >= 1.0f) {
        shown.missed = acc_sec.missed;
        char *p = shown.missed_s;
        memcpy(p, "MISS ", 5);
        p = put_int(p + 5, acc_sec.missed);
        memcpy(p, "/s", 3);
        acc_sec.missed = 0;
        acc_sec.t = 0;
        band_next = 0;
    }
    refresh_band();
    frame_ms = (float)(timer_us_gettime64() - t0) * 0.001f;
    draw_ms = 0;
    draw_tris = 0;
}

static uint32_t load_color(float f) {
    return f < 0.8f ? argb(255, 70, 220, 90) : f < 1.0f ? argb(255, 255, 190, 40) : argb(255, 255, 60, 50);
}

/* Box layout in texels of the overlay texture (drawn 1:1 on screen). */
#define TEX_W 256
#define TEX_H 256
#define BOX_W 248
#define ROWS (1 + R_TIMES + 2)
#define ROW_H 14
#define BOX_H (ROWS * ROW_H + 6)
#define BAND_H 16
#define BANDS ((BOX_H + BAND_H - 1) / BAND_H)
#define LABEL_X 6
#define AVG_X 96
#define MAX_X 146
#define BAR_X0 154
#define BAR_X1 242
#define TEXT_SCALE 0.38f
#define GLYPH_H 16               /* FONT_ROW_H * TEXT_SCALE, rounded up */
#define BG_COLOR argb(255, 6, 14, 36)

static uint16_t box[TEX_W * BOX_H] __attribute__((aligned(32)));
static pvr_ptr_t box_tex;
static pvr_poly_hdr_t box_hdr __attribute__((aligned(32)));
static int band_next = BANDS;    /* next band to redraw and upload */

static uint16_t rgb565(uint32_t c) {
    return (uint16_t)(((c >> 8) & 0xf800) | ((c >> 5) & 0x07e0) | ((c >> 3) & 0x001f));
}

/* The font atlas shrunk to TEXT_SCALE, one glyph at a time on first use:
   premultiplied colour and coverage, 0..255. */
typedef struct { uint8_t r, g, b, a; } Texel;
static Texel *glyph_px[96];
static uint8_t glyph_w[96];

static const Texel *glyph(unsigned c) {
    if(glyph_px[c - 32] || !font_glyphs[c - 32].w) return glyph_px[c - 32];
    const dk_glyph_t *g = &font_glyphs[c - 32];
    const uint16_t *atlas = (const uint16_t *)(dk_textures_pak + dk_tex_assets[TEX_FONT].offset);
    int w = (int)ceilf(g->w * TEXT_SCALE);
    Texel *px = malloc(sizeof(Texel) * w * GLYPH_H);
    if(!px) return NULL;
    for(int oy = 0; oy < GLYPH_H; oy++) {
        int sy0 = (int)(oy / TEXT_SCALE), sy1 = (int)((oy + 1) / TEXT_SCALE);
        if(sy1 > FONT_ROW_H) sy1 = FONT_ROW_H;
        for(int ox = 0; ox < w; ox++) {
            int sx0 = (int)(ox / TEXT_SCALE), sx1 = (int)((ox + 1) / TEXT_SCALE);
            if(sx1 > g->w) sx1 = g->w;
            unsigned r = 0, gr = 0, b = 0, a = 0, n = 0;
            for(int sy = sy0; sy < sy1; sy++)
                for(int sx = sx0; sx < sx1; sx++) {
                    /* Twiddled 512x256: two 256x256 blocks side by side. */
                    unsigned x = g->x + sx, y = g->row * FONT_ROW_H + sy, t = 0;
                    for(int bit = 0; bit < 8; bit++)
                        t |= ((x >> bit) & 1u) << (bit * 2 + 1) | ((y >> bit) & 1u) << (bit * 2);
                    uint16_t w16 = atlas[(x >> 8) * 65536 + t];
                    unsigned ta = (w16 >> 12) * 17;
                    r += ((w16 >> 8) & 15) * 17 * ta;
                    gr += ((w16 >> 4) & 15) * 17 * ta;
                    b += (w16 & 15) * 17 * ta;
                    a += ta;
                    n++;
                }
            Texel *t = &px[oy * w + ox];
            if(!n) n = 1;
            t->r = (uint8_t)(r / (255 * n));
            t->g = (uint8_t)(gr / (255 * n));
            t->b = (uint8_t)(b / (255 * n));
            t->a = (uint8_t)(a / n);
        }
    }
    glyph_w[c - 32] = (uint8_t)w;
    glyph_px[c - 32] = px;
    return px;
}

/* x / 255 for x up to 255 * 510 without the SH-4's slow software divide. */
#define DIV255(x) ((((x) + 128) * 257) >> 16)

/* Drawing into the box, clipped to rows [cy0, cy1). Glyphs only ever land
   on the plain background, so they blend against its colour. */
static int cy0, cy1;

static void put_rect(int x0, int y0, int x1, int y1, uint32_t col) {
    if(y0 < cy0) y0 = cy0;
    if(y1 > cy1) y1 = cy1;
    uint16_t c = rgb565(col);
    for(int y = y0; y < y1; y++)
        for(int x = x0; x < x1; x++) box[y * TEX_W + x] = c;
}

static void put_text(int x, int y, int align, uint32_t col, const char *s) {
    float pen = (float)x, w = hud_text_width(s, TEXT_SCALE);
    if(align == ALIGN_RIGHT) pen -= w;
    if(y >= cy1 || y + GLYPH_H <= cy0) return;
    const unsigned bg_r = (BG_COLOR >> 16) & 255, bg_g = (BG_COLOR >> 8) & 255, bg_b = BG_COLOR & 255;
    const unsigned tr = (col >> 16) & 255, tg = (col >> 8) & 255, tb = col & 255;
    for(; *s; s++) {
        unsigned c = (unsigned char)*s;
        if(c < 32 || c > 127) c = '?';
        const dk_glyph_t *g = &font_glyphs[c - 32];
        const Texel *px = glyph(c);
        if(px) {
            int gx = (int)(pen + g->ox * TEXT_SCALE + 0.5f), gw = glyph_w[c - 32];
            for(int oy = 0; oy < GLYPH_H; oy++) {
                int by = y + oy;
                if(by < cy0 || by >= cy1) continue;
                for(int ox = 0; ox < gw; ox++) {
                    int bx = gx + ox;
                    const Texel *t = &px[oy * gw + ox];
                    if(!t->a || bx < 0 || bx >= BOX_W) continue;
                    unsigned ia = 255 - t->a;
                    unsigned r = DIV255(bg_r * ia + t->r * tr), gg = DIV255(bg_g * ia + t->g * tg),
                             b = DIV255(bg_b * ia + t->b * tb);
                    box[by * TEX_W + bx] = (uint16_t)((r >> 3) << 11 | (gg >> 2) << 5 | (b >> 3));
                }
            }
        }
        pen += g->adv * TEXT_SCALE;
    }
}

/* A bar for avg and a tick for max, both as fractions of the full scale. */
static void put_bar(int y, float avg, float max) {
    int y0 = y + 4, y1 = y + 11, w = BAR_X1 - BAR_X0;
    put_rect(BAR_X0, y0, BAR_X1, y1, argb(255, 30, 40, 60));
    int a = (int)(w * clampf(avg, 0, 1) + 0.5f), m = BAR_X0 + (int)(w * clampf(max, 0, 1) + 0.5f);
    if(a > 0) put_rect(BAR_X0, y0, BAR_X0 + a, y1, load_color(avg));
    if(m < BAR_X0 + 1) m = BAR_X0 + 1;
    if(m > BAR_X1 - 1) m = BAR_X1 - 1;
    put_rect(m - 1, y0 - 1, m + 1, y1 + 1, load_color(max));
}

/* Redraw rows [y0, y1) of the box. */
static void compose(int y0, int y1) {
    cy0 = y0;
    cy1 = y1;
    put_rect(0, y0, BOX_W, y1, BG_COLOR);
    const uint32_t white = 0xffffffff, dim = argb(255, 150, 170, 200), head = argb(255, 255, 220, 80);
    int y = 2;
    put_text(LABEL_X, y, ALIGN_LEFT, head, "MS");
    put_text(AVG_X, y, ALIGN_RIGHT, head, "AVG");
    put_text(MAX_X, y, ALIGN_RIGHT, head, "MAX");
    put_text(BAR_X0, y, ALIGN_LEFT, head, "/ 16.68");
    y += ROW_H;
    int bars_top = y;
    for(int i = 0; i < R_TIMES; i++, y += ROW_H) {
        bool phase = i >= R_UPD && i <= R_TR;
        put_text(LABEL_X + (phase ? 10 : 0), y, ALIGN_LEFT, phase ? dim : white, row_names[i]);
        put_text(AVG_X, y, ALIGN_RIGHT, white, shown.avg_s[i]);
        put_text(MAX_X, y, ALIGN_RIGHT, dim, shown.max_s[i]);
        put_bar(y, shown.avg[i] / FRAME_MS, shown.max[i] / FRAME_MS);
    }
    /* 80% tick through the time bars. */
    int tx = BAR_X0 + (int)((BAR_X1 - BAR_X0) * 0.8f);
    put_rect(tx, bars_top + 3, tx + 1, y, argb(255, 170, 180, 200));
    put_text(LABEL_X, y, ALIGN_LEFT, white, "VTX");
    put_text(AVG_X, y, ALIGN_RIGHT, white, shown.vtx_s);
    put_text(MAX_X, y, ALIGN_RIGHT, dim, shown.vtx_peak_s);
    put_bar(y, shown.vtx / VTX_BUF_KIB, shown.vtx_peak / VTX_BUF_KIB);
    y += ROW_H;
    put_text(LABEL_X, y, ALIGN_LEFT, white, "TRIS");
    put_text(AVG_X, y, ALIGN_RIGHT, white, shown.tris_s);
    put_text(MAX_X, y, ALIGN_RIGHT, dim, shown.tris_max_s);
    put_text(BAR_X0, y, ALIGN_LEFT, shown.missed ? argb(255, 255, 60, 50) : argb(255, 70, 220, 90),
             shown.missed_s);
}

/* One band a frame after the figures change. The PVR may be reading the
   texture meanwhile; a frame with half the box updated is harmless. */
static void refresh_band(void) {
    if(band_next >= BANDS || !box_tex) return;
    int y0 = band_next * BAND_H, y1 = y0 + BAND_H > BOX_H ? BOX_H : y0 + BAND_H;
    compose(y0, y1);
    pvr_txr_load(&box[y0 * TEX_W], (uint8_t *)box_tex + y0 * TEX_W * 2, (y1 - y0) * TEX_W * 2);
    band_next++;
}

void perfhud_draw(bool split) {
    uint64_t t0 = timer_us_gettime64();
    int tris0 = rstats.tris;
    if(!box_tex) {
        static bool failed;
        if(failed) return;
        box_tex = pvr_mem_malloc(TEX_W * TEX_H * 2);
        if(!box_tex) {
            failed = true;
            printf("DreamKarts perfhud: no VRAM for the overlay\n");
            return;
        }
        pvr_poly_cxt_t c;
        pvr_poly_cxt_txr(&c, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED, TEX_W, TEX_H, box_tex,
                         PVR_FILTER_NEAREST);
        c.gen.culling = PVR_CULLING_NONE;
        c.gen.clip_mode = PVR_USERCLIP_INSIDE;
        c.gen.fog_type = PVR_FOG_DISABLE;
        c.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        c.txr.env = PVR_TXRENV_REPLACE;
        c.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&box_hdr, &c);
        /* Shrink every glyph now, at the first frame drawn, rather than
           whenever a character first turns up mid-race. */
        for(unsigned ch = 33; ch < 127; ch++) glyph(ch);
        band_next = 0;
    }
    /* Top-left, under the lap counter; in split screen inside the top view. */
    float x = 8, y = split ? 48.0f : 78.0f;
    r_hdr(&box_hdr);
    r_quad2d(x, y, x + BOX_W, y + BOX_H, 0, 0, (float)BOX_W / TEX_W, (float)BOX_H / TEX_H, 0xffffffff, 19.0f);
    draw_tris += rstats.tris - tris0;
    draw_ms += (float)(timer_us_gettime64() - t0) * 0.001f;
}

/* Last second's frame rate (main loop's game.fps) in a small box where the
   overlay would sit. A handful of glyphs costs the PVR next to nothing. */
void perfhud_draw_fps(bool split) {
    float k = split ? 0.72f : 1.0f, x = split ? 14.0f : 24.0f, y = split ? 48.0f : 78.0f;
    int fps = (int)(game.fps + 0.5f);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d FPS", fps);
    uint32_t col = fps >= 58 ? argb(255, 70, 220, 90) : fps >= 50 ? argb(255, 255, 190, 40) : argb(255, 255, 60, 50);
    hud_panel(x, y, x + 100 * k, y + 30 * k, argb(150, 20, 60, 160), argb(150, 10, 30, 90), 15);
    hud_text(x + 50 * k, y + 2 * k, 0.6f * k, ALIGN_CENTER, col, buf);
}
