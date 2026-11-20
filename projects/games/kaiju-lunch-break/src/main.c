/* Kaiju Lunch Break: boot, the frame loop, the camera and the render order. */
#include "kl.h"
#include "cel.h"
#include "city.h"
#include "kaiju.h"
#include "life.h"
#include "fx.h"
#include "score.h"
#include "hud.h"
#include "audio.h"
#include "save.h"
#include "autotest.h"
#include "film.h"
#include "softreset.h"
#include "version.h"
#include "engine/frame.h"
#include "engine/prof.h"
#include "engine/ui.h"

#define ENG_RAND_IMPL
#include "engine/rand.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

Game game;
Camera cam;
static float cam_yaw, cam_h = 40.0f, cam_dist = 70.0f;
static uint32_t city_seed = 1;

void game_set_state(int state) {
    game.state = state;
    game.time = 0;
    game.paused = false;
}

#ifdef KL_FILM
void game_world_reset(uint32_t seed);
void game_camera(float dt, float turn);
#define world_reset game_world_reset
#define camera_update game_camera
#else
static void world_reset(uint32_t seed);
static void camera_update(float dt, float turn);
#endif

#ifndef KL_FILM
static
#endif
void world_reset(uint32_t seed) {
    city_build(seed);
    kaiju_reset();
    life_reset();
    score_reset();
    fx_clear();
    cam_yaw = 0;
    cam_h = 40.0f;
    cam_dist = 70.0f;
}

void game_start(void) {
#ifndef KL_AUTOTEST
    city_seed = (uint32_t)timer_us_gettime64() | 1u;
#endif
    world_reset(city_seed++);
    game.clock = LUNCH_SECONDS;
    game.intro = 2.4f;
    game.outro = 0;
    game_set_state(ST_PLAY);
    audio_music_next();
    audio_sfx(KL_SFX_ROAR, 230);
}

static void title_start(void) {
    world_reset(7);
    game_set_state(ST_TITLE);
    save_cards();
}

/* ---- The camera: over the kaiju's shoulder, and over the rooftops ---- */
#ifndef KL_FILM
static
#endif
void camera_update(float dt, float turn) {
    if(game.state != ST_PLAY) {
        /* A slow circle of the scene. */
        V3 at = vadd(kj.pos, v3(0, 26, 0));
        cam.fov = 1.1f;
        cam_look_at(&cam, orbit_point(at, game.time * 0.16f, game.state == ST_TITLE ? 130.0f : 170.0f, 78.0f), at);
        return;
    }
    cam_yaw = wrap_angle(cam_yaw + turn * 2.0f * dt);
    if(kj.speed > 3.0f && turn == 0) cam_yaw = wrap_angle(cam_yaw + wrap_angle(kj.heading - cam_yaw) * smooth_k(1.1f, dt));
    V3 f = heading_fwd(cam_yaw), chest = vadd(kj.pos, v3(0, 24 + kj.y * 0.6f, 0));
    /* High enough to see the kaiju over whatever stands between; among
       towers that would be straight down, so it comes in closer first. */
    static const float dists[3] = {70.0f, 48.0f, 32.0f};
    float need = 40.0f, dist = dists[0];
    for(int d = 0; d < 3; d++) {
        dist = dists[d];
        need = 40.0f - 5.0f * (float)d;
        for(int k = 0; k < 6; k++) {
            float t = 0.08f + 0.13f * (float)k;     /* 0 at the camera, 1 at the kaiju */
            V3 p = vmad(kj.pos, f, -dist * (1 - t));
            int b = city_at(p.x, p.z);
            if(b < 0) continue;
            float h = (city_b[b].h + 7.0f - chest.y * t) / (1 - t);
            if(h > need) need = h;
        }
        if(need <= 80.0f) break;
    }
    if(need > 120.0f) need = 120.0f;
    cam_h += (need - cam_h) * smooth_k(need > cam_h ? 5.0f : 0.9f, dt);
    cam_dist += (dist - cam_dist) * smooth_k(3.0f, dt);
    dist = cam_dist;
    V3 pos = vadd(vmad(kj.pos, f, -dist), v3(0, cam_h, 0));
    float sh = fx_shake_now() * 1.6f;
    if(sh > 0) pos = vadd(pos, v3(frand_s() * sh, frand_s() * sh, frand_s() * sh));
    cam.fov = 1.15f;
    cam_look_at(&cam, pos, vadd(chest, vscale(f, 20.0f)));
}

#ifdef KL_FILM
void game_camera_face(float yaw) {
    cam_yaw = yaw;
}
#endif

/* ---- Updating ---- */
static void sim(const Ctl *c, float dt, float cam_turn) {
    if(game.state == ST_PLAY && fx_frozen(dt)) return;
    kaiju_update(c, dt);
    city_update(dt);
    life_update(dt);
    fx_update(dt);
    score_update(dt);
    camera_update(dt, cam_turn);
}

static void __attribute__((unused)) player_ctl(Ctl *c, float *cam_turn) {
    memset(c, 0, sizeof(*c));
    V3 r = heading_right(cam_yaw), f = heading_fwd(cam_yaw);
    float sx = pad.stick_x, sy = -pad.stick_y;
    c->mx = r.x * sx + f.x * sy;
    c->mz = r.z * sx + f.z * sy;
    c->claw = (pad.pressed & BTN_A) != 0;
    c->jump = (pad.pressed & BTN_B) != 0;
    c->tail = (pad.pressed & BTN_X) != 0;
    c->breath = (pad.held & BTN_Y) != 0;
    c->chomp = (pad.pressed & BTN_R) != 0;
    c->grab = (pad.held & BTN_R) != 0;
    c->dash = (pad.held & BTN_L) != 0;
    c->roar = (pad.pressed & BTN_UP) != 0;
    *cam_turn = (pad.held & BTN_RIGHT) ? 1.0f : (pad.held & BTN_LEFT) ? -1.0f : 0;
}

static void play_update(float dt) {
    Ctl c = {0};
    float cam_turn = 0;
    if(game.paused) {
        if(input_menu_up()) {
            game.pause_sel = (game.pause_sel + PAUSE_ITEMS - 1) % PAUSE_ITEMS;
            audio_sfx(KL_SFX_MOVE, 150);
        }
        if(input_menu_down()) {
            game.pause_sel = (game.pause_sel + 1) % PAUSE_ITEMS;
            audio_sfx(KL_SFX_MOVE, 150);
        }
        bool flip = (game.pause_sel == PAUSE_FPS || game.pause_sel == PAUSE_MUSIC) && (input_menu_left() || input_menu_right());
        if(pad.pressed & BTN_START) game.paused = false;
        else if((pad.pressed & BTN_A) || flip) {
            audio_sfx(KL_SFX_OK, 170);
            switch(game.pause_sel) {
                case PAUSE_RESUME: game.paused = false; break;
                case PAUSE_RESTART: game_start(); break;
                case PAUSE_FPS: game.show_fps = !game.show_fps; break;   /* (and stay in the menu) */
                case PAUSE_MUSIC:
                    save.options ^= SAVE_OPT_NO_MUSIC;
                    save_changed();
                    break;
                default: title_start(); break;
            }
        }
        return;
    }
    if(game.intro > 0) game.intro -= dt;
    else if(game.outro > 0) {
        /* The bell has gone: the last of it falls, then the bill. */
        if((game.outro += dt) > 2.6f) {
            score_bank();
            save_submit(score.total, score.best_mult, score_grade(score.total));
            game_set_state(ST_RESULTS);
        }
    }
    else {
        if(pad.pressed & BTN_START) {
            game.paused = true;
            game.pause_sel = 0;
            audio_sfx(KL_SFX_OK, 150);
            return;
        }
#ifdef KL_AUTOTEST
        bot_ctl(&c, dt);
#else
        player_ctl(&c, &cam_turn);
#endif
        float before = game.clock;
        game.clock -= dt;
        if(game.clock <= 10.0f && (int)before != (int)game.clock) audio_sfx(KL_SFX_BEEP, 170);
        if(game.clock <= 0) {
            game.clock = 0;
            game.outro = 0.001f;
            audio_sfx(KL_SFX_BELL, 255);
        }
    }
    sim(&c, dt, cam_turn);
}

static void update(float dt) {
#ifdef KL_FILM
    film_update(dt);
    return;
#endif
    game.time += dt;
    Ctl c = {0};
    switch(game.state) {
        case ST_TITLE:
            /* The bot has lunch behind the logo. */
            bot_ctl(&c, dt);
            sim(&c, dt, 0);
            if(city_standing() < city_total() * 2 / 3) world_reset(7 + (uint32_t)game.time);
            if(save_hold()) break;   /* the memory cards first */
            if(pad.pressed & (BTN_START | BTN_A)) {
                audio_sfx(KL_SFX_OK, 170);
                game_start();
            }
            break;
        case ST_PLAY:
            play_update(dt);
            break;
        case ST_RESULTS:
            sim(&c, dt, 0);
            if(game.time > 2.6f) {
                if(pad.pressed & (BTN_A | BTN_START)) game_start();
                else if(pad.pressed & BTN_B) title_start();
            }
            break;
    }
}

/* ---- Rendering ---- */
static void render(void) {
    r_frame_begin();
    r_list(PVR_LIST_OP_POLY);
    r_set_camera(&cam);
    r_load_vp();
    city_draw_sky();
    city_draw_op();
    prof_phase_mark(PH_CITY);
    life_draw_op();
    prof_phase_mark(PH_LIFE);
    kaiju_draw();
    prof_phase_mark(PH_KAIJU);
    fx_draw_op();

    r_list(PVR_LIST_PT_POLY);
    r_load_vp();
    city_draw_sky_pt();
    kaiju_draw_pt();
    city_draw_pt();
    life_draw_pt();
    fx_draw_pt();
    prof_phase_mark(PH_FX);

    r_list(PVR_LIST_TR_POLY);
    r_load_vp();
    fx_draw_tr();
    switch(game.state) {
        case ST_TITLE:
            hud_draw_title();
            save_draw();
            break;
        case ST_PLAY:
#ifdef KL_FILM
            if(film_hud())
#endif
            hud_draw();
            if(game.paused) hud_draw_pause();
            break;
        case ST_RESULTS: hud_draw_results(); break;
    }
    r_frame_end();
    prof_phase_mark(PH_TR);
    prof_phase_frame();
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\nKaiju Lunch Break %s starting (KallistiOS, %s %s)\n", KL_GIT_SHA, __DATE__, __TIME__);
    input_init();
    render_init();
    audio_init();
    cel_init();
    hud_init();
    fx_init();
    life_init();
    if(!city_init() || !kaiju_init()) {
        printf("KL: out of memory at boot\n");
        return 1;
    }
    title_start();
    printf("KL boot: %lu KiB VRAM free, %lu KiB of memory never used\n", (unsigned long)(pvr_mem_available() / 1024),
           (unsigned long)((0x8d000000u - (uintptr_t)sbrk(0)) / 1024));
#ifdef KL_AUTOTEST
    autotest_boot();
#endif
#ifdef KL_FILM
    film_start();
#endif
    frame_init();
    while(!game.quit) {
        frame_begin();
        float dt_real = eng_frame.dt_real;
        input_update();
        switch(softreset_poll(game.state == ST_TITLE)) {
            case SOFTRESET_TITLE: title_start(); break;
            case SOFTRESET_QUIT: game.quit = true; continue;
        }
        prof_phase_begin();
        for(int s = 0; s < eng_frame.steps; s++) {
            update(eng_frame.dt);
            pad.pressed = pad2.pressed = 0;
            pad.released = pad2.released = 0;
            input_step_taken();
        }
        ui_time = game.time;
        audio_update(dt_real);
        prof_phase_mark(PH_UPD);
        game.frame++;
        render();
        if(frame_end()) game.fps = eng_frame.fps;
        game.cpu_ms = eng_frame.cpu_ms;
#ifdef KL_AUTOTEST
        autotest_frame(dt_real);
#endif
    }
    audio_shutdown();
    softreset_leave();
    return 0;
}
