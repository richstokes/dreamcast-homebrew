/* Ridgeline: boot, the frame loop, driving and the render order. */
#include "air.h"
#include "rl.h"
#include "world.h"
#include "lamps.h"
#include "terrain.h"
#include "truck.h"
#include "truckdraw.h"
#include "camera.h"
#include "hud.h"
#include "audio.h"
#include "ai.h"
#include "fx.h"
#include "records.h"
#include "deer.h"
#include "menu.h"
#include "autotest.h"
#include "demo.h"
#include "sound_assets.h"
#include "softreset.h"
#include "version.h"
#include "engine/prof.h"
#include "engine/frame.h"
#include "engine/ui.h"
#include "birds.h"
#include "tod.h"
#include "online.h"
#include "film.h"
#include "vmuscreen.h"
#include "campfire.h"
#include "glow.h"
#include "desert.h"
#include "dunes.h"
#include "outland.h"
#include <malloc.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

/* Single player starts with a full line-up; the select page can thin it out. */
Game game = {.companions = 3};
/* The frame's phases for the autotest's log line (engine/prof.h). */
enum { PH_UPD, PH_OP, PH_PT, PH_TR };

static float recover_hold;

void game_set_state(int state) {
    if(game.state == ST_SOUND && state != ST_SOUND) soundtest_stop();
    game.state = state;
    game.time = 0;
    /* The truck select shows the hour the drive will start at. */
    if(state == ST_SELECT) {
        tod_set_hour(tod_hours[game.start_tod]);
        menu_select_begin();
    }
    if(state == ST_SELECT || state == ST_TITLE || state == ST_SOUND || state == ST_ONLINE) {
        for(int i = 0; i < MAX_TRUCKS; i++) trucks[i].remote = trucks[i].absent = false;
        game.ntrucks = state == ST_SELECT ? 1 : 0;
        trucks[0].model = game.truck_model;
        trucks[0].livery = game.livery;
        trucks[0].cpu = false;
        truck_place(&trucks[0], v3(lvl_spawns[1][0] + 4.0f, 0, lvl_spawns[1][2] + 6.0f), 0.6f);
        trucks[0].mode = DRIVE_2WD;
        fx_clear();
    }
}

void game_start_drive(void) {
    game.ntrucks = 1 + game.companions;
    /* The player in the second slot of the line-up, CPUs either side. */
    static const int slot[4] = {1, 2, 0, 3};
    for(int i = 0; i < game.ntrucks; i++) {
        Truck *t = &trucks[i];
        t->model = (game.truck_model + i) % TRUCK_MODELS;
        t->livery = i == 0 ? game.livery : (game.livery + i) % 4;
        t->cpu = i > 0;
        t->tcs_off = false;   /* traction control starts on */
        const float *s = lvl_spawns[slot[i]];
        truck_place(t, v3(s[0], 0, s[2]), s[3]);
        t->lights = LIGHTS_OFF;
        if(i > 0) ai_start(i, 1 + (i - 1) % 3);
    }
    game.best_y = trucks[0].pos.y;
    game.summit_frac = 0;
    game.views_found = 0;
    game.last_view = -1;
    game.summit_reached = false;
    game.clock = 0;
    records_start();
    deer_reset();
    game.paused = false;
    game.photo = false;
    camera_reset();
    fx_clear();
    birds_reset();
    audio_drive_start();
    game_set_state(ST_DRIVE);
    tod_set_hour(tod_hours[game.start_tod]);
    hud_message(lvl->name, 0xfff8f0dc, 3.0f);
}

/* ---- Driving ---- */

static void __attribute__((unused)) player_controls(Truck *p, float dt) {
    TruckInput *in = &p->in;
    in->steer = pad.stick_x;
    /* A progressive pedal, as trucks map theirs: the first half of the
       trigger's travel asks for about a third of the power, so a 2WD slide
       can be fed and held rather than switched on. */
    float x = pad.rtrig;
    in->throttle = x * sqrtf(x);
    /* Low range softens the pedal further, as many 4L throttle maps do:
       light pedal gives about the wheel torque high range would, rising to
       the full low-range shove only near the floor, so a crawl isn't a
       lurch. */
    if(p->mode == DRIVE_LOCK) {
        float k = 1.0f / p->spec.low_range;
        in->throttle *= k + (1.0f - k) * x * x;
    }
    in->brake = pad.ltrig;
    in->handbrake = (pad.held & BTN_A) != 0;
    p->horn = (pad.held & BTN_UP) != 0;
    /* No message: the HUD's headlight lamp shows which beam is on. */
    if(pad.pressed & BTN_DOWN) p->lights = (uint8_t)((p->lights + 1) % LIGHTS_COUNT);
    /* D-pad left/right step the drive mode. Held 2 s on a truck with
       traction control, left switches that off or on instead, so there
       left shifts on release after a tap rather than on the press. */
    static float left_hold;
    static bool left_done;
    int step = 0;
    if(pad.pressed & BTN_RIGHT) step = 1;
    if(!p->spec.tcs) {
        left_hold = 0;
        left_done = false;
        if(pad.pressed & BTN_LEFT) step = -1;
    }
    else if(pad.held & BTN_LEFT) {
        if(!left_done && (left_hold += dt) >= 2.0f) {
            p->tcs_off = !p->tcs_off;
            left_done = true;
            hud_message(p->tcs_off ? "TRACTION CONTROL OFF" : "TRACTION CONTROL ON", 0xffffd070, 1.6f);
        }
    }
    else {
        if(left_hold > 0 && left_hold < 0.5f && !left_done) step = -1;
        left_hold = 0;
        left_done = false;
    }
    if(step) {
        int m = p->mode + step;
        if(m >= 0 && m < DRIVE_COUNT) {
            static const char *const names[DRIVE_COUNT] = {"2WD  REAR DRIVE", "4WD  CENTRE LOCKED",
                                                            "LOW RANGE + DIFF LOCKS"};
            int r = truck_set_mode(p, m);
            if(r == MODE_CHANGED) hud_message(names[m], 0xffffd070, 1.6f);
            else if(r == MODE_TOO_FAST) hud_message("SLOW DOWN TO CHANGE RANGE", 0xffff9050, 1.6f);
        }
    }
    if(pad.pressed & BTN_B) game.cam_mode = (game.cam_mode + 1) % 3;
    if(pad.pressed & BTN_X) game.map_mode = (game.map_mode + 1) % 3;
    if(pad.held & BTN_Y) {
        recover_hold += dt;
        if(recover_hold > 1.0f) {
            truck_recover(p);
            camera_reset();
            recover_hold = 0;
            hud_message("RECOVERED", 0xfff8f0dc, 1.2f);
        }
    }
    else recover_hold = 0;
    hud_hold_k = 0;
    if(recover_hold > 0.15f) {
        hud_hold_k = recover_hold;
        hud_hold_label = "HOLD Y: RECOVER";
    }
    else if(left_hold > 0.5f && !left_done) {
        hud_hold_k = left_hold / 2.0f;
        hud_hold_label = p->tcs_off ? "HOLD LEFT: TCS ON" : "HOLD LEFT: TCS OFF";
    }
}

_Static_assert(LVL_MAX_VIEWS <= sizeof(game.views_found) * 8, "a viewpoint without a bit is found again every frame");

static void progress_update(void) {
    const Truck *p = &trucks[0];
    if(p->pos.y > game.best_y) game.best_y = p->pos.y;
    game.summit_frac = clampf((game.best_y - LEVEL_START_Y) / (LEVEL_SUMMIT_Y - 1.5f - LEVEL_START_Y), 0, 1);
    for(int i = 0; i < LVL_VIEWS; i++) {
        if(game.views_found & (1u << i)) continue;
        const lvl_view_t *v = &lvl_views[i];
        float dx = p->pos.x - v->x, dz = p->pos.z - v->z;
        if(dx * dx + dz * dz < 16.0f * 16.0f && fabsf(p->pos.y - v->y) < 10.0f) {
            game.views_found |= 1u << i;
            game.last_view = i;
            char buf[48];
            snprintf(buf, sizeof(buf), "VIEWPOINT: %s", v->name);
            hud_message(buf, 0xff90f0b0, 3.0f);
            audio_sfx(RL_SFX_CHIME, 220);
        }
    }
    float dx = p->pos.x - LEVEL_SUMMIT_X, dz = p->pos.z - LEVEL_SUMMIT_Z;
    if(!game.summit_reached && dx * dx + dz * dz < 18.0f * 18.0f) {
        game.summit_reached = true;
        game.summit_frac = 1.0f;
        char line[64];
        snprintf(line, sizeof(line), "SUMMIT!  %s", lvl->name);
        hud_message(line, 0xffffe070, 7.0f);
        records_summit(line, sizeof(line));
        hud_message_sub(line);
        audio_sfx(RL_SFX_SUMMIT, 240);
        if(game.online) online_summit();
    }
}

static void drive_update(float dt) {
    Truck *p = &trucks[0];
#ifdef RL_FILM
    film_update(dt);
    return;
#endif
    if(game.demo) {
        demo_update(dt);
        return;
    }
    bool menu = game.paused;
    if(menu) {
        menu_pause_update(dt);
        /* Online, the mountain runs on behind the menu (the others are
           still driving), the truck left in neutral. */
        if(!game.online || game.state != ST_DRIVE) return;
    }
    else if(pad.pressed & BTN_START) {
        game.paused = menu = true;
        game.pause_sel = 0;
        game.view_sel = game.last_view;
        audio_sfx(RL_SFX_MENU_SELECT, 160);
        if(!game.online) return;
    }
    if(menu) {
        TruckInput none = {0};
        p->in = none;
        p->horn = false;
        hud_hold_k = 0;
    }
    else {
#ifdef RL_AUTOTEST
        if(!autotest_controls(p, dt)) ai_drive(0, dt);
#else
        player_controls(p, dt);
#endif
    }
    if(!game.online)
        for(int i = 1; i < game.ntrucks; i++) ai_drive(i, dt);
    for(int i = 0; i < game.ntrucks; i++)
        if(!trucks[i].remote) truck_update(&trucks[i], dt);
    if(game.online) online_drive_step(dt);
    progress_update();
    records_update(dt);
    game.clock += dt;
    fx_update(dt);
    if(!game.photo) camera_follow(p, dt);
}

static void update(float dt) {
    game.time += dt;
    switch(game.state) {
        case ST_TITLE: menu_title_update(dt); break;
        case ST_SELECT: menu_select_update(dt); break;
        case ST_DRIVE: drive_update(dt); break;
        case ST_SOUND: soundtest_update(dt); break;
        case ST_ONLINE: online_update(dt); break;
    }
    /* The clock stops on the truck select (it shows the start hour) and
       in the pause menu. A jump of the clock there, or in photo mode, is
       relit faster than the drive can afford; the title's slow turn of
       the day a little faster, as its flyover has less to spare. */
    bool run_clock = game.state != ST_SELECT && !(game.state == ST_DRIVE && game_frozen());
#ifdef RL_FILM
    float budget = 12.0f;   /* Flycast has time to spare: time-lapses relight quickly */
#else
    float budget = game.state == ST_DRIVE ? (game.paused ? 4.0f : 1.0f) : game.state == ST_SELECT ? 4.0f : 1.5f;
#endif
    tod_update(dt, run_clock, budget);
    world_update(dt);
    birds_update(dt);
    air_update(dt);
    desert_update(dt);
    dunes_update(dt);
    campfire_update(dt);
    glow_update(dt);
    if(!game_frozen()) deer_update_all(dt);
    hud_update(dt);
}

/* ---- Rendering ---- */

static void render(void) {
    prof_phase_begin();
    r_frame_begin();
    r_list(PVR_LIST_OP_POLY);
    r_set_camera(&cam);
    bool no_trucks = game.state == ST_TITLE || game.state == ST_SOUND || game.state == ST_ONLINE;
    r_load_vp();
    world_draw_sky();
    lamps_begin(!no_trucks);
    world_draw_op();
    lamps_end();
    birds_draw();
    air_draw_op();
    dunes_draw_op();
    extern uint64_t world_us[3];
    uint64_t tt = timer_us_gettime64();
    if(!no_trucks) truckdraw_draw_op();
    deer_draw_all();
    world_us[2] += timer_us_gettime64() - tt;
    prof_phase_mark(PH_OP);
    RL_WHERE(5);
    r_list(PVR_LIST_PT_POLY);
    r_load_vp();
    world_draw_pt();
    desert_draw_pt();
    prof_phase_mark(PH_PT);
    RL_WHERE(6);
    r_list(PVR_LIST_TR_POLY);
    r_load_vp();
    PROF_BEGIN(ptr);
    world_draw_tr();
    if(!no_trucks) truckdraw_draw_tr();
    glow_draw_tr();
    campfire_draw_tr();
    fx_draw();
    air_draw_tr();
    desert_draw_tr();
    dunes_draw_tr();
    world_draw_flare();
    if(game.state == ST_TITLE) menu_title_draw();
    else if(game.state == ST_SELECT) menu_select_draw();
    else if(game.state == ST_SOUND) soundtest_draw();
    else if(game.state == ST_ONLINE) online_draw();
    else if(game.demo) demo_draw();
#ifndef RL_FILM   /* film builds draw no HUD */
    else {
        if(!game.photo) hud_draw();
        if(game.online) online_draw_hud();
        if(game.paused) menu_pause_draw();
    }
#endif
    if(game.show_fps && !game.photo && game.state == ST_DRIVE) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f FPS  %.1f MS", (double)game.fps, (double)game.cpu_ms);
        hud_text_c(320, 14, 0.5f, ALIGN_CENTER, 0xe0ffffff, buf);
    }
#ifdef RL_AUTOTEST
    autotest_draw();
#endif
    PROF_LAP(PS_TR, ptr);
    RL_WHERE(7);
    r_frame_end();
    RL_WHERE(8);
    prof_phase_mark(PH_TR);
    prof_phase_frame();
}

static void memory_report(void);

/* ---- Memory ---- */

extern uint8_t rl_textures_pak[], rl_textures_pak_end[];
static uintptr_t spent_at;

void *spent_alloc(size_t bytes) {
    if(!spent_at) spent_at = ((uintptr_t)rl_textures_pak + 31) & ~(uintptr_t)31;
    bytes = (bytes + 31) & ~(size_t)31;
    if(spent_at + bytes > (uintptr_t)rl_textures_pak_end) return memalign(32, bytes);
    void *p = (void *)spent_at;
    spent_at += bytes;
    return p;
}

/* ---- Areas ---- */

/* The loaded area's grid (level.h) lives in the room the sounds were
   linked into, spent once the sound chip has them (audio.h). */
static void *level_buf;

/* The ground, lit for the hour; then everything that stands on it. */
static void area_ground(int area) {
    if(!level_buf) {
        uint32_t room;
        level_buf = audio_spent_pak(&room);
        if(room < LEVEL_GRID_BYTES) level_buf = memalign(32, LEVEL_GRID_BYTES);
        if(!level_buf) rl_no_memory("the area's ground");
    }
    game.area = area;
    terrain_load(area, level_buf);
    tod_init(tod.hour);
}

static void area_world(void) {
    world_build();
    outland_init();
    birds_init();
    air_init();
    desert_init();
    dunes_init();
    campfire_init();
    glow_init();
    deer_reset();
    hud_area();
    fx_clear();
}

/* A card over a still frame while the area loads: the world is rebuilt
   in one go, with nothing to draw from meanwhile. */
static void loading_card(const char *name) {
    for(int f = 0; f < 2; f++) {
        r_frame_begin();
        r_list(PVR_LIST_OP_POLY);
        r_list(PVR_LIST_PT_POLY);
        r_list(PVR_LIST_TR_POLY);
        ui_dim();
        ui_panel(170, 196, 470, 284);
        hud_text_c(320, 226, 0.9f, ALIGN_CENTER, 0xffffffff, name);
        hud_text_c(320, 258, 0.5f, ALIGN_CENTER, 0xffd8d0c0, "LOADING");
        r_frame_end();
    }
}

void game_load_area(int area) {
    if(area == game.area) return;
    uint64_t t0 = timer_us_gettime64();
    loading_card(rl_levels[area]->name);
    area_ground(area);
    area_world();
    /* Whatever stood on the old ground stands on the new. */
    if(game.state != ST_DRIVE) game_set_state(game.state);
    camera_reset();
    printf("Ridgeline area: %s in %lu ms\n", lvl->name, (unsigned long)((timer_us_gettime64() - t0) / 1000));
    memory_report();
}

/* The console has 16 MB for the program, its data and the heap. */
static void memory_report(void) {
    extern void *sbrk(intptr_t);
    struct mallinfo mi = mallinfo();
    printf("Ridgeline memory: heap %lu KiB in use, %lu KiB free in it, %lu KiB above the break, %lu KiB of the textures' room\n",
           (unsigned long)mi.uordblks / 1024, (unsigned long)mi.fordblks / 1024,
           (unsigned long)(0x8d000000u - (uintptr_t)sbrk(0)) / 1024,
           (unsigned long)(spent_at ? ((uintptr_t)rl_textures_pak_end - spent_at) / 1024 : 0));
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\nRidgeline %s starting (KallistiOS, %s %s)\n", RL_GIT_SHA, __DATE__, __TIME__);
    uint64_t boot_t0 = timer_us_gettime64();
    input_init();
    render_init();
    /* Sounds first: KOS copies each through a scratch buffer on the way to
       the AICA, and by the end of this list the heap has no room for one.
       (The loaded area's grid then unpacks into the room they came from.) */
    audio_init();
    game.start_tod = TOD_MORNING;
    tod.hour = tod_hours[game.start_tod];
    area_ground(AREA_CASCADE);
    world_init();
    hud_init();
    menu_init();
    truckdraw_init();
    fx_init();
    area_world();
    records_load();
    printf("Ridgeline boot: %lu ms, %lu KiB VRAM free\n", (unsigned long)((timer_us_gettime64() - boot_t0) / 1000),
           (unsigned long)(pvr_mem_available() / 1024));
    memory_report();
    game_set_state(ST_TITLE);
#ifdef RL_AUTOTEST
    autotest_boot();
#endif
#ifdef RL_FILM
    film_start();
#endif
#ifdef ENG_PROF
    prof_start();
#endif
    frame_init();
    while(!game.quit) {
        /* The step follows the display (engine/frame.h). */
        RL_WHERE(1);
        frame_begin();
        RL_WHERE(2);
        prof_phase_begin();
        float dt_real = eng_frame.dt_real;
        int steps = eng_frame.steps;
#ifdef RL_AUTOTEST
        steps = autotest_steps(steps);
#endif
        input_update();
        switch(softreset_poll(game.state == ST_TITLE)) {
            case SOFTRESET_TITLE:
                if(game.online) online_quit();
                else if(game.demo) demo_stop();
                else {
                    audio_drive_stop();
                    game.paused = false;
                    game_set_state(ST_TITLE);
                }
                break;
            case SOFTRESET_QUIT:
                game.quit = true;
                continue;
        }
        if(game.online) online_tick();
        RL_WHERE(3);
        for(int s = 0; s < steps; s++) {
            update(eng_frame.dt);
            pad.pressed = pad2.pressed = 0;
            pad.released = pad2.released = 0;
            input_step_taken();
        }
        ui_time = game.time;
        if(game.state == ST_DRIVE) audio_update(dt_real);
        vmuscreen_update(dt_real);
        prof_phase_mark(PH_UPD);
        RL_WHERE(4);
        render();
        RL_WHERE(10);
#ifdef ENG_PROF
        prof_frame_end();
#endif
        if(frame_end()) game.fps = eng_frame.fps;
        game.cpu_ms = eng_frame.cpu_ms;
#ifdef RL_AUTOTEST
        autotest_frame(dt_real);
#endif
    }
    audio_drive_stop();
    records_finish();
    softreset_leave();
    return 0;
}
