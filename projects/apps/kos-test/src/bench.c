/* CPU, memory and kernel benchmarks, the results table, the VMU baseline
   and the benchmarks menu. Each result is the fastest of several timed runs;
   auto mode repeats every benchmark step and keeps the median, with the
   spread between passes, so numbers from different KOS builds can be
   compared knowing how much they wobble on their own. */
#include "bench.h"
#include "gfx.h"

#include <dc/fmath.h>
#include <dc/fs_dcload.h>
#include <dc/matrix.h>
#include <dc/pvr.h>
#include <dc/sound/sound.h>
#include <dc/spu.h>
#include <dc/sq.h>
#include <kos/mutex.h>
#include <kos/sem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dc/video.h>

typedef struct {
    const char *key;    /* column name in benchmark_results/benchmarks.csv */
    const char *name;
    const char *unit;
    bool lower;         /* lower is better: times and CPU cost */
} bench_info_t;

static const bench_info_t info[BENCH_COUNT] = {
    [B_CRC32] = { "crc32", "CRC32 (table)", "MB/s" },
    [B_SIEVE] = { "sieve", "Sieve to 1M", "runs/s" },
    [B_QSORT] = { "qsort", "qsort 20k ints", "sorts/s" },
    [B_SPRINTF] = { "snprintf", "snprintf", "k calls/s" },
    [B_FMADD] = { "fmadd", "Float multiply-add", "MFLOPS" },
    [B_FDIV] = { "fdiv", "Float divide chain", "M/s" },
    [B_FSQRT] = { "fsqrt", "Float sqrt chain", "M/s" },
    [B_FTRV] = { "ftrv", "mat_transform (FTRV)", "M verts/s" },
    [B_FIPR] = { "fipr", "FIPR dot product", "M/s" },
    [B_SINF] = { "sinf", "libm sinf", "M calls/s" },
    [B_FSCA] = { "fsca", "FSCA sin/cos", "M/s" },
    [B_DMADD] = { "dmadd", "Double multiply-add", "MFLOPS" },
    [B_MEMCPY] = { "memcpy", "memcpy RAM", "MB/s" },
    [B_MEMSET] = { "memset", "memset RAM", "MB/s" },
    [B_MEMREAD] = { "memread", "Read RAM (sum)", "MB/s" },
    [B_SQCPY] = { "sqcpy", "sq_cpy RAM", "MB/s" },
    [B_P2WRITE] = { "p2write", "Uncached write", "MB/s" },
    [B_VRAM_SQ] = { "vram_sq", "Texture load (SQ)", "MB/s" },
    [B_VRAM_DMA] = { "vram_dma", "Texture load (DMA)", "MB/s" },
    [B_ARAM_WRITE] = { "aram_write", "Sound RAM write", "MB/s" },
    [B_ARAM_DMA] = { "aram_dma", "Sound RAM DMA", "MB/s" },
    [B_THD_SWITCH] = { "thd_switch", "Thread switch", "k/s" },
    [B_MALLOC] = { "malloc", "malloc + free", "k pairs/s" },
    [B_MUTEX] = { "mutex", "Mutex lock/unlock", "M/s" },
    [B_TIMER] = { "timer", "timer_us_gettime64", "M calls/s" },
    [B_GPU_FILL_OP] = { "gpu_fill_op", "GPU opaque overdraw", "Mpix/s" },
    [B_GPU_FILL_TR] = { "gpu_fill_tr", "GPU blended fill", "Mpix/s" },
    [B_GPU_FILL_TEX] = { "gpu_fill_tex", "GPU textured fill", "Mpix/s" },
    [B_GPU_TRI_FLAT] = { "gpu_tri_flat", "GPU flat triangles", "M tris/s" },
    [B_GPU_TRI_TEX] = { "gpu_tri_tex", "GPU textured tris", "M tris/s" },
    [B_GPU_SPRITE] = { "gpu_sprite", "GPU sprites", "M/s" },
    [B_GPU_TA_PRIM] = { "ta_prim", "TA submit pvr_prim", "M verts/s" },
    [B_GPU_TA_DR] = { "ta_dr", "TA submit direct", "M verts/s" },
    [B_GPU_SCENE] = { "gpu_scene", "GPU 3D scene", "fps" },
    [B_VMU_BLOCK] = { "vmu_block", "VMU block read", "ms", true },
    [B_VMU_CLOCK] = { "vmu_clock", "VMU clock read", "ms", true },
    [B_VMU_DIR] = { "vmu_dir", "VMU directory read", "ms", true },
    [B_ROM_READ] = { "rom_read", "Romdisk read", "MB/s" },
    [B_ROM_OPEN] = { "rom_open", "Romdisk open + close", "k/s" },
    [B_ROM_STAT] = { "rom_stat", "Romdisk stat", "k/s" },
    [B_ROM_DIR] = { "rom_dir", "Romdisk directory (64)", "dirs/s" },
    [B_RAM_WRITE] = { "ram_write", "Ramdisk write", "MB/s" },
    [B_RAM_READ] = { "ram_read", "Ramdisk read", "MB/s" },
    [B_GD_READ] = { "gd_read", "GD-ROM sequential read", "KB/s" },
    [B_GD_SEEK] = { "gd_seek", "GD-ROM random read", "ms", true },
    [B_MEMCPY16] = { "memcpy16", "memcpy 16 B", "M calls/s" },
    [B_MEMCPY64] = { "memcpy64", "memcpy 64 B", "M calls/s" },
    [B_MEMCPY256] = { "memcpy256", "memcpy 256 B", "M calls/s" },
    [B_MEMCPY64U] = { "memcpy64u", "memcpy 64 B misaligned", "M calls/s" },
    [B_MEMSET64] = { "memset64", "memset 64 B", "M calls/s" },
    [B_STRLEN64] = { "strlen64", "strlen 63 chars", "M calls/s" },
    [B_STREAM_CPU] = { "stream_cpu", "Sound stream CPU", "%", true },
};

#define SAMPLES 5

static float samples[BENCH_COUNT][SAMPLES];
static int nsamples[BENCH_COUNT];
static float current[BENCH_COUNT];      /* median of the samples */
static int reps = 1;
static unsigned run_serial;
static float base[BENCH_MAX];
static int base_count;
static char base_kos[16], base_build[24];

bool bench_baseline_valid(void) {
    return base_count > 0;
}

void bench_load_baseline(const float *values, int count, const char *kos,
                         const char *build) {
    if(count > BENCH_MAX)
        count = BENCH_MAX;
    memcpy(base, values, (size_t)count * sizeof(float));
    base_count = count;
    snprintf(base_kos, sizeof(base_kos), "%.15s", kos);
    snprintf(base_build, sizeof(base_build), "%.23s", build);
}

static float median(const float *v, int n) {
    float t[SAMPLES];

    memcpy(t, v, (size_t)n * sizeof(float));
    for(int i = 1; i < n; i++)
        for(int j = i; j > 0 && t[j - 1] > t[j]; j--) {
            float x = t[j]; t[j] = t[j - 1]; t[j - 1] = x;
        }
    return n & 1 ? t[n / 2] : (t[n / 2 - 1] + t[n / 2]) / 2;
}

/* (max - min) / median, in percent. */
static float spread(int id) {
    float lo = samples[id][0], hi = lo;

    for(int i = 1; i < nsamples[id]; i++) {
        if(samples[id][i] < lo) lo = samples[id][i];
        if(samples[id][i] > hi) hi = samples[id][i];
    }
    return current[id] > 0 ? (hi - lo) / current[id] * 100 : 0;
}

static void result_line(kt_report_t *r, int id) {
    char noise[24] = "";
    const bench_info_t *b = &info[id];
    float v = current[id];

    if(nsamples[id] > 1)
        snprintf(noise, sizeof(noise), " +/-%.1f%%", (double)spread(id) / 2);
    if(id < base_count && base[id] > 0) {
        float pct = (v - base[id]) / base[id] * 100;
        float better = b->lower ? -pct : pct;
        report_add(r, KT_INFO, b->name, "%.2f %s%s (%+.1f%% vs %.2f)%s", (double)v, b->unit,
                   noise, (double)pct, (double)base[id],
                   better < -5 ? " SLOWER" : better > 5 ? " FASTER" : "");
    }
    else
        report_add(r, KT_INFO, b->name, "%.2f %s%s", (double)v, b->unit, noise);
}

void bench_result(kt_report_t *r, int id, double value) {
    if(nsamples[id] < SAMPLES)
        samples[id][nsamples[id]++] = (float)value;
    current[id] = median(samples[id], nsamples[id]);
    /* With repeats the lines wait for the medians, at the end. */
    if(reps <= 1)
        result_line(r, id);
}

bool bench_first_pass(unsigned *seen) {
    bool first = *seen != run_serial;

    *seen = run_serial;
    return first;
}

void bench_begin_run(int passes) {
    memset(nsamples, 0, sizeof(nsamples));
    run_serial++;
    reps = passes < 1 ? 1 : passes > SAMPLES ? SAMPLES : passes;
}

void bench_finish_run(kt_report_t *r) {
    char events[96];

    if(maple_events_text(events, sizeof(events)))
        report_add(r, KT_INFO, "Maple hot-plug", "%s", events);
    if(reps > 1)
        for(int i = 0; i < BENCH_COUNT; i++)
            if(nsamples[i])
                result_line(r, i);
    if(base_count)
        report_add(r, KT_INFO, "Baseline", "KOS %s, built %s", base_kos, base_build);
    else
        report_add(r, KT_INFO, "Baseline", "none: save one from the benchmarks menu");
    results_dump(r);
}

/* ---- Results for the host ---- */

static bool is_bench_line(const kt_line_t *l) {
    for(int i = 0; i < BENCH_COUNT; i++)
        if(nsamples[i] && !strcmp(l->name, info[i].name))
            return true;
    return false;
}

/* '|' separates the fields. */
static const char *field(const char *s) {
    static char buf[2][80];
    static int which;
    char *o = buf[which ^= 1];

    snprintf(o, sizeof(buf[0]), "%s", s);
    for(char *p = o; *p; p++)
        if(*p == '|')
            *p = '/';
    return o;
}

float measure_cpu_mhz(void);
const char *cable_name(int cable);

/* The whole run as one block of KT lines, sent with a single write: under
   dcload-ip each write is one exchange with dc-tool-ip with interrupts off,
   where a line at a time would be a hundred. Read by
   tools/record_results.py; bump the format when a line changes shape. */
void results_dump(const kt_report_t *r) {
    const size_t cap = 40 * 1024;
    char *buf = malloc(cap);
    size_t n = 0;
    float mhz = measure_cpu_mhz();

    if(!buf)
        return;
#define PUT(...) do { if(n < cap) n += (size_t)snprintf(buf + n, cap - n, __VA_ARGS__); } while(0)
    PUT("KT RESULTS BEGIN\n");
    PUT("KT format 3\n");
    PUT("KT target %s\n", cycles_available() ? "console" : "emulator");
    PUT("KT kos %s\n", KOS_VERSION_STRING);
    PUT("KT kos_commit %s\n", kt_kos_commit);
    PUT("KT kos_patches %s\n", kt_kos_patches);
    PUT("KT kos_test_commit %s\n", kt_test_commit);
    PUT("KT built %s\n", kt_build_date);
    PUT("KT gcc %s\n", __VERSION__);
    PUT("KT loader %s\n", dcload_type == DCLOAD_TYPE_IP ? "dcload-ip" :
        dcload_type == DCLOAD_TYPE_SER ? "dcload-serial" : "none");
    if(mhz > 0)
        PUT("KT cpu_mhz %.2f\n", (double)mhz);
    else
        PUT("KT cpu_mhz n/a\n");
    PUT("KT video %s %dx%d\n", cable_name(vid_check_cable()), vid_mode->width, vid_mode->height);
    PUT("KT run %s\n", field(r->title));
    PUT("KT passes %d\n", reps);
    char events[96];
    maple_events_text(events, sizeof(events));
    PUT("KT maple_events %s\n", events);
    PUT("KT summary pass=%d fail=%d skip=%d ms=%lu\n", r->pass, r->fail, r->skip,
        (unsigned long)r->ms);
    for(int i = 0; i < r->count; i++) {
        const kt_line_t *l = &r->line[i];
        if(!is_bench_line(l))
            PUT("KT TEST %s|%s|%s\n", status_name(l->status), field(l->name), field(l->detail));
    }
    /* key|median|unit|name|spread %|samples|hi or lo (which way is better)
       |each pass's value */
    for(int i = 0; i < BENCH_COUNT; i++)
        if(nsamples[i]) {
            PUT("KT BENCH %s|%.4f|%s|%s|%.2f|%d|%s|", info[i].key, (double)current[i],
                info[i].unit, info[i].name, (double)spread(i), nsamples[i],
                info[i].lower ? "lo" : "hi");
            for(int k = 0; k < nsamples[i]; k++)
                PUT("%s%.4g", k ? "," : "", (double)samples[i][k]);
            PUT("\n");
        }
    PUT("KT RESULTS END\n");
#undef PUT
    if(n > cap)
        n = cap;
    fflush(stdout);
    write(STDOUT_FILENO, buf, n);
    free(buf);
}

/* The record holds the first BENCH_MAX benchmarks; later ones are in the
   results only. */
int bench_save_baseline(void) {
    kt_save_t rec;
    int n = 0, keep = BENCH_COUNT < BENCH_MAX ? BENCH_COUNT : BENCH_MAX;

    for(int i = 0; i < BENCH_COUNT; i++)
        if(nsamples[i]) n++;
    if(!n)
        return -2;
    if(vmu_save_read(&rec) != 0)
        memset(&rec, 0, sizeof(rec));
    for(int i = 0; i < keep; i++)
        if(nsamples[i])
            rec.bench[i] = current[i];
        else if(i >= (int)rec.bench_count)
            rec.bench[i] = 0;
    rec.bench_count = (uint32_t)keep;
    rec.bench_runs++;
    snprintf(rec.kos, sizeof(rec.kos), "%s", KOS_VERSION_STRING);
    snprintf(rec.build, sizeof(rec.build), "%s", kt_build_date);
    vmu_save_seal(&rec);
    if(vmu_save_write(&rec) < 0)
        return -1;
    bench_load_baseline(rec.bench, (int)rec.bench_count, rec.kos, rec.build);
    return 0;
}

/* ---- Timing ---- */

/* Units per second from the fastest of runs calls to fn. */
static double best_rate(void (*fn)(void *), void *arg, double units, int runs) {
    uint64_t best = UINT64_MAX;

    fn(arg);    /* warm the caches */
    for(int i = 0; i < runs; i++) {
        uint64_t t = now_us();
        fn(arg);
        t = now_us() - t;
        if(t < best)
            best = t;
    }
    return best ? units * 1e6 / (double)best : 0;
}

static volatile uint32_t sink_u;
static volatile float sink_f;
static volatile double sink_d;

/* ---- Integer ---- */

/* 1 MB each, from one block so every build sees the same layout: big_a on
   a 16 KB boundary and big_b two cache lines off one, so copies between
   them never fight over the direct-mapped operand cache's sets. */
static uint8_t *big_block, *big_a, *big_b;

static void need_buffers(void) {
    if(!big_block) {
        big_block = memalign(16384, 2 * 1024 * 1024 + 16384);
        big_a = big_block;
        big_b = big_block + 1024 * 1024 + 16384 - 64;
        uint32_t seed = 1;
        for(int i = 0; i < 1024 * 1024 / 4; i++)
            ((uint32_t *)big_a)[i] = rng_next(&seed);
    }
}

static void free_buffers(void) {
    free(big_block);
    big_block = big_a = big_b = NULL;
}

static void w_crc(void *a) { sink_u = crc32_buf(0, big_a, 256 * 1024); }

static void w_sieve(void *a) {
    uint8_t *s = big_b;
    const int n = 1000000;
    memset(s, 1, n);
    for(int i = 2; i * i < n; i++)
        if(s[i])
            for(int j = i * i; j < n; j += i)
                s[j] = 0;
    int c = 0;
    for(int i = 2; i < n; i++)
        c += s[i];
    sink_u = (uint32_t)c;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void w_qsort(void *a) {
    uint32_t *v = (uint32_t *)big_b;
    memcpy(v, big_a, 20000 * 4);
    qsort(v, 20000, 4, cmp_u32);
    sink_u = v[0];
}

static void w_sprintf(void *a) {
    char buf[64];
    for(int i = 0; i < 1000; i++)
        snprintf(buf, sizeof(buf), "%d %s %08x %.3f", i, "kos", (unsigned)i * 2654435761u,
                 (double)i * 0.37);
    sink_u = (uint32_t)buf[0];
}

static void step_int(kt_report_t *r) {
    need_buffers();
    bench_result(r, B_CRC32, best_rate(w_crc, NULL, 0.25, 5));
    bench_result(r, B_SIEVE, best_rate(w_sieve, NULL, 1, 5));
    bench_result(r, B_QSORT, best_rate(w_qsort, NULL, 1, 5));
    bench_result(r, B_SPRINTF, best_rate(w_sprintf, NULL, 1, 5));
}

/* ---- Floating point ---- */

#define FLOOP 200000

static void w_fmadd(void *a) {
    float x0 = 1, x1 = 1.1f, x2 = 1.2f, x3 = 1.3f, x4 = 1.4f, x5 = 1.5f, x6 = 1.6f, x7 = 1.7f;
    const float m = 0.999999f, c = 0.000001f;
    for(int i = 0; i < FLOOP; i++) {
        x0 = x0 * m + c; x1 = x1 * m + c; x2 = x2 * m + c; x3 = x3 * m + c;
        x4 = x4 * m + c; x5 = x5 * m + c; x6 = x6 * m + c; x7 = x7 * m + c;
    }
    sink_f = x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7;
}

static void w_fdiv(void *a) {
    float x = 12345.0f;
    const float d = 1.0001f;
    for(int i = 0; i < FLOOP; i++)
        x = x / d;
    sink_f = x;
}

static void w_fsqrt(void *a) {
    float x = 2.0f;
    for(int i = 0; i < FLOOP; i++)
        x = sqrtf(x + 1.0f);
    sink_f = x;
}

static vector_t *vin, *vout;

static void w_ftrv(void *a) {
    for(int k = 0; k < 16; k++)
        mat_transform(vin, vout, 1024, sizeof(vector_t));
    sink_f = vout[7].x;
}

static float fipr4(const float *a, const float *b) {
    register float a0 __asm__("fr0") = a[0];
    register float a1 __asm__("fr1") = a[1];
    register float a2 __asm__("fr2") = a[2];
    register float a3 __asm__("fr3") = a[3];
    register float b0 __asm__("fr4") = b[0];
    register float b1 __asm__("fr5") = b[1];
    register float b2 __asm__("fr6") = b[2];
    register float b3 __asm__("fr7") = b[3];
    __asm__("fipr fv4,fv0" : "+f"(a3) : "f"(a0), "f"(a1), "f"(a2), "f"(b0), "f"(b1), "f"(b2), "f"(b3));
    return a3;
}

static void w_fipr(void *a) {
    const float *v = (const float *)vin;
    float s = 0;
    for(int k = 0; k < 16; k++)
        for(int i = 0; i < 1023; i++)
            s += fipr4(v + i * 4, v + i * 4 + 4);
    sink_f = s;
}

static void w_sinf(void *a) {
    float s = 0;
    for(int i = 0; i < 20000; i++)
        s += sinf((float)i * 0.001f);
    sink_f = s;
}

static void w_fsca(void *a) {
    float s = 0;
    for(int i = 0; i < 20000; i++)
        s += fsin((float)i * 0.001f);
    sink_f = s;
}

static void w_dmadd(void *a) {
    double x0 = 1, x1 = 1.1, x2 = 1.2, x3 = 1.3;
    const double m = 0.999999, c = 0.000001;
    for(int i = 0; i < FLOOP / 2; i++) {
        x0 = x0 * m + c; x1 = x1 * m + c; x2 = x2 * m + c; x3 = x3 * m + c;
    }
    sink_d = x0 + x1 + x2 + x3;
}

static void step_float(kt_report_t *r) {
    static matrix_t m __attribute__((aligned(32))) = {
        { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 5, 6, 7, 1 }
    };

    vin = memalign(32, 1024 * sizeof(vector_t));
    vout = memalign(32, 1024 * sizeof(vector_t) + 64);
    vout = (vector_t *)((uint8_t *)vout + 64);     /* off the source's cache lines */
    for(int i = 0; i < 1024; i++)
        vin[i] = (vector_t){ (float)i, (float)(i & 63), 1.0f + (float)(i % 7), 1.0f };
    bench_result(r, B_FMADD, best_rate(w_fmadd, NULL, FLOOP * 16 / 1e6, 5));
    bench_result(r, B_FDIV, best_rate(w_fdiv, NULL, FLOOP / 1e6, 5));
    bench_result(r, B_FSQRT, best_rate(w_fsqrt, NULL, FLOOP / 1e6, 5));
    mat_load(&m);
    bench_result(r, B_FTRV, best_rate(w_ftrv, NULL, 16 * 1024 / 1e6, 5));
    bench_result(r, B_FIPR, best_rate(w_fipr, NULL, 16 * 1023 / 1e6, 5));
    bench_result(r, B_SINF, best_rate(w_sinf, NULL, 20000 / 1e6, 5));
    bench_result(r, B_FSCA, best_rate(w_fsca, NULL, 20000 / 1e6, 5));
    bench_result(r, B_DMADD, best_rate(w_dmadd, NULL, FLOOP / 2 * 8 / 1e6, 5));
    free(vin);
    free((uint8_t *)vout - 64);
}

/* ---- Memory ---- */

static void w_memcpy(void *a) { memcpy(big_b, big_a, 1024 * 1024); }
static void w_memset(void *a) { memset(big_b, (int)sink_u & 0xff, 1024 * 1024); }

static void w_read(void *a) {
    const uint32_t *p = (const uint32_t *)big_a;
    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    for(int k = 0; k < 4; k++)
        for(int i = 0; i < 262144; i += 4) {
            s0 += p[i]; s1 += p[i + 1]; s2 += p[i + 2]; s3 += p[i + 3];
        }
    sink_u = s0 + s1 + s2 + s3;
}

static void w_sqcpy(void *a) { sq_cpy(big_b, big_a, 1024 * 1024); sq_wait(); }

static void w_p2write(void *a) {
    volatile uint32_t *p = (volatile uint32_t *)(((uintptr_t)big_b & 0x1fffffff) | 0xa0000000);
    for(int i = 0; i < 65536; i += 4) {
        p[i] = i; p[i + 1] = i; p[i + 2] = i; p[i + 3] = i;
    }
}

static pvr_ptr_t vram;
static uint32_t aram;

static void w_vram_sq(void *a) { pvr_txr_load(big_a, vram, 1024 * 1024); }
static void w_vram_dma(void *a) { pvr_txr_load_dma(big_a, vram, 1024 * 1024, true, NULL, NULL); }
static void w_aram(void *a) { spu_memload(aram, big_a, 256 * 1024); }
static void w_aram_dma(void *a) { spu_dma_transfer(big_a, aram, 256 * 1024, 1, NULL, NULL); }

static void step_memory(kt_report_t *r) {
    need_buffers();
    bench_result(r, B_MEMCPY, best_rate(w_memcpy, NULL, 1, 5));
    bench_result(r, B_MEMSET, best_rate(w_memset, NULL, 1, 5));
    bench_result(r, B_MEMREAD, best_rate(w_read, NULL, 4, 5));
    bench_result(r, B_SQCPY, best_rate(w_sqcpy, NULL, 1, 5));
    dcache_purge_range((uintptr_t)big_b, 1024 * 1024);
    bench_result(r, B_P2WRITE, best_rate(w_p2write, NULL, 0.25, 5));
    dcache_wback_range((uintptr_t)big_a, 1024 * 1024);

    pvr_wait_render_done();
    vram = pvr_mem_malloc(1024 * 1024);
    if(vram) {
        bench_result(r, B_VRAM_SQ, best_rate(w_vram_sq, NULL, 1, 4));
        bench_result(r, B_VRAM_DMA, best_rate(w_vram_dma, NULL, 1, 4));
        pvr_mem_free(vram);
    }
    else
        report_add(r, KT_SKIP, "Texture load", "no 1 MB of VRAM free");
    snd_init();
    aram = snd_mem_malloc(256 * 1024);
    if(aram) {
        bench_result(r, B_ARAM_WRITE, best_rate(w_aram, NULL, 0.25, 4));
        bench_result(r, B_ARAM_DMA, best_rate(w_aram_dma, NULL, 0.25, 4));
        snd_mem_free(aram);
    }
}

/* ---- Kernel ---- */

static semaphore_t s_ping, s_pong;

static void *ponger(void *a) {
    for(int i = 0; i < 2000; i++) {
        sem_wait(&s_ping);
        sem_signal(&s_pong);
    }
    return NULL;
}

static void w_switch(void *a) {
    sem_init(&s_ping, 0);
    sem_init(&s_pong, 0);
    kthread_t *t = thd_create(false, ponger, NULL);
    for(int i = 0; i < 2000; i++) {
        sem_signal(&s_ping);
        sem_wait(&s_pong);
    }
    thd_join(t, NULL);
    sem_destroy(&s_ping);
    sem_destroy(&s_pong);
}

static void w_malloc(void *a) {
    static void *slot[64];
    uint32_t seed = 42;
    for(int i = 0; i < 10000; i++) {
        int k = (int)(rng_next(&seed) & 63);
        free(slot[k]);
        slot[k] = malloc(16 + (rng_next(&seed) & 2047));
    }
    for(int k = 0; k < 64; k++) {
        free(slot[k]);
        slot[k] = NULL;
    }
}

static mutex_t bench_lock = MUTEX_INITIALIZER;

static void w_mutex(void *a) {
    for(int i = 0; i < 50000; i++) {
        mutex_lock(&bench_lock);
        mutex_unlock(&bench_lock);
    }
}

static void w_timer(void *a) {
    uint64_t s = 0;
    for(int i = 0; i < 50000; i++)
        s += timer_us_gettime64();
    sink_u = (uint32_t)s;
}

static void step_kernel(kt_report_t *r) {
    bench_result(r, B_THD_SWITCH, best_rate(w_switch, NULL, 4.0, 3));
    bench_result(r, B_MALLOC, best_rate(w_malloc, NULL, 10.0, 5));
    bench_result(r, B_MUTEX, best_rate(w_mutex, NULL, 0.05, 5));
    bench_result(r, B_TIMER, best_rate(w_timer, NULL, 0.05, 5));
    free_buffers();
}

const kt_step_t bench_cpu_steps[] = {
    { "Integer benchmarks", step_int },
    { "Floating point benchmarks", step_float },
    { "Memory benchmarks", step_memory },
    { "Kernel benchmarks", step_kernel },
};
const int bench_cpu_step_count = sizeof(bench_cpu_steps) / sizeof(bench_cpu_steps[0]);

/* ---- Running ---- */

static kt_report_t bench_report, all_report;

/* Every benchmark list passes times over, after the given test lists. */
static void start_runs(const char *title, const kt_step_t *const *tests, const int *tcounts,
                       int ntests, int which, int passes, kt_report_t *r) {
    const kt_step_t *lists[16];
    int counts[16], n = 0;

    for(int i = 0; i < ntests; i++) {
        lists[n] = tests[i];
        counts[n++] = tcounts[i];
    }
    for(int p = 0; p < passes; p++) {
        if(which != 2) {
            lists[n] = bench_cpu_steps; counts[n++] = bench_cpu_step_count;
            lists[n] = bench_io_steps; counts[n++] = bench_io_step_count;
        }
        if(which != 1) {
            lists[n] = bench_gpu_steps; counts[n++] = bench_gpu_step_count;
        }
    }
    bench_begin_run(passes);
    runner_start_multi(title, lists, counts, n, r, bench_finish_run);
}

static void run_tests_and_benches(const char *title, int passes, bool vmu) {
    const kt_step_t *tests[] = { vmu_auto_steps, selftest_steps, cpu_steps };
    const int counts[] = { vmu_auto_step_count, selftest_step_count, cpu_step_count };

    start_runs(title, tests + !vmu, counts + !vmu, 3 - !vmu, 0, passes, &all_report);
}

void run_everything(void) {
    run_tests_and_benches("RUN ALL TESTS", 1, false);
}

/* Built with AUTO_VMU=1, auto mode also writes and checks a test save on
   the first VMU. */
void run_auto(void) {
#ifdef KT_AUTO_VMU
    run_tests_and_benches("AUTO RUN", 3, true);
#else
    run_tests_and_benches("AUTO RUN", 3, false);
#endif
}

/* 0 everything, 1 CPU, memory and I/O, 2 GPU */
static void run_benches(int which) {
    static const char *const titles[3] = { "BENCHMARKS", "CPU & I/O BENCHMARKS", "GPU BENCHMARKS" };

    start_runs(titles[which], NULL, NULL, 0, which, 1, &bench_report);
}

/* ---- Menu ---- */

static const char *const items[] = {
    "RUN ALL BENCHMARKS",
    "CPU, MEMORY & I/O ONLY",
    "GPU ONLY",
    "SHOW LAST RESULTS",
    "SAVE RESULTS AS VMU BASELINE",
};
#define ITEMS ((int)(sizeof(items) / sizeof(items[0])))

static int sel;
static char message[80];
static uint32_t message_color;

static void bench_enter(void) {
    message[0] = 0;
}

static void bench_update(const kt_input_t *in) {
    if(in->repeat & CONT_DPAD_DOWN) sel = (sel + 1) % ITEMS;
    if(in->repeat & CONT_DPAD_UP) sel = (sel + ITEMS - 1) % ITEMS;
    if(in->pressed & (CONT_B | CONT_START)) {
        kt_pop();
        return;
    }
    if(!(in->pressed & CONT_A))
        return;
    message[0] = 0;
    if(sel <= 2)
        run_benches(sel);
    else if(sel == 3) {
        if(bench_report.count)
            report_show(&bench_report);
        else {
            snprintf(message, sizeof(message), "Nothing run yet.");
            message_color = C_WARN;
        }
    }
    else {
        int rv = vmu_selected_port() < 0 ? -3 : bench_save_baseline();
        message_color = rv ? C_FAIL : C_PASS;
        snprintf(message, sizeof(message), "%s",
                 rv == 0 ? "Baseline saved to the VMU." :
                 rv == -2 ? "Run the benchmarks first." :
                 rv == -3 ? "No VMU selected: run the VMU test from the main menu." :
                 "VMU write failed.");
    }
}

static void bench_draw(void) {
    ui_background();
    ui_header("BENCHMARKS", KOS_VERSION_STRING);
    ui_menu(110, 56, 420, items, ITEMS, sel, 0, ITEMS);
    float y = 56 + ITEMS * 28 + 20;
    if(base_count)
        gfx_textf(110, y, C_DIM, "Baseline on VMU: KOS %s, built %s", base_kos, base_build);
    else
        gfx_text(110, y, C_DIM, "No baseline loaded. Save one here, or run the VMU test.");
    gfx_text(110, y + 22, C_DIM, "Results go to the serial console as KT BENCH lines.");
    gfx_text(110, y + 40, C_DIM, "Flycast timings are not console timings.");
    if(message[0])
        gfx_text(110, y + 70, message_color, message);
    ui_footer("UP/DOWN SELECT   A RUN   B BACK");
}

const kt_screen_t bench_screen = {
    "Benchmarks", bench_enter, bench_update, bench_draw, NULL
};
