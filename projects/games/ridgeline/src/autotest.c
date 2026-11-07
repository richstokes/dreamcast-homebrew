/* Ridgeline test builds (autotest.h). */
#include "autotest.h"
#ifdef RL_AUTOTEST
#include "truck.h"
#include "ai.h"
#include "camera.h"
#include "hud.h"
#include "terrain.h"
#include "engine/prof.h"

#ifndef RL_AT_TRUCK
#define RL_AT_TRUCK 1
#endif
#ifndef RL_AT_ROUTE
#define RL_AT_ROUTE 1          /* 1 logging road, 2 ridge line, 3 creek bed */
#endif
#ifndef RL_AT_COMPANIONS
#define RL_AT_COMPANIONS 3
#endif
#ifndef RL_AT_SECONDS
#define RL_AT_SECONDS 600
#endif

extern uint64_t phase_us[4];
extern int phase_frames;

static struct {
    float win_t, win_cpu, win_cpu_max;
    int win_frames, win_over;
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
};
#define TOUR_N (int)(sizeof(tour_views) / sizeof(tour_views[0]))

void autotest_boot(void) {
    game.truck_model = RL_AT_TRUCK;
    game.livery = 0;
    game.companions = RL_AT_COMPANIONS;
#ifdef RL_CAPTURE
    at.tour = 0;
    printf("RL AT capture: title, select, scenery tour, then the drive\n");
#else
    at.tour = TOUR_N;
    game_start_drive();
    ai_start(0, RL_AT_ROUTE);
#endif
    game.show_fps = true;
    printf("RL AT start: truck %d route %d companions %d\n", RL_AT_TRUCK, RL_AT_ROUTE, RL_AT_COMPANIONS);
}

void autotest_controls(Truck *p, float dt) {
    (void)dt;
    /* Lights on for a stretch so they are drawn and costed. */
    p->lights = fmodf(game.clock, 60.0f) > 40.0f;
}

int autotest_steps(int steps) {
#ifdef RL_CAPTURE
    return 1;
#else
    return steps;
#endif
}

static void shot(const char *name) {
    printf("RL SHOT %s\n", name);
}

void autotest_frame(float dt_real) {
#ifdef RL_CAPTURE
    /* Title, then select, then the tour, then drive. */
    static int frames;
    frames++;
    /* Start for the main menu, down past Online to the sound test, play a
       loop and a one-shot, back out and on to the truck select. */
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
        if(frames == 230 && menus_done) {
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
        if(frames == 180) {
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
        if(at.tour_frames >= 60) {
            at.tour++;
            at.tour_frames = 0;
            if(at.tour == TOUR_N) {
                game.photo = false;
                game.cam_scripted = false;
                game.paused = false;
                camera_reset();
                game.clock = 0;
            }
        }
        return;
    }
#endif
    if(game.state != ST_DRIVE || at.done) return;
    float cpu = game.cpu_ms;
    at.win_t += dt_real;
    at.win_frames++;
    at.win_cpu += cpu;
    if(cpu > at.win_cpu_max) at.win_cpu_max = cpu;
    if(dt_real > 1.5f / 60.0f) at.win_over++;
    at.all_frames++;
    at.all_cpu += cpu;
    if(cpu > at.all_cpu_max) at.all_cpu_max = cpu;
    if(dt_real > 1.5f / 60.0f) at.all_over++;
#ifdef RL_CAPTURE
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
    if(at.win_t >= 10.0f) {
        float f = phase_frames ? 1.0f / (phase_frames * 1000.0f) : 0;
        extern uint64_t world_us[3];
        printf("RL AT split terrain=%.2f trees=%.2f trucks=%.2f ms\n", (double)(world_us[0] * f), (double)(world_us[1] * f),
               (double)(world_us[2] * f));
        memset(world_us, 0, sizeof(world_us));
        extern int ter_count[4];
        printf("RL AT terrain verts: near3 %d near6 %d skirts %d far %d\n", (int)(ter_count[0] * f * 1000),
               (int)(ter_count[1] * f * 1000), (int)(ter_count[2] * f * 1000), (int)(ter_count[3] * f * 1000));
        memset(ter_count, 0, sizeof(ter_count));
        printf("RL AT t=%.0f fps=%.1f cpu=%.2f/%.2f over=%d upd=%.2f op=%.2f pt=%.2f tr=%.2f tris=%d verts=%d "
               "pos=%.0f,%.0f,%.0f v=%.1f rpm=%.0f g=%d mode=%d prog=%.2f summit=%.0f%%\n",
               (double)game.clock, (double)(at.win_frames / at.win_t), (double)(at.win_cpu / at.win_frames),
               (double)at.win_cpu_max, at.win_over, (double)(phase_us[0] * f), (double)(phase_us[1] * f),
               (double)(phase_us[2] * f), (double)(phase_us[3] * f), rstats.tris, rstats.verts, (double)p->pos.x,
               (double)p->pos.y, (double)p->pos.z, (double)p->speed, (double)p->rpm, p->gear, p->mode,
               (double)ai_progress(0), (double)(game.summit_frac * 100.0f));
#ifdef ENG_PROF
        prof_print();
#endif
        memset(phase_us, 0, sizeof(phase_us));
        phase_frames = 0;
        at.win_t = at.win_cpu = at.win_cpu_max = 0;
        at.win_frames = at.win_over = 0;
    }
    if(game.summit_reached || game.clock > RL_AT_SECONDS) {
        printf("RL AUTOTEST: PASS %s in %.0f s, summit %.0f%%, views %d, cpu avg %.2f max %.2f ms, %d of %d frames late\n",
               game.summit_reached ? "summit reached" : "timed out", (double)game.clock,
               (double)(game.summit_frac * 100.0f), __builtin_popcount(game.views_found),
               (double)(at.all_cpu / at.all_frames), (double)at.all_cpu_max, at.all_over, at.all_frames);
        at.done = true;
    }
}

void autotest_draw(void) {
}
#endif
