/* GPU benchmarks. Each draws its own frames for about two thirds of a
   second; GPU time is the PVR's render time from pvr_get_stats, CPU time is
   how long submitting to the tile accelerator took. Also the lit, textured
   torus that the 3D scene benchmark and the GPU test page share. */
#include "bench.h"
#include "gfx.h"
#include "gpu.h"

#include <dc/matrix.h>
#include <dc/matrix3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 40
#define SKIP_FRAMES 8

/* ---- Direct rendering helpers ---- */

static inline void dr_hdr(const pvr_poly_hdr_t *h) {
    uint32_t *d = pvr_dr_target();
    const uint32_t *s = (const uint32_t *)h;
    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
    d[4] = s[4]; d[5] = s[5]; d[6] = s[6]; d[7] = s[7];
    pvr_dr_commit(d);
}

static inline void dr_vert(uint32_t flags, float x, float y, float z, float u, float v,
                           uint32_t argb) {
    pvr_vertex_t *vt = pvr_dr_target();
    vt->flags = flags;
    vt->x = x;
    vt->y = y;
    vt->z = z;
    vt->u = u;
    vt->v = v;
    vt->argb = argb;
    vt->oargb = 0;
    pvr_dr_commit(vt);
}

static inline void dr_quad(float x0, float y0, float x1, float y1, float z, uint32_t argb) {
    dr_vert(PVR_CMD_VERTEX, x0, y0, z, 0, 0, argb);
    dr_vert(PVR_CMD_VERTEX, x1, y0, z, 1, 0, argb);
    dr_vert(PVR_CMD_VERTEX, x0, y1, z, 0, 1, argb);
    dr_vert(PVR_CMD_VERTEX_EOL, x1, y1, z, 1, 1, argb);
}

/* ---- Shared test texture ---- */

static pvr_ptr_t tex256;

/* 256x256 RGB565: a colour gradient with a checker and grid lines. */
pvr_ptr_t gpu_test_texture(void) {
    if(tex256)
        return tex256;
    uint16_t *px = malloc(256 * 256 * 2);
    for(int y = 0; y < 256; y++)
        for(int x = 0; x < 256; x++) {
            int r = x, g = y, b = 255 - (x + y) / 2;
            if(((x >> 5) ^ (y >> 5)) & 1) { r = r * 3 / 4; g = g * 3 / 4; b = b * 3 / 4; }
            if((x & 31) == 0 || (y & 31) == 0) r = g = b = 255;
            px[y * 256 + x] = (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
        }
    tex256 = pvr_mem_malloc(256 * 256 * 2);
    pvr_txr_load_ex(px, tex256, 256, 256, PVR_TXRLOAD_16BPP);
    free(px);
    return tex256;
}

/* ---- The torus ---- */

#define TU 48
#define TV 24

static float tor_pos[(TU + 1) * (TV + 1)][3];
static float tor_nrm[(TU + 1) * (TV + 1)][3];
static float tor_uv[(TU + 1) * (TV + 1)][2];
static pvr_poly_hdr_t tor_hdr, tor_hdr_flat;
static float tor_angle;
static float tor_x, tor_y, tor_dist = 4.0f;
static float tor_cx = SCREEN_W / 2, tor_cy = SCREEN_H / 2;
static bool tor_textured = true;

static void torus_init(void) {
    static bool done;
    pvr_poly_cxt_t cxt;

    if(done)
        return;
    done = true;
    for(int u = 0; u <= TU; u++)
        for(int v = 0; v <= TV; v++) {
            float a = u * 2 * (float)M_PI / TU, b = v * 2 * (float)M_PI / TV;
            float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
            int i = u * (TV + 1) + v;
            tor_pos[i][0] = (1.0f + 0.42f * cb) * ca;
            tor_pos[i][1] = 0.42f * sb;
            tor_pos[i][2] = (1.0f + 0.42f * cb) * sa;
            tor_nrm[i][0] = cb * ca;
            tor_nrm[i][1] = sb;
            tor_nrm[i][2] = cb * sa;
            tor_uv[i][0] = (float)u / TU * 4;
            tor_uv[i][1] = (float)v / TV;
        }
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256,
                     gpu_test_texture(), PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.gen.shading = true;
    pvr_poly_compile(&tor_hdr, &cxt);
    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&tor_hdr_flat, &cxt);
}

void torus_set(float angle, float x, float y, float dist, bool textured) {
    tor_angle = angle;
    tor_x = x;
    tor_y = y;
    tor_dist = dist;
    tor_textured = textured;
}

void torus_view(float cx, float cy) {
    tor_cx = cx;
    tor_cy = cy;
}

int torus_triangles(void) {
    return TU * TV * 2;
}

/* Submit the torus straight to the TA; for gfx_direct on the opaque list. */
void torus_submit(void *unused) {
    static matrix_t rot __attribute__((aligned(32)));
    float a = tor_angle, b = tor_angle * 0.7f;
    float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
    /* R = Ry(a) * Rx(b), as rows */
    float R[3][3] = {
        { ca, sa * sb, sa * cb },
        { 0, cb, -sb },
        { -sa, ca * sb, ca * cb },
    };

    torus_init();
    memset(rot, 0, sizeof(rot));
    for(int i = 0; i < 3; i++)
        for(int j = 0; j < 3; j++)
            rot[j][i] = R[i][j];
    rot[3][3] = 1;
    mat_identity();
    mat_perspective(tor_cx, tor_cy, 1.0f / tanf(30.0f * (float)M_PI / 180.0f), 0.5f, 50.0f);
    mat_translate(tor_x, tor_y, -tor_dist);
    mat_apply(&rot);

    /* Light in object space: R^T times the world light. */
    const float lw[3] = { 0.4f, -0.6f, 0.7f };
    float lo[3];
    for(int j = 0; j < 3; j++)
        lo[j] = R[0][j] * lw[0] + R[1][j] * lw[1] + R[2][j] * lw[2];

    dr_hdr(tor_textured ? &tor_hdr : &tor_hdr_flat);
    for(int u = 0; u < TU; u++) {
        for(int v = 0; v <= TV; v++) {
            for(int k = 0; k < 2; k++) {
                int i = (u + k) * (TV + 1) + v;
                float x = tor_pos[i][0], y = tor_pos[i][1], z = tor_pos[i][2];
                mat_trans_single(x, y, z);
                float d = tor_nrm[i][0] * lo[0] + tor_nrm[i][1] * lo[1] + tor_nrm[i][2] * lo[2];
                if(d < 0) d = 0;
                int c = 60 + (int)(d * 195);
                uint32_t argb = tor_textured ? 0xff000000 | c << 16 | c << 8 | c :
                                0xff000000 | c << 16 | (c * 3 / 4) << 8 | (c / 3);
                bool last = v == TV && k == 1;
                dr_vert(last ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX, x, y, z, tor_uv[i][0],
                        tor_uv[i][1], argb);
            }
        }
    }
}

/* ---- Frame runner ---- */

typedef struct {
    void (*op)(void *);
    void (*tr)(void *);
    void (*pt)(void *);
    void *arg;
} bench_lists_t;

static uint64_t cpu_us_acc;
static const bench_lists_t *cur_lists;

static void timed_op(void *a) {
    uint64_t t = now_us();
    cur_lists->op(cur_lists->arg);
    cpu_us_acc += now_us() - t;
}

static void timed_tr(void *a) {
    uint64_t t = now_us();
    cur_lists->tr(cur_lists->arg);
    cpu_us_acc += now_us() - t;
}

static void timed_pt(void *a) {
    uint64_t t = now_us();
    cur_lists->pt(cur_lists->arg);
    cpu_us_acc += now_us() - t;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

typedef struct {
    float gpu_us;   /* median render time */
    float cpu_us;   /* median submit time */
} bench_time_t;

static bench_time_t run_frames(const char *label, const bench_lists_t *l) {
    float gpu[FRAMES], cpu[FRAMES];
    int n = 0;
    pvr_stats_t st;
    bench_time_t out = { 0 };

    cur_lists = l;
    for(int f = 0; f < FRAMES; f++) {
        cpu_us_acc = 0;
        gfx_begin();
        if(l->op) gfx_direct(PVR_LIST_OP_POLY, timed_op, NULL);
        if(l->tr) gfx_direct(PVR_LIST_TR_POLY, timed_tr, NULL);
        if(l->pt) gfx_direct(PVR_LIST_PT_POLY, timed_pt, NULL);
        gfx_rect(8, 8, strlen(label) * FONT_W + 16, 24, 0xc0000000);
        gfx_text(16, 12, C_HI, label);
        gfx_end();
        pvr_get_stats(&st);
        if(f >= SKIP_FRAMES) {
            gpu[n] = st.rnd_last_time / 1000.0f;
            cpu[n] = (float)cpu_us_acc;
            n++;
        }
    }
    pvr_wait_ready();
    qsort(gpu, n, sizeof(float), cmp_float);
    qsort(cpu, n, sizeof(float), cmp_float);
    out.gpu_us = gpu[n / 2];
    out.cpu_us = cpu[n / 2];
    return out;
}

static void result_gpu(kt_report_t *r, int id, double units, float us) {
    if(us <= 0)
        report_add(r, KT_SKIP, "GPU timing", "no render time from pvr_get_stats");
    else
        bench_result(r, id, units / us);
}

/* ---- Workloads ---- */

#define FILL_OP_LAYERS 16
#define FILL_TR_LAYERS 8

static pvr_poly_hdr_t hdr_tr_tex;

static void op_layers(void *a) {
    dr_hdr(&gfx_hdr_op_flat);
    for(int i = 0; i < FILL_OP_LAYERS; i++) {
        uint32_t c = 0xff000000 | (uint32_t)(i * 15) << 16 | (uint32_t)(255 - i * 15) << 8 | 0x40;
        dr_quad(0, 0, SCREEN_W, SCREEN_H, 1.0f + i, c);
    }
}

static void tr_layers(void *a) {
    dr_hdr(&gfx_hdr_tr_flat);
    for(int i = 0; i < FILL_TR_LAYERS; i++)
        dr_quad(0, 0, SCREEN_W, SCREEN_H, 10.0f, 0x20000000 | (uint32_t)(i * 30) << 16 | 0x80ff);
}

static void tr_tex_layers(void *a) {
    dr_hdr(&hdr_tr_tex);
    for(int i = 0; i < FILL_TR_LAYERS; i++)
        dr_quad(0, 0, SCREEN_W, SCREEN_H, 10.0f, 0x30ffffff);
}

#define GRID_W 80
#define GRID_H 60
#define GRID_QUADS (GRID_W * GRID_H)

static pvr_poly_hdr_t hdr_op_tex;
static bool grid_textured;

static void grid_dr(void *a) {
    dr_hdr(grid_textured ? &hdr_op_tex : &gfx_hdr_op_flat);
    for(int y = 0; y < GRID_H; y++)
        for(int x = 0; x < GRID_W; x++) {
            float x0 = x * 8.0f, y0 = y * 8.0f;
            uint32_t c = 0xff000000 | (uint32_t)(x * 3) << 16 | (uint32_t)(y * 4) << 8 | 0x80;
            dr_vert(PVR_CMD_VERTEX, x0, y0, 2.0f, 0, 0, c);
            dr_vert(PVR_CMD_VERTEX, x0 + 7, y0, 2.0f, 0.03f, 0, c);
            dr_vert(PVR_CMD_VERTEX, x0, y0 + 7, 2.0f, 0, 0.03f, c);
            dr_vert(PVR_CMD_VERTEX_EOL, x0 + 7, y0 + 7, 2.0f, 0.03f, 0.03f, c);
        }
}

static void grid_prim(void *a) {
    pvr_vertex_t v __attribute__((aligned(32)));

    pvr_prim(&gfx_hdr_op_flat, sizeof(pvr_poly_hdr_t));
    v.oargb = 0;
    v.u = v.v = 0;
    v.z = 2.0f;
    for(int y = 0; y < GRID_H; y++)
        for(int x = 0; x < GRID_W; x++) {
            float x0 = x * 8.0f, y0 = y * 8.0f;
            v.argb = 0xff000000 | (uint32_t)(x * 3) << 16 | (uint32_t)(y * 4) << 8 | 0x80;
            v.flags = PVR_CMD_VERTEX; v.x = x0; v.y = y0; pvr_prim(&v, sizeof(v));
            v.x = x0 + 7; pvr_prim(&v, sizeof(v));
            v.x = x0; v.y = y0 + 7; pvr_prim(&v, sizeof(v));
            v.flags = PVR_CMD_VERTEX_EOL; v.x = x0 + 7; pvr_prim(&v, sizeof(v));
        }
}

static pvr_sprite_hdr_t hdr_sprite;

static void sprites(void *a) {
    dr_hdr((const pvr_poly_hdr_t *)&hdr_sprite);
    uint32_t uv0 = PVR_PACK_16BIT_UV(0, 0), uv1 = PVR_PACK_16BIT_UV(0.25f, 0),
             uv2 = PVR_PACK_16BIT_UV(0.25f, 0.25f);
    for(int y = 0; y < GRID_H; y++)
        for(int x = 0; x < GRID_W; x++) {
            float x0 = x * 8.0f, y0 = y * 8.0f;
            uint32_t *d = pvr_dr_target();
            pvr_sprite_txr_t *s = (pvr_sprite_txr_t *)d;
            s->flags = PVR_CMD_VERTEX_EOL;
            s->ax = x0; s->ay = y0; s->az = 2.0f;
            s->bx = x0 + 7; s->by = y0; s->bz = 2.0f;
            s->cx = x0 + 7;
            pvr_dr_commit(d);
            /* The second half of the 64-byte sprite */
            float *e = pvr_dr_target();
            e[0] = y0 + 7;      /* cy */
            e[1] = 2.0f;        /* cz */
            e[2] = x0;          /* dx */
            e[3] = y0 + 7;      /* dy */
            ((uint32_t *)e)[4] = 0;
            ((uint32_t *)e)[5] = uv0;
            ((uint32_t *)e)[6] = uv1;
            ((uint32_t *)e)[7] = uv2;
            pvr_dr_commit(e);
        }
}

static void headers_init(void) {
    static bool done;
    pvr_poly_cxt_t cxt;
    pvr_sprite_cxt_t sc;

    if(done)
        return;
    done = true;
    pvr_ptr_t t = gpu_test_texture();
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256,
                     t, PVR_FILTER_BILINEAR);
    cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = false;
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr_tr_tex, &cxt);
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256,
                     t, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_poly_compile(&hdr_op_tex, &cxt);
    pvr_sprite_cxt_txr(&sc, PVR_LIST_OP_POLY, PVR_TXRFMT_RGB565 | PVR_TXRFMT_TWIDDLED, 256, 256,
                       t, PVR_FILTER_BILINEAR);
    sc.gen.culling = PVR_CULLING_NONE;
    sc.depth.comparison = PVR_DEPTHCMP_GEQUAL;
    pvr_sprite_compile(&hdr_sprite, &sc);
    hdr_sprite.argb = 0xffffffff;
    hdr_sprite.oargb = 0;
}

/* ---- Steps ---- */

static void step_fill(kt_report_t *r) {
    headers_init();
    const double px = SCREEN_W * SCREEN_H;
    bench_lists_t l = { op_layers, NULL, NULL, NULL };
    bench_time_t t = run_frames("OPAQUE OVERDRAW x16", &l);
    result_gpu(r, B_GPU_FILL_OP, px * FILL_OP_LAYERS, t.gpu_us);
    l = (bench_lists_t){ NULL, tr_layers, NULL, NULL };
    t = run_frames("BLENDED FILL x8", &l);
    result_gpu(r, B_GPU_FILL_TR, px * FILL_TR_LAYERS, t.gpu_us);
    l = (bench_lists_t){ NULL, tr_tex_layers, NULL, NULL };
    t = run_frames("TEXTURED BLENDED FILL x8", &l);
    result_gpu(r, B_GPU_FILL_TEX, px * FILL_TR_LAYERS, t.gpu_us);
}

static void step_tris(kt_report_t *r) {
    headers_init();
    bench_lists_t l = { grid_dr, NULL, NULL, NULL };
    grid_textured = false;
    bench_time_t t = run_frames("9600 FLAT TRIANGLES", &l);
    result_gpu(r, B_GPU_TRI_FLAT, GRID_QUADS * 2, t.gpu_us);
    if(t.cpu_us > 0)
        bench_result(r, B_GPU_TA_DR, GRID_QUADS * 4 / t.cpu_us);
    grid_textured = true;
    t = run_frames("9600 TEXTURED TRIANGLES", &l);
    result_gpu(r, B_GPU_TRI_TEX, GRID_QUADS * 2, t.gpu_us);
    l = (bench_lists_t){ grid_prim, NULL, NULL, NULL };
    t = run_frames("9600 TRIANGLES VIA PVR_PRIM", &l);
    if(t.cpu_us > 0)
        bench_result(r, B_GPU_TA_PRIM, GRID_QUADS * 4 / t.cpu_us);
    l = (bench_lists_t){ sprites, NULL, NULL, NULL };
    t = run_frames("4800 SPRITES", &l);
    result_gpu(r, B_GPU_SPRITE, GRID_QUADS, t.gpu_us);
}

static void step_scene(kt_report_t *r) {
    bench_lists_t l = { torus_submit, NULL, NULL, NULL };
    float cpu = 0, gpu = 0;

    for(int k = 0; k < 2; k++) {
        torus_set(k * 1.3f, 0, 0, 3.2f, true);
        bench_time_t t = run_frames("3D SCENE: LIT TEXTURED TORUS", &l);
        cpu += t.cpu_us / 2;
        gpu += t.gpu_us / 2;
    }
    float frame = cpu > gpu ? cpu : gpu;
    if(frame > 0)
        bench_result(r, B_GPU_SCENE, 1e6 / frame);
    report_add(r, KT_INFO, "3D scene", "%d tris, CPU %.2f ms, GPU %.2f ms", torus_triangles(),
               (double)cpu / 1000, (double)gpu / 1000);
}

const kt_step_t bench_gpu_steps[] = {
    { "GPU fill rate", step_fill },
    { "GPU triangles and sprites", step_tris },
    { "GPU 3D scene", step_scene },
};
const int bench_gpu_step_count = sizeof(bench_gpu_steps) / sizeof(bench_gpu_steps[0]);
