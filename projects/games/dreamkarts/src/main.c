/* DreamKarts: a kart racer for the Sega Dreamcast (KallistiOS).

   Main loop, race flow, cameras and the player's controls. Physics run at a
   fixed 60 Hz; rendering is vsynced through the PVR. */
#include "dk.h"
#include "audio.h"
#include "fx.h"
#include "hud.h"
#include "kart.h"
#include "models.h"
#include "render.h"
#include "title.h"
#include "track.h"
#include "world.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

Game game;

typedef struct {
    uint32_t paint1, chao, tip;
    float skill, lane;
} Rival;

static const Rival rivals[MAX_KARTS - 1] = {
    {0xff2c6ee6, 0xffffa8d0, 0xffff5a96, 1.000f, -1.4f},   /* BUBBLES */
    {0xffff8c14, 0xffffd25a, 0xffff8c28, 0.990f, 1.6f},    /* MANGO */
    {0xff9646dc, 0xffff8c78, 0xffffdc78, 0.980f, -0.4f},   /* CORAL */
    {0xff28b450, 0xffe6ebf5, 0xff96c8ff, 0.970f, 0.8f},    /* PEBBLE */
    {0xff32323c, 0xff6e5a96, 0xffe63c50, 0.960f, -2.0f},   /* SUNNY */
};

static float accel_press_t = -1.0f;
static float finish_wait;
static bool countdown_beeped[4];

#ifdef DK_CAPTURE
/* Scripted presses so a capture run walks title -> menu -> race on its own;
   "DK SHOT <name>" lines tell the capture tool when to grab the window. */
static void capture_script(void) {
    static float t;
    static int step;
    static bool shot[8];
    t += DK_DT;
    const Kart *p = &karts[game.player];
    if(game.state == ST_TITLE && game.time > 3.5f && !shot[0]) { shot[0] = true; printf("DK SHOT title\n"); }
    if(game.state == ST_TITLE && game.time > 7.0f) { pad.pressed |= BTN_START; step = 0; }
    if(game.state == ST_MENU) {
        if(game.time > 2.0f && !shot[1]) { shot[1] = true; printf("DK SHOT menu\n"); }
        if(game.time > 3.5f + step * 0.5f && step < 3) { pad.pressed |= BTN_A; step++; }
    }
    if(game.state == ST_INTRO && game.time > 1.6f && !shot[2]) { shot[2] = true; printf("DK SHOT intro\n"); }
    if(game.state == ST_COUNTDOWN && game.countdown > 1.2f && !shot[7]) { shot[7] = true; printf("DK SHOT countdown\n"); }
    if(game.state == ST_RACE && p->drift == DRIFT_ACTIVE && p->stage >= 3 && !shot[3]) { shot[3] = true; printf("DK SHOT drift\n"); }
    if(game.state == ST_RACE && p->boost_t > 0.3f && game.race_time > 8.0f && !shot[4]) { shot[4] = true; printf("DK SHOT boost\n"); }
    if(game.state == ST_RACE && track_section(p->tp.s) == SEC_BOARDWALK && !shot[5] && game.race_time > 12.0f) { shot[5] = true; printf("DK SHOT boardwalk\n"); }
    if(game.state == ST_RESULTS && game.time > 1.5f && !shot[6]) { shot[6] = true; printf("DK SHOT results\n"); }
}
#endif

#ifdef DK_AUTOTEST
static struct {
    int drifts, boosts[4], walls, dash, laps;
    float min_fps, cpu_sum, cpu_max;
    int cpu_n;
    float log_t;
    bool done;
} at = {.min_fps = 999};
#endif

static void update_places(void);

/* ------------------------------------------------------------------ */

void race_setup(void) {
    kart_count = game.mode == MODE_GP ? MAX_KARTS : 1;
    game.player = game.mode == MODE_GP ? MAX_KARTS - 1 : 0;
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        memset(k, 0, sizeof(*k));
        k->cpu = i != game.player;
        if(k->cpu) {
            const Rival *r = &rivals[i];
            k->paint1 = r->paint1;
            k->chao = r->chao;
            k->chao_tip = r->tip;
            k->skill = r->skill;
            k->lane = r->lane;
        }
        kart_place_on_grid(k, game.mode == MODE_GP ? i : 0);
        models_paint_kart(i);
    }
    game.race_time = 0;
    game.countdown = 0;
    game.finished_count = 0;
    game.final_lap_announced = false;
    game.pause_sel = 0;
    game.paused = false;
    accel_press_t = -1.0f;
    finish_wait = 0;
    memset(countdown_beeped, 0, sizeof(countdown_beeped));
    fx_clear();
    hud_reset();
    update_places();
    game.cam_yaw = karts[game.player].heading;
    game.cam_height = 2.3f;
}

void game_set_state(int state) {
    game.state = state;
    game.time = 0;
    if(state == ST_TITLE || state == ST_MENU) {
        if(state == ST_TITLE) title_enter();
        audio_kart_loops(NULL, DK_DT);
        audio_music_rate(1.0f);
        audio_music_target(210);
    }
    if(state == ST_INTRO) {
        audio_music_start();
        audio_music_target(150);
    }
}

/* ------------------------------------------------------------------ */
/* Cameras                                                             */

static void camera_basis(Camera *c, V3 pos, V3 look) {
    c->pos = pos;
    c->fwd = vnorm(vsub(look, pos));
    c->right = vnorm(vcross(v3(0, 1, 0), c->fwd));
    c->up = vcross(c->fwd, c->right);
}

static void chase_camera(Camera *cam, const Kart *k, float dt) {
    float top = kart_top_speed(k);
    float n = clampf(fabsf(k->speed) / top, 0, 1.4f);
    float target_yaw = k->heading + k->vis_yaw * 0.3f;
    game.cam_yaw += wrap_angle(target_yaw - game.cam_yaw) * smooth_k(5.0f, dt);
    game.cam_yaw = wrap_angle(game.cam_yaw);
    bool boost = k->boost_t > 0;
    float dist_t = 5.3f + n * 0.8f + (boost ? 0.7f : 0.0f);
    game.cam_dist += (dist_t - game.cam_dist) * smooth_k(4.0f, dt);
    float h_t = 2.25f + k->pos.y * 0.5f;
    game.cam_height += (h_t - game.cam_height) * smooth_k(6.0f, dt);
    V3 back = heading_fwd(game.cam_yaw);
    V3 pos = vadd(vmad(k->pos, back, -game.cam_dist), v3(0, game.cam_height, 0));
    pos.y = fmaxf(pos.y, 0.8f);
    V3 look = vadd(vmad(k->pos, back, 3.0f), v3(0, 0.95f + k->pos.y * 0.3f, 0));
    if(game.cam.shake > 0) {
        float s = game.cam.shake;
        pos = vadd(pos, v3(frand_s() * s, frand_s() * s, frand_s() * s));
        game.cam.shake = fmaxf(0.0f, s - dt * 1.2f);
    }
    float shake = game.cam.shake;
    camera_basis(cam, pos, look);
    cam->shake = shake;
    float fov_t = 1.30f + n * 0.06f + (boost ? 0.14f : 0.0f);
    static float fov = 1.30f;
    fov += (fov_t - fov) * smooth_k(5.0f, dt);
    cam->fov = fov;
}

static void intro_camera(Camera *cam, float t) {
    const Kart *k = &karts[game.player];
    float e = clampf(t, 0, 1);
    e = e * e * (3 - 2 * e);
    V3 back = heading_fwd(k->heading);
    V3 start = vadd(track_point(k->tp.s + 60.0f, 14.0f), v3(0, 28.0f, 0));
    V3 end = vadd(vmad(k->pos, back, -5.3f), v3(0, 2.25f, 0));
    V3 look0 = vadd(k->pos, v3(0, 0, 0));
    V3 look1 = vadd(vmad(k->pos, back, 3.0f), v3(0, 0.95f, 0));
    /* Curve the path out over the sea side for a sweeping fly-in. */
    V3 mid = vadd(vlerp(start, end, e), vscale(v3(0, 1, 0), sinf(e * DK_PI) * 6.0f));
    camera_basis(cam, mid, vlerp(look0, look1, e));
    cam->fov = lerpf(1.1f, 1.30f, e);
    cam->shake = 0;
    game.cam_yaw = k->heading;
    game.cam_dist = 5.3f;
    game.cam_height = 2.25f;
}

static void results_camera(Camera *cam, float dt) {
    const Kart *k = &karts[game.player];
    float a = game.time * 0.3f + k->heading + DK_PI * 0.75f;
    V3 target = vadd(k->pos, v3(0, 0.9f, 0));
    camera_basis(cam, vadd(target, v3(sinf(a) * 6.5f, 2.2f, cosf(a) * 6.5f)), target);
    cam->fov = 1.2f;
    (void)dt;
}

/* ------------------------------------------------------------------ */
/* Race                                                                */

static void player_input(KartInput *in) {
    memset(in, 0, sizeof(*in));
    float steer = pad.stick_x;
    if(pad.held & BTN_LEFT) steer = -1;
    if(pad.held & BTN_RIGHT) steer = 1;
    in->steer = steer;
    in->accel = (pad.held & BTN_A) != 0;
    in->brake = (pad.held & BTN_B) != 0;
    in->drift = (pad.held & (BTN_R | BTN_L | BTN_X)) != 0;
}

static void update_places(void) {
    int order[MAX_KARTS];
    for(int i = 0; i < kart_count; i++) order[i] = i;
    for(int i = 1; i < kart_count; i++) {
        int v = order[i], j = i - 1;
        while(j >= 0) {
            const Kart *a = &karts[order[j]], *b = &karts[v];
            bool b_ahead;
            if(a->finished && b->finished) b_ahead = b->finish_time < a->finish_time;
            else if(a->finished != b->finished) b_ahead = b->finished;
            else b_ahead = b->progress > a->progress;
            if(!b_ahead) break;
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
    for(int i = 0; i < kart_count; i++) {
        karts[order[i]].place = i + 1;
        game.finish_order[i] = order[i];
    }
}

static void finalize_results(void) {
    /* Unfinished rivals get a projected time from their remaining distance. */
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        if(k->finished) continue;
        float remaining = (float)(RACE_LAPS + 1) * track.length - k->progress;
        k->finish_time = game.race_time + fmaxf(remaining, 0) / (kart_top_speed(k) * 0.92f);
        k->finished = true;
    }
    update_places();
}

static void player_events(Kart *p) {
    uint32_t ev = p->events;
    if(ev & EV_LAP) {
        if(p->lap == RACE_LAPS && !game.final_lap_announced) {
            game.final_lap_announced = true;
            hud_message("FINAL LAP!", argb(255, 255, 210, 50), 2.4f);
            audio_sfx(DK_SFX_FINAL_LAP, 230, 128);
            audio_music_rate(1.06f);
        }
        else {
            audio_sfx(DK_SFX_LAP, 220, 128);
        }
        float t = p->lap_times[p->lap - 2];
        hud_lap_popup(p->lap - 1, t, t <= p->best_lap + 0.001f && p->lap > 2);
    }
    if(ev & EV_FINISH) {
        audio_sfx(DK_SFX_FINISH, 240, 128);
        if(game.mode == MODE_TT || p->place <= 3) audio_sfx(DK_SFX_CHAO_HAPPY, 210, 128);
        audio_music_target(90);
        fx_confetti(vadd(p->pos, v3(0, 1.5f, 0)), 90);
        finish_wait = 0;
    }
    if(ev & (EV_WALL)) game.cam.shake = fmaxf(game.cam.shake, 0.12f);
    if(ev & (EV_BOOST | EV_DASH | EV_ROCKET)) game.cam.shake = fmaxf(game.cam.shake, 0.04f);
}

static void race_step(float dt, bool drive) {
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        KartInput in;
        bool ai = k->cpu || k->finished;
#ifdef DK_AUTOTEST
        ai = true;
#endif
        if(ai) ai_drive(k, &in, dt);
        else player_input(&in);
        kart_update(k, &in, dt, drive);
    }
    karts_collide();
    update_places();
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        if(k->events & EV_FINISH) game.finished_count++;
        fx_kart(k, dt);
        audio_kart_events(k, i == game.player);
    }
    player_events(&karts[game.player]);
#ifdef DK_AUTOTEST
    {
        uint32_t ev = karts[game.player].events;
        if(ev & EV_DRIFT_START) at.drifts++;
        if(ev & EV_BOOST) at.boosts[karts[game.player].boost_stage & 3]++;
        if(ev & EV_WALL) at.walls++;
        if(ev & EV_DASH) at.dash++;
        if(ev & EV_LAP) at.laps++;
    }
#endif
}

static void countdown_step(float dt) {
    float before = game.countdown;
    game.countdown += dt;
    for(int i = 0; i < 4; i++) {
        if(!countdown_beeped[i] && game.countdown >= (float)i) {
            countdown_beeped[i] = true;
            audio_sfx(i < 3 ? DK_SFX_COUNT_BEEP : DK_SFX_GO, 230, 128);
        }
    }
    /* Rocket start: press accelerate just as "2" fades, hold it through GO. */
    if(pad.pressed & BTN_A) accel_press_t = before;
    if(!(pad.held & BTN_A)) accel_press_t = -1.0f;
#ifdef DK_AUTOTEST
    accel_press_t = 1.9f;
#endif
    if(game.countdown >= 3.0f) {
        Kart *p = &karts[game.player];
        if(accel_press_t >= 1.55f && accel_press_t <= 2.4f) {
            kart_start_boost(p, 1.0f, 1.12f);
            p->events |= EV_ROCKET;
            audio_sfx(DK_SFX_BOOST, 230, 128);
        }
        else if(accel_press_t >= 0 && accel_press_t < 1.3f) {
            p->burnout_t = 0.85f;
            audio_sfx(DK_SFX_BUMP, 150, 128);
        }
        for(int i = 0; i < kart_count; i++) {
            if(!karts[i].cpu) continue;
            if((dk_rand() % 100) < 45) kart_start_boost(&karts[i], 0.8f + frand() * 0.4f, 1.08f);
        }
        game.state = ST_RACE;
        game.time = 0;
    }
}

static void pause_update(void) {
    if(input_menu_up()) {
        game.pause_sel = (game.pause_sel + 2) % 3;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_menu_down()) {
        game.pause_sel = (game.pause_sel + 1) % 3;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(pad.pressed & (BTN_START | BTN_B)) {
        game.paused = false;
        audio_sfx(DK_SFX_MENU_BACK, 180, 128);
        audio_music_target(150);
        return;
    }
    if(pad.pressed & BTN_A) {
        audio_sfx(DK_SFX_MENU_SELECT, 200, 128);
        game.paused = false;
        if(game.pause_sel == 1) {
            race_setup();
            game_set_state(ST_INTRO);
        }
        else if(game.pause_sel == 2) {
            race_setup();
            game_set_state(ST_TITLE);
        }
        else {
            audio_music_target(150);
        }
    }
}

static void update(float dt) {
    game.time += dt;
    game.title_t += dt;
    world_update(dt);
    hud_update(dt);
    switch(game.state) {
        case ST_TITLE:
        case ST_MENU:
            title_update(dt);
            /* Idle karts on the grid still bounce a little. */
            for(int i = 0; i < kart_count; i++) {
                KartInput in = {0};
                kart_update(&karts[i], &in, dt, false);
            }
            break;
        case ST_INTRO:
            for(int i = 0; i < kart_count; i++) {
                KartInput in = {0};
                kart_update(&karts[i], &in, dt, false);
            }
            if(game.time >= 3.2f || (game.time > 0.5f && (pad.pressed & BTN_A))) {
                game.state = ST_COUNTDOWN;
                game.time = 0;
                game.countdown = 0;
            }
            break;
        case ST_COUNTDOWN:
            if(pad.pressed & BTN_START) {
                game.paused = !game.paused;
                if(game.paused) audio_music_target(60);
            }
            else if(game.paused) pause_update();
            if(game.paused) break;
            countdown_step(dt);
            for(int i = 0; i < kart_count; i++) {
                KartInput in = {0};
                kart_update(&karts[i], &in, dt, false);
            }
            break;
        case ST_RACE: {
            if(!game.paused && (pad.pressed & BTN_START)) {
                game.paused = true;
                game.pause_sel = 0;
                audio_sfx(DK_SFX_MENU_SELECT, 180, 128);
                audio_music_target(60);
                pad.pressed = 0;
            }
            if(game.paused) {
                pause_update();
                break;
            }
            game.countdown += dt;
            game.race_time += dt;
            race_step(dt, true);
            Kart *p = &karts[game.player];
            if(p->finished) {
                finish_wait += dt;
                if(finish_wait > 4.5f) {
                    finalize_results();
                    game_set_state(ST_RESULTS);
                }
            }
            break;
        }
        case ST_RESULTS:
            game.race_time += dt;
            race_step(dt, true);
            if(game.time > 1.0f && (pad.pressed & BTN_A)) {
                audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
                race_setup();
                game_set_state(ST_INTRO);
            }
            else if(game.time > 1.0f && (pad.pressed & BTN_B)) {
                audio_sfx(DK_SFX_MENU_BACK, 200, 128);
                race_setup();
                game_set_state(ST_TITLE);
            }
            break;
    }
    fx_update(dt);
}

static void update_camera(float dt) {
    switch(game.state) {
        case ST_TITLE:
        case ST_MENU:
            title_camera(&game.cam, dt);
            break;
        case ST_INTRO:
            intro_camera(&game.cam, game.time / 3.2f);
            break;
        case ST_RESULTS:
            results_camera(&game.cam, dt);
            break;
        default:
            chase_camera(&game.cam, &karts[game.player], game.paused ? 0 : dt);
            break;
    }
}

/* Accumulated microseconds per phase: update, world OP, karts, PT, TR. */
static uint64_t prof_us[6];
static int prof_frames;
#define PROF_MARK(i) do { uint64_t _n = timer_us_gettime64(); prof_us[i] += _n - prof_t; prof_t = _n; } while(0)

static void render(void) {
    uint64_t prof_t = timer_us_gettime64();
    r_set_camera(&game.cam);
    r_frame_begin();

    r_list(PVR_LIST_OP_POLY);
    world_draw_op();
    PROF_MARK(1);
    models_draw_karts();
    PROF_MARK(2);

    r_list(PVR_LIST_PT_POLY);
    world_draw_pt();
    PROF_MARK(3);

    r_list(PVR_LIST_TR_POLY);
    world_draw_tr();
    models_draw_shadows();
    fx_draw();
    switch(game.state) {
        case ST_TITLE:
        case ST_MENU:
            title_draw();
            break;
        case ST_INTRO: {
            float a = clampf(game.time * 2.0f, 0, 1) * clampf((3.2f - game.time) * 2.0f, 0, 1);
            static const char *const cc_names[] = {"50cc", "100cc", "150cc"};
            hud_panel(0, 330, 640, 410, argb((int)(170 * a), 20, 70, 180), argb((int)(170 * a), 10, 30, 90), 15);
            hud_text(320 + (1 - a) * 200, 334, 1.2f, ALIGN_CENTER, color_alpha(argb(255, 255, 220, 60), a), "EMERALD BEACH");
            char buf[48];
            snprintf(buf, sizeof(buf), "%s  -  %s", game.mode == MODE_GP ? "GRAND PRIX" : "TIME TRIAL", cc_names[game.cc]);
            hud_text(320 - (1 - a) * 200, 378, 0.65f, ALIGN_CENTER, color_alpha(0xffffffff, a), buf);
            break;
        }
        case ST_COUNTDOWN:
        case ST_RACE:
            hud_draw_race();
            hud_draw_countdown();
            if(karts[game.player].finished) {
                float t = finish_wait;
                float s = t < 0.3f ? 0.4f + t * 2.5f : 1.15f + 0.03f * sinf(t * 6);
                hud_bigtext(4, 320, 190, s, 0xffffffff);
            }
            if(game.paused) hud_draw_pause();
            break;
        case ST_RESULTS:
            hud_draw_results();
            break;
    }
    r_frame_end();
    PROF_MARK(4);
    prof_frames++;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\nDreamKarts starting (KallistiOS, %s %s)\n", __DATE__, __TIME__);
    input_init();
    render_init();
    track_init();
    world_init();
    models_init();
    fx_init();
    hud_init();
    title_init();
    audio_init();

    game.cc = CC_100;
    game.mode = MODE_GP;
    game.cam_dist = 5.3f;
    race_setup();
#if defined(DK_AUTOTEST) && !defined(DK_CAPTURE)
    game.cc = CC_150;
#ifdef DK_AT_TT
    game.mode = MODE_TT;
#endif
    race_setup();
    game_set_state(ST_INTRO);
#else
    game_set_state(ST_TITLE);
    audio_music_start();
    audio_music_target(210);
#endif

    uint64_t last = timer_us_gettime64();
    uint64_t fps_t0 = last;
    int fps_frames = 0;
    float acc = 0;
    while(!game.quit) {
        pvr_wait_ready();
        uint64_t now = timer_us_gettime64();
        float dt_real = (float)(now - last) * 1e-6f;
        last = now;
        if(dt_real > 0.1f) dt_real = 0.1f;
        acc += dt_real;
        int steps = (int)(acc / DK_DT + 0.25f);
        if(steps > 4) {
            steps = 4;
            acc = 0;
        }
        else {
            acc -= steps * DK_DT;
        }

        uint64_t upd_t0 = timer_us_gettime64();
        input_update();
        for(int s = 0; s < steps; s++) {
#ifdef DK_CAPTURE
            capture_script();
#endif
            update(DK_DT);
            pad.pressed = 0;
            pad.released = 0;
        }
        update_camera(dt_real);
        audio_update(dt_real);
        if(game.state == ST_RACE || game.state == ST_COUNTDOWN || game.state == ST_RESULTS)
            audio_kart_loops(game.paused ? NULL : &karts[game.player], dt_real);
        else
            audio_kart_loops(NULL, dt_real);
        prof_us[0] += timer_us_gettime64() - upd_t0;
        render();

        uint64_t end = timer_us_gettime64();
        game.cpu_ms = (float)(end - now) * 0.001f;
        fps_frames++;
        if(end - fps_t0 >= 1000000) {
            game.fps = fps_frames * 1e6f / (float)(end - fps_t0);
            fps_frames = 0;
            fps_t0 = end;
#ifndef DK_AUTOTEST
            if(game.state == ST_RACE)
                printf("DK PERF fps=%.1f cpu=%.2fms tris=%d verts=%d\n", (double)game.fps, (double)game.cpu_ms,
                       rstats.tris, rstats.verts);
#endif
        }
#ifdef DK_AUTOTEST
        if(game.state == ST_RACE || game.state == ST_RESULTS) {
            const Kart *p = &karts[game.player];
            at.cpu_sum += game.cpu_ms;
            at.cpu_n++;
            if(game.cpu_ms > at.cpu_max) at.cpu_max = game.cpu_ms;
            if(game.fps > 0 && game.fps < at.min_fps && game.race_time > 2.0f) at.min_fps = game.fps;
            at.log_t += dt_real;
            if(at.log_t >= 2.0f) {
                at.log_t = 0;
                float f = prof_frames ? 1.0f / (prof_frames * 1000.0f) : 0;
                printf("DK AT t=%.1f fps=%.1f cpu=%.2fms [upd %.2f world %.2f karts %.2f pt %.2f tr %.2f] tris=%d "
                       "lap=%d place=%d spd=%.1f s=%.0f d=%.1f drift=%d stage=%d\n",
                       (double)game.race_time, (double)game.fps, (double)game.cpu_ms, (double)(prof_us[0] * f),
                       (double)(prof_us[1] * f), (double)(prof_us[2] * f), (double)(prof_us[3] * f),
                       (double)(prof_us[4] * f), rstats.tris, p->lap, p->place,
                       (double)p->speed, (double)p->tp.s, (double)p->tp.d, p->drift, p->stage);
                memset(prof_us, 0, sizeof(prof_us));
                prof_frames = 0;
            }
            if(game.state == ST_RESULTS && game.time > 3.0f && !at.done) {
                at.done = true;
                printf("DK AUTOTEST: player finish %.2fs place %d laps", (double)p->finish_time, p->place);
                for(int l = 0; l < RACE_LAPS; l++) printf(" %.2f", (double)p->lap_times[l]);
                printf("\nDK AUTOTEST: drifts %d boosts blue %d orange %d purple %d dash %d walls %d\n",
                       at.drifts, at.boosts[1], at.boosts[2], at.boosts[3], at.dash, at.walls);
                printf("DK AUTOTEST: fps min %.1f, cpu avg %.2fms max %.2fms\n", (double)at.min_fps,
                       (double)(at.cpu_sum / (at.cpu_n ? at.cpu_n : 1)), (double)at.cpu_max);
                for(int i = 0; i < kart_count; i++)
                    printf("DK AUTOTEST: %d. %s %.2f\n", i + 1, game.finish_order[i] == game.player ? "YOU" : kart_names[game.finish_order[i]],
                           (double)karts[game.finish_order[i]].finish_time);
                bool ok = p->finish_time > 0 && p->finish_time < 150.0f && at.drifts > 3;
                printf("DK AUTOTEST: %s\n", ok ? "PASS" : "FAIL");
            }
        }
#endif
    }
    audio_shutdown();
    render_shutdown();
    return 0;
}
