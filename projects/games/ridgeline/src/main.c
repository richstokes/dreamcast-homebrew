/* Ridgeline: boot, the frame loop, driving and the render order. */
#include "rl.h"
#include "world.h"
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

KOS_INIT_FLAGS(INIT_DEFAULT);

Game game;
/* The frame's phases for the autotest's log line (engine/prof.h). */
enum { PH_UPD, PH_OP, PH_PT, PH_TR };

static float recover_hold;

void game_set_state(int state) {
    if(game.state == ST_SOUND && state != ST_SOUND) soundtest_stop();
    game.state = state;
    game.time = 0;
    if(state == ST_SELECT || state == ST_TITLE || state == ST_SOUND) {
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
        t->model = i == 0 ? game.truck_model : (game.truck_model + i) & 1;
        t->livery = i == 0 ? game.livery : (game.livery + i) % 4;
        t->cpu = i > 0;
        t->tcs_off = false;   /* traction control starts on */
        const float *s = lvl_spawns[slot[i]];
        truck_place(t, v3(s[0], 0, s[2]), s[3]);
        t->lights = false;
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
    hud_message("CASCADE RIDGE", 0xfff8f0dc, 3.0f);
}

/* ---- Driving ---- */

static void __attribute__((unused)) player_controls(Truck *p, float dt) {
    TruckInput *in = &p->in;
    in->steer = pad.stick_x;
    /* A progressive pedal, as trucks map theirs: the first half of the
       trigger's travel asks for about a third of the power, so a 2WD slide
       can be fed and held rather than switched on. */
    in->throttle = pad.rtrig * sqrtf(pad.rtrig);
    in->brake = pad.ltrig;
    in->handbrake = (pad.held & BTN_A) != 0;
    /* D-pad up is the horn; held 2 s on a truck with traction control it
       switches that off or on instead, and the horn stops. */
    static float up_hold;
    static bool up_done;
    bool up = (pad.held & BTN_UP) != 0;
    if(!up) {
        up_hold = 0;
        up_done = false;
    }
    else if(p->spec.tcs && !up_done && (up_hold += dt) >= 2.0f) {
        p->tcs_off = !p->tcs_off;
        up_done = true;
        hud_message(p->tcs_off ? "TRACTION CONTROL OFF" : "TRACTION CONTROL ON", 0xffffd070, 1.6f);
    }
    p->horn = up && !up_done;
    if(pad.pressed & BTN_DOWN) {
        p->lights = !p->lights;
        hud_message(p->lights ? "LIGHTS ON" : "LIGHTS OFF", 0xfff8f0dc, 1.2f);
    }
    if(pad.pressed & (BTN_LEFT | BTN_RIGHT)) {
        int m = p->mode + ((pad.pressed & BTN_RIGHT) ? 1 : -1);
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
    else if(p->spec.tcs && up_hold > 0.5f && !up_done) {
        hud_hold_k = up_hold / 2.0f;
        hud_hold_label = p->tcs_off ? "HOLD UP: TCS ON" : "HOLD UP: TCS OFF";
    }
}

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
        hud_message("SUMMIT!  CASCADE RIDGE", 0xffffe070, 7.0f);
        char line[64];
        records_summit(line, sizeof(line));
        hud_message_sub(line);
        audio_sfx(RL_SFX_SUMMIT, 240);
    }
}

static void drive_update(float dt) {
    Truck *p = &trucks[0];
    if(game.demo) {
        demo_update(dt);
        return;
    }
    if(game.paused) {
        menu_pause_update(dt);
        return;
    }
    if(pad.pressed & BTN_START) {
        game.paused = true;
        game.pause_sel = 0;
        game.view_sel = game.last_view;
        audio_sfx(RL_SFX_MENU_SELECT, 160);
        return;
    }
#ifdef RL_AUTOTEST
    if(!autotest_controls(p, dt)) ai_drive(0, dt);
#else
    player_controls(p, dt);
#endif
    for(int i = 1; i < game.ntrucks; i++) ai_drive(i, dt);
    for(int i = 0; i < game.ntrucks; i++) truck_update(&trucks[i], dt);
    progress_update();
    records_update(dt);
    game.clock += dt;
    fx_update(dt);
    camera_follow(p, dt);
}

static void update(float dt) {
    game.time += dt;
    switch(game.state) {
        case ST_TITLE: menu_title_update(dt); break;
        case ST_SELECT: menu_select_update(dt); break;
        case ST_DRIVE: drive_update(dt); break;
        case ST_SOUND: soundtest_update(dt); break;
    }
    world_update(dt);
    birds_update(dt);
    if(!game.paused) deer_update_all(dt);
    hud_update(dt);
}

/* ---- Rendering ---- */

static void render(void) {
    prof_phase_begin();
    r_frame_begin();
    r_list(PVR_LIST_OP_POLY);
    r_set_camera(&cam);
    r_load_vp();
    world_draw_sky();
    world_draw_op();
    birds_draw();
    extern uint64_t world_us[3];
    uint64_t tt = timer_us_gettime64();
    bool no_trucks = game.state == ST_TITLE || game.state == ST_SOUND;
    if(!no_trucks) truckdraw_draw_op();
    deer_draw_all();
    world_us[2] += timer_us_gettime64() - tt;
    prof_phase_mark(PH_OP);
    r_list(PVR_LIST_PT_POLY);
    r_load_vp();
    world_draw_pt();
    prof_phase_mark(PH_PT);
    r_list(PVR_LIST_TR_POLY);
    r_load_vp();
    PROF_BEGIN(ptr);
    world_draw_tr();
    if(!no_trucks) truckdraw_draw_tr();
    fx_draw();
    world_draw_flare();
    if(game.state == ST_TITLE) menu_title_draw();
    else if(game.state == ST_SELECT) menu_select_draw();
    else if(game.state == ST_SOUND) soundtest_draw();
    else if(game.demo) demo_draw();
    else {
        if(!game.photo) hud_draw();
        if(game.paused) menu_pause_draw();
    }
    if(game.show_fps && !game.photo && game.state == ST_DRIVE) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f FPS  %.1f MS", (double)game.fps, (double)game.cpu_ms);
        hud_text_c(320, 14, 0.5f, ALIGN_CENTER, 0xe0ffffff, buf);
    }
#ifdef RL_AUTOTEST
    autotest_draw();
#endif
    PROF_LAP(PS_TR, ptr);
    r_frame_end();
    prof_phase_mark(PH_TR);
    prof_phase_frame();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\nRidgeline %s starting (KallistiOS, %s %s)\n", RL_GIT_SHA, __DATE__, __TIME__);
    uint64_t boot_t0 = timer_us_gettime64();
    input_init();
    render_init();
    terrain_init();
    world_init();
    world_build();
    hud_init();
    menu_init();
    truckdraw_init();
    fx_init();
    birds_init();
    audio_init();
    records_load();
    printf("Ridgeline boot: %lu ms, %lu KiB VRAM free\n", (unsigned long)((timer_us_gettime64() - boot_t0) / 1000),
           (unsigned long)(pvr_mem_available() / 1024));
    game_set_state(ST_TITLE);
#ifdef RL_AUTOTEST
    autotest_boot();
#endif
#ifdef ENG_PROF
    prof_start();
#endif
    frame_init();
    while(!game.quit) {
        /* The step follows the display (engine/frame.h). */
        frame_begin();
        prof_phase_begin();
        float dt_real = eng_frame.dt_real;
        int steps = eng_frame.steps;
#ifdef RL_AUTOTEST
        steps = autotest_steps(steps);
#endif
        input_update();
        switch(softreset_poll(game.state == ST_TITLE)) {
            case SOFTRESET_TITLE:
                if(game.demo) demo_stop();
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
        for(int s = 0; s < steps; s++) {
            update(eng_frame.dt);
            pad.pressed = pad2.pressed = 0;
            pad.released = pad2.released = 0;
            input_step_taken();
        }
        ui_time = game.time;
        if(game.state == ST_DRIVE) audio_update(dt_real);
        prof_phase_mark(PH_UPD);
        render();
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
    softreset_leave();
    return 0;
}
