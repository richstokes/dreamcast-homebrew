/* The benchmark builds' section counters (prof.h), banked per counter-1
   event each frame and reported once a log window. */
#include "dk.h"
#include "prof.h"

#ifdef DK_PROF
uint32_t prof_cyc[PS_COUNT], prof_ev[PS_COUNT], prof_n[PN_COUNT];
static uint64_t prof_n_win[PN_COUNT];
static uint64_t prof_cyc_win[PROF_EVENTS][PS_COUNT], prof_ev_win[PROF_EVENTS][PS_COUNT];
static int prof_ev_frames[PROF_EVENTS], prof_ev_mode;
static const perf_cntr_event_t prof_ev_kind[PROF_EVENTS] = {
    PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE, PMCR_PIPELINE_FREEZE_BY_FPU_MODE, PMCR_OPERAND_CACHE_MISS_MODE
};

void prof_frame_end(void) {
    for(int i = 0; i < PS_COUNT; i++) {
        prof_cyc_win[prof_ev_mode][i] += prof_cyc[i];
        prof_ev_win[prof_ev_mode][i] += prof_ev[i];
        prof_cyc[i] = prof_ev[i] = 0;
    }
    for(int i = 0; i < PN_COUNT; i++) {
        prof_n_win[i] += prof_n[i];
        prof_n[i] = 0;
    }
    prof_ev_frames[prof_ev_mode]++;
    prof_ev_mode = (prof_ev_mode + 1) % PROF_EVENTS;
    perf_cntr_start(PRFC1, prof_ev_kind[prof_ev_mode], PMCR_COUNT_CPU_CYCLES);
}

/* One line per log window: each section's ms a frame, the share of its
   cycles stalled on operand-cache misses (D) and on the FPU (F), and its
   operand-cache misses a frame (M). */
void prof_print(void) {
    static const char *const names[PS_COUNT] = {"mats", "light", "mxform", "msubmit", "dolphins", "chunks", "sky",
                                                "sea", "foam", "upd", "world", "karts", "pt", "shadows", "tr", "u-input",
                                                "u-step", "u-camera", "u-audio", "u-other"};
    char line[PS_COUNT * 40 + PN_COUNT * 20 + 16], *o = line;
    o += sprintf(o, "DK PC");
    for(int i = 0; i < PS_COUNT; i++) {
        uint64_t cyc = 0;
        int frames = 0;
        for(int m = 0; m < PROF_EVENTS; m++) {
            cyc += prof_cyc_win[m][i];
            frames += prof_ev_frames[m];
        }
        float ms = frames ? (float)cyc * 5e-6f / frames : 0;
        float share[2];
        for(int m = 0; m < 2; m++)
            share[m] = prof_cyc_win[m][i] ? 100.0f * (float)prof_ev_win[m][i] / (float)prof_cyc_win[m][i] : 0;
        float miss = prof_ev_frames[2] ? (float)prof_ev_win[2][i] / prof_ev_frames[2] : 0;
        o += sprintf(o, " %s=%.2f/D%.0f/F%.0f/M%.0f", names[i], (double)ms, (double)share[0], (double)share[1],
                     (double)miss);
    }
    /* Work a frame: strip vertices transformed/emitted, strips culled and
       clipped, mesh vertices, triangles sent and back faces, vertices lit. */
    static const char *const counts[PN_COUNT] = {"sv", "se", "scull", "sclip", "mv", "mt", "mback", "lv", "sin",
                                                 "sout", "strips", "hdrs"};
    int frames = prof_ev_frames[0] + prof_ev_frames[1] + prof_ev_frames[2];
    o += sprintf(o, " |");
    for(int i = 0; i < PN_COUNT; i++)
        o += sprintf(o, " %s=%lu", counts[i], (unsigned long)(frames ? prof_n_win[i] / frames : 0));
    puts(line);
    memset(prof_n_win, 0, sizeof(prof_n_win));
    memset(prof_cyc_win, 0, sizeof(prof_cyc_win));
    memset(prof_ev_win, 0, sizeof(prof_ev_win));
    memset(prof_ev_frames, 0, sizeof(prof_ev_frames));
}

void prof_start(void) {
    perf_cntr_start(PRFC1, prof_ev_kind[0], PMCR_COUNT_CPU_CYCLES);
}
#endif
