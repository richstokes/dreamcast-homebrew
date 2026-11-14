/* Ridgeline test builds (autotest.h). */
#include "autotest.h"
#include "progress.h"
#include "records.h"
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
static const float tour_cascade[][6] = {
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
#define TOUR_CASCADE_N (int)(sizeof(tour_cascade) / sizeof(tour_cascade[0]))
/* The desert's (-DRL_AT_AREA=1). */
static const float tour_desert[][6] = {
    {0, 9, -300, 10, 50, -100},         /* the flats, looking up at the tiers */
    {-60, 8, -250, 250, 30, -160},      /* east along the wall */
    {60, 8, -240, -300, 50, -150},      /* west to the upland */
    {8, 12, -215, 20, 40, -110},        /* the fork, the fins ahead */
    {30, 44, -100, 60, 60, 20},         /* off the top of the first fin, the bench and the second */
    {150, 14, -215, 230, 40, -140},     /* the rim road's first dugway */
    {-150, 13, -176, -200, 30, -100},   /* the canyon's mouth */
    {-190, 30, -60, -180, 60, 10},      /* in the canyon, the park */
    {-180, 46, 20, -160, 70, 70},       /* the upper canyon */
    {262, 30, -176, 60, 0, -260},       /* down over the flats from the dugway */
    {90, 50, -80, 20, 80, 50},          /* the second fin */
    {40, 82, 44, -10, 88, 74},          /* the arch from the head of the fin */
    {-60, 84, 70, 60, 104, 100},        /* the shelf across the mesa's cliff */
    {24, 137, 226, 30, 20, 320},        /* the overlook, north */
    {24, 146, 214, 0, 20, -280},        /* from the overlook, south */
    {-140, 150, 250, 60, 20, 320},      /* the basin from the rim, west of the overlook */
    {-128, 12, -214, -100, 8, -238},    /* the hot tubs */
    {-276, 10, -252, -260, 8, -272},    /* the goblins */
};
#define TOUR_DESERT_N (int)(sizeof(tour_desert) / sizeof(tour_desert[0]))
/* The dunes' (-DRL_AT_AREA=2). */
static const float tour_dunes[][6] = {
    {0, 7, -300, 10, 18, -40},          /* the camp, looking up the dune field */
    {14, 6, -262, -50, 0, -380},        /* the lake and its islands */
    {-20, 5, -270, -70, 3, -290},       /* the trailers */
    {8, 6, -214, -4, 14, -80},          /* the fork */
    {-112, 16, -172, -226, 22, -40},    /* up the razorback */
    {-244, 34, -44, -160, 12, 40},      /* off its crest into the bowl */
    {30, 9, -172, 4, 12, -70},          /* the powerline's trough */
    {44, 21, 30, 34, 34, 110},          /* the ramp up the rimrock */
    {296, 6, -196, 304, 12, -60},       /* the shore road and the marsh */
    {292, -1, -84, 252, 6, -126},       /* the buried grove */
    {-110, 34, -110, -200, 8, -30},     /* over the big dunes and a pothole between them */
    {-50, 38, 92, 80, 60, 230},         /* the king dune from the meet */
    {102, 77, 243, 20, 0, -300},        /* from the top, south over the field and the lake */
    {86, 76, 226, 260, 20, 360},        /* from the top, north-east down the slip face */
    {132, 7, -214, 132, 3, -180},       /* the cattail pond */
    {-70, 5, -300, -64, 2, -352},       /* the bar to Pelican Island */
    {-150, 7, -262, -200, 2, -278},     /* the mud flats' wallow */
    {90, 8, -230, 60, 16, -100},        /* under the wires */
};
#define TOUR_DUNES_N (int)(sizeof(tour_dunes) / sizeof(tour_dunes[0]))
/* Loch Lowland's (-DRL_AT_AREA=3). */
static const float tour_lowland[][6] = {
    {0, 7, -300, 8, 60, 0},             /* from the beach, up the corrie to the pass */
    {60, 7, -296, -200, 70, -110},      /* west along the shore: the buttress, the pines */
    {-70, 6, -300, 200, 70, -90},       /* east: the bothy, the burn, the birches */
    {-10, 16, -214, 20, 50, -60},       /* the fork, the bridge on the right */
    {-6, 28, -140, 8, 70, 120},         /* up the corrie's floor to its head */
    {136, 58, -78, 126, 76, 60},        /* along the drove road's shelf */
    {124, 104, 104, 40, 98, 196},       /* the hairpins from above the east wall */
    {6, 118, 226, 16, 20, -300},        /* from the pass, down the corrie to the loch */
    {176, 158, 288, 40, 20, -300},      /* from the top, the same */
    {-30, 118, 196, 172, 152, 277},     /* from the pass, up the ridge to the mast in its cloud */
    {-40, 5, -244, -64, 2, -264},       /* the cattle on the in-bye, the fank behind */
    {-228, 66, -134, -280, 74, -60},    /* Lochan Dubh under its crag */
    {-140, 126, 66, 60, 50, -10},       /* off the crag's edge into the corrie */
    {40, 8, -300, -60, 2, -350},        /* the jetty, the crannog, the headland's stones */
    {-136, 9, -330, -152, 7, -346},     /* the standing stones */
    {0, 16, -206, 26, 12, -186},        /* the stone bridge */
    {14, 110, 284, -10, 104, 260},      /* the camp by the lochan north of the pass */
    {-30, 44, 30, 20, 80, 150},         /* the falls and the staircase */
};
#define TOUR_LOWLAND_N (int)(sizeof(tour_lowland) / sizeof(tour_lowland[0]))
_Static_assert(TOUR_CASCADE_N == TOUR_DESERT_N && TOUR_CASCADE_N == TOUR_DUNES_N && TOUR_CASCADE_N == TOUR_LOWLAND_N,
               "the tours are as long as each other");
#define TOUR_N TOUR_CASCADE_N
#define tour_views \
    (game.area == AREA_MOAB ? tour_desert : game.area == AREA_DUNES ? tour_dunes : game.area == AREA_LOWLAND ? tour_lowland : tour_cascade)

#if defined(RL_CAPTURE) && !defined(RL_HW) && !defined(RL_AT_TOUR_ONLY)
/* The day over the meadow (tour view 0) and the west (view 5): the
   hours, each shot once the world has been relit for it (Flycast only:
   relighting the whole world at once stalls a console bench's first
   window). */
static const float tod_shot_hours[] = {5.5f, 6.3f, 8.0f, 12.5f, 17.0f, 18.3f, 19.0f, 22.0f};
#define TOD_SHOTS (int)(sizeof(tod_shot_hours) / sizeof(tod_shot_hours[0]))
/* (Loch Lowland's are the corrie from the beach and the loch from the pass) */
#define tod_shot_views (game.area == AREA_LOWLAND ? tod_views_lowland : tod_views)
static const int tod_views[2] = {0, 5}, tod_views_lowland[2] = {0, 7};
#define TOD_N (TOD_SHOTS * 2)
#else
#define TOD_N 0
#endif

#ifdef RL_AT_VMUTEST
#include "records.h"
/* make vmutest, on blank cards: a summit time saves and reads back, a
   slower one doesn't replace it, the last truck's time keeps its own
   slot, apart from the Bobcat's, and the desert's and the dunes' times
   keep theirs. And the progress kept with them: viewpoints found, the
   trail log's stamps (and the trucks those earn), an extreme trail's
   time and the unlock code's flag. */
static void vmu_test(void) {
    char line[64];
    records_load();
    game.truck_model = 1;
    records_start();
    game.clock = 581.34f;
    bool first = records_summit(line, sizeof(line), NULL);
    printf("RL AT vmu: %s\n", line);
    records_start();
    game.clock = 640.0f;
    bool second = records_summit(line, sizeof(line), NULL);
    printf("RL AT vmu: %s\n", line);
    game.truck_model = TRUCK_MODELS - 1;   /* times past the first two trucks' sit in best_more */
    records_start();
    game.clock = 702.5f;
    bool third = records_summit(line, sizeof(line), NULL);
    printf("RL AT vmu: %s\n", line);
    /* The other area's times are its own. */
    game.area = AREA_MOAB;
    game.truck_model = 1;
    records_start();
    game.clock = 333.3f;
    bool fourth = records_summit(line, sizeof(line), NULL);
    printf("RL AT vmu: %s\n", line);
    game.area = AREA_DUNES;
    records_start();
    game.clock = 444.4f;
    bool fifth = records_summit(line, sizeof(line), NULL);
    printf("RL AT vmu: %s\n", line);
    game.area = AREA_LOWLAND;
    records_start();
    game.clock = 555.5f;
    bool sixth = records_summit(line, sizeof(line), NULL);
    printf("RL AT vmu: %s\n", line);
    records_set_name("PEBBLES");   /* the online name, kept with the times */
    records_add_views(AREA_MOAB, 0x8005);
    records_add_stamps(AREA_LOWLAND, STAMP_SUMMIT | STAMP_BIT(LOG_EXTREME, STAMP_TOP | STAMP_NIGHT));
    game.area = AREA_DUNES;
    game.truck_model = TRUCK_MODELS - 1;
    bool x_first = records_extreme(2345, line, sizeof(line)), x_slower = records_extreme(3000, line, sizeof(line));
    records_set_cheat(true);
    records_set_told(0x44);
    bool back = records_load();
    Done d;
    for(int a = 0; a < AREA_COUNT; a++) {
        d.views[a] = records_views(a);
        d.stamps[a] = records_stamps(a);
    }
    /* The Highlands' extreme trail earns the Kestrel. And every area
       above has a best time, which is a summit reached: on loading, each
       gets its summit and the top by that way (a save from before the
       trail log is credited the same way), so the One Ten and the Goose
       are earned too; the Eagle (all four trails) is not. */
    bool prog = back && x_first && !x_slower && records_best(TRUCK_MODELS - 1, WAY_EXTREME) == 2345 &&
                records_views(AREA_MOAB) == 0x8005 && records_views(AREA_CASCADE) == 0 &&
                records_stamps(AREA_LOWLAND) ==
                    (STAMP_SUMMIT | STAMP_BIT(WAY_LOGGING, STAMP_TOP) | STAMP_BIT(LOG_EXTREME, STAMP_TOP | STAMP_NIGHT)) &&
                records_stamps(AREA_CASCADE) == (STAMP_SUMMIT | STAMP_BIT(WAY_LOGGING, STAMP_TOP)) &&
                records_cheat() && records_told() == 0x44 && unlock_met(1, &d) && unlock_met(6, &d) && unlock_met(5, &d) &&
                !unlock_met(7, &d) && unlock_met(2, &d) && done_views(&d) == 3;
    printf("RL AT vmu: progress %s (views %04x, stamps %04x, extreme %lu, cheat %d, told %02x)\n", prog ? "ok" : "WRONG",
           records_views(AREA_MOAB), records_stamps(AREA_LOWLAND), (unsigned long)records_best(TRUCK_MODELS - 1, WAY_EXTREME),
           records_cheat(), records_told());
    game.truck_model = 1;
    game.area = AREA_LOWLAND;
    uint32_t loch = records_best(1, WAY_LOGGING);
    game.area = AREA_DUNES;
    uint32_t dunes = records_best(1, WAY_LOGGING);
    game.area = AREA_MOAB;
    uint32_t desert = records_best(1, WAY_LOGGING);
    game.area = AREA_CASCADE;
    uint32_t got = records_best(1, WAY_LOGGING), other = records_best(0, WAY_LOGGING);
    uint32_t last = records_best(TRUCK_MODELS - 1, WAY_LOGGING), bobcat = records_best(2, WAY_LOGGING);
    bool named = strcmp(records_name(), "PEBBLES") == 0;
    if(first && !second && third && fourth && fifth && got == 5813 && other == 0 && last == 7025 && bobcat == 0 && desert == 3333 &&
       dunes == 4444 && sixth && loch == 5555 && named && prog)
        printf("RL AUTOTEST: PASS vmu save read back %lu, %lu, the desert's %lu, the dunes' %lu and the loch's %lu, name %s\n",
               (unsigned long)got, (unsigned long)last, (unsigned long)desert, (unsigned long)dunes, (unsigned long)loch,
               records_name());
    else
        printf("RL AUTOTEST: FAIL vmu save: first %d second %d third %d fourth %d fifth %d sixth %d read back %lu other %lu last %lu "
               "bobcat %lu desert %lu dunes %lu loch %lu name \"%s\"\n", first, second, third, fourth, fifth, sixth, (unsigned long)got,
               (unsigned long)other, (unsigned long)last, (unsigned long)bobcat, (unsigned long)desert, (unsigned long)dunes,
               (unsigned long)loch,
               records_name());
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
#ifdef RL_AT_AREA
    game_load_area(RL_AT_AREA);   /* -DRL_AT_AREA=1: the desert */
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
#ifdef RL_AT_TRAIL
    /* -DRL_AT_TRAIL=150 (with -DRL_AT_ROUTE=5): that many seconds into
       the extreme trail's run, one recover, as a player's: back to the
       last rest, and the run no longer clean (the log's "Ridgeline
       progress" lines say where it landed and what was stamped). */
    static bool recovered;
    if(!recovered && progress_trail_on() && progress_trail_clock() > (float)(RL_AT_TRAIL)) {
        recovered = true;
        progress_recover();
        camera_reset();
    }
#endif
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
#ifdef RL_AT_TOUR_ONLY
    printf("RL AUTOTEST: PASS tour done\n");
    at.done = true;
#endif
}
#endif

#ifdef RL_AT_FBDUMP
/* -DRL_AT_FBDUMP=2: each shot's picture goes out through the serial
   console as well (the frame the PVR is showing a few frames after the RL
   SHOT line, RGB565, a base64 line a row; 2 is every other pixel of every
   other row, 1 all of them), for a Mac whose screen is locked, where
   Flycast's window can't be photographed. Flycast must run with
   rend.EmulateFramebuffer=yes, so the picture is in the guest's video
   memory, and rend.vsync=no, or it waits for a display that is off. Its
   serial port keeps to its baud rate: a half-size picture takes ~18 s, a
   whole one over a minute. tools/fbshots.py turns the log back into
   pictures. */
static char fb_name[20];
static int fb_wait;

static void fb_dump(void) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const int step = RL_AT_FBDUMP + 0 > 1 ? 2 : 1, w = 640 / step, h = 480 / step;
    uint32_t sof = *(volatile uint32_t *)0xa05f8050 & 0x00fffffc;
    const uint16_t *fb = (const uint16_t *)(0xa5000000 + sof);
    static uint8_t row[1280 + 2];
    static char line[1720];
    printf("RL FB %s %d %d\n", fb_name, w, h);
    for(int y = 0; y < h; y++) {
        const uint16_t *p = fb + y * step * 640;
        for(int x = 0; x < w; x++) {
            uint16_t v = p[x * step];
            row[x * 2] = (uint8_t)v;
            row[x * 2 + 1] = (uint8_t)(v >> 8);
        }
        row[w * 2] = row[w * 2 + 1] = 0;
        int n = 0;
        for(int i = 0; i < w * 2; i += 3) {
            uint32_t v = (uint32_t)row[i] << 16 | (uint32_t)row[i + 1] << 8 | row[i + 2];
            line[n++] = b64[v >> 18];
            line[n++] = b64[(v >> 12) & 63];
            line[n++] = b64[(v >> 6) & 63];
            line[n++] = b64[v & 63];
        }
        line[n] = '\0';
        printf("RL FB:%s\n", line);
    }
    printf("RL FB END\n");
}
#endif

static void __attribute__((unused)) shot(const char *name) {
#ifdef RL_AT_FBDUMP
    snprintf(fb_name, sizeof(fb_name), "%s", name);
    fb_wait = 8;
#endif
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
#ifdef RL_AT_FBDUMP
    if(fb_wait > 0 && --fb_wait == 0) fb_dump();
#endif
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
#ifdef RL_AT_TOUR_ONLY
    /* -DRL_AT_TOUR_ONLY: straight to the scenery tour, and done after it. */
    if(game.state != ST_DRIVE) {
        game_start_drive();
        ai_start(0, RL_AT_ROUTE);
        return;
    }
#endif
    if(game.state == ST_TITLE) {
        if(sound_seen && !menus_done) {
            menus_done = true;
            frames = 200;
        }
#ifdef RL_AT_LOCKS
        /* -DRL_AT_LOCKS: the trucks locked as a player's are (test builds
           otherwise drive anything), with a little done already so the
           trail log has stamps to show. The title's code first: every
           truck unlocked, then locked again. */
        static const uint32_t code[5] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A | BTN_START};
        if(frames == 2 && !menus_done) {
            /* (stamps for the card, but no summit and no extreme trail:
               nothing earned yet but the Taco's first seven viewpoints) */
            records_add_stamps(AREA_CASCADE, STAMP_BIT(0, STAMP_TOP | STAMP_CLEAN) | STAMP_BIT(2, STAMP_TOP | STAMP_NIGHT));
            records_add_views(AREA_CASCADE, 0x2b);
            records_add_views(AREA_MOAB, 0x7);
        }
        for(int k = 0; k < 5 && !menus_done; k++) {
            if(frames == 20 + k * 8) input_film_tap(0, code[k]);
            if(frames == 78 + k * 8) input_film_tap(0, code[k]);
        }
        if(frames == 70 && !menus_done) {
            printf("RL AT locks: the code %s\n", records_cheat() ? "unlocked every truck" : "DID NOTHING");
            shot("title-code");
        }
        if(frames == 116 && !menus_done) printf("RL AT locks: the code again %s\n", records_cheat() ? "DID NOTHING" : "locked them");
#endif
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
        if(frames == 60) shot("select-area");
        if(frames == 100) input_film_tap(0, BTN_RIGHT);
        if(frames == 140) shot("select-area-other");
        if(frames == 150) input_film_tap(0, BTN_LEFT);
        if(frames == 170) input_film_tap(0, BTN_A);
        if(frames == 230) shot("select");
#ifdef RL_AT_LOCKS
        /* On to a truck still to be earned, A (refused), the next one,
           and back. */
        if(frames == 244) input_film_tap(0, BTN_RIGHT);
        if(frames == 256) {
            printf("RL AT locks: the select is on a truck that is %s\n", progress_unlocked(game.truck_model) ? "NOT LOCKED" : "locked");
            shot("select-locked");
        }
        if(frames == 270) input_film_tap(0, BTN_A);
        if(frames == 276) input_film_tap(0, BTN_START);
        if(frames == 282) input_film_tap(0, BTN_RIGHT);
        if(frames == 294) shot("select-locked-2");
        if(frames == 308 || frames == 314) input_film_tap(0, BTN_LEFT);
        if(frames == 326)
            printf("RL AT locks: A and Start on it %s, and back on a truck that is %s\n",
                   game.state == ST_SELECT ? "were refused" : "STARTED A DRIVE", progress_unlocked(game.truck_model) ? "the player's" : "LOCKED");
        const int later = 90;
#else
        const int later = 0;
#endif
        if(frames == 250 + later) input_film_tap(0, BTN_A);
        if(frames == 300 + later) shot("select-drive");
        if(frames == 330 + later) {
            game_start_drive();
            ai_start(0, RL_AT_ROUTE);
            game.paused = false;
            frames = 0;
        }
        return;
    }
    if(at.tour < TOUR_N) {
#ifdef RL_AT_TOUR_ROUTE
        /* -DRL_AT_TOUR_ROUTE=2: the tour is a drive up that route instead,
           the views a chase camera's at even steps along it. */
        const lvl_route_t *rt = &lvl_routes[RL_AT_TOUR_ROUTE];
        /* (Along its own stretch: the extreme trail's, between its gates.) */
        int k = rt->own0 + (int)((at.tour + 0.5f) * (rt->own1 - rt->own0 + 1) / TOUR_N);
        int k1 = k + 4 < rt->n ? k + 4 : rt->n - 1, k0 = k > 2 ? k - 2 : 0;
        V3 p = v3(rt->pts[k0].x, rt->pts[k0].y + 3.4f, rt->pts[k0].z);
        float tv[6] = {p.x, p.y, p.z, rt->pts[k1].x, rt->pts[k1].y + 1.0f, rt->pts[k1].z};
        const float *v = tv;
#else
        const float *v = tour_views[at.tour];
        V3 p = v3(v[0], v[1], v[2]);
#endif
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
        if(at.tour_frames > 20 && at.tour_frames <= 60) {
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
            printf("RL AT tour-%d cpu %.2f/%.2f render %.2f/%.2f ms tris %d\n", at.tour, (double)(tcpu / 40),
                   (double)tcpu_max, (double)(trnd / 40), (double)trnd_max, rstats.tris);
            tcpu = tcpu_max = trnd = trnd_max = 0;
        }
        /* (a tour-only capture holds each view until its screenshot is in) */
#ifdef RL_AT_TOUR_ONLY
        if(at.tour_frames >= 110) {
#else
        if(at.tour_frames >= 60) {
#endif
            at.tour++;
            at.tour_frames = 0;
            if(at.tour == TOUR_N + TOD_N) tour_done();
        }
        return;
    }
#if defined(RL_CAPTURE) && !defined(RL_HW) && !defined(RL_AT_TOUR_ONLY)
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
    static uint64_t vbuf_sum;
    static uint32_t vbuf_max;
    {
        pvr_stats_t ps;
        pvr_get_stats(&ps);
        vbuf_sum += ps.vtx_buffer_used;
        if(ps.vtx_buffer_used > vbuf_max) vbuf_max = (uint32_t)ps.vtx_buffer_used;
    }
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
        extern uint64_t lamps_us;
        printf("RL AT split terrain=%.2f trees=%.2f trucks=%.2f lamps=%.2f relight=%.2f ms at %.1f h\n",
               (double)(world_us[0] * f), (double)(world_us[1] * f), (double)(world_us[2] * f), (double)(lamps_us * f),
               (double)(tod_us * f), (double)tod.hour);
        memset(world_us, 0, sizeof(world_us));
        lamps_us = 0;
        tod_us = 0;
        tod_us_last = 0;
        printf("RL AT worst %.2f ms: upd=%.2f op=%.2f pt=%.2f tr=%.2f relight=%.2f\n", (double)worst_cpu,
               worst_ph[0] * 1e-3, worst_ph[1] * 1e-3, worst_ph[2] * 1e-3, worst_ph[3] * 1e-3, worst_relight * 1e-3);
        worst_cpu = 0;
        extern int ter_count[4], world_tufts;
        printf("RL AT terrain verts: near3 %d near6 %d skirts %d far %d; tufts %d\n", (int)(ter_count[0] * f * 1000),
               (int)(ter_count[1] * f * 1000), (int)(ter_count[2] * f * 1000), (int)(ter_count[3] * f * 1000), world_tufts);
        memset(ter_count, 0, sizeof(ter_count));
        /* The Tile Accelerator's vertex buffer, too: what it kept of the
           frame (it drops what is wholly off screen). */
        printf("RL AT gpu: render %.2f/%.2f ms, vertex buffer %lu/%lu KiB\n", (double)(at.win.rnd_sum / at.win.frames),
               (double)at.win.rnd_max, (unsigned long)(vbuf_sum / at.win.frames / 1024), (unsigned long)(vbuf_max / 1024));
        vbuf_sum = vbuf_max = 0;
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
