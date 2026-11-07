/* Engine: test build helpers (qa.h). */
#include "qa.h"
#include <kos.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void qa_shot(const char *fmt, ...) {
    char name[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(name, sizeof(name), fmt, ap);
    va_end(ap);
    printf(ENG_QA_TAG " SHOT %s\n", name);
}

float qa_window_frame(QaWindow *w, float dt_real, int steps, float cpu_ms) {
    w->t += dt_real;
    w->frames++;
    if(dt_real > 1.5f / 60.0f) w->missed++;
    if(steps >= 2) w->multi++;
    else if(steps == 0) w->zero++;
    w->cpu_sum += cpu_ms;
    if(cpu_ms > w->cpu_max) w->cpu_max = cpu_ms;
    float rnd = 0;
    pvr_stats_t ps;
    if(pvr_get_stats(&ps) == 0 && ps.rnd_last_time < 100000000ull) {   /* nothing rendered yet reads as garbage */
        rnd = (float)ps.rnd_last_time * 1e-6f;
        if(ps.vtx_buffer_used_max > w->vtx_max) w->vtx_max = ps.vtx_buffer_used_max;
    }
    w->rnd_sum += rnd;
    if(rnd > w->rnd_max) w->rnd_max = rnd;
    return rnd;
}

void qa_window_reset(QaWindow *w) {
    memset(w, 0, sizeof(*w));
}
