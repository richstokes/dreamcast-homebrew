/* DreamKarts: a kart racer for the Sega Dreamcast (KallistiOS).

   Main loop, race flow, cameras and the player's controls. Physics run at a
   fixed 60 Hz; rendering is vsynced through the PVR. The scripted builds'
   tooling (autotest, captures, tours, demo recording) is in autotest.c. */
#include "dk.h"
#include "audio.h"
#include "fx.h"
#include "hud.h"
#include "ui.h"
#include "kart.h"
#include "models.h"
#include "render.h"
#include "rumble.h"
#include "vmuscreen.h"
#include "title.h"
#include "flappy.h"
#include "track.h"
#include "world.h"
#include "biplane.h"
#include "dolphins.h"
#include "items.h"
#include "online.h"
#include "save.h"
#include "spectate.h"
#include "film.h"
#include "gp.h"
#include "prof.h"
#include "version.h"
#include "perfhud.h"
#include "autotest.h"
#include "../../../common/softreset.h"

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
    return game.num_players > 1 && (game.state == ST_COUNTDOWN || game.state == ST_RACE || game.state == ST_KARTS);
}

/* Accumulated microseconds per phase: update, world OP, karts, PT, TR
   (prof.h); prof_last holds the latest frame's. */
uint64_t prof_us[6];
static uint32_t prof_last[6];
int prof_frames;
#define PROF_MARK(i) do { uint64_t _n = timer_us_gettime64(); prof_last[i] = (uint32_t)(_n - prof_t); \
                          prof_us[i] += _n - prof_t; prof_t = _n; } while(0)

static void update_places(void);

/* ------------------------------------------------------------------ */

void race_setup(void) {
    bool rivals = game.mode != MODE_TT;   /* Grand Prix and single races have CPUs */
    int humans = game.num_players < 1 ? 1 : game.num_players;
    game.num_players = humans;
    kart_count = rivals ? MAX_KARTS : humans;
    /* Humans start at the back of a full grid, alone at the front otherwise. */
    for(int p = 0; p < humans; p++)
        game.pl[p].kart = rivals ? MAX_KARTS - humans + p : p;
#ifdef DK_AT_SOLO
    /* Captures of the track itself: the humans alone, no rivals in the way. */
    kart_count = humans;
    for(int p = 0; p < humans; p++) game.pl[p].kart = p;
#endif
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
        /* From a Grand Prix's second race the grid follows the standings. */
        kart_place_on_grid(k, gp.active && gp.race > 0 ? gp_grid_slot(i) : i);
        models_paint_kart(i);
    }
    if(!game.online) {
        game.laps = game.setup_laps;
        /* No power-ups in a solo time trial, so lap times stay honest. */
        game.items = game.setup_items && !(game.mode == MODE_TT && game.num_players == 1);
    }
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
        pl->wrong_way_t = 0;
    }
    fx_clear();
    dolphins_reset();
    biplane_reset();
    world_race_progress(0);
    hud_reset();
    update_places();
    items_reset(game.online ? online_items_seed() : dk_rand());
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
    if(state == ST_RESULTS) game.results_sel = RESULTS_AGAIN;
    if(state == ST_TITLE || state == ST_MAIN || state == ST_ONLINE) gp.active = false;
    if(state == ST_TITLE || state == ST_MENU || state == ST_TRACKS || state == ST_MAIN || state == ST_ONLINE ||
       state == ST_KARTS) {
        if(state == ST_TITLE) title_enter();
        audio_kart_loops(0, NULL, DK_DT);
        audio_kart_loops(1, NULL, DK_DT);
        if(state == ST_TITLE) audio_music_title();
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

/* behind: turned round, from in front of the kart, a little higher and
   looking further back so the kart doesn't hide who's chasing. */
static void chase_camera(Player *pl, float dt, bool behind) {
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
    V3 dir = heading_fwd(pl->cam_yaw);
    if(behind) dir = vscale(dir, -1.0f);
    V3 pos = vmad(k->pos, dir, -pl->cam_dist);
    /* A floor under the camera, 0.8 m up, lowered with a road below zero
       (Apple Valley's turn 1, Neon Skyway's dip, Cyberstone's valley):
       a fixed one held the camera up there and tipped it to look down
       steeply until the road climbed back. */
    pos.y = fmaxf(pl->cam_height, fminf(0.8f, k->gnd + 0.8f));
    V3 look = vmad(k->pos, dir, behind ? 7.0f : 3.0f);
    look.y = k->gnd + 0.95f + air * 0.6f;
    if(behind) pos.y += 0.5f;
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

/* The spectator director's chase shots use the same camera. */
void game_chase_camera(Player *pl, float dt) {
    chase_camera(pl, dt, false);
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
    /* The Grand Prix standings page circles the leader. */
    const Kart *k = &karts[gp.active && gp.page == 1 ? gp.rank[0] : game.player];
    float a = game.time * 0.3f + k->heading + DK_PI * 0.75f;
    V3 target = vadd(k->pos, v3(0, 0.9f, 0));
    camera_basis(cam, vadd(target, v3(sinf(a) * 6.5f, 2.2f, cosf(a) * 6.5f)), target);
    cam->fov = 1.2f;
    (void)dt;
}

/* ------------------------------------------------------------------ */
/* Race                                                                */

/* Squeezing both triggers together (the second within 0.25 s of the
   first) looks behind until either is let go. The squeeze stops the
   triggers drifting until both are let go, so it doesn't start a slide or
   charge a jump. */
static float trig_t[MAX_PLAYERS];        /* seconds a trigger has been held */
static bool trig_squeeze[MAX_PLAYERS], look_back[MAX_PLAYERS];
static bool wave_press[MAX_PLAYERS];
static bool item_press[MAX_PLAYERS], item_back[MAX_PLAYERS];

static bool holding_item(int p) {
    return game.items && p < game.num_players && karts[game.pl[p].kart].item != ITEM_NONE;
}

static void emote_input(float dt) {
    for(int p = 0; p < MAX_PLAYERS; p++) {
        const Pad *pd = input_pad(p);
        uint32_t lr = pd->held & (BTN_L | BTN_R);
        trig_t[p] = lr ? trig_t[p] + dt : 0;
        /* Y fires a held power-up (backwards with down held) and waves
           otherwise. */
        item_press[p] = (pd->pressed & BTN_Y) && holding_item(p);
        item_back[p] = (pd->held & BTN_DOWN) || pd->stick_y > 0.5f;
        wave_press[p] = (pd->pressed & BTN_Y) && !item_press[p];
        if(lr == (BTN_L | BTN_R) && (pd->pressed & lr) && trig_t[p] <= 0.25f + dt) trig_squeeze[p] = true;
        if(!lr) trig_squeeze[p] = false;
        look_back[p] = trig_squeeze[p] && lr == (BTN_L | BTN_R);
    }
}

static int human_of(int kart) {
    for(int p = 0; p < game.num_players; p++)
        if(game.pl[p].kart == kart) return p;
    return -1;
}

/* A CPU near a waving human waves back a moment later. */
static void cpu_wave_back(const Kart *waver) {
    Kart *best = NULL;
    float best_d = 20.0f;
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        if(!k->cpu || k->wave_t > 0 || k->wave_back_t > 0 || kart_cheering(k)) continue;
        float d = vlen(vsub(k->pos, waver->pos));
        if(d < best_d) {
            best_d = d;
            best = k;
        }
    }
    if(best) best->wave_back_t = 0.45f + 0.35f * frand();
}

static void wave_events(void) {
    for(int i = 0; i < kart_count; i++)
        if((karts[i].events & EV_WAVE) && human_of(i) >= 0) cpu_wave_back(&karts[i]);
}

/* Karts waiting to race (intro, countdown): they can't drive, but humans
   can wave. */
static void idle_karts_step(float dt) {
    for(int i = 0; i < kart_count; i++) {
        if(karts[i].remote) continue;
        KartInput in = {0};
        int hp = human_of(i);
        if(hp >= 0) in.wave = wave_press[hp];
        kart_update(&karts[i], &in, dt, false);
    }
    wave_events();
    for(int i = 0; i < kart_count; i++)
        if(karts[i].events & EV_WAVE) audio_kart_events(&karts[i], kart_is_human(i));
}

static void player_input(int player, KartInput *in) {
    const Pad *pd = input_pad(player);
    memset(in, 0, sizeof(*in));
    float steer = pd->stick_x;
    if(pd->held & BTN_LEFT) steer = -1;
    if(pd->held & BTN_RIGHT) steer = 1;
    in->steer = steer;
    in->accel = (pd->held & BTN_A) != 0;
    in->brake = (pd->held & BTN_B) != 0;
    in->drift = (pd->held & BTN_X) || (!trig_squeeze[player] && (pd->held & (BTN_R | BTN_L)));
    in->item = item_press[player];
    in->item_back = item_back[player];
}

static void update_places(void) {
    int order[MAX_KARTS];
    for(int i = 0; i < kart_count; i++) order[i] = i;
    for(int i = 1; i < kart_count; i++) {
        int v = order[i], j = i - 1;
        while(j >= 0) {
            const Kart *a = &karts[order[j]], *b = &karts[v];
            bool b_ahead;
            if(a->gone != b->gone) b_ahead = a->gone;   /* a dropped-out rival places last */
            else if(a->finished && b->finished) b_ahead = b->finish_time < a->finish_time;
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

/* The leader's share of the race: karts cross the line onto lap 1 at the
   start and onto laps + 1 at the flag. */
static float race_frac(void) {
    float lead = 0;
    for(int i = 0; i < kart_count; i++)
        if(!karts[i].gone) lead = fmaxf(lead, karts[i].progress);
    return (lead - track.length) / ((float)game.laps * track.length);
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

static void player_events(int pi, float dt) {
    Player *pl = &game.pl[pi];
    Kart *p = &karts[pl->kart];
    uint32_t ev = p->events;
    /* Wrong way: moving back along the track for a second or so. A spin,
       a fall or a quick reverse off a wall doesn't count, and it clears a
       little after the kart is pointing the right way again. */
    bool wrong = !p->finished && p->spin_t <= 0 && !p->over_edge && vdot(p->vel, p->tp.tangent) < -5.0f;
    pl->wrong_way_t = wrong ? fminf(pl->wrong_way_t + dt, 2.0f) : fmaxf(pl->wrong_way_t - 2.0f * dt, 0.0f);
#ifdef DK_AUTOTEST
    at_player_events(pi);
#endif
    if(ev & EV_LAP) {
        if(p->lap == game.laps && !pl->final_lap_seen) {
            pl->final_lap_seen = true;
            hud_message(pi, "FINAL LAP!", argb(255, 255, 210, 50), 2.4f);
            if(!game.final_lap_announced) {
                game.final_lap_announced = true;
                audio_sfx(DK_SFX_FINAL_LAP, 230, 128);
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
        fx_confetti(vadd(p->pos, v3(0, 1.5f, 0)), p->pos.y, game.num_players > 1 ? 50 : 90);
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
        int hp = human_of(i);
        bool ai = k->cpu || k->finished || hp < 0;
#if defined(DK_AUTOTEST) || defined(DK_FILM)
        ai = true;
#endif
        if(ai) ai_drive(k, &in, dt);
        else player_input(hp, &in);
#ifdef DK_AUTOTEST
        at_kart_input(k, hp, &in, dt);
#endif
        /* Humans can still wave once the CPU has taken over after the line. */
        if(hp >= 0) in.wave = wave_press[hp];
        kart_update(k, &in, dt, drive);
        items_kart(k, &in, dt);
    }
    wave_events();
    items_update(dt);
    karts_collide();
    update_places();
    world_race_progress(race_frac());
    dolphins_update(dt);
    biplane_update(dt);
    for(int i = 0; i < kart_count; i++) {
        Kart *k = &karts[i];
        if(k->events & EV_FINISH) game.finished_count++;
        fx_kart(k, dt);
        audio_kart_events(k, kart_is_human(i));
    }
    for(int p = 0; p < game.num_players; p++) {
        player_events(p, dt);
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
    at_race_step();
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
    /* Rocket start: press accelerate just as "2" fades, hold it through GO.
       A spectator has no kart of its own. */
    for(int pi = 0; pi < game.num_players && !spectate_on(); pi++) {
        Player *pl = &game.pl[pi];
        const Pad *pd = input_pad(pi);
        if(pd->pressed & BTN_A) pl->accel_press_t = before;
        if(!(pd->held & BTN_A)) pl->accel_press_t = -1.0f;
        audio_kart_throttle(pi, (pd->held & BTN_A) != 0);
#if defined(DK_AUTOTEST) || defined(DK_FILM)
        pl->accel_press_t = 1.9f;
#endif
    }
    if(game.countdown >= 3.0f) {
        for(int pi = 0; pi < game.num_players && !spectate_on(); pi++) {
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
    /* Y hides the pause panel and every HUD element so the frame can be
       screenshotted clean; Y again brings them back. While hidden only
       Start or B (resume) act, so a blind A cannot restart or quit. */
    if(pd->pressed & BTN_Y) {
        game.hud_hidden = !game.hud_hidden;
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
        return;
    }
    if(game.hud_hidden && !(pd->pressed & (BTN_START | BTN_B))) return;
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
        game.hud_hidden = false;
        audio_sfx(DK_SFX_MENU_BACK, 180, 128);
        audio_music_target(150);
        return;
    }
    /* The music switch flips with A or left/right and keeps the game paused. */
    if(game.pause_sel == PAUSE_MUSIC && ((pd->pressed & BTN_A) || input_left(o) || input_right(o))) {
        save.music = !audio_music_enabled();
        audio_music_enable(save.music);
        save_store();
        audio_sfx(DK_SFX_MENU_MOVE, 180, 128);
        return;
    }
    /* NEXT SONG skips to the next of the shuffle and keeps the game paused. */
    if(game.pause_sel == PAUSE_NEXT_SONG && ((pd->pressed & BTN_A) || input_right(o))) {
        audio_music_next();
        audio_sfx(DK_SFX_MENU_SELECT, 200, 128);
        return;
    }
    /* PERF steps off, overlay, FPS with A or right, back with left. */
    if(game.pause_sel == PAUSE_PERF && ((pd->pressed & BTN_A) || input_left(o) || input_right(o))) {
        perfhud_set_mode((perfhud_mode + (input_left(o) ? PERF_MODES - 1 : 1)) % PERF_MODES);
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

/* After a single race, time trial or versus: race again, pick another
   course or back to the title. Either pad chooses. */
static void results_menu_update(void) {
    if(game.time < 1.0f) return;
    int dir = 0;
    for(int p = 0; p < game.num_players; p++) dir = input_left(p) ? -1 : input_right(p) ? 1 : dir;
    if(dir) {
        game.results_sel = (game.results_sel + dir + RESULTS_ITEMS) % RESULTS_ITEMS;
        audio_sfx(DK_SFX_MENU_MOVE, 200, dir < 0 ? 90 : 166);
    }
    uint32_t pressed = pad.pressed | pad2.pressed;
    if(pressed & BTN_A) {
        audio_sfx(DK_SFX_MENU_SELECT, 220, 128);
        race_setup();
        if(game.results_sel == RESULTS_AGAIN) {
            game_set_state(ST_INTRO);
        }
        else if(game.results_sel == RESULTS_TRACK) {
            audio_sfx(DK_SFX_WHOOSH, 160, 128);
            title_tracks_enter();
        }
        else {
            game_set_state(ST_TITLE);
        }
    }
    else if(pressed & BTN_B) {
        audio_sfx(DK_SFX_MENU_BACK, 200, 128);
        race_setup();
        game_set_state(ST_TITLE);
    }
}

/* A+B+X+Y+Start away from the title: straight back to it, leaving any
   online lobby on the way. */
static void soft_reset(void) {
    if(game.online) {
        net_end();
        spectate_set(false);
        game.online = false;
    }
    game.paused = false;
    race_setup();
    game_set_state(ST_TITLE);
    pad.pressed = pad2.pressed = 0;
}

static void update(float dt) {
    game.time += dt;
    game.title_t += dt;
    emote_input(dt);
    world_update(dt);
    hud_update(dt);
    if(game.online && spectate_on() && (game.state == ST_COUNTDOWN || game.state == ST_RACE || game.state == ST_RESULTS))
        spectate_update(dt);
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
        case ST_KARTS:
#ifdef DK_AT_NAME
            /* The name test goes online from the title a couple of seconds
               in, as a player would, with the boot read done by then. */
            if(game.state == ST_TITLE && game.time > 2.0f) { online_enter(); break; }
#endif
            title_update(dt);
            /* Idle karts on the grid still bounce a little. */
            for(int i = 0; i < kart_count; i++) {
                KartInput in = {0};
                kart_update(&karts[i], &in, dt, false);
            }
            break;
        case ST_FLAPPY:
            flappy_update(dt);
            break;
        case ST_INTRO:
            idle_karts_step(dt);
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
                idle_karts_step(dt);
                break;
            }
            if(game.paused) {
                pause_update();
                break;
            }
            if(start_pressed() >= 0) {
                game.paused = true;
                game.hud_hidden = false;
                game.pause_owner = start_pressed();
                game.pause_sel = 0;
                audio_music_target(60);
                break;
            }
            countdown_step(dt);
            idle_karts_step(dt);
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
                game.hud_hidden = false;
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
            bool done = false;
            if(home == game.num_players) {
                finish_wait += dt;
                done = finish_wait > 4.5f;
            }
            else if(home > 0 && game.humans_done_t > 20.0f) {
                done = true;
            }
            if(done) {
                finalize_results();
                if(gp.active) gp_race_done();
                game_set_state(ST_RESULTS);
            }
            if(home == 1 && finish_wait == 0) audio_music_target(game.num_players > 1 ? 130 : 90);
            break;
        }
        case ST_RESULTS:
            game.race_time += dt;
            race_step(dt, true);
            if(game.online) break;
            if(gp.active) gp_results_update();
            else results_menu_update();
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
        case ST_KARTS:
            title_kart_cameras();
            break;
        case ST_FLAPPY:
            flappy_camera(&game.cam);
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
            if(game.online && spectate_on()) {
                spectate_camera(&game.pl[0].cam, dt);
                break;
            }
            for(int p = 0; p < game.num_players; p++)
                chase_camera(&game.pl[p], game.paused ? 0 : dt, look_back[p] && !game.paused);
            break;
    }
}


/* Views this frame: one full-screen view, or a top/bottom split while two
   players race or pick their karts. Every view is drawn inside each PVR list in turn. */
static int view_count(void) {
    return split_screen() ? 2 : 1;
}

/* Racing or on the kart select, each player has their own camera. */
static bool player_views(void) {
    return game.state == ST_COUNTDOWN || game.state == ST_RACE || game.state == ST_KARTS;
}

static void set_view(int v) {
    /* Split: two 7-tile views with the HUD band (tile row 7) between. */
    if(view_count() == 2) r_set_viewport(v ? 256.0f : 0.0f, v ? 480.0f : 224.0f);
    else r_set_viewport(0, 480.0f);
    r_set_camera(player_views() ? &game.pl[v].cam : &game.cam);
}

static int view_kart(int v) {
    return player_views() ? game.pl[v].kart : game.player;
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
    PROF_BEGIN(pp);
    int views = view_count();
    /* FlappyKarts keeps the course's sky, clouds and mountains and draws
       its own ground, columns and kart; the scenery, sea and effects stay
       off for the frame. */
    bool flappy = game.state == ST_FLAPPY;
    uint32_t probe_saved = dk_probe_off;
    if(flappy) dk_probe_off |= PROBE_WORLD | PROBE_SEA | PROBE_FOAM | PROBE_PT | PROBE_SHADOWS | PROBE_FX;
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
    PROF_LAP(PS_PHASE_WORLD, pp);
    if(!(dk_probe_off & PROBE_KARTS))
        for(int v = 0; v < views; v++) {
            set_view(v);
            if(flappy) {
                flappy_draw_op();
                continue;
            }
            models_draw_karts(view_kart(v));
            PROF_BEGIN(pd);
            dolphins_draw();
            biplane_draw();
            PROF_LAP(PS_DOLPHINS, pd);
        }
    PROF_MARK(2);
    PROF_LAP(PS_PHASE_KARTS, pp);
    /* The pause menu's Y toggle hides the overlays like the -hud probe. */
    bool hud_off = (dk_probe_off & PROBE_HUD) || (game.paused && game.hud_hidden);
    if(perfhud_mode == PERF_OVERLAY && !hud_off && !(dk_probe_off & PROBE_PERFHUD)) {
        r_set_viewport(0, 480.0f);
        perfhud_draw(views > 1);
    }

    r_list(PVR_LIST_PT_POLY);
    if(!(dk_probe_off & PROBE_PT))
        for(int v = 0; v < views; v++) {
            set_view(v);
            world_draw_pt();
        }
    PROF_MARK(3);
    PROF_LAP(PS_PHASE_PT, pp);

    r_list(PVR_LIST_OP_MOD);
    if(!(dk_probe_off & PROBE_SHADOWS))
        for(int v = 0; v < views; v++) {
            set_view(v);
            models_draw_shadow_volumes(v);
        }
    PROF_LAP(PS_SHADOWS, pp);

    r_list(PVR_LIST_TR_POLY);
    for(int v = 0; v < views; v++) {
        set_view(v);
        world_draw_tr();
        biplane_draw_tr();
        if(!(dk_probe_off & PROBE_SHADOWS)) models_draw_shadows(v);
        if(!(dk_probe_off & PROBE_FX)) {
            items_draw();
            fx_draw();
        }
        if(flappy) flappy_draw_tr();
    }
    r_set_viewport(0, 480.0f);
    if(!hud_off) switch(game.state) {
        case ST_TITLE:
        case ST_MAIN:
        case ST_MENU:
        case ST_TRACKS:
        case ST_KARTS:
            title_draw();
            break;
        case ST_ONLINE:
            online_draw();
            break;
        case ST_FLAPPY:
            flappy_draw_hud();
            break;
        case ST_INTRO: {
            float a = clampf(game.time * 2.0f, 0, 1) * clampf((3.2f - game.time) * 2.0f, 0, 1);
            static const char *const cc_names[] = {"50cc", "100cc", "150cc"};
            hud_panel(0, 330, 640, 410, argb((int)(170 * a), 20, 70, 180), argb((int)(170 * a), 10, 30, 90), 15);
            hud_text(320 + (1 - a) * 200, 334, 1.2f, ALIGN_CENTER, color_alpha(argb(255, 255, 220, 60), a), track.name);
            char buf[64];
            const char *mode = game.mode == MODE_GP ? "GRAND PRIX" : game.mode == MODE_RACE ? "SINGLE RACE"
                               : game.num_players > 1 ? "VERSUS" : "TIME TRIAL";
            float s = 0.65f;
            if(gp.active) {
                snprintf(buf, sizeof(buf), "%sGRAND PRIX  -  RACE %d OF %d  -  %s", game.num_players > 1 ? "2P " : "",
                         gp.race + 1, gp.count, cc_names[game.cc]);
                s = 0.58f;
            }
            else {
                snprintf(buf, sizeof(buf), "%s%s  -  %s", game.num_players > 1 ? "2P " : "", mode, cc_names[game.cc]);
            }
            hud_text(320 - (1 - a) * 200, 378, s, ALIGN_CENTER, color_alpha(0xffffffff, a), buf);
            break;
        }
        case ST_COUNTDOWN:
        case ST_RACE:
            if(game.online && spectate_on()) {
                spectate_draw();
                hud_draw_countdown(0, 480);
            }
            else if(views == 1) {
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
            else if(gp.active) gp_draw_results();
            else hud_draw_results();
            break;
    }
    if(perfhud_mode == PERF_FPS && !hud_off && !(dk_probe_off & PROBE_PERFHUD)) perfhud_draw_fps(views > 1);
    r_frame_end();
    dk_probe_off = probe_saved;
    PROF_MARK(4);
    PROF_LAP(PS_PHASE_TR, pp);
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
    vmuscreen_update(0.05f);    /* the logo on the VMUs from the first loading screen */
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

/* ------------------------------------------------------------------ */
/* For autotest.c: the camera tours draw frames of their own and the demo
   recorder runs whole races. Only those builds have these, so elsewhere
   the loop's functions stay static (render() inlines into main()). */

#if defined(DK_AT_VIEWS) || defined(DK_AT_CAMTOUR)
void game_render(void) {
    render();
}
#endif

#ifdef DK_RECORD
void game_boot_screen(uint64_t t0, float progress, const char *what) {
    boot_screen(t0, progress, what);
}

/* A countdown step: the clock, and the karts waiting on the grid. */
void game_countdown_step(float dt) {
    countdown_step(dt);
    idle_karts_step(dt);
}

void game_race_step(float dt) {
    race_step(dt, true);
}
#endif

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    printf("\nDreamKarts %s starting (KallistiOS, %s %s)\n", DK_GIT_SHA, __DATE__, __TIME__);
    uint64_t boot_t0 = timer_us_gettime64();
    input_init();
    render_init();
    perfhud_reserve();
    hud_init();
    ui_init();
    title_init();
    flappy_init();
    boot_screen(boot_t0, 0.1f, "WAKING UP THE CHAO");
    audio_disc_init();   /* before the save worker: see audio.h */
    save_init();   /* the card is read while the rest loads */
    audio_music_volume(save.volume);
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
    items_init();
    dolphins_init();
    biplane_init();
    boot_screen(boot_t0, 0.7f, "TUNING STEEL DRUMS");
    audio_init();
    boot_screen(boot_t0, 1.0f, "READY!");
#ifdef DK_AT_FIXED
    game.title_t = 0;   /* the loading screen set it from the clock */
#endif
    printf("DreamKarts boot: %lu ms\n", (unsigned long)((timer_us_gettime64() - boot_t0) / 1000));

    online_init();
    game.cc = CC_100;
    game.setup_laps = game.laps = DEFAULT_LAPS;
    game.setup_items = true;
#ifdef DK_AT_NO_ITEMS
    game.setup_items = false;
#endif
    game.mode = MODE_GP;
#if defined(DK_AUTOTEST) || defined(DK_FILM)
    game.mode = MODE_RACE;   /* the scripted runs race one course */
#endif
#ifdef DK_AT_GP
    game.mode = MODE_GP;     /* ...or a whole Grand Prix */
#endif
    game.num_players = 1;
    game.pl[0].colour = 0;
    game.pl[1].colour = 1;
#if defined(DK_AT_2P) && defined(DK_CAPTURE)
    game.num_players = 2;
    game.pl[0].colour = 6;
#endif
#if defined(DK_AT_CC) && defined(DK_CAPTURE)
    game.cc = DK_AT_CC;     /* the menu starts on this class */
#endif
    race_setup();
#ifdef DK_AT_CAMTOUR
    camera_tour();
#endif
#ifdef DK_AT_VIEWS
    view_tour();
#endif
#ifdef DK_RECORD
    record_races(boot_t0);
    return 0;
#endif
#ifdef DK_AT_CUSTOM
    /* Online test: go in with a custom kart (LIME body, RED trim, BLACK
       Chao) so the lobby sends it and the race paints it. */
    save.custom_on = true;
    save.custom = (KartCustom){3, 0, 15};
    save_store();
    game.pl[0].colour = COLOUR_CUSTOM;
    race_setup();
#endif
#if defined(DK_AT_ONLINE) || defined(DK_FILM_SPECTATE)
    online_enter();
#elif defined(DK_AUTOTEST) && !defined(DK_CAPTURE) && !defined(DK_AT_NAME)
    game.cc = CC_150;
#ifdef DK_AT_CC
    game.cc = DK_AT_CC;
#endif
#ifdef DK_AT_TT
    game.mode = MODE_TT;
#endif
#ifdef DK_AT_2P
    game.num_players = 2;
#endif
#ifdef DK_AT_GP
    gp_start();
#else
    race_setup();
    game_set_state(ST_INTRO);
#endif
#else
    game_set_state(ST_TITLE);   /* opens on the theme */
    audio_music_target(210);
#endif

#ifdef DK_PROF
    prof_start();
#endif
    uint64_t last = timer_us_gettime64();
    uint64_t fps_t0 = last;
    int fps_frames = 0;
    /* Physics runs one step per displayed frame. pvr_wait_ready() paces
       the loop to the display, whose period is not 1/60 s (16.68 ms on an
       NTSC console, whatever the host gives in Flycast), so a fixed 1/60
       step would owe the clock a whole step every few seconds and run
       twice in one frame: a one-frame hitch of everything. The step is the
       measured vblank period instead, nudged by up to 1% toward the time
       still owed so game time stays on the clock; only a frame that missed
       vblanks takes more than one step. */
    float vbl_dt = DK_DT;   /* smoothed length of a single-vblank frame */
    float owed = 0;         /* clock time not yet stepped: timer jitter, normally */
    while(!game.quit) {
        pvr_wait_ready();
        uint64_t now = timer_us_gettime64();
        float dt_real = (float)(now - last) * 1e-6f;
        last = now;
        if(dt_real > 0.1f) dt_real = 0.1f;
        owed += dt_real;
        int steps = 1;
        if(dt_real < 1.5f * vbl_dt) {
            vbl_dt = clampf(vbl_dt + (dt_real - vbl_dt) * 0.01f, 0.8f * DK_DT, 1.25f * DK_DT);
        }
        else {
            steps = (int)(dt_real / vbl_dt + 0.5f);
            if(steps > 4) {   /* a stall: forget the time rather than replay it */
                steps = 4;
                owed = steps * vbl_dt;
            }
        }
        float dt = vbl_dt + clampf((owed / steps - vbl_dt) * 0.1f, -0.01f * vbl_dt, 0.01f * vbl_dt);
        owed = clampf(owed - steps * dt, -vbl_dt, vbl_dt);
#ifdef DK_AT_FIXED
        /* Repeatable captures: one 1/60 s step a frame whatever the host's
           timing, and the frame held still while a screenshot is taken. */
        dt_real = dt = DK_DT;
        steps = 1;
        if(at_hold_frame()) {
            steps = 0;
            dt_real = 0;
        }
#endif

        uint64_t upd_t0 = timer_us_gettime64();
        PROF_BEGIN(pu);
        PROF_BEGIN(pus);
        input_update();
        switch(softreset_poll(game.state == ST_TITLE)) {
            case SOFTRESET_TITLE:
                soft_reset();
                break;
            case SOFTRESET_QUIT:
                game.quit = true;
                continue;
        }
        if(save_tick()) {
            audio_music_enable(save.music);
            audio_music_volume(save.volume);
        }
        if(game.online) online_tick();
#if defined(DK_HW) && defined(DK_CAPTURE)
        if(probe_step(prof_last)) {
            steps = 0;
            at_skip = 1;
        }
        pvr_set_presort_mode((dk_probe_off & PROBE_AUTOSORT) == 0);
#endif
        PROF_LAP(PS_U_INPUT, pus);
        for(int s = 0; s < steps; s++) {
#ifdef DK_CAPTURE
            capture_script();
#endif
#ifdef DK_FILM
            film_step(dt);
#endif
            update(dt);
            pad.pressed = pad2.pressed = 0;
            pad.released = pad2.released = 0;
            input_step_taken();
        }
        PROF_LAP(PS_U_STEP, pus);
        update_camera(dt_real);
        PROF_LAP(PS_U_CAMERA, pus);
        audio_update(dt_real);
        bool engines = (game.state == ST_RACE || game.state == ST_COUNTDOWN || game.state == ST_RESULTS) &&
                       !game.paused;
        for(int p = 0; p < MAX_PLAYERS; p++)
            audio_kart_loops(p, engines && p < game.num_players ? &karts[game.pl[p].kart] : NULL, dt_real);
        biplane_audio(engines && game.state == ST_RACE);
        PROF_LAP(PS_U_AUDIO, pus);
        rumble_update(dt_real, (game.state == ST_RACE || game.state == ST_COUNTDOWN) && !game.paused);
        vmuscreen_update(dt_real);
        PROF_LAP(PS_U_OTHER, pus);
        prof_us[0] += timer_us_gettime64() - upd_t0;
        PROF_LAP(PS_PHASE_UPD, pu);
        render();
#ifdef DK_PROF
        prof_frame_end();
#endif

        uint64_t end = timer_us_gettime64();
        game.cpu_ms = (float)(end - now) * 0.001f;
        {
            /* This frame's share of the phase totals (the autotest zeroes
               them every log line), kept up while the overlay is off so
               switching it on doesn't start with one huge frame. */
            static uint64_t prof_prev[5];
            float phase_ms[5];
            for(int i = 0; i < 5; i++) {
                uint64_t d = prof_us[i] >= prof_prev[i] ? prof_us[i] - prof_prev[i] : prof_us[i];
                phase_ms[i] = (float)d * 0.001f;
                prof_prev[i] = prof_us[i];
            }
            int missed = dt_real > 1.5f * vbl_dt ? (int)(dt_real / vbl_dt + 0.5f) - 1 : 0;
#ifdef DK_AUTOTEST
            /* Leave out the frames a screenshot or log line stalled, as the
               autotest's own timing does. */
            if(at_skip == 0)
#endif
                if(perfhud_mode == PERF_OVERLAY) perfhud_frame(dt_real, game.cpu_ms, phase_ms, missed);
        }
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
        at_frame(dt_real, vbl_dt, steps);
#endif
    }
    save_finish();
    rumble_shutdown();
    audio_shutdown();
    render_shutdown();
    return 0;
}
