/* Console microbenchmark for DreamKarts' assembly vertex paths
   (src/xform.S): cycles a vertex (or triangle) for each routine, with its
   data in the operand cache and streamed from RAM, and the share of those
   cycles stalled on operand-cache misses. A run takes about a minute, so
   it is the quick way to try a change to a loop before a full hwbench. Store-queue writes go to a cached
   scratch line (as the benchmark's -ta probe does), so no PVR is needed.

   make -C tools/microbench, then ../../../../tools/dc-hw.sh run -c -o
   -u 'MICRO: done' micro.elf. */
#include <kos.h>
#include <dc/perfctr.h>
#include <stdlib.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

typedef struct { float x, y, z, u, v; uint32_t c; } RV;
typedef struct { float near_w, xmax, y0, y1; uint8_t *codes; const uint32_t *col; } XfParams;
typedef struct { float x, y, z; uint16_t group, pad; } KV;
typedef struct { const pvr_vertex_t *pv; const uint8_t *codes; uint32_t clip_mask, sent, back; } MeshSubmit;
uint32_t dk_xf_stage(const RV *v, int n, pvr_vertex_t *out, const XfParams *p);
uint32_t dk_xf_stage_kv(const KV *v, int n, pvr_vertex_t *out, const XfParams *p);
void dk_sq_run(const pvr_vertex_t *src, int n);
void dk_xf_emit(const RV *v, int n);
int dk_sq_tris(MeshSubmit *ms, const uint16_t *idx, int ntri, int cull);
typedef struct { const float (*pos)[4]; const uint32_t *col; uint32_t sent, back; } StripSubmit;
uint32_t dk_xf_pos(const float (*pos)[4], int n, float (*out)[4], const XfParams *p);
void dk_sq_strips(StripSubmit *ss, const uint16_t *strips);

#define BIG (1 << 21)   /* bytes streamed for the cold cases, > 100x the cache */
static uint8_t sink[64] __attribute__((aligned(64)));
static pvr_vertex_t stage[1024] __attribute__((aligned(32)));
static uint8_t codes[1024];

static inline uint32_t cyc(void) { return *(volatile uint32_t *)0xff100008; }
static inline uint32_t ev(void) { return *(volatile uint32_t *)0xff100010; }

static void report(const char *name, uint32_t c, uint32_t e, int units) {
    printf("MICRO: %-28s %6.1f cycles each, %4.1f%% stalled on cache misses\n", name, (double)c / units,
           c ? 100.0 * e / c : 0.0);
}

/* A camera looking down +Z from (0, 2, -10) with the game's projection. */
static void load_camera(void) {
    static float m[16] __attribute__((aligned(8)));
    float F = 320.0f / 0.7f, CX = 320, CY = 240;
    float row[4][4] = {{F, 0, CX, CX * 10}, {0, -F, CY, F * 2 + CY * 10}, {0, 0, 1, 10}, {0, 0, 1, 10}};
    for(int c = 0; c < 4; c++)
        for(int r = 0; r < 4; r++) m[c * 4 + r] = row[r][c];
    float *p = m;
    __asm__ volatile(
        "fschg\n"
        "fmov.d @%0+,xd0\n fmov.d @%0+,xd2\n fmov.d @%0+,xd4\n fmov.d @%0+,xd6\n"
        "fmov.d @%0+,xd8\n fmov.d @%0+,xd10\n fmov.d @%0+,xd12\n fmov.d @%0+,xd14\n"
        "fschg\n"
        : "+r"(p) : : "memory");
}

int main(int argc, char **argv) {
    perf_cntr_start(PRFC1, PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, PMCR_COUNT_CPU_CYCLES);
    RV *rv = memalign(32, BIG);
    float *pos = memalign(32, BIG);
    uint32_t *col = memalign(32, BIG / 3);
    uint16_t *idx = memalign(32, 6 * 4096);
    int nrv = BIG / sizeof(RV), npos = BIG / 12;
    for(int i = 0; i < nrv; i++)
        rv[i] = (RV){(float)(i % 64) - 32.0f, (float)((i * 7) % 13) * 0.3f, 5.0f + (float)(i % 97),
                     0.1f * i, 0.2f, 0xff808080u + i};
    for(int i = 0; i < npos * 3; i++) pos[i] = ((float *)rv)[(i / 3) * 6 + i % 3];
    for(int i = 0; i < npos && i < BIG / 12; i++) col[i % (BIG / 12)] = 0xff406080u + i;
    for(int t = 0; t < 4096; t++) {
        idx[t * 3] = (uint16_t)(t % 200);
        idx[t * 3 + 1] = (uint16_t)((t * 3 + 1) % 200);
        idx[t * 3 + 2] = (uint16_t)((t * 7 + 2) % 200);
    }
    load_camera();
    KV *kv = memalign(32, 1024 * sizeof(KV));
    for(int i = 0; i < 1024; i++) kv[i] = (KV){pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2], (uint16_t)(i % 300), 0};
    XfParams xp = {0.25f, 640.0f, 0.0f, 480.0f, codes, col};
    uint32_t saved_dr = pvr_dr_addr;
    pvr_dr_addr = (uint32_t)sink;
    uint32_t c0, e0;

    for(int pass = 0; pass < 2; pass++) {
        printf("MICRO: pass %d\n", pass);
        /* Hot: one 11- and one 200-vertex block, repeated. */
        for(int n = 11; n <= 200; n += 189) {
            dk_xf_stage(rv, n, stage, &xp);
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_xf_stage(rv, n, stage, &xp);
            char name[48];
            snprintf(name, sizeof(name), "xf_stage hot n=%d", n);
            report(name, cyc() - c0, ev() - e0, 200 * n);
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_xf_stage_kv(kv, n, stage, &xp);
            snprintf(name, sizeof(name), "xf_stage_kv hot n=%d", n);
            report(name, cyc() - c0, ev() - e0, 200 * n);
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_xf_emit(rv, n);
            snprintf(name, sizeof(name), "xf_emit hot n=%d", n);
            report(name, cyc() - c0, ev() - e0, 200 * n);
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_sq_run(stage, n);
            snprintf(name, sizeof(name), "sq_run hot n=%d", n);
            report(name, cyc() - c0, ev() - e0, 200 * n);
        }
        /* Cold: stream the whole array in 11-vertex strips. */
        int strips = nrv / 11;
        c0 = cyc(); e0 = ev();
        for(int s = 0; s < strips; s++) dk_xf_stage(rv + s * 11, 11, stage, &xp);
        report("xf_stage cold n=11", cyc() - c0, ev() - e0, strips * 11);

        c0 = cyc(); e0 = ev();
        for(int s = 0; s < strips; s++) dk_xf_emit(rv + s * 11, 11);
        report("xf_emit cold n=11", cyc() - c0, ev() - e0, strips * 11);
        c0 = cyc(); e0 = ev();
        for(int s = 0; s < nrv / 240; s++) dk_xf_stage(rv + s * 240, 240, stage, &xp);
        report("xf_stage cold n=240", cyc() - c0, ev() - e0, (nrv / 240) * 240);
        /* Hot again with the source placed so it cannot share cache lines
           with the staging buffer (8 KB apart, modulo the 16 KB cache). */
        {
            uintptr_t want = ((uintptr_t)stage + 8192) & 16383;
            uintptr_t base = (uintptr_t)rv + (1 << 20);
            base += (want - (base & 16383)) & 16383;
            RV *far = (RV *)base;
            memcpy(far, rv, 200 * sizeof(RV));
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_xf_stage(far, 200, stage, &xp);
            report("xf_stage hot n=200 apart (b)", cyc() - c0, ev() - e0, 200 * 200);
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_xf_stage(far, 200, stage, &xp);
            report("xf_stage hot n=200 apart", cyc() - c0, ev() - e0, 200 * 200);
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_xf_stage(far, 11, stage, &xp);
            report("xf_stage hot n=11 apart", cyc() - c0, ev() - e0, 200 * 11);
        }
        /* Triangles from 200 staged vertices, all in view and front-facing
           or not as the synthetic indices fall. */
        dk_xf_stage(rv, 200, stage, &xp);
        for(int i = 0; i < 200; i++) codes[i] = 0;
        MeshSubmit ms = {stage, codes, 16, 0, 0};
        c0 = cyc(); e0 = ev();
        for(int r = 0; r < 20; r++) dk_sq_tris(&ms, idx, 4096, 1);
        report("sq_tris cull (per tri)", cyc() - c0, ev() - e0, 20 * 4096);
        printf("MICRO: sq_tris sent %lu back %lu\n", (unsigned long)ms.sent, (unsigned long)ms.back);
        c0 = cyc(); e0 = ev();
        for(int r = 0; r < 20; r++) dk_sq_tris(&ms, idx, 4096, 0);
        report("sq_tris no cull (per tri)", cyc() - c0, ev() - e0, 20 * 4096);
        /* The kart fast path: 200 distinct positions, then strips of 8
           triangles over them (synthetic, so front and back faces mix). */
        static float p4[200][4] __attribute__((aligned(32))), pout[200][4] __attribute__((aligned(32)));
        static uint16_t kstrips[4096 / 8 * 21 + 1];
        for(int i = 0; i < 200; i++) {
            p4[i][0] = pos[i * 3];
            p4[i][1] = pos[i * 3 + 1];
            p4[i][2] = pos[i * 3 + 2];
        }
        int o = 0;
        for(int s = 0; s < 4096 / 8; s++) {
            kstrips[o++] = 10;
            for(int k = 0; k < 10; k++) {
                kstrips[o++] = (uint16_t)(((s * 7 + k * 13) % 200) * 16);
                kstrips[o++] = (uint16_t)(((s + k) % 150) * 4);
            }
        }
        kstrips[o] = 0;
        c0 = cyc(); e0 = ev();
        for(int r = 0; r < 200; r++) dk_xf_pos(p4, 200, pout, &xp);
        report("xf_pos hot n=200", cyc() - c0, ev() - e0, 200 * 200);
        StripSubmit ss = {pout, col, 0, 0};
        c0 = cyc(); e0 = ev();
        for(int r = 0; r < 20; r++) dk_sq_strips(&ss, kstrips);
        report("sq_strips (per tri)", cyc() - c0, ev() - e0, 20 * 4096);
        printf("MICRO: sq_strips sent %lu back %lu\n", (unsigned long)ss.sent, (unsigned long)ss.back);
    }
    /* The kart path with controlled cache placement: vertices, colours
       and the staging window at chosen offsets modulo 16 KB. */
    {
        uint8_t *arena = memalign(16384, 1 << 18);
        static const struct { int kv, col, stg; const char *name; } lay[] = {
            {0, 4096, 8192, "apart"}, {0, 1024, 8192, "col on kv"}, {0, 4096, 1024, "stage on kv"}};
        for(int k = 0; k < 3; k++) {
            KV *pkv = (KV *)(arena + lay[k].kv);
            uint32_t *pcol = (uint32_t *)(arena + 65536 + lay[k].col);
            pvr_vertex_t *pst = (pvr_vertex_t *)(arena + 131072 + lay[k].stg);
            for(int i = 0; i < 200; i++) pkv[i] = (KV){pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2], (uint16_t)(i * 7 % 150), 0};
            for(int i = 0; i < 150; i++) pcol[i] = col[i];
            XfParams kp = xp;
            kp.col = pcol;
            dk_xf_stage_kv(pkv, 200, pst, &kp);
            c0 = cyc(); e0 = ev();
            for(int r = 0; r < 200; r++) dk_xf_stage_kv(pkv, 200, pst, &kp);
            char name[48];
            snprintf(name, sizeof(name), "xf_stage_kv n=200 %s", lay[k].name);
            report(name, cyc() - c0, ev() - e0, 200 * 200);
        }
    }
    /* dk_xf_emit's last two vertices must be dk_xf_stage's, the last
       flagged end of strip (the sink only holds the last two). */
    {
        static pvr_vertex_t ref[512] __attribute__((aligned(32)));
        int bad = 0;
        for(int n = 1; n <= 300; n++)
            for(int off = 0; off < 3; off++) {
                const RV *src = rv + off * 41;
                dk_xf_stage(src, n, ref, &xp);
                ref[n - 1].flags = 0xf0000000;
                pvr_dr_addr = (uint32_t)sink;
                memset(sink, 0x55, sizeof(sink));
                dk_xf_emit(src, n);
                const pvr_vertex_t *last = (const pvr_vertex_t *)pvr_dr_addr;
                const pvr_vertex_t *prev = (const pvr_vertex_t *)(pvr_dr_addr ^ 32);
                if(memcmp(last, &ref[n - 1], 32)) bad++;
                if(n >= 2 && memcmp(prev, &ref[n - 2], 32)) bad++;
            }
        printf("MICRO: xf_emit vs xf_stage mismatches %d of 900\n", bad);
        pvr_dr_addr = (uint32_t)sink;
    }
    pvr_dr_addr = saved_dr;
    printf("MICRO: done\n");
    return 0;
}
