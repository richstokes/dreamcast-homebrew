/* Ridgeline test builds (autotest.h). */
#include "autotest.h"
#ifdef RL_AUTOTEST
#include "truck.h"
#include "ai.h"
#include "camera.h"
#include "hud.h"
#include "terrain.h"
#include "engine/prof.h"
#include "engine/frame.h"
#include "engine/qa.h"
#include "tod.h"
#include "world.h"
#ifdef RL_AT_ONLINE
#include "online.h"
#endif

#ifndef RL_AT_TRUCK
#define RL_AT_TRUCK 1
#endif
#ifndef RL_AT_ROUTE
#define RL_AT_ROUTE 1          /* 1 logging road, 2 ridge line, 3 creek bed */
#endif
#ifndef RL_AT_COMPANIONS
#define RL_AT_COMPANIONS 3
#endif
#ifndef RL_AT_TOD
#define RL_AT_TOD TOD_AFTERNOON   /* when the drive starts (tod.h) */
#endif

#ifdef RL_AT_FXBENCH
#define RL_FXB_DRIVE 15.0f     /* seconds of normal driving, then... */
#define RL_FXB_SPIN 25.0f      /* ...this long spinning in mud */
#define RL_AT_WINDOW 5.0f
#else
#define RL_AT_WINDOW 10.0f
#endif

/* Capture builds and console benches run for a set time and pass when
   it's up. The plain autotest is a test of the CPU driver: it passes only
   at the summit, and fails when the climb stops making progress or takes
   too long (the slowest hostsim climb, the pickup up the creek, is ~730 s;
   the player here can be held up behind a slower companion). */
#ifndef RL_AT_SECONDS
#if defined(RL_AT_FXBENCH)
#define RL_AT_SECONDS (RL_FXB_DRIVE + RL_FXB_SPIN)
#elif defined(RL_HW)
#define RL_AT_SECONDS 180
#elif defined(RL_CAPTURE)
#define RL_AT_SECONDS 600
#else
#define RL_AT_SECONDS 900
#endif
#endif
#ifndef RL_AT_STALL
#define RL_AT_STALL 120.0f     /* seconds without route progress: stuck */
#endif

static struct {
    QaWindow win;                /* this log window (engine/qa.h) */
    float all_cpu, all_cpu_max;
    int all_frames, all_over;
    int shot;
    float next_shot;
    bool done;
    int tour, tour_frames;
} at;

/* Fixed views for screenshots of the scenery: position, target. */
static const float tour_views[][6] = {
    {10, 8, -300, 20, 60, 60},          /* the meadow, looking up the mountain */
    {-90, 30, -120, -40, 100, 70},      /* the creek valley */
    {150, 70, -40, 20, 120, 120},       /* the east flank and the logging road */
    {-10, 125, 30, 30, 165, 140},       /* the high meadow and the lake */
    {60, 175, 120, -100, 60, -300},     /* from near the summit, down to the meadow */
    {-60, 30, -200, 60, 10, -100},      /* toward the sun across the meadow */
    {224, 38, 20, 238, 32, 32},         /* the washout on the logging road */
    {195, 63, 82, 192, 58, 101},        /* the log bridge, coming up the road */
    {138, 31, -96, 152, 42, -50},       /* up the shortcut */
    {-112, 8, -268, -130, 3, -250},     /* the boulder field */
    {-50, 4, -212, -95, 3, -221},       /* across the deep ford to the stone arch */
    {255, 8.5f, -116, 249, 6, -121},    /* the old pickup in the woods */
    {7, 101.5f, 91, -2, 98.5f, 86},     /* the high camp */
    {140, 3.5f, -214, 152, 1.2f, -194}, /* deer in the east meadow */
    {72, 92, 12, 92, 84, 32},           /* where the ridge line meets the logging road */
    {-52, 97, 18, -78, 82, 50},         /* the creek bed's last climb, by the lake */
    {40, 95, -60, 30, 122, 140},        /* the cloud on the summit, from the south flank */
    {-20, 116, 114, -37, 117, 132},     /* inside it on the summit trail */
};
#define TOUR_N (int)(sizeof(tour_views) / sizeof(tour_views[0]))

#if defined(RL_CAPTURE) && !defined(RL_HW)
/* The day over the meadow (tour view 0) and the west (view 5): the
   hours, each shot once the world has been relit for it (Flycast only:
   relighting the whole world at once stalls a console bench's first
   window). */
static const float tod_shot_hours[] = {5.5f, 6.3f, 8.0f, 12.5f, 17.0f, 18.3f, 19.0f, 22.0f};
#define TOD_SHOTS (int)(sizeof(tod_shot_hours) / sizeof(tod_shot_hours[0]))
static const int tod_shot_views[2] = {0, 5};
#define TOD_N (TOD_SHOTS * 2)
#else
#define TOD_N 0
#endif

#ifdef RL_AT_VMUTEST
#include "records.h"
/* make vmutest, on blank cards: a summit time saves and reads back, a
   slower one doesn't replace it, and the last truck's time keeps its own
   slot, apart from the Bobcat's. */
static void vmu_test(void) {
    char line[64];
    records_load();
    game.truck_model = 1;
    records_start();
    game.clock = 581.34f;
    bool first = records_summit(line, sizeof(line));
    printf("RL AT vmu: %s\n", line);
    records_start();
    game.clock = 640.0f;
    bool second = records_summit(line, sizeof(line));
    printf("RL AT vmu: %s\n", line);
    game.truck_model = TRUCK_MODELS - 1;   /* times past the first two trucks' sit in best_more */
    records_start();
    game.clock = 702.5f;
    bool third = records_summit(line, sizeof(line));
    printf("RL AT vmu: %s\n", line);
    records_set_name("PEBBLES");   /* the online name, kept with the times */
    records_load();
    uint32_t got = records_best(1, WAY_LOGGING), other = records_best(0, WAY_LOGGING);
    uint32_t last = records_best(TRUCK_MODELS - 1, WAY_LOGGING), bobcat = records_best(2, WAY_LOGGING);
    bool named = strcmp(records_name(), "PEBBLES") == 0;
    if(first && !second && third && got == 5813 && other == 0 && last == 7025 && bobcat == 0 && named)
        printf("RL AUTOTEST: PASS vmu save read back %lu and %lu, name %s\n", (unsigned long)got,
               (unsigned long)last, records_name());
    else
        printf("RL AUTOTEST: FAIL vmu save: first %d second %d third %d read back %lu other %lu last %lu bobcat %lu "
               "name \"%s\"\n", first, second, third, (unsigned long)got, (unsigned long)other, (unsigned long)last,
               (unsigned long)bobcat, records_name());
}
#endif

#ifdef RL_HW
/* What a console bench was doing when it stopped, for make hwreport, which
   reads rl_wd out of the console's memory with dc-tool-ip once dcload is
   back in control. A watchdog thread wakes twice a second and, if no frame
   has finished for 3 s, writes where the main thread is into the report
   and exits to dcload (dcload only answers dc-tool-ip from inside its own
   calls). It never prints: a print can be the thing that is stuck. Under
   dcload a print waits for the Mac's reply with interrupts off, and a
   reply that never comes stops the console there, inside dcload; then
   in_print is set and the report stays empty, as no other thread can run. */
#include <kos/thread.h>
#include <arch/cache.h>
#include <stdarg.h>
#include <dc/pvr.h>

volatile int rl_where;
volatile struct {
    uint32_t magic;          /* "RLWD" */
    uint32_t frames, prints, in_print, stalls;
    int32_t where, tour, tour_frames;
    char report[4096];
} rl_wd = {.magic = 0x44574c52u};
static kthread_t *wd_main;
static int wd_len;

static int wd_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf((char *)rl_wd.report + wd_len, sizeof(rl_wd.report) - wd_len, fmt, ap);
    va_end(ap);
    if(n > 0) wd_len += n;
    if(wd_len > (int)sizeof(rl_wd.report) - 1) wd_len = sizeof(rl_wd.report) - 1;
    return n;
}

/* This file's prints, marked so a hang inside one shows. */
static int at_printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rl_wd.in_print = 1;
    int n = vprintf(fmt, ap);
    rl_wd.in_print = 0;
    rl_wd.prints++;
    va_end(ap);
    return n;
}
#define printf at_printf

static void *watchdog(void *arg) {
    (void)arg;
    uint32_t last = 0;
    int still = 0;
    for(;;) {
        thd_sleep(500);
        uint32_t f = rl_wd.frames;
        if(f != last) {
            last = f;
            still = 0;
            continue;
        }
        if(++still != 6) continue;
        const kthread_t *m = wd_main;
        wd_len = 0;
        wd_printf("stall at frame %lu, where %d\n", (unsigned long)f, rl_where);
        wd_printf("main: state %d wait '%s' pc %08lx pr %08lx sp %08lx sr %08lx\n", (int)m->state,
                  m->wait_msg ? m->wait_msg : "-", (unsigned long)m->context.pc, (unsigned long)m->context.pr,
                  (unsigned long)m->context.r[15], (unsigned long)m->context.sr);
        const uint32_t *sp = (const uint32_t *)m->context.r[15];
        wd_printf("stack code:");
        for(int i = 0; i < 96; i++)
            if(sp[i] >= 0x8c010000u && sp[i] < 0x8c070000u) wd_printf(" %08lx", (unsigned long)sp[i]);
        wd_printf("\nasic %08lx %08lx %08lx  ta opb %08lx/%08lx vert %08lx/%08lx sync %08lx\n",
                  (unsigned long)*(volatile uint32_t *)0xa05f6900, (unsigned long)*(volatile uint32_t *)0xa05f6904,
                  (unsigned long)*(volatile uint32_t *)0xa05f6908, (unsigned long)PVR_GET(PVR_TA_OPB_POS),
                  (unsigned long)PVR_GET(PVR_TA_OPB_END), (unsigned long)PVR_GET(PVR_TA_VERTBUF_POS),
                  (unsigned long)PVR_GET(PVR_TA_VERTBUF_END), (unsigned long)PVR_GET(PVR_SYNC_STATUS));
        thd_pslist(wd_printf);
        rl_wd.stalls++;
        dcache_wback_range((uintptr_t)&rl_wd, sizeof(rl_wd));
        arch_exit();
    }
    return NULL;
}
#endif

void autotest_boot(void) {
#ifdef RL_HW
    wd_main = thd_get_current();
    thd_create(true, watchdog, NULL);
#endif
#ifdef RL_AT_VMUTEST
    vmu_test();
#endif
    game.truck_model = RL_AT_TRUCK;
    game.livery = 0;
    game.companions = RL_AT_COMPANIONS;
    game.start_tod = RL_AT_TOD;
#if defined(RL_AT_ONLINE)
    at.tour = TOUR_N + TOD_N;
    online_autotest_boot();
#elif defined(RL_CAPTURE) && !defined(RL_AT_FXBENCH)
    at.tour = 0;
    printf("RL AT capture: title, select, scenery tour%s, then the drive\n", TOD_N ? ", the day over the meadow" : "");
#else
    at.tour = TOUR_N + TOD_N;
    game_start_drive();
    ai_start(0, RL_AT_ROUTE);
#endif
    game.show_fps = true;
    printf("RL AT start: truck %d route %d companions %d\n", RL_AT_TRUCK, RL_AT_ROUTE, RL_AT_COMPANIONS);
}

bool autotest_controls(Truck *p, float dt) {
    (void)dt;
#ifdef RL_AT_FXBENCH
    /* Flat out in 2WD with traction control off, weaving, so all the
       tyres throw what they can. */
    if(game.clock >= RL_FXB_DRIVE) {
        p->in.throttle = 1;
        p->in.brake = 0;
        p->in.steer = 0.35f * sinf(game.clock * 1.3f);
        p->in.handbrake = false;
        p->mode = DRIVE_2WD;
        p->tcs_off = true;
        return true;
    }
#endif
    /* Each light setting for a third of every minute, so all are drawn and costed. */
    p->lights = (uint8_t)(fmodf(game.clock, 60.0f) / 20.0f);
    return false;
}

int autotest_steps(int steps) {
#ifdef RL_CAPTURE
    return 1;
#else
    return steps;
#endif
}

#ifdef RL_CAPTURE
/* The scripted views are over: back to the drive at its start hour. */
static void tour_done(void) {
    game.photo = false;
    game.cam_scripted = false;
    game.paused = false;
    camera_reset();
    game.clock = 0;
    tod_set_hour(tod_hours[game.start_tod]);
}
#endif

static void __attribute__((unused)) shot(const char *name) {
#ifdef RL_HW
    rl_wd.in_print = 1;
    qa_shot("%s", name);
    rl_wd.in_print = 0;
    rl_wd.prints++;
#else
    qa_shot("%s", name);
#endif
}

void autotest_frame(float dt_real) {
#ifdef RL_HW
    rl_wd.frames++;
    rl_wd.where = rl_where;
    rl_wd.tour = at.tour;
    rl_wd.tour_frames = at.tour_frames;
#endif
#ifdef RL_CAPTURE
    /* Title, then select, then the tour, then drive. */
    static int frames;
    frames++;
    /* Start for the main menu, down past Online to the sound test, play a
       loop and a one-shot, back out, down to the controls and back, and on
       to the truck select. */
    static bool menus_done, sound_seen;
    if(game.state == ST_TITLE) {
        if(sound_seen && !menus_done) {
            menus_done = true;
            frames = 200;
        }
        if(frames == 120 && !menus_done) input_film_tap(0, BTN_START);
        if(frames == 200 && !menus_done) shot("title");
        if(frames == 230 && !menus_done) input_film_tap(0, BTN_DOWN);
        if(frames == 250 && !menus_done) shot("title-online");
        if(frames == 300 && !menus_done) input_film_tap(0, BTN_DOWN);
        if(frames == 312 && !menus_done) input_film_tap(0, BTN_A);
        if(frames == 220 && menus_done) input_film_tap(0, BTN_DOWN);
        if(frames == 232 && menus_done) input_film_tap(0, BTN_A);
        if(frames == 270 && menus_done) shot("controls");
        if(frames == 350 && menus_done) input_film_tap(0, BTN_B);
        if(frames == 370 && menus_done) {
            game_set_state(ST_SELECT);
            frames = 0;
        }
        return;
    }
    if(game.state == ST_SOUND) {
        sound_seen = true;
        if(frames >= 340 && frames < 460 && frames % 8 == 0) input_film_tap(0, BTN_DOWN);
        if(frames == 464) input_film_tap(0, BTN_A);
        if(frames == 476 || frames == 484) input_film_tap(0, BTN_RIGHT);
        if(frames == 500) shot("sound");
        if(frames == 560) input_film_tap(0, BTN_B);
        return;
    }
    if(game.state == ST_SELECT) {
        if(frames == 150) shot("select");
        if(frames == 200) input_film_tap(0, BTN_A);
        if(frames == 260) shot("select-drive");
        if(frames == 290) {
            game_start_drive();
            ai_start(0, RL_AT_ROUTE);
            game.paused = false;
            frames = 0;
        }
        return;
    }
    if(at.tour < TOUR_N) {
        const float *v = tour_views[at.tour];
        V3 p = v3(v[0], v[1], v[2]);
        float g = terrain_height(p.x, p.z);
        if(p.y < g + 3.0f) p.y = g + 3.0f;
        camera_set(p, v3(v[3], v[4], v[5]), 1.2f);
        game.photo = true;
        game.cam_scripted = true;
        game.paused = true;
        if(++at.tour_frames == 40) {
            char name[16];
            snprintf(name, sizeof(name), "tour-%d", at.tour);
            shot(name);
        }
        /* Each view timed once it has settled: CPU and the PVR's render. */
        static float tcpu, tcpu_max, trnd, trnd_max;
        if(at.tour_frames > 20) {
            pvr_stats_t ps;
            pvr_get_stats(&ps);
            float rnd = (float)ps.rnd_last_time * 1e-6f;
            if(!(rnd < 1000.0f)) rnd = 0;
            tcpu += game.cpu_ms;
            trnd += rnd;
            if(game.cpu_ms > tcpu_max) tcpu_max = game.cpu_ms;
            if(rnd > trnd_max) trnd_max = rnd;
        }
        if(at.tour_frames == 60) {
            printf("RL AT tour-%d cpu %.2f/%.2f render %.2f/%.2f ms tris %d\n", at.tour, (double)(tcpu / 39),
                   (double)tcpu_max, (double)(trnd / 39), (double)trnd_max, rstats.tris);
            tcpu = tcpu_max = trnd = trnd_max = 0;
        }
        if(at.tour_frames >= 60) {
            at.tour++;
            at.tour_frames = 0;
            if(at.tour == TOUR_N + TOD_N) tour_done();
        }
        return;
    }
#if defined(RL_CAPTURE) && !defined(RL_HW)
    if(at.tour < TOUR_N + TOD_N) {
        /* The same two views through the day. */
        int k = at.tour - TOUR_N;
        const float *v = tour_views[tod_shot_views[k / TOD_SHOTS]];
        float hour = tod_shot_hours[k % TOD_SHOTS];
        V3 p = v3(v[0], v[1], v[2]);
        float g = terrain_height(p.x, p.z);
        if(p.y < g + 3.0f) p.y = g + 3.0f;
        camera_set(p, v3(v[3], v[4], v[5]), 1.2f);
        game.photo = true;
        game.cam_scripted = true;
        game.paused = true;
        if(at.tour_frames == 0) {
            uint64_t t0 = timer_us_gettime64();
            tod_set_hour(hour);
            tod_relight_all();
            printf("RL AT tod %.1f: relit in %lu ms\n", (double)hour, (unsigned long)((timer_us_gettime64() - t0) / 1000));
        }
        /* Shot early and held: the capture tool grabs the window a
           quarter of a second after the line. */
        if(++at.tour_frames == 15) {
            char name[24];
            snprintf(name, sizeof(name), "tod-%d-%02d%02d", k / TOD_SHOTS, (int)hour, (int)(fmodf(hour, 1.0f) * 60.0f + 0.5f));
            shot(name);
        }
        if(at.tour_frames >= 50) {
            at.tour++;
            at.tour_frames = 0;
            if(at.tour == TOUR_N + TOD_N) tour_done();
        }
        return;
    }
#endif
#endif
#ifdef RL_AT_ONLINE
    online_autotest(dt_real);
#endif
    if(game.state != ST_DRIVE || at.done) return;
    float cpu = game.cpu_ms;
    /* The window's slowest frame, phase by phase, with its relighting. */
    static float worst_cpu;
    static uint32_t worst_ph[4], worst_relight;
    static uint64_t tod_us_last;
    uint32_t relight_us = (uint32_t)(tod_us - tod_us_last);
    tod_us_last = tod_us;
    if(cpu > worst_cpu) {
        worst_cpu = cpu;
        memcpy(worst_ph, prof_phase_last, sizeof(worst_ph));
        worst_relight = relight_us;
    }
    qa_window_frame(&at.win, dt_real, eng_frame.steps, cpu);
    at.all_frames++;
    at.all_cpu += cpu;
    if(cpu > at.all_cpu_max) at.all_cpu_max = cpu;
    if(dt_real > 1.5f / 60.0f) at.all_over++;
#ifdef RL_AT_FXBENCH
    /* Into the meadow's mud, once. */
    static bool fxb_placed;
    if(!fxb_placed && game.clock >= RL_FXB_DRIVE) {
        truck_place(&trucks[0], v3(54, 0, -258), ENG_PI / 2);
        camera_reset();
        fxb_placed = true;
        printf("RL AT fxbench: spinning in mud\n");
    }
    static int fxb_shots;
    if(fxb_shots < 2 && game.clock >= RL_FXB_DRIVE + 6.0f + fxb_shots * 8.0f) shot(fxb_shots++ ? "fxbench-2" : "fxbench-1");
#elif defined(RL_CAPTURE)
    /* The pause menu: FPS counter on, photo mode, the camera swung round
       as the stick would, back out and on again. */
    static int pf;
    pf++;
    if(pf == 300) input_film_tap(0, BTN_START);
    if(pf == 330) shot("pause");
    if(pf == 340 || pf == 352) input_film_tap(0, BTN_DOWN);
    if(pf == 364) input_film_tap(0, BTN_A);
    if(pf == 380) shot("pause-fps");
    if(pf == 390) input_film_tap(0, BTN_Y);
    if(pf == 420) shot("photo");
    if(pf > 430 && pf < 520) {
        float sx = pad.stick_x, sy = pad.stick_y;
        pad.stick_x = 1.0f;
        pad.stick_y = -0.5f;
        camera_photo(&trucks[0], RL_DT);
        pad.stick_x = sx;
        pad.stick_y = sy;
    }
    if(pf == 525) shot("photo-orbit");
    if(pf == 530) input_film_tap(0, BTN_B);
    if(pf == 545) shot("pause-again");
    if(pf == 555) input_film_tap(0, BTN_START);
    if(pf == 600) shot("resumed-fps");
    /* The large map and no map (X). */
    if(pf == 610) game.map_mode = 1;
    if(pf == 630) shot("map-large");
    if(pf == 640) game.map_mode = 2;
    if(pf == 660) shot("map-off");
    if(pf == 670) game.map_mode = 0;
    if(game.clock >= at.next_shot) {
        char name[24];
        snprintf(name, sizeof(name), "drive-%03d", (int)game.clock);
        shot(name);
        at.next_shot += 20.0f;
    }
#endif
    const Truck *p = &trucks[0];
    if(isnan(p->pos.x) || isnan(p->pos.y)) {
        printf("RL AUTOTEST: FAIL the truck's position went NaN at %.1f s\n", (double)game.clock);
        at.done = true;
        return;
    }
    if(at.win.t >= RL_AT_WINDOW) {
        float f = prof_phase_frames ? 1.0f / (prof_phase_frames * 1000.0f) : 0;
        extern uint64_t world_us[3];
        printf("RL AT split terrain=%.2f trees=%.2f trucks=%.2f relight=%.2f ms at %.1f h\n", (double)(world_us[0] * f),
               (double)(world_us[1] * f), (double)(world_us[2] * f), (double)(tod_us * f), (double)tod.hour);
        memset(world_us, 0, sizeof(world_us));
        tod_us = 0;
        tod_us_last = 0;
        printf("RL AT worst %.2f ms: upd=%.2f op=%.2f pt=%.2f tr=%.2f relight=%.2f\n", (double)worst_cpu,
               worst_ph[0] * 1e-3, worst_ph[1] * 1e-3, worst_ph[2] * 1e-3, worst_ph[3] * 1e-3, worst_relight * 1e-3);
        worst_cpu = 0;
        extern int ter_count[4], world_tufts;
        printf("RL AT terrain verts: near3 %d near6 %d skirts %d far %d; tufts %d\n", (int)(ter_count[0] * f * 1000),
               (int)(ter_count[1] * f * 1000), (int)(ter_count[2] * f * 1000), (int)(ter_count[3] * f * 1000), world_tufts);
        memset(ter_count, 0, sizeof(ter_count));
        printf("RL AT gpu: render %.2f/%.2f ms\n", (double)(at.win.rnd_sum / at.win.frames), (double)at.win.rnd_max);
        printf("RL AT t=%.0f fps=%.1f cpu=%.2f/%.2f over=%d upd=%.2f op=%.2f pt=%.2f tr=%.2f tris=%d verts=%d "
               "pos=%.0f,%.0f,%.0f v=%.1f rpm=%.0f g=%d mode=%d prog=%.2f summit=%.0f%%\n",
               (double)game.clock, (double)(at.win.frames / at.win.t), (double)(at.win.cpu_sum / at.win.frames),
               (double)at.win.cpu_max, at.win.missed, (double)(prof_phase_us[0] * f), (double)(prof_phase_us[1] * f),
               (double)(prof_phase_us[2] * f), (double)(prof_phase_us[3] * f), rstats.tris, rstats.verts, (double)p->pos.x,
               (double)p->pos.y, (double)p->pos.z, (double)p->speed, (double)p->rpm, p->gear, p->mode,
               (double)ai_progress(0), (double)(game.summit_frac * 100.0f));
#ifdef ENG_PROF
        prof_print();
#endif
        prof_phase_reset();
        qa_window_reset(&at.win);
    }
    const char *fail = NULL;
#if defined(RL_AT_ONLINE)
    /* online_autotest() decides. */
    bool end = false;
    const char *how = "";
#elif defined(RL_CAPTURE)
    bool end = game.clock > RL_AT_SECONDS;
    const char *how = "bench ran";
#else
    static float best_prog, best_t;
    float prog = ai_progress(0);
    if(prog > best_prog + 0.005f) {
        best_prog = prog;
        best_t = game.clock;
    }
    bool end = game.summit_reached;
    const char *how = "summit reached";
    if(!end && game.clock - best_t > RL_AT_STALL) fail = "stuck: no route progress";
    if(!end && game.clock > RL_AT_SECONDS) fail = "no summit in time";
#endif
    if(fail) {
        printf("RL AUTOTEST: FAIL %s at %.0f s, summit %.0f%%, route %.2f, pos %.0f,%.0f,%.0f\n", fail, (double)game.clock,
               (double)(game.summit_frac * 100.0f), (double)ai_progress(0), (double)p->pos.x, (double)p->pos.y,
               (double)p->pos.z);
        at.done = true;
    }
    else if(end) {
        printf("RL AUTOTEST: PASS %s in %.0f s, summit %.0f%%, views %d, cpu avg %.2f max %.2f ms, %d of %d frames late\n",
               how, (double)game.clock, (double)(game.summit_frac * 100.0f), __builtin_popcount(game.views_found),
               (double)(at.all_cpu / at.all_frames), (double)at.all_cpu_max, at.all_over, at.all_frames);
        at.done = true;
    }
}

void autotest_draw(void) {
}
#endif
