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
#include "rumble.h"
#include "title.h"
#include "track.h"
#include "world.h"
#include "dolphins.h"
#include "online.h"
#include <stddef.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

Game game;
uint32_t dk_probe_off;

/* CPU driver personalities, in grid order: top-speed factor, lane bias. */
static const struct { float skill, lane; } rival_ai[MAX_KARTS] = {
    {1.000f, -1.4f}, {0.990f, 1.6f}, {0.980f, -0.4f}, {0.970f, 0.8f}, {0.960f, -2.0f}, {0.955f, 1.1f}
};

static float finish_wait;
static bool countdown_beeped[4];

static bool split_screen(void) {
    return game.num_players > 1 && (game.state == ST_COUNTDOWN || game.state == ST_RACE);
}

#ifdef DK_AUTOTEST
#ifdef DK_HW
#define AT_LOG_SECS 10.0f   /* every printf under dcload-ip stalls the console */
#else
#define AT_LOG_SECS 2.0f
#endif
static int at_skip;         /* frames to leave out of timing after slow I/O */
#endif

#ifdef DK_CAPTURE
/* Flycast: print a marker for tools/capture.py to grab the window.
   Hardware: write the displayed framebuffer to dc-tool-ip's /pc/ share. */
static void take_shot(const char *name) {
#ifdef DK_HW
    char path[48];
    snprintf(path, sizeof(path), "/pc/dk-%s.ppm", name);
    vid_screen_shot(path);
    at_skip = 3;
#else
    printf("DK SHOT %s\n", name);
#endif
}

/* Scripted presses so a capture run walks title -> menu -> race on its own;
   "DK SHOT <name>" lines tell the capture tool when to grab the window. */
static void capture_script(void) {
    static float t;
    static int step;
    static bool shot[20];
    t += DK_DT;
    const Kart *p = &karts[game.player];
    if(game.state == ST_TITLE && game.time > 3.5f && !shot[0]) { shot[0] = true; take_shot("title"); }
    /* Hidden trigger track cycle: R to the next track, grab it, L back. */
    static int trig_step;
    if(game.state == ST_TITLE && game.time > 4.2f && trig_step == 0) { trig_step = 1; pad.pressed |= BTN_R; }
    if(game.state == ST_TITLE && game.time > 5.2f && trig_step == 1) { trig_step = 2; take_shot("title-next"); }
    if(game.state == ST_TITLE && game.time > 6.0f && trig_step == 2) { trig_step = 3; pad.pressed |= BTN_L; }
    if(game.state == ST_TITLE && game.time > 7.0f) { pad.pressed |= BTN_START; step = 0; }
    if(game.state == ST_MAIN) {
        if(game.time > 1.5f && !shot[19]) { shot[19] = true; take_shot("main-menu"); }
        if(game.time > 2.5f) pad.pressed |= BTN_START;   /* single player (or two with DK_AT_2P) */
    }
    if(game.state == ST_MENU) {
        if(game.time > 2.0f && !shot[1]) { shot[1] = true; take_shot("menu"); }
        if(game.time > 3.5f && step < 1) { pad.pressed |= BTN_START; step++; }
    }
    if(game.state == ST_TRACKS) {
        if(game.time > 2.5f && !shot[9]) { shot[9] = true; take_shot("tracks"); }
        if(game.time > 4.0f && step < 2) { pad.pressed |= BTN_START; step++; }
    }
    if(game.state == ST_RACE && p->big_air && p->pos.y > 6.0f && !shot[10]) { shot[10] = true; take_shot("jump"); }
    if(game.state == ST_INTRO && game.time > 1.6f && !shot[2]) { shot[2] = true; take_shot("intro"); }
    if(game.state == ST_COUNTDOWN && game.countdown > 1.2f && !shot[7]) { shot[7] = true; take_shot("countdown"); }
    if(game.countdown > 3.15f && game.countdown < 3.5f && !shot[17]) { shot[17] = true; take_shot("go"); }
    if(game.state == ST_RACE && p->drift == DRIFT_ACTIVE && p->stage >= 3 && !shot[3]) { shot[3] = true; take_shot("drift"); }
    if(game.state == ST_RACE && p->boost_t > 0.3f && game.race_time > 8.0f && !shot[4]) { shot[4] = true; take_shot("boost"); }
    if(game.state == ST_RACE && track_section(p->tp.s) == SEC_BOARDWALK && !shot[5] && game.race_time > 12.0f) { shot[5] = true; take_shot("boardwalk"); }
    if(game.state == ST_RACE && dolphins_show_time() > 0.85f && !shot[11]) { shot[11] = true; take_shot("dolphins"); }
    if(game.state == ST_RACE && dolphins_show_time() > 3.65f && !shot[12]) { shot[12] = true; take_shot("dolphins2"); }
    if(game.state == ST_RESULTS && game.time > 1.5f && !shot[6]) { shot[6] = true; take_shot("results"); }
    if(game.online && game.state == ST_COUNTDOWN && game.countdown > 1.2f && !shot[17]) { shot[17] = true; take_shot("online-countdown"); }
    if(game.online && game.state == ST_RACE && game.race_time > 9.0f && !shot[18]) { shot[18] = true; take_shot("online-race"); }
    if(game.state == ST_RACE && !p->grounded && p->pos.y - track_height(p->tp.s) > 0.9f && !p->big_air &&
       game.race_time > 5.0f && !shot[13]) { shot[13] = true; take_shot("crest"); }
    if(game.state == ST_RACE && track.has_bridge && fabsf(track_delta(track.bridge_s, p->tp.s)) < 30.0f &&
       game.race_time > 20.0f && !shot[14]) { shot[14] = true; take_shot("viaduct"); }
    if(game.state == ST_RACE && track.id == TRACK_TEMPLE && fabsf(track_delta(track_seg_s(6) + 20.0f, p->tp.s)) < 3.0f &&
       !shot[15]) { shot[15] = true; take_shot("switchback"); }
    if(game.state == ST_RACE && track.id == TRACK_TEMPLE && fabsf(track_delta(track_seg_s(10), p->tp.s)) < 3.0f &&
       !shot[16]) { shot[16] = true; take_shot("descent"); }
#ifndef DK_HW
    /* Pause mid-race, switch the music off, grab the menu, switch it back on. */
    static float pause_t;
    if(game.state == ST_RACE && game.race_time > 6.0f && !shot[8] && !game.online) {
        if(!game.paused) { pad.pressed |= BTN_START; pause_t = 0; }
        else {
            pause_t += DK_DT;
            game.pause_sel = PAUSE_MUSIC;
            if(pause_t > 0.3f && audio_music_enabled() && pause_t < 1.0f) pad.pressed |= BTN_A;
            if(pause_t > 1.2f) { shot[8] = true; take_shot("pause"); }
        }
    }
    if(shot[8] && game.paused && (pause_t += DK_DT) > 1.6f) {
        if(!audio_music_enabled()) pad.pressed |= BTN_A;
        else pad.pressed |= BTN_START;
    }
#endif
}
#endif

#if defined(DK_HW) && defined(DK_CAPTURE)
/* Freeze a heavy scene and re-render it with one feature group off at a
   time; PVR render time per configuration shows where the GPU goes. */
static const struct { const char *name; uint32_t off; } probe_cfg[] = {
    {"base", 0}, {"-sky", PROBE_SKY}, {"-sea", PROBE_SEA}, {"-world", PROBE_WORLD},
    {"-karts", PROBE_KARTS}, {"-pt", PROBE_PT}, {"-distant", PROBE_DISTANT}, {"-foam", PROBE_FOAM},
    {"-shadows", PROBE_SHADOWS}, {"-fx", PROBE_FX}, {"-hud", PROBE_HUD}, {"autosort", PROBE_AUTOSORT},
    {"-alltr", PROBE_DISTANT | PROBE_FOAM | PROBE_SHADOWS | PROBE_FX | PROBE_HUD}
};
#define PROBE_CFGS ((int)(sizeof(probe_cfg) / sizeof(probe_cfg[0])))
#define PROBE_FRAMES 24
#define PROBE_SETTLE 4
static struct {
    const char *scene;
    int cfg, frame;
    float sum[PROBE_CFGS];
    int n[PROBE_CFGS];
    float cpu[PROBE_CFGS];
    bool done[3];
} probe;

/* Returns true while probing (the simulation stays frozen). */
static bool probe_step(void) {
    const Kart *p = &karts[game.player];
    if(!probe.scene) {
        if(game.online) return false;   /* freezing an online race would desync it */
        const char *scene = NULL;
        if(game.state == ST_RACE && game.race_time > 0.6f && !probe.done[0]) { probe.done[0] = true; scene = "grid"; }
        else if(game.state == ST_RACE && p->drift == DRIFT_ACTIVE && p->stage >= 2 && game.race_time > 20.0f &&
                !probe.done[1]) { probe.done[1] = true; scene = "drift"; }
        else if(game.state == ST_RACE && track_section(p->tp.s) == SEC_BOARDWALK && game.race_time > 35.0f &&
                !probe.done[2]) { probe.done[2] = true; scene = "boardwalk"; }
        if(!scene) return false;
        memset(&probe, 0, offsetof(__typeof__(probe), done));
        probe.scene = scene;
    }
    /* Account the frame just rendered with the previous configuration. */
    pvr_stats_t ps;
    if(probe.frame >= PROBE_SETTLE && pvr_get_stats(&ps) == 0 && ps.rnd_last_time < 100000000ull) {
        probe.sum[probe.cfg] += (float)ps.rnd_last_time * 1e-6f;
        probe.cpu[probe.cfg] += game.cpu_ms;
        probe.n[probe.cfg]++;
    }
    if(++probe.frame >= PROBE_FRAMES) {
        probe.frame = 0;
        if(++probe.cfg >= PROBE_CFGS) {
            printf("DK GPU %s:", probe.scene);
            for(int i = 0; i < PROBE_CFGS; i++)
                printf(" %s=%.2f", probe_cfg[i].name, (double)(probe.n[i] ? probe.sum[i] / probe.n[i] : 0));
            printf(" (cpu base %.2f)\n", (double)(probe.n[0] ? probe.cpu[0] / probe.n[0] : 0));
            dk_probe_off = 0;
            probe.scene = NULL;
            at_skip = 3;
            return false;
        }
    }
    dk_probe_off = probe_cfg[probe.cfg].off;
    return true;
}
#endif

#ifdef DK_AUTOTEST
static struct {
    int drifts, boosts[4], walls, dash, laps, jumps;
    float min_fps, cpu_sum, cpu_max;
    int cpu_n;
    float log_t;
    bool done;
    int win_frames, win_missed, frames, missed;
    float win_cpu_max, rnd_max_ms;
    size_t vtx_max;
} at = {.min_fps = 999};
#endif

static void update_places(void);

/* ------------------------------------------------------------------ */

void race_setup(void) {
    bool gp = game.mode == MODE_GP;
    int humans = game.num_players < 1 ? 1 : game.num_players;
    game.num_players = humans;
    kart_count = gp ? MAX_KARTS : humans;
    /* Humans start at the back of a Grand Prix grid, alone at the front
       otherwise. */
    for(int p = 0; p < humans; p++)
        game.pl[p].kart = gp ? MAX_KARTS - humans + p : p;
    game.player = game.pl[0].kart;
    int next_colour = 0, rival = 0;
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        memset(k, 0, sizeof(*k));
        int hp = -1;
        for(int p = 0; p < humans; p++)
            if(game.pl[p].kart == i) hp = p;
        k->cpu = hp < 0;
        if(!k->cpu) {
            kart_apply_colour(k, game.pl[hp].colour);
            k->name = hp ? "P2" : "P1";
        }
        else {
            /* Rivals take the presets the humans did not choose. */
            for(;;) {
                bool taken = false;
                for(int p = 0; p < humans; p++)
                    if(game.pl[p].colour == next_colour) taken = true;
                if(!taken) break;
                next_colour++;
            }
            kart_apply_colour(k, next_colour);
            k->name = kart_colours[next_colour % COLOUR_COUNT].name;
            next_colour++;
            k->skill = rival_ai[rival].skill;
            k->lane = rival_ai[rival].lane;
            rival++;
        }
        kart_place_on_grid(k, i);
        models_paint_kart(i);
    }
    if(!game.online) game.laps = DEFAULT_LAPS;
    race_reset();
}

void race_reset(void) {
    int humans = game.num_players;
    game.race_time = 0;
    game.countdown = 0;
    game.finished_count = 0;
    game.final_lap_announced = false;
    game.pause_sel = 0;
    game.paused = false;
    game.humans_done_t = -1;
    finish_wait = 0;
    memset(countdown_beeped, 0, sizeof(countdown_beeped));
    for(int p = 0; p < humans; p++) {
        Player *pl = &game.pl[p];
        pl->accel_press_t = -1.0f;
        pl->cam_yaw = karts[pl->kart].heading;
        pl->cam_height = 2.25f;
        pl->cam_dist = 5.3f;
        pl->fov = 1.30f;
        pl->cam.shake = 0;
        pl->final_lap_seen = false;
    }
    fx_clear();
    dolphins_reset();
    hud_reset();
    update_places();
}

void game_load_track(int id) {
    game.track = (id % TRACK_COUNT + TRACK_COUNT) % TRACK_COUNT;
    track_init(game.track);
    world_build();
    race_setup();
}

void game_set_state(int state) {
    game.state = state;
    game.time = 0;
    if(state == ST_TITLE || state == ST_MENU || state == ST_TRACKS || state == ST_MAIN || state == ST_ONLINE) {
        if(state == ST_TITLE) title_enter();
        audio_kart_loops(0, NULL, DK_DT);
        audio_kart_loops(1, NULL, DK_DT);
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

static void chase_camera(Player *pl, float dt) {
    const Kart *k = &karts[pl->kart];
    Camera *cam = &pl->cam;
    float top = kart_top_speed(k);
    float n = clampf(fabsf(k->speed) / top, 0, 1.4f);
    float target_yaw = k->heading + k->vis_yaw * 0.3f;
    pl->cam_yaw += wrap_angle(target_yaw - pl->cam_yaw) * smooth_k(5.0f, dt);
    pl->cam_yaw = wrap_angle(pl->cam_yaw);
    bool boost = k->boost_t > 0;
    float dist_t = 5.3f + n * 0.8f + (boost ? 0.7f : 0.0f);
    pl->cam_dist += (dist_t - pl->cam_dist) * smooth_k(4.0f, dt);
    /* Height tracks the road the kart last touched (so ramps lift the
       camera) plus half of any air above it. */
    float air = k->pos.y - k->gnd;
    float h_t = 2.25f + k->gnd + air * 0.5f;
    pl->cam_height += (h_t - pl->cam_height) * smooth_k(6.0f, dt);
    V3 back = heading_fwd(pl->cam_yaw);
    V3 pos = vmad(k->pos, back, -pl->cam_dist);
    pos.y = fmaxf(pl->cam_height, 0.8f);
    V3 look = vmad(k->pos, back, 3.0f);
    look.y = k->gnd + 0.95f + air * 0.6f;
    float shake = cam->shake;
    if(shake > 0) {
        pos = vadd(pos, v3(frand_s() * shake, frand_s() * shake, frand_s() * shake));
        shake = fmaxf(0.0f, shake - dt * 1.2f);
    }
    camera_basis(cam, pos, look);
    cam->shake = shake;
    float fov_t = 1.30f + n * 0.06f + (boost ? 0.14f : 0.0f);
    pl->fov += (fov_t - pl->fov) * smooth_k(5.0f, dt);
    cam->fov = pl->fov;
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

static void player_input(int player, KartInput *in) {
    const Pad *pd = input_pad(player);
    memset(in, 0, sizeof(*in));
    float steer = pd->stick_x;
    if(pd->held & BTN_LEFT) steer = -1;
    if(pd->held & BTN_RIGHT) steer = 1;
    in->steer = steer;
    in->accel = (pd->held & BTN_A) != 0;
    in->brake = (pd->held & BTN_B) != 0;
    in->drift = (pd->held & (BTN_R | BTN_L | BTN_X)) != 0;
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
        float remaining = (float)(game.laps + 1) * track.length - k->progress;
        k->finish_time = game.race_time + fmaxf(remaining, 0) / (kart_top_speed(k) * 0.92f);
        k->finished = true;
    }
    update_places();
}

static void player_events(int pi) {
    Player *pl = &game.pl[pi];
    Kart *p = &karts[pl->kart];
    uint32_t ev = p->events;
    if(ev & EV_LAP) {
        if(p->lap == game.laps && !pl->final_lap_seen) {
            pl->final_lap_seen = true;
            hud_message(pi, "FINAL LAP!", argb(255, 255, 210, 50), 2.4f);
            if(!game.final_lap_announced) {
                game.final_lap_announced = true;
                audio_sfx(DK_SFX_FINAL_LAP, 230, 128);
                audio_music_rate(1.06f);
            }
        }
        else {
            audio_sfx(DK_SFX_LAP, 220, 128);
        }
        float t = p->lap_times[p->lap - 2];
        hud_lap_popup(pi, p->lap - 1, t, t <= p->best_lap + 0.001f && p->lap > 2);
    }
    if(ev & EV_FINISH) {
        audio_sfx(DK_SFX_FINISH, 240, 128);
        if(game.mode == MODE_TT || p->place <= 3) audio_sfx(DK_SFX_CHAO_HAPPY, 210, 128);
        fx_confetti(vadd(p->pos, v3(0, 1.5f, 0)), game.num_players > 1 ? 50 : 90);
    }
    if(ev & (EV_WALL)) pl->cam.shake = fmaxf(pl->cam.shake, 0.12f);
    if(ev & (EV_BOOST | EV_DASH | EV_ROCKET)) pl->cam.shake = fmaxf(pl->cam.shake, 0.04f);
}

static void race_step(float dt, bool drive) {
    if(game.online) online_remote_karts(dt);
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        if(k->remote) continue;
        KartInput in;
        int hp = -1;
        for(int p = 0; p < game.num_players; p++)
            if(game.pl[p].kart == i) hp = p;
        bool ai = k->cpu || k->finished || hp < 0;
#ifdef DK_AUTOTEST
        ai = true;
#endif
        if(ai) ai_drive(k, &in, dt);
        else player_input(hp, &in);
        kart_update(k, &in, dt, drive);
    }
    karts_collide();
    update_places();
    dolphins_update(dt);
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        if(k->events & EV_FINISH) game.finished_count++;
        fx_kart(k, dt);
        audio_kart_events(k, kart_is_human(i));
    }
    for(int p = 0; p < game.num_players; p++) {
        player_events(p);
        rumble_kart(p, &karts[game.pl[p].kart]);
    }
#ifdef DK_PATH_LOG
    /* For server/cmd/dk-bot -path: the player's kart 12 times a second. */
    {
        static float next;
        const Kart *k = &karts[game.player];
        if(game.state == ST_RACE && game.race_time >= next) {
            next = game.race_time + 1.0f / 12.0f;
            printf("DK PATH %d %.2f %.2f %.2f %.4f %.2f %d %.1f\n", (int)(game.race_time * 1000), (double)k->pos.x,
                   (double)k->pos.y, (double)k->pos.z, (double)k->heading, (double)k->speed, k->lap,
                   (double)k->tp.s);
        }
    }
#endif
#ifdef DK_AUTOTEST
    {
        uint32_t ev = karts[game.player].events;
        if(ev & EV_DRIFT_START) at.drifts++;
        if(ev & EV_BOOST) at.boosts[karts[game.player].boost_stage & 3]++;
        if(ev & EV_WALL) at.walls++;
        if(ev & EV_DASH) at.dash++;
        if(ev & EV_LAP) at.laps++;
        if(ev & EV_JUMP) at.jumps++;
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
    for(int pi = 0; pi < game.num_players; pi++) {
        Player *pl = &game.pl[pi];
        const Pad *pd = input_pad(pi);
        if(pd->pressed & BTN_A) pl->accel_press_t = before;
        if(!(pd->held & BTN_A)) pl->accel_press_t = -1.0f;
#ifdef DK_AUTOTEST
        pl->accel_press_t = 1.9f;
#endif
    }
    if(game.countdown >= 3.0f) {
        for(int pi = 0; pi < game.num_players; pi++) {
            Player *pl = &game.pl[pi];
            Kart *p = &karts[pl->kart];
            if(pl->accel_press_t >= 1.55f && pl->accel_press_t <= 2.4f) {
                kart_start_boost(p, 1.0f, 1.12f);
                p->events |= EV_ROCKET;
                audio_sfx(DK_SFX_BOOST, 230, 128);
                rumble_pulse(pi, 6, 30, 0.45f);
            }
            else if(pl->accel_press_t >= 0 && pl->accel_press_t < 1.3f) {
                p->burnout_t = 0.85f;
                audio_sfx(DK_SFX_BUMP, 150, 128);
                rumble_pulse(pi, 4, 14, 0.2f);
            }
        }
        for(int i = 0; i < kart_count; i++) {
            if(!karts[i].cpu) continue;
            if((dk_rand() % 100) < 45) kart_start_boost(&karts[i], 0.8f + frand() * 0.4f, 1.08f);
        }
        game.state = ST_RACE;
        game.time = 0;
    }
}

/* START from any player: returns that player's index, or -1. */
static int start_pressed(void) {
    for(int p = 0; p < game.num_players; p++)
        if(input_pad(p)->pressed & BTN_START) return p;
    return -1;
}

static void pause_update(void) {
    int o = game.pause_owner;
    const Pad *pd = input_pad(o);
    if(input_up(o)) {
        game.pause_sel = (game.pause_sel + PAUSE_ITEMS - 1) % PAUSE_ITEMS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(input_down(o)) {
        game.pause_sel = (game.pause_sel + 1) % PAUSE_ITEMS;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
    }
    if(pd->pressed & (BTN_START | BTN_B)) {
        game.paused = false;
        audio_sfx(DK_SFX_MENU_BACK, 180, 128);
        audio_music_target(150);
        return;
    }
    /* The music switch flips with A or left/right and keeps the game paused. */
    if(game.pause_sel == PAUSE_MUSIC && ((pd->pressed & BTN_A) || input_left(o) || input_right(o))) {
        audio_music_enable(!audio_music_enabled());
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
        return;
    }
    if(pd->pressed & BTN_A) {
        audio_sfx(DK_SFX_MENU_SELECT, 200, 128);
        game.paused = false;
        if(game.pause_sel == PAUSE_RESTART) {
            race_setup();
            game_set_state(ST_INTRO);
        }
        else if(game.pause_sel == PAUSE_QUIT) {
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
        case ST_ONLINE:
            online_update(dt);
            for(int i = 0; i < kart_count; i++) {
                KartInput in = {0};
                if(!karts[i].remote) kart_update(&karts[i], &in, dt, false);
            }
            break;
        case ST_TITLE:
        case ST_MAIN:
        case ST_MENU:
        case ST_TRACKS:
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
            if(game.time >= 3.2f || (game.time > 0.5f && ((pad.pressed | pad2.pressed) & BTN_A))) {
                game.state = ST_COUNTDOWN;
                game.time = 0;
                game.countdown = 0;
            }
            break;
        case ST_COUNTDOWN:
            if(game.online) {
                /* The server's clock sets the countdown; no pausing online. */
                if(online_race_input()) break;
                game.countdown = online_countdown() - dt;
                countdown_step(dt);
                online_remote_karts(dt);
                for(int i = 0; i < kart_count; i++) {
                    KartInput in = {0};
                    if(!karts[i].remote) kart_update(&karts[i], &in, dt, false);
                }
                break;
            }
            if(game.paused) {
                pause_update();
                break;
            }
            if(start_pressed() >= 0) {
                game.paused = true;
                game.pause_owner = start_pressed();
                game.pause_sel = 0;
                audio_music_target(60);
                break;
            }
            countdown_step(dt);
            for(int i = 0; i < kart_count; i++) {
                KartInput in = {0};
                kart_update(&karts[i], &in, dt, false);
            }
            break;
        case ST_RACE: {
            if(game.online) {
                /* The server ends the race and gives the results. */
                if(online_race_input()) break;
                game.countdown += dt;
                game.race_time = online_race_time();
                race_step(dt, true);
                break;
            }
            if(!game.paused && start_pressed() >= 0) {
                game.paused = true;
                game.pause_owner = start_pressed();
                game.pause_sel = 0;
                audio_sfx(DK_SFX_MENU_SELECT, 180, 128);
                audio_music_target(60);
                pad.pressed = pad2.pressed = 0;
            }
            if(game.paused) {
                pause_update();
                break;
            }
            game.countdown += dt;
            game.race_time += dt;
            race_step(dt, true);
            /* Results once every human is home (a short pause to enjoy the
               finish), or 20 s after the first in a two-player race. */
            int home = 0;
            for(int p = 0; p < game.num_players; p++)
                if(karts[game.pl[p].kart].finished) home++;
            if(home > 0) {
                if(game.humans_done_t < 0) game.humans_done_t = 0;
                game.humans_done_t += dt;
            }
            if(home == game.num_players) {
                finish_wait += dt;
                if(finish_wait > 4.5f) {
                    finalize_results();
                    game_set_state(ST_RESULTS);
                }
            }
            else if(home > 0 && game.humans_done_t > 20.0f) {
                finalize_results();
                game_set_state(ST_RESULTS);
            }
            if(home == 1 && finish_wait == 0) audio_music_target(game.num_players > 1 ? 130 : 90);
            break;
        }
        case ST_RESULTS:
            game.race_time += dt;
            race_step(dt, true);
            if(game.online) break;
            if(game.time > 1.0f && ((pad.pressed | pad2.pressed) & BTN_A)) {
                audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
                race_setup();
                game_set_state(ST_INTRO);
            }
            else if(game.time > 1.0f && ((pad.pressed | pad2.pressed) & BTN_B)) {
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
        case ST_MAIN:
        case ST_MENU:
        case ST_TRACKS:
            title_camera(&game.cam, dt);
            break;
        case ST_ONLINE:
            online_camera(&game.cam, dt);
            break;
        case ST_INTRO:
            intro_camera(&game.cam, game.time / 3.2f);
            break;
        case ST_RESULTS:
            results_camera(&game.cam, dt);
            break;
        default:
            for(int p = 0; p < game.num_players; p++)
                chase_camera(&game.pl[p], game.paused ? 0 : dt);
            break;
    }
}

/* Accumulated microseconds per phase: update, world OP, karts, PT, TR. */
static uint64_t prof_us[6];
static int prof_frames;
#define PROF_MARK(i) do { uint64_t _n = timer_us_gettime64(); prof_us[i] += _n - prof_t; prof_t = _n; } while(0)

/* Views this frame: one full-screen view, or a top/bottom split while two
   players race. Every view is drawn inside each PVR list in turn. */
static int view_count(void) {
    return split_screen() ? 2 : 1;
}

static void set_view(int v) {
    bool racing = game.state == ST_COUNTDOWN || game.state == ST_RACE;
    /* Split: two 7-tile views with the HUD band (tile row 7) between. */
    if(view_count() == 2) r_set_viewport(v ? 256.0f : 0.0f, v ? 480.0f : 224.0f);
    else r_set_viewport(0, 480.0f);
    r_set_camera(racing ? &game.pl[v].cam : &game.cam);
}

static int view_kart(int v) {
    bool racing = game.state == ST_COUNTDOWN || game.state == ST_RACE;
    return racing ? game.pl[v].kart : game.player;
}

static void draw_finish_banner(int pi, float y0, float y1) {
    const Kart *k = &karts[game.pl[pi].kart];
    if(!k->finished) return;
    float t = game.race_time - k->finish_time;
    float base = (y1 - y0) / 480.0f;
    float s = (t < 0.3f ? 0.4f + t * 2.5f : 1.15f + 0.03f * sinf(t * 6)) * (base < 1 ? 0.62f : 1.0f);
    hud_bigtext(4, 320, y0 + (y1 - y0) * 0.4f, s, 0xffffffff);
}

static void render(void) {
    uint64_t prof_t = timer_us_gettime64();
    int views = view_count();
    world_split = views > 1;
    world_set_fog(world_split);
    models_begin_frame();
    r_frame_begin();

    r_list(PVR_LIST_OP_POLY);
    for(int v = 0; v < views; v++) {
        set_view(v);
        world_draw_op();
    }
    PROF_MARK(1);
    if(!(dk_probe_off & PROBE_KARTS))
        for(int v = 0; v < views; v++) {
            set_view(v);
            models_draw_karts(view_kart(v));
            dolphins_draw();
        }
    PROF_MARK(2);

    r_list(PVR_LIST_PT_POLY);
    if(!(dk_probe_off & PROBE_PT))
        for(int v = 0; v < views; v++) {
            set_view(v);
            world_draw_pt();
        }
    PROF_MARK(3);

    r_list(PVR_LIST_TR_POLY);
    for(int v = 0; v < views; v++) {
        set_view(v);
        world_draw_tr();
        if(!(dk_probe_off & PROBE_SHADOWS)) models_draw_shadows();
        if(!(dk_probe_off & PROBE_FX)) fx_draw();
    }
    r_set_viewport(0, 480.0f);
    if(!(dk_probe_off & PROBE_HUD)) switch(game.state) {
        case ST_TITLE:
        case ST_MAIN:
        case ST_MENU:
        case ST_TRACKS:
            title_draw();
            break;
        case ST_ONLINE:
            online_draw();
            break;
        case ST_INTRO: {
            float a = clampf(game.time * 2.0f, 0, 1) * clampf((3.2f - game.time) * 2.0f, 0, 1);
            static const char *const cc_names[] = {"50cc", "100cc", "150cc"};
            hud_panel(0, 330, 640, 410, argb((int)(170 * a), 20, 70, 180), argb((int)(170 * a), 10, 30, 90), 15);
            hud_text(320 + (1 - a) * 200, 334, 1.2f, ALIGN_CENTER, color_alpha(argb(255, 255, 220, 60), a), track.name);
            char buf[48];
            const char *mode = game.mode == MODE_GP ? "GRAND PRIX" : game.num_players > 1 ? "VERSUS" : "TIME TRIAL";
            snprintf(buf, sizeof(buf), "%s%s  -  %s", game.num_players > 1 ? "2P " : "", mode, cc_names[game.cc]);
            hud_text(320 - (1 - a) * 200, 378, 0.65f, ALIGN_CENTER, color_alpha(0xffffffff, a), buf);
            break;
        }
        case ST_COUNTDOWN:
        case ST_RACE:
            if(views == 1) {
                hud_draw_race(0, 0, 480);
                hud_draw_countdown(0, 480);
                draw_finish_banner(0, 0, 480);
            }
            else {
                for(int v = 0; v < 2; v++) {
                    float y0 = v ? 256.0f : 0.0f, y1 = y0 + 224.0f;
                    hud_draw_race(v, y0, y1);
                    hud_draw_countdown(y0, y1);
                    draw_finish_banner(v, y0, y1);
                }
                hud_draw_split_overlay();
            }
            if(game.paused) hud_draw_pause();
            if(game.online) online_draw_race();
            break;
        case ST_RESULTS:
            if(game.online) online_draw_results();
            else hud_draw_results();
            break;
    }
    r_frame_end();
    PROF_MARK(4);
    prof_frames++;
}

/* One frame of the loading screen between boot steps. */
static void boot_screen(uint64_t t0, float progress, const char *what) {
    float t = (float)(timer_us_gettime64() - t0) * 1e-6f;
    pvr_wait_ready();
    r_frame_begin();
    r_list(PVR_LIST_OP_POLY);
    r_list(PVR_LIST_TR_POLY);
    r_set_viewport(0, 480.0f);
    title_draw_loading(progress, what, t);
    r_frame_end();
#if defined(DK_CAPTURE) && !defined(DK_HW)
    /* Hold the halfway screen long enough for capture.py to grab it. */
    static bool shot_taken;
    if(progress > 0.5f && !shot_taken) {
        shot_taken = true;
        take_shot("loading");
        uint64_t hold = timer_us_gettime64();
        while(timer_us_gettime64() - hold < 1200000) boot_screen(t0, progress, what);
    }
#endif
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\nDreamKarts starting (KallistiOS, %s %s)\n", __DATE__, __TIME__);
    uint64_t boot_t0 = timer_us_gettime64();
    input_init();
    render_init();
    hud_init();
    title_init();
    boot_screen(boot_t0, 0.1f, "WAKING UP THE CHAO");
#ifdef DK_AT_TRACK
    game.track = DK_AT_TRACK;
#endif
    track_init(game.track);
    world_init();
    boot_screen(boot_t0, 0.25f, "PLANTING PALM TREES");
    world_build();
    boot_screen(boot_t0, 0.55f, "POLISHING KARTS");
    models_init();
    fx_init();
    dolphins_init();
    boot_screen(boot_t0, 0.7f, "TUNING STEEL DRUMS");
    audio_init();
    boot_screen(boot_t0, 1.0f, "READY!");
    printf("DreamKarts boot: %lu ms\n", (unsigned long)((timer_us_gettime64() - boot_t0) / 1000));

    online_init();
    game.cc = CC_100;
    game.laps = DEFAULT_LAPS;
    game.mode = MODE_GP;
    game.num_players = 1;
    game.pl[0].colour = 0;
    game.pl[1].colour = 1;
#if defined(DK_AT_2P) && defined(DK_CAPTURE)
    game.num_players = 2;
    game.pl[0].colour = 6;
#endif
    race_setup();
#if defined(DK_AT_ONLINE) || defined(DK_AT_NAME)
    online_enter();
#elif defined(DK_AUTOTEST) && !defined(DK_CAPTURE)
    game.cc = CC_150;
#ifdef DK_AT_TT
    game.mode = MODE_TT;
#endif
#ifdef DK_AT_2P
    game.num_players = 2;
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
        if(game.online) online_tick();
#if defined(DK_HW) && defined(DK_CAPTURE)
        if(probe_step()) {
            steps = 0;
            at_skip = 1;
        }
        pvr_set_presort_mode((dk_probe_off & PROBE_AUTOSORT) == 0);
#endif
        for(int s = 0; s < steps; s++) {
#ifdef DK_CAPTURE
            capture_script();
#endif
            update(DK_DT);
            pad.pressed = pad2.pressed = 0;
            pad.released = pad2.released = 0;
        }
        update_camera(dt_real);
        audio_update(dt_real);
        bool engines = (game.state == ST_RACE || game.state == ST_COUNTDOWN || game.state == ST_RESULTS) &&
                       !game.paused;
        for(int p = 0; p < MAX_PLAYERS; p++)
            audio_kart_loops(p, engines && p < game.num_players ? &karts[game.pl[p].kart] : NULL, dt_real);
        rumble_update(dt_real, (game.state == ST_RACE || game.state == ST_COUNTDOWN) && !game.paused);
        prof_us[0] += timer_us_gettime64() - upd_t0;
        render();

        uint64_t end = timer_us_gettime64();
        game.cpu_ms = (float)(end - now) * 0.001f;
        fps_frames++;
        if(end - fps_t0 >= 1000000) {
            game.fps = fps_frames * 1e6f / (float)(end - fps_t0);
            fps_frames = 0;
            fps_t0 = end;
#if defined(DK_PERF_LOG) && !defined(DK_AUTOTEST)
            /* Off by default: under dcload-ip each printf runs with IRQs
               off, costs a frame or more and can freeze the console. */
            if(game.state == ST_RACE)
                printf("DK PERF fps=%.1f cpu=%.2fms tris=%d verts=%d\n", (double)game.fps, (double)game.cpu_ms,
                       rstats.tris, rstats.verts);
#endif
        }
#ifdef DK_AUTOTEST
        {
            static const char *const state_names[] = {"title", "menu", "tracks", "intro", "countdown", "race", "results",
                                                     "main", "online"};
            const Kart *p = &karts[game.player];
            /* A displayed frame that took more than 1.5 vblanks dropped one. */
            if(at_skip > 0) {
                at_skip--;
            }
            else {
                at.win_frames++;
                at.frames++;
                if(dt_real > 1.5f / 60.0f) {
                    at.win_missed++;
                    at.missed++;
                }
                if(game.cpu_ms > at.win_cpu_max) at.win_cpu_max = game.cpu_ms;
                pvr_stats_t ps;
                if(pvr_get_stats(&ps) == 0 && ps.rnd_last_time < 100000000ull) {
                    float rnd = (float)ps.rnd_last_time * 1e-6f;
                    if(rnd > at.rnd_max_ms) at.rnd_max_ms = rnd;
                    if(ps.vtx_buffer_used_max > at.vtx_max) at.vtx_max = ps.vtx_buffer_used_max;
                }
                if(game.state == ST_RACE || game.state == ST_RESULTS) {
                    at.cpu_sum += game.cpu_ms;
                    at.cpu_n++;
                    if(game.cpu_ms > at.cpu_max) at.cpu_max = game.cpu_ms;
                    if(game.fps > 0 && game.fps < at.min_fps && game.race_time > 2.0f) at.min_fps = game.fps;
                }
            }
            at.log_t += dt_real;
            if(at.log_t >= AT_LOG_SECS) {
                at.log_t = 0;
                float f = prof_frames ? 1.0f / (prof_frames * 1000.0f) : 0;
                printf("DK AT %s t=%.1f fps=%.1f missed=%d/%d cpu=%.2fms max=%.2fms [upd %.2f world %.2f karts %.2f "
                       "pt %.2f tr %.2f] pvr=%.2fms tris=%d lap=%d place=%d spd=%.1f drift=%d stage=%d\n",
                       state_names[game.state], (double)game.race_time, (double)game.fps, at.win_missed,
                       at.win_frames, (double)game.cpu_ms, (double)at.win_cpu_max, (double)(prof_us[0] * f),
                       (double)(prof_us[1] * f), (double)(prof_us[2] * f), (double)(prof_us[3] * f),
                       (double)(prof_us[4] * f), (double)at.rnd_max_ms, rstats.tris, p->lap, p->place,
                       (double)p->speed, p->drift, p->stage);
                memset(prof_us, 0, sizeof(prof_us));
                prof_frames = 0;
                at.win_frames = at.win_missed = 0;
                at.win_cpu_max = at.rnd_max_ms = 0;
                at_skip = 2;
            }
            if(game.state == ST_RESULTS && game.time > 3.0f && !at.done && !game.online) {
                at.done = true;
                printf("DK AUTOTEST: player finish %.2fs place %d laps", (double)p->finish_time, p->place);
                for(int l = 0; l < game.laps; l++) printf(" %.2f", (double)p->lap_times[l]);
                printf("\nDK AUTOTEST: drifts %d boosts blue %d orange %d purple %d dash %d walls %d\n",
                       at.drifts, at.boosts[1], at.boosts[2], at.boosts[3], at.dash, at.walls);
                printf("DK AUTOTEST: rumble effects sent %d\n", rumble_sends);
                audio_autotest_report();
                printf("DK AUTOTEST: track %s, ramp jumps %d (player %d), gap rescues %d\n", track.name, kart_jumps,
                       at.jumps, kart_rescues);
                printf("DK AUTOTEST: fps min %.1f, missed vblanks %d of %d frames, cpu avg %.2fms max %.2fms, "
                       "vertex buffer peak %lu KiB\n", (double)at.min_fps, at.missed, at.frames,
                       (double)(at.cpu_sum / (at.cpu_n ? at.cpu_n : 1)), (double)at.cpu_max,
                       (unsigned long)(at.vtx_max / 1024));
                for(int i = 0; i < kart_count; i++)
                    printf("DK AUTOTEST: %d. %s %.2f\n", i + 1, karts[game.finish_order[i]].name,
                           (double)karts[game.finish_order[i]].finish_time);
                bool ok = p->finish_time > 0 && p->finish_time < 150.0f && at.drifts > 3 && kart_rescues == 0 &&
                          (!track.has_jump || at.jumps >= game.laps);
                printf("DK AUTOTEST: %s\n", ok ? "PASS" : "FAIL");
#ifdef DK_HW
                game.quit = true;   /* hand the console back to dcload-ip */
#endif
            }
        }
#endif
    }
    rumble_shutdown();
    audio_shutdown();
    render_shutdown();
    return 0;
}
