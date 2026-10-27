/* Monitor test patterns, as on an arcade board's service menu: colour bars,
   crosshatch, purity fields, grey scale, ramps, safe areas, sharpness and
   geometry. A hides the caption so a field can be checked edge to edge. */
#include "gfx.h"
#include "kt.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int page;
static bool caption = true;

/* 8x8 textures drawn 1:1 for the one-pixel patterns */
static pvr_ptr_t fine_tex[3];
static pvr_poly_hdr_t fine_hdr[3];

static void fine_init(void) {
    uint16_t px[64];

    if(fine_tex[0])
        return;
    for(int t = 0; t < 3; t++) {
        for(int y = 0; y < 8; y++)
            for(int x = 0; x < 8; x++) {
                bool on = t == 0 ? ((x ^ y) & 1) : t == 1 ? (x & 1) : (y & 1);
                px[y * 8 + x] = on ? 0xffff : 0x0000;
            }
        fine_tex[t] = pvr_mem_malloc(sizeof(px));
        pvr_txr_load_ex(px, fine_tex[t], 8, 8, PVR_TXRLOAD_16BPP);
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565, 8, 8, fine_tex[t],
                         PVR_FILTER_NEAREST);
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
        pvr_poly_compile(&fine_hdr[t], &cxt);
    }
}

static void field(uint32_t c) {
    gfx_op_rect(0, 0, SCREEN_W, SCREEN_H, 2, c);
}

static void bars(void) {
    /* 75% bars, then the reverse-blue row and a PLUGE strip */
    static const uint32_t top[7] = { 0xffbfbfbf, 0xffbfbf00, 0xff00bfbf, 0xff00bf00,
                                     0xffbf00bf, 0xffbf0000, 0xff0000bf };
    static const uint32_t mid[7] = { 0xff0000bf, 0xff131313, 0xffbf00bf, 0xff131313,
                                     0xff00bfbf, 0xff131313, 0xffbfbfbf };
    float w = SCREEN_W / 7.0f;
    for(int i = 0; i < 7; i++) {
        gfx_op_rect(i * w, 0, w + 1, 320, 2, top[i]);
        gfx_op_rect(i * w, 320, w + 1, 40, 2, mid[i]);
    }
    float q = SCREEN_W / 6.0f;
    gfx_op_rect(0, 360, q, 120, 2, 0xff00214c);
    gfx_op_rect(q, 360, q, 120, 2, 0xffffffff);
    gfx_op_rect(2 * q, 360, q, 120, 2, 0xff32006a);
    gfx_op_rect(3 * q, 360, q, 120, 2, 0xff131313);
    gfx_op_rect(4 * q, 360, q / 3, 120, 2, 0xff090909);    /* PLUGE -4% */
    gfx_op_rect(4 * q + q / 3, 360, q / 3, 120, 2, 0xff131313);
    gfx_op_rect(4 * q + 2 * q / 3, 360, q / 3, 120, 2, 0xff1d1d1d);  /* +4% */
    gfx_op_rect(5 * q, 360, q, 120, 2, 0xff131313);
}

static void circle(float cx, float cy, float r, uint32_t c) {
    const int n = 96;
    for(int i = 0; i < n; i++) {
        float a0 = i * 2 * (float)M_PI / n, a1 = (i + 1) * 2 * (float)M_PI / n;
        gfx_line(cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), 2, c);
    }
}

static void crosshatch(void) {
    field(0xff000000);
    for(int x = 0; x <= SCREEN_W; x += 40)
        gfx_op_rect(x == SCREEN_W ? x - 1 : x, 0, 1, SCREEN_H, 3, 0xffffffff);
    for(int y = 0; y <= SCREEN_H; y += 40)
        gfx_op_rect(0, y == SCREEN_H ? y - 1 : y, SCREEN_W, 1, 3, 0xffffffff);
    gfx_op_rect(SCREEN_W / 2 - 1, 0, 2, SCREEN_H, 3, 0xffffffff);
    gfx_op_rect(0, SCREEN_H / 2 - 1, SCREEN_W, 2, 3, 0xffffffff);
    circle(SCREEN_W / 2, SCREEN_H / 2, 200, 0xffffffff);
}

static void dots(void) {
    field(0xff000000);
    for(int y = 20; y < SCREEN_H; y += 40)
        for(int x = 20; x < SCREEN_W; x += 40)
            gfx_op_rect(x - 1, y - 1, 2, 2, 3, 0xffffffff);
}

static void greys(void) {
    for(int i = 0; i < 16; i++) {
        int v = i * 255 / 15;
        gfx_op_rect(i * 40, 0, 40, SCREEN_H, 2, 0xff000000 | (uint32_t)v << 16 | (uint32_t)v << 8 | (uint32_t)v);
    }
}

static void ramps(void) {
    static const uint32_t c[4] = { 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff };
    for(int k = 0; k < 4; k++)
        gfx_op_grad4(0, k * 120, SCREEN_W, 120, 2, 0xff000000, c[k], 0xff000000, c[k]);
}

static void safe_areas(void) {
    field(0xff101010);
    gfx_box(0, 0, SCREEN_W, SCREEN_H, 2, 0xffff4040);
    gfx_box(SCREEN_W * 0.05f, SCREEN_H * 0.05f, SCREEN_W * 0.9f, SCREEN_H * 0.9f, 2, 0xffffd040);
    gfx_box(SCREEN_W * 0.1f, SCREEN_H * 0.1f, SCREEN_W * 0.8f, SCREEN_H * 0.8f, 2, 0xff40ff40);
    gfx_rect(SCREEN_W / 2 - 20, SCREEN_H / 2, 40, 1, 0xffffffff);
    gfx_rect(SCREEN_W / 2, SCREEN_H / 2 - 20, 1, 40, 0xffffffff);
    gfx_text(SCREEN_W * 0.05f + 6, SCREEN_H * 0.05f + 6, 0xffffd040, "90% ACTION SAFE");
    gfx_text(SCREEN_W * 0.1f + 6, SCREEN_H * 0.1f + 6, 0xff40ff40, "80% TITLE SAFE");
    gfx_text(6, 6, 0xffff4040, "100%");
    /* corner ticks every 8 pixels show how much the display crops */
    for(int i = 0; i < 5; i++) {
        float d = i * 8.0f;
        gfx_rect(d, SCREEN_H / 2 - 30, 1, 60, 0xffffffff);
        gfx_rect(SCREEN_W - 1 - d, SCREEN_H / 2 - 30, 1, 60, 0xffffffff);
        gfx_rect(SCREEN_W / 2 - 30, d, 60, 1, 0xffffffff);
        gfx_rect(SCREEN_W / 2 - 30, SCREEN_H - 1 - d, 60, 1, 0xffffffff);
    }
}

static void fine_block(int t, float x, float y, float w, float h) {
    gfx_quad(PVR_LIST_OP_POLY, &fine_hdr[t], x, y, x + w, y + h, 3, 0, 0, w / 8, h / 8,
             0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff);
}

static void sharpness(void) {
    fine_init();
    field(0xff000000);
    fine_block(0, 32, 48, 176, 176);
    fine_block(1, 232, 48, 176, 176);
    fine_block(2, 432, 48, 176, 176);
    gfx_text_center(120, 228, C_TEXT, "1PX CHECKER", false);
    gfx_text_center(320, 228, C_TEXT, "1PX COLUMNS", false);
    gfx_text_center(520, 228, C_TEXT, "1PX ROWS", false);
    /* line pairs getting finer: 8, 4, 2, 1 pixels */
    for(int k = 0; k < 4; k++) {
        int w = 8 >> k;
        float x0 = 32 + k * 150;
        for(int x = 0; x < 128; x += 2 * w)
            gfx_op_rect(x0 + x, 270, w, 120, 3, 0xffffffff);
        char s[16];
        snprintf(s, sizeof(s), "%d PX", w);
        gfx_text_center(x0 + 64, 394, C_TEXT, s, false);
    }
}

static void geometry(void) {
    field(0xff000020);
    circle(SCREEN_W / 2, SCREEN_H / 2, 230, 0xffffffff);
    circle(SCREEN_W / 2, SCREEN_H / 2, 120, 0xffffffff);
    for(int i = 0; i < 4; i++) {
        float x = i & 1 ? SCREEN_W - 70 : 10, y = i & 2 ? SCREEN_H - 70 : 10;
        gfx_box(x, y, 60, 60, 2, 0xffffffff);
        circle(x + 30, y + 30, 24, 0xffffd040);
    }
    gfx_rect(SCREEN_W / 2 - 1, 0, 2, SCREEN_H, 0xff808080);
    gfx_rect(0, SCREEN_H / 2 - 1, SCREEN_W, 2, 0xff808080);
}

static void red(void) { field(0xffff0000); }
static void green(void) { field(0xff00ff00); }
static void blue(void) { field(0xff0000ff); }
static void white(void) { field(0xffffffff); }
static void black(void) { field(0xff000000); }

static const struct {
    const char *name;
    void (*draw)(void);
} pages[] = {
    { "COLOUR BARS", bars },
    { "CROSSHATCH", crosshatch },
    { "CONVERGENCE DOTS", dots },
    { "PURITY: RED", red },
    { "PURITY: GREEN", green },
    { "PURITY: BLUE", blue },
    { "PURITY: WHITE", white },
    { "PURITY: BLACK", black },
    { "GREY SCALE (16 STEPS)", greys },
    { "COLOUR RAMPS", ramps },
    { "SAFE AREAS", safe_areas },
    { "SHARPNESS", sharpness },
    { "GEOMETRY", geometry },
};
#define PAGES ((int)(sizeof(pages) / sizeof(pages[0])))

static void monitor_enter(void) {
    caption = true;
}

static void monitor_update(const kt_input_t *in) {
    if(in->pressed & (CONT_B | CONT_START)) {
        kt_pop();
        return;
    }
    if(in->pressed & CONT_DPAD_RIGHT) page = (page + 1) % PAGES;
    if(in->pressed & CONT_DPAD_LEFT) page = (page + PAGES - 1) % PAGES;
    if(in->pressed & CONT_A) caption = !caption;
}

static void monitor_draw(void) {
    char s[64];

    pages[page].draw();
    if(!caption)
        return;
    snprintf(s, sizeof(s), "%d/%d %s", page + 1, PAGES, pages[page].name);
    float w = 340;
    gfx_rect(SCREEN_W / 2 - w / 2, SCREEN_H - 92, w, 46, 0xd0000000);
    gfx_box(SCREEN_W / 2 - w / 2, SCREEN_H - 92, w, 46, 1, C_LINE);
    gfx_text_center(SCREEN_W / 2, SCREEN_H - 88, C_HI, s, false);
    gfx_text_center(SCREEN_W / 2, SCREEN_H - 68, C_DIM, "LEFT/RIGHT  A HIDE TEXT  B BACK", false);
}

const kt_screen_t monitor_screen = {
    "Monitor test", monitor_enter, monitor_update, monitor_draw, NULL
};

#ifdef KT_AUTOTEST
int monitor_page_count(void) {
    return PAGES;
}

void monitor_autotest_page(int p) {
    if(kt_current() != &monitor_screen)
        kt_push(&monitor_screen);
    page = p;
    KT_LOG("Monitor page %d: %s\n", p + 1, pages[p].name);
}
#endif
