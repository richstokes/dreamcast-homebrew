/* Test reports, the screen that shows them and the step runner. */
#include "gfx.h"
#include "kt.h"

#include <dc/fs_dcload.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Under dcload-ip every printf is a network round trip with interrupts off,
   so console runs log only failures and one summary line per report. */
static bool log_line(kt_status_t st) {
    return dcload_type != DCLOAD_TYPE_IP || st == KT_FAIL;
}

const char *status_name(kt_status_t st) {
    switch(st) {
        case KT_PASS: return "PASS";
        case KT_FAIL: return "FAIL";
        case KT_SKIP: return "SKIP";
        default: return "INFO";
    }
}

uint32_t status_color(kt_status_t st) {
    switch(st) {
        case KT_PASS: return C_PASS;
        case KT_FAIL: return C_FAIL;
        case KT_SKIP: return C_SKIP;
        default: return C_INFO;
    }
}

void report_begin(kt_report_t *r, const char *title) {
    memset(r, 0, sizeof(*r));
    snprintf(r->title, sizeof(r->title), "%s", title);
    r->start_ms = timer_ms_gettime64();
}

static void report_vadd(kt_report_t *r, kt_status_t st, const char *name,
                        const char *fmt, va_list ap) {
    if(st == KT_PASS) r->pass++;
    else if(st == KT_FAIL) r->fail++;
    else if(st == KT_SKIP) r->skip++;
    if(r->count >= KT_REPORT_MAX)
        return;
    kt_line_t *l = &r->line[r->count++];
    l->status = (uint8_t)st;
    snprintf(l->name, sizeof(l->name), "%s", name);
    vsnprintf(l->detail, sizeof(l->detail), fmt, ap);
    if(log_line(st))
        KT_LOG("[%s] %s: %s\n", status_name(st), l->name, l->detail);
}

void report_add(kt_report_t *r, kt_status_t st, const char *name,
                const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    report_vadd(r, st, name, fmt, ap);
    va_end(ap);
}

void report_check(kt_report_t *r, bool ok, const char *name,
                  const char *fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    report_vadd(r, ok ? KT_PASS : KT_FAIL, name, fmt, ap);
    va_end(ap);
}

void report_end(kt_report_t *r) {
    r->ms = timer_ms_gettime64() - r->start_ms;
    KT_LOG("REPORT %s: pass=%d fail=%d skip=%d ms=%lu\n", r->title, r->pass,
           r->fail, r->skip, (unsigned long)r->ms);
}

/* ---- Report screen ---- */

#define ROW_H 18
#define ROWS ((UI_BOTTOM - 76) / ROW_H)

static kt_report_t *shown;
static int scroll;

static void report_enter(void) {
    scroll = 0;
}

static void report_update(const kt_input_t *in) {
    int max = shown->count - ROWS;

    if(in->repeat & CONT_DPAD_DOWN) scroll++;
    if(in->repeat & CONT_DPAD_UP) scroll--;
    if(in->repeat & CONT_DPAD_RIGHT) scroll += ROWS;
    if(in->repeat & CONT_DPAD_LEFT) scroll -= ROWS;
    if(scroll > max) scroll = max;
    if(scroll < 0) scroll = 0;
    if(in->pressed & (CONT_B | CONT_START | CONT_A))
        kt_pop();
}

static void report_draw(void) {
    char right[48];

    ui_background();
    snprintf(right, sizeof(right), "%lu.%02lu s", (unsigned long)(shown->ms / 1000),
             (unsigned long)(shown->ms % 1000 / 10));
    ui_header(shown->title, right);

    float x = gfx_textf(12, 46, C_PASS, "PASS %d", shown->pass);
    x = gfx_textf(x + 24, 46, shown->fail ? C_FAIL : C_DIM, "FAIL %d", shown->fail);
    x = gfx_textf(x + 24, 46, C_SKIP, "SKIP %d", shown->skip);
    if(!shown->fail && shown->pass)
        gfx_text(x + 40, 46, C_PASS, "ALL CHECKS PASSED");
    else if(shown->fail)
        gfx_text(x + 40, 46, C_FAIL, "FAILURES FOUND");
    gfx_rect(8, 66, SCREEN_W - 16, 1, C_LINE);

    for(int i = 0; i < ROWS && scroll + i < shown->count; i++) {
        const kt_line_t *l = &shown->line[scroll + i];
        float y = 72 + i * ROW_H;

        if((scroll + i) & 1)
            gfx_rect(8, y - 1, SCREEN_W - 16, ROW_H, 0x14ffffff);
        gfx_text(12, y, status_color(l->status), status_name(l->status));
        gfx_text_clip(56, y, C_TEXT, l->name, 29);
        gfx_text_clip(296, y, l->status == KT_FAIL ? C_FAIL : C_DIM, l->detail, 42);
    }
    if(shown->count > ROWS)
        gfx_textf(SCREEN_W - 120, 46, C_DIM, "%d-%d / %d", scroll + 1,
                  scroll + ROWS < shown->count ? scroll + ROWS : shown->count,
                  shown->count);
    ui_footer("UP/DOWN SCROLL   LEFT/RIGHT PAGE   A/B BACK");
}

static const kt_screen_t report_screen = {
    "Report", report_enter, report_update, report_draw, NULL
};

void report_show(kt_report_t *r) {
    shown = r;
    kt_push(&report_screen);
}

/* ---- Runner ---- */

#define RUN_MAX 160

static const kt_step_t *run_steps[RUN_MAX];
static int run_count, run_next, run_frames;
static kt_report_t *run_report;
static void (*run_done)(kt_report_t *r);
static char run_title[40];
static uint64_t run_started;

static void runner_update(const kt_input_t *in) {
    (void)in;
    /* Draw at least one frame naming the step before running it. */
    if(run_frames++ < 1)
        return;
    if(run_next < run_count) {
        const kt_step_t *s = run_steps[run_next];
        s->fn(run_report);
        run_next++;
        return;
    }
    report_end(run_report);
    if(run_done)
        run_done(run_report);
    shown = run_report;
    kt_replace(&report_screen);
}

static void runner_draw(void) {
    char right[32];
    uint64_t ms = timer_ms_gettime64() - run_started;

    ui_background();
    snprintf(right, sizeof(right), "%lu s", (unsigned long)(ms / 1000));
    ui_header(run_title, right);

    float pct = run_count ? (float)run_next / run_count : 1.0f;
    gfx_text(20, 52, C_DIM, "RUNNING");
    gfx_text_big(20, 70, C_HI, run_next < run_count ? run_steps[run_next]->name : "FINISHING");
    gfx_rect(20, 104, 600, 14, 0xff0e2442);
    gfx_rect(20, 104, 600 * pct, 14, C_PASS);
    gfx_box(20, 104, 600, 14, 1, C_LINE);
    gfx_textf(20, 124, C_DIM, "STEP %d OF %d   PASS %d  FAIL %d  SKIP %d",
              run_next + 1 > run_count ? run_count : run_next + 1, run_count,
              run_report->pass, run_report->fail, run_report->skip);

    int first = run_report->count > 16 ? run_report->count - 16 : 0;
    for(int i = first; i < run_report->count; i++) {
        const kt_line_t *l = &run_report->line[i];
        float y = 152 + (i - first) * 18;
        gfx_text(20, y, status_color(l->status), status_name(l->status));
        gfx_text_clip(64, y, C_TEXT, l->name, 28);
        gfx_text_clip(300, y, C_DIM, l->detail, 40);
    }
    ui_footer("PLEASE WAIT");
}

static void runner_enter(void) {
    run_frames = 0;
    run_started = timer_ms_gettime64();
}

static const kt_screen_t runner_screen = {
    "Running", runner_enter, runner_update, runner_draw, NULL
};

void runner_start_multi(const char *title, const kt_step_t *const *lists,
                        const int *counts, int nlists, kt_report_t *r,
                        void (*done)(kt_report_t *r)) {
    run_count = 0;
    for(int l = 0; l < nlists; l++)
        for(int i = 0; i < counts[l] && run_count < RUN_MAX; i++)
            run_steps[run_count++] = &lists[l][i];
    run_next = 0;
    run_report = r;
    run_done = done;
    snprintf(run_title, sizeof(run_title), "%s", title);
    report_begin(r, title);
    KT_LOG("RUN %s: %d steps\n", title, run_count);
    kt_push(&runner_screen);
}

void runner_start(const char *title, const kt_step_t *steps, int count,
                  kt_report_t *r, void (*done)(kt_report_t *r)) {
    const kt_step_t *lists[1] = { steps };
    int counts[1] = { count };

    runner_start_multi(title, lists, counts, 1, r, done);
}
