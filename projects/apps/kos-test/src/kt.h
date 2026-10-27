/* KOS_Test: shared types for the screens, the test report and the step
   runner. Every screen is a kt_screen_t; main.c keeps a stack of them. */
#ifndef KT_H
#define KT_H

#include <kos.h>
#include <stdbool.h>
#include <stdint.h>

#define KT_TITLE "KOS_Test"
#define KT_VERSION "1.0"

/* Colours, ARGB8888 */
#define C_BG     0xff071426
#define C_PANEL  0xff0e2442
#define C_PANEL2 0xff163564
#define C_LINE   0xff2c5a96
#define C_TEXT   0xffe6eef8
#define C_DIM    0xff8296b4
#define C_HI     0xffffd23c
#define C_SEL    0xff23549a
#define C_PASS   0xff3ee070
#define C_FAIL   0xffff4a4a
#define C_SKIP   0xff9aa0aa
#define C_INFO   0xff5cc8ff
#define C_WARN   0xffffa030

/* Menu input from every controller and keyboard, as CONT_* bits. */
typedef struct {
    uint32_t held;
    uint32_t pressed;   /* went down this frame */
    uint32_t repeat;    /* pressed, plus auto-repeat while held (D-pad) */
    int ltrig, rtrig;
} kt_input_t;

void input_poll(kt_input_t *in);
/* Wait until nothing is held, so a screen change does not act on the press
   that caused it. */
void input_flush(void);

typedef struct kt_screen {
    const char *title;
    void (*enter)(void);
    void (*update)(const kt_input_t *in);
    void (*draw)(void);
    void (*leave)(void);
} kt_screen_t;

void kt_push(const kt_screen_t *s);
void kt_pop(void);
/* Replace the current screen (runner -> report). */
void kt_replace(const kt_screen_t *s);
const kt_screen_t *kt_current(void);
uint32_t kt_frame(void);
void kt_quit(void);

/* ---- Test reports ---- */
typedef enum { KT_PASS, KT_FAIL, KT_SKIP, KT_INFO } kt_status_t;

typedef struct {
    char name[30];
    char detail[64];
    uint8_t status;
} kt_line_t;

#define KT_REPORT_MAX 200

typedef struct {
    char title[40];
    kt_line_t line[KT_REPORT_MAX];
    int count, pass, fail, skip;
    uint64_t start_ms, ms;
} kt_report_t;

void report_begin(kt_report_t *r, const char *title);
void report_add(kt_report_t *r, kt_status_t st, const char *name,
                const char *fmt, ...) __attribute__((format(printf, 4, 5)));
/* PASS when ok, FAIL otherwise. */
void report_check(kt_report_t *r, bool ok, const char *name,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void report_end(kt_report_t *r);
/* Push a scrollable screen showing r. */
void report_show(kt_report_t *r);
const char *status_name(kt_status_t st);
uint32_t status_color(kt_status_t st);

/* ---- Step runner ----
   Runs one step per frame between drawn frames, so the screen shows what is
   running. A step may draw its own frames (the GPU benchmarks do). */
typedef void (*kt_step_fn)(kt_report_t *r);
typedef struct {
    const char *name;
    kt_step_fn fn;
} kt_step_t;

/* done (may be NULL) runs after the last step and the summary, before the
   report shows. */
void runner_start(const char *title, const kt_step_t *steps, int count,
                  kt_report_t *r, void (*done)(kt_report_t *r));
/* Several step lists run back to back into one report. */
void runner_start_multi(const char *title, const kt_step_t *const *lists,
                        const int *counts, int nlists, kt_report_t *r,
                        void (*done)(kt_report_t *r));

/* ---- Screens and suites (one module each) ---- */
extern const kt_screen_t menu_screen;
extern const kt_screen_t sysinfo_screen;
extern const kt_screen_t input_screen;
extern const kt_screen_t selftest_screen;
extern const kt_screen_t bench_screen;
extern const kt_screen_t gpu_screen;
extern const kt_screen_t monitor_screen;
extern const kt_screen_t sound_screen;
extern const kt_screen_t net_screen;
extern const kt_screen_t vmu_screen;

/* Step lists for the run-everything item. */
extern const kt_step_t selftest_steps[];
extern const int selftest_step_count;
extern const kt_step_t cpu_steps[];
extern const int cpu_step_count;
extern const kt_step_t bench_cpu_steps[];
extern const int bench_cpu_step_count;
extern const kt_step_t bench_gpu_steps[];
extern const int bench_gpu_step_count;
extern const kt_step_t bench_io_steps[];
extern const int bench_io_step_count;
extern const kt_step_t vmu_auto_steps[];
extern const int vmu_auto_step_count;

void cpu_suite_run(void);
void run_everything(void);
/* Run everything with the benchmarks three times over (auto mode). */
void run_auto(void);

/* VMU test: the boot prompt, and the card picked there (or -1). */
void vmu_prompt_start(void);
int vmu_selected_port(void);
int vmu_selected_unit(void);

/* Benchmark results (bench.h has the ids). */
#define BENCH_MAX 48     /* kept in the VMU baseline */
/* Start a run that repeats each benchmark passes times (median kept). */
void bench_begin_run(int passes);
void bench_finish_run(kt_report_t *r);
/* Send r and this run's benchmarks to the serial console or dc-tool-ip as
   one KT RESULTS block. */
void results_dump(const kt_report_t *r);
/* The VMU baseline: load from, save to the selected card. */
bool bench_baseline_valid(void);
int bench_save_baseline(void);
void bench_load_baseline(const float *values, int count, const char *kos,
                         const char *build);

/* VMU save record, shared by the VMU test and benchmark baselines. */
#define KT_SAVE_NAME "KOSTEST"
#define KT_SAVE_MAGIC 0x5453544b /* "KTST" */
#define KT_SAVE_VERSION 1
#define KT_SAVE_PAYLOAD 384      /* + 640 header/icon = 2 blocks */

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t runs;               /* VMU tests run on this card */
    uint32_t bench_runs;
    char kos[16];                /* KOS version of the baseline */
    char build[24];              /* build date of the baseline */
    uint32_t bench_count;
    float bench[BENCH_MAX];
    uint8_t fill[KT_SAVE_PAYLOAD - 60 - BENCH_MAX * 4 - 4]; /* test pattern */
    uint32_t crc;
} kt_save_t;

_Static_assert(sizeof(kt_save_t) == KT_SAVE_PAYLOAD, "save record size");

int vmu_save_write(const kt_save_t *rec);
int vmu_save_read(kt_save_t *rec);
void vmu_save_seal(kt_save_t *rec);
bool vmu_save_valid(const kt_save_t *rec);

/* VMU LCD banner on every VMU screen. */
void vmu_lcd_banner(const char *line1, const char *line2);

/* ---- Small helpers (util.c) ---- */
uint32_t crc32_buf(uint32_t crc, const void *data, size_t len);
uint32_t rng_next(uint32_t *state);
void fmt_bytes(char *out, size_t n, uint64_t bytes);
uint64_t now_us(void);
size_t mem_unallocated(void);
void maple_events_start(void);
int maple_events_text(char *out, size_t n);
size_t mem_above_break(void);
/* SH-4 performance counter 1 cycles (0 under Flycast, which lacks them). */
void cycles_start(void);
uint64_t cycles_read(void);
bool cycles_available(void);
extern const char *const kt_build_date;
/* Short commit of the KOS tree built against, a hash of its local changes
   ("none" when clean), and this repository's commit (with -dirty). */
extern const char *const kt_kos_commit;
extern const char *const kt_kos_patches;
extern const char *const kt_test_commit;
/* Serial log, kept short: dcload-ip stalls the console on every printf. */
#define KT_LOG(...) printf("KT " __VA_ARGS__)

#ifdef KT_AUTOTEST
/* Scripted runs: menu screens advance on their own. */
bool autotest_active(void);
void autotest_next(void);
#endif

#endif
