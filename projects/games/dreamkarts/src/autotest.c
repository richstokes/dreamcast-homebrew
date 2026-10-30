/* DreamKarts' scripted builds: the autotest's timing, checks and verdict,
   the capture script and its screenshots, the console's GPU probe, the
   camera tours and the demo race recorder. Each piece builds only with its
   own -D flags (README), so a release build has none of it; main.c calls
   in from its frame loop and race step. */
#include "autotest.h"
#include "audio.h"
#include "biplane.h"
#include "dolphins.h"
#include "flappy.h"
#include "fx.h"
#include "gp.h"
#include "hud.h"
#include "items.h"
#include "models.h"
#include "net.h"
#include "online.h"
#include "perfhud.h"
#include "prof.h"
#include "render.h"
#include "rumble.h"
#include "spectate.h"
#include "title.h"
#include "track.h"
#include "world.h"
#include <stddef.h>

#ifdef DK_AUTOTEST
#ifdef DK_HW
#define AT_LOG_SECS 10.0f   /* every printf under dcload-ip stalls the console */
#else
#define AT_LOG_SECS 2.0f
#endif
int at_skip;   /* frames to leave out of timing after slow I/O */
static struct {
    int drifts, boosts[4], walls, dash, laps, jumps, tricks;
    int items, uses, spears, mines, spun;
    float min_fps, cpu_sum, cpu_max;
    int cpu_n;
    float log_t;
    bool done;
    bool gp_done, gp_fail;   /* Grand Prix: verdict printed, a race failed */
    int win_frames, win_missed, frames, missed;
    int win_multi, win_zero, multi, zero;   /* frames that ran 2+ or 0 physics steps */
    float win_cpu_max, rnd_max_ms;
    size_t vtx_max;
    /* The drawn karts against the road as drawn: the deepest any tyre was
       drawn under it, and kart-frames by depth (clear, under 1, 2, 5,
       10 cm and over). */
    float sink_max;
    int sink_n, sink_hist[6];
} at = {.min_fps = 999};
#endif
#ifdef DK_AT_FIXED
static int at_hold;   /* frames to hold still for a screenshot */
#endif

#ifdef DK_CAPTURE
/* Flycast: print a marker for tools/capture.py to grab the window.
   Hardware: write the displayed framebuffer to dc-tool-ip's /pc/ share. */
void take_shot(const char *name) {
#ifdef DK_HW
    char path[48];
    snprintf(path, sizeof(path), "/pc/dk-%s.ppm", name);
    vid_screen_shot(path);
    at_skip = 3;
#else
#ifdef DK_AT_FIXED
    /* State fingerprint: the karts' motion and race state and the random
       generator; runs of any build of the same game must print the same. */
    uint32_t h = 2166136261u;
    for(int k = 0; k < MAX_KARTS; k++) {
        const Kart *kt = &karts[k];
        float f[] = {kt->pos.x, kt->pos.y, kt->pos.z, kt->vel.x, kt->vel.z, kt->vy, kt->heading, kt->speed,
                     kt->steer, kt->charge, kt->progress, kt->vis_yaw, kt->boost_t, kt->emote_bob};
        int n[] = {kt->drift, kt->stage, kt->lap, kt->place};
        const uint8_t *b = (const uint8_t *)f;
        for(size_t i = 0; i < sizeof(f); i++) h = (h ^ b[i]) * 16777619u;
        b = (const uint8_t *)n;
        for(size_t i = 0; i < sizeof(n); i++) h = (h ^ b[i]) * 16777619u;
    }
    printf("DK SHOT %s\nDK STATE %s karts=%08lx rand=%08lx t=%.4f\n", name, name, (unsigned long)h,
           (unsigned long)dk_rand_peek(), (double)game.time);
    at_hold = 150;
#else
    printf("DK SHOT %s\n", name);
#endif
#endif
}

/* Scripted presses so a capture run walks title -> menu -> race on its own;
   "DK SHOT <name>" lines tell the capture tool when to grab the window. */
void capture_script(void) {
#ifdef DK_AT_SETTINGS
    title_settings_autotest(DK_DT);   /* title.c walks the menus instead */
    return;
#endif
#ifdef DK_AT_FLAPPY
    flappy_autotest();   /* flappy.c plays the mini game instead */
    return;
#endif
    static float t;
    static int step;
    static bool shot[72];
    t += DK_DT;
    const Kart *p = &karts[game.player];
    /* Power-ups: an emerald coming up, the gem held, a spear in flight,
       the barrier, a kart spun out nearby and a mine on the road ahead. */
    if(game.state == ST_RACE && game.items && !shot[40] && game.race_time > 3.0f)
        for(int b = 0; b < track.nbands; b++) {
            V3 ep;
            float ahead = track_delta(p->tp.s, track.bands[b].s);
            if(!items_emerald(b, &ep, NULL) || ahead < 11.0f || ahead > 16.0f || fabsf(ep.y - p->pos.y) > 3.0f) continue;
            /* Only with a clear view: nobody between us and it to take it first. */
            bool clear = true;
            for(int i = 0; i < kart_count; i++) {
                float a = track_delta(p->tp.s, karts[i].tp.s);
                if(i != game.player && a > -2.0f && a < ahead + 6.0f) clear = false;
            }
            if(!clear) continue;
            shot[40] = true;
            take_shot("emerald");
        }
    if(game.state == ST_RACE && p->item != ITEM_NONE && p->item_t > 0.05f && !shot[41]) { shot[41] = true; take_shot("item"); }
    static float spear_shot_t;
    if(game.state == ST_RACE && (p->events & EV_SPEAR)) spear_shot_t = 0.001f;
    if(spear_shot_t > 0 && (spear_shot_t += DK_DT) > 0.35f && !shot[42]) { shot[42] = true; take_shot("spear"); }
    if(game.state == ST_RACE && p->shield_t > SHIELD_LEN - 0.6f && !shot[43]) { shot[43] = true; take_shot("barrier"); }
    if(game.state == ST_RACE && !shot[48]) {
        float m = items_mine_ahead(p);
        if(m > 16.0f && m < 24.0f) { shot[48] = true; take_shot("mine"); }
    }
    /* Our own mine: a look behind once it has landed (-DDK_AT_MINE_TEST
       hands us one). */
    static float mine_t;
    if(game.state == ST_RACE && (p->events & EV_MINE) && !shot[49]) mine_t = 0.001f;
    if(mine_t > 0 && (mine_t += DK_DT) > 0.05f && mine_t < 1.2f && !game.paused) {
        if(mine_t < 0.05f + DK_DT * 1.5f) pad.pressed |= BTN_L | BTN_R;
        pad.held |= BTN_L | BTN_R;
        if(mine_t > 0.5f && !shot[49]) { shot[49] = true; take_shot("mine-behind"); }
    }
    if(game.state == ST_RACE && !shot[44])
        for(int i = 0; i < kart_count; i++)
            if(karts[i].spin_t > 0.45f && karts[i].spin_t < 0.7f && vlen(vsub(karts[i].pos, p->pos)) < 22.0f) {
                shot[44] = true;
                take_shot("spin");
            }
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
#ifdef DK_AT_LAPS
        /* Walk the setup menu like a player: time trial with DK_AT_TT, then
           down to LAPS and right until it reads DK_AT_LAPS (3, 5 or 10). */
        static float tap_t, laps_t;
        static int taps;
        if(game.time > 2.3f && game.setup_laps != DK_AT_LAPS && (tap_t += DK_DT) > 0.3f) {
            tap_t = 0;
#ifdef DK_AT_TT
            if(game.mode != MODE_TT) input_film_tap(0, BTN_RIGHT);
            else
#endif
            input_film_tap(0, taps++ < 2 ? BTN_DOWN : BTN_RIGHT);
        }
        if(game.setup_laps == DK_AT_LAPS) laps_t += DK_DT;
        if(laps_t > 0.6f && !shot[28]) { shot[28] = true; take_shot("menu-laps"); }
        if(shot[28] && laps_t > 1.2f && step < 1) { pad.pressed |= BTN_START; step++; }
#else
        if(game.time > 3.5f && step < 1) { pad.pressed |= BTN_START; step++; }
#endif
    }
    if(game.state == ST_KARTS) {
        /* Two-player kart select: both ready up. */
        if(game.time > 1.5f && !shot[46]) { shot[46] = true; take_shot("karts"); }
        if(game.time > 2.0f) { pad.pressed |= BTN_A; pad2.pressed |= BTN_A; }
        if(game.time > 2.1f && !shot[47]) { shot[47] = true; take_shot("karts-ready"); }
    }
    if(game.state == ST_TRACKS) {
        if(game.time > 2.5f && !shot[9]) { shot[9] = true; take_shot("tracks"); }
        if(game.time > 4.0f && step < 2) { pad.pressed |= BTN_START; step++; }
    }
    if(game.state == ST_RACE && p->big_air && p->pos.y > 6.0f && !shot[10]) { shot[10] = true; take_shot("jump"); }
    if(game.state == ST_INTRO && game.time > 1.6f && !shot[2]) { shot[2] = true; take_shot("intro"); }
    if(game.state == ST_COUNTDOWN && game.countdown > 1.2f && !shot[7]) { shot[7] = true; take_shot("countdown"); }
    if(game.countdown > 3.15f && game.countdown < 3.5f && !shot[17]) { shot[17] = true; take_shot("go"); }
    /* Wave on the grid (Y); the nearest CPU waves back. */
    if(game.state == ST_COUNTDOWN && !game.online && game.countdown > 0.2f && p->wave_t <= 0 && !shot[20])
        pad.pressed |= BTN_Y;
    if(game.state == ST_COUNTDOWN && p->wave_t > 0 && p->wave_t < 1.2f && !shot[20]) { shot[20] = true; take_shot("wave"); }
    if(game.state == ST_COUNTDOWN && p->wave_t > 0 && p->wave_t < 1.0f && !shot[22]) {
        shot[22] = true;
        take_shot("wave2");
        pad.pressed |= BTN_Y;   /* the left arm takes over */
    }
    if(game.state == ST_COUNTDOWN && shot[22] && p->arm_up[0] > 0.95f && !shot[23]) { shot[23] = true; take_shot("wave-left"); }
    /* A win: the cheer from the chase camera before the results cover it. */
    if(game.state == ST_RACE && p->finished && !shot[21]) {
        if(kart_cheering(p) && p->cheer_t > 1.3f) { shot[21] = true; take_shot("cheer"); }
        else if(!kart_cheering(p) && p->place > 1) { shot[21] = true; printf("DK CAPTURE: no cheer shot, finished %d\n", p->place); }
    }
    if(game.state == ST_RACE && p->drift == DRIFT_ACTIVE && p->stage >= 3 && !shot[3]) { shot[3] = true; take_shot("drift"); }
    if(game.state == ST_RACE && p->boost_t > 0.3f && game.race_time > 8.0f && !shot[4]) { shot[4] = true; take_shot("boost"); }
    if(game.state == ST_RACE && track_section(p->tp.s) == SEC_BOARDWALK && !shot[5] && game.race_time > 12.0f) { shot[5] = true; take_shot("boardwalk"); }
    if(game.state == ST_RACE && dolphins_show_time() > 0.85f && !shot[11]) { shot[11] = true; take_shot("dolphins"); }
    if(game.state == ST_RACE && dolphins_show_time() > 3.65f && !shot[12]) { shot[12] = true; take_shot("dolphins2"); }
    /* The biplane: every 0.8 s of its show, through the S and the fade. */
    {
        float bt = biplane_show_time();
        int k = bt > 0.8f ? (int)(bt / 0.8f) - 1 : -1;
        static bool bshot[16];
        if(game.state == ST_RACE && k >= 0 && k < 15 && !bshot[k]) {
            bshot[k] = true;
            char name[16];
            snprintf(name, sizeof(name), "biplane%02d", k);
            take_shot(name);
        }
    }
    /* Into lap 2, where a time trial's clock reads the lap against BEST. */
    if(game.state == ST_RACE && p->lap == 2 && game.race_time - p->lap_start > 5.0f && !shot[63]) {
        shot[63] = true;
        take_shot("lap2");
    }
    if(game.state == ST_RESULTS && game.time > 1.5f && !shot[6]) { shot[6] = true; take_shot(gp.active ? "gp-results" : "results"); }
#ifdef DK_AT_GP
    /* Grand Prix: the standings after the first race, the next race's intro
       with the grid in standings order, and the final standings. */
    if(game.state == ST_RESULTS && gp.active && gp.page == 1 && game.time > 1.5f) {
        if(!gp_over() && !shot[52]) { shot[52] = true; take_shot("gp-standings"); }
        if(gp_over() && !shot[53]) { shot[53] = true; take_shot("gp-final"); }
    }
    if(game.state == ST_INTRO && gp.race == 1 && game.time > 1.6f && !shot[54]) { shot[54] = true; take_shot("gp-intro2"); }
    if(game.state == ST_COUNTDOWN && gp.race == 1 && game.countdown > 0.3f && !shot[55]) { shot[55] = true; take_shot("gp-grid2"); }
#endif
#ifdef DK_AT_NEXT_TRACK
    /* Single race: NEW TRACK from the results, the next course along, and
       race that too (the summary comes after the second race). */
    static int again;
    if(game.state == ST_RESULTS && !gp.active && again == 0 && game.time > 2.0f) { input_film_tap(0, BTN_RIGHT); again = 1; }
    if(game.state == ST_RESULTS && again == 1 && game.time > 2.6f) { pad.pressed |= BTN_A; again = 2; }
    if(game.state == ST_TRACKS && again == 2 && game.time > 1.5f) { input_film_tap(0, BTN_RIGHT); again = 3; }
    if(game.state == ST_TRACKS && again == 3 && game.time > 3.0f) { take_shot("tracks-again"); again = 4; }
    if(game.state == ST_TRACKS && again == 4 && game.time > 3.8f) { pad.pressed |= BTN_START; again = 5; }
#endif
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
    /* Neon Skyway: the waves, up the loop, over the crossing and the
       plunge. */
    if(game.state == ST_RACE && track.in_sky && !p->grounded && p->pos.y < track_road_y(p->tp.s, 0) - 4.0f &&
       !shot[27]) { shot[27] = true; take_shot("fall"); }
    if(game.state == ST_RACE && track.id == TRACK_SKYWAY) {
        static const struct { int seg; float at; const char *name; } sky_shots[4] = {
            {2, 40.0f, "waves"}, {3, 70.0f, "loop"}, {4, 22.0f, "crossing"}, {4, 46.0f, "plunge"}};
        for(int i = 0; i < 4; i++)
            if(!shot[23 + i] && fabsf(track_delta(track_seg_s(sky_shots[i].seg) + sky_shots[i].at, p->tp.s)) < 3.0f) {
                shot[23 + i] = true;
                take_shot(sky_shots[i].name);
            }
    }
    /* Spiral Canyon: into the first tunnel, up the corkscrew, through the
       summit hoop, down the second corkscrew and out of its tunnel, and the
       chicane on the long way home. */
    if(game.state == ST_RACE && track.id == TRACK_CANYON) {
        static const struct { int seg; float at; const char *name; } dr_shots[11] = {
            {2, 2.0f, "ds-tunnel"}, {3, 70.0f, "ds-corkscrew"}, {6, 4.0f, "ds-tunnel2"}, {4, 20.0f, "ds-summit"},
            {5, 100.0f, "ds-descent"}, {9, 2.0f, "ds-chicane"}, {3, 170.0f, "ds-g1"}, {5, 6.0f, "ds-g2"},
            {7, 10.0f, "ds-g3"}, {14, 30.0f, "ds-g4"}, {8, 6.0f, "ds-g5"}};
        for(int i = 0; i < 11; i++)
            if(!shot[34 + (i < 6 ? i : i + 10)] && fabsf(track_delta(track_seg_s(dr_shots[i].seg) + dr_shots[i].at, p->tp.s)) < 3.0f) {
                shot[34 + (i < 6 ? i : i + 10)] = true;
                take_shot(dr_shots[i].name);
            }
    }
    /* Tsudoken Drift: under the torii at the line past the grandstand, turn
       1's hairpin, out of turn 4 under the second torii, the long lefts
       with the pagoda behind, turn 8, the run at the jump with Fuji ahead,
       and the sweeper home. */
    if(game.state == ST_RACE && track.id == TRACK_TSUDOKEN) {
        static const struct { int seg; float at; const char *name; } tk_shots[7] = {
            {0, 25.0f, "tk-grandstand"}, {1, 20.0f, "tk-hairpin"}, {8, 10.0f, "tk-torii"}, {11, 20.0f, "tk-lefts"},
            {15, 20.0f, "tk-turn8"}, {16, 60.0f, "tk-straight"}, {17, 60.0f, "tk-sweeper"}};
        for(int i = 0; i < 7; i++)
            if(!shot[56 + i] && fabsf(track_delta(track_seg_s(tk_shots[i].seg) + tk_shots[i].at, p->tp.s)) < 3.0f) {
                shot[56 + i] = true;
                take_shot(tk_shots[i].name);
            }
    }
    /* Apple Valley: race control at the line, the turn 2 hairpin, turn 4,
       up the west side, turn 9's hairpin, the long left at 12, turn 13 onto
       the top straight and turn 14 at its end. */
    if(game.state == ST_RACE && track.id == TRACK_APPLE) {
        static const struct { int seg; float at; const char *name; } av_shots[8] = {
            {0, 95.0f, "av-line"}, {3, 6.0f, "av-hairpin2"}, {7, 4.0f, "av-turn4"}, {10, 6.0f, "av-west"},
            {17, 10.0f, "av-hairpin9"}, {21, 20.0f, "av-turn12"}, {24, 10.0f, "av-top"}, {25, 8.0f, "av-hairpin14"}};
        for(int i = 0; i < 8; i++)
            if(!shot[64 + i] && fabsf(track_delta(track_seg_s(av_shots[i].seg) + av_shots[i].at, p->tp.s)) < 3.0f) {
                shot[64 + i] = true;
                take_shot(av_shots[i].name);
            }
    }
    /* Cyberstone: the first chicane, the crest, down the drop, the
       National straight's chicane and round the banked Luffield helix. */
    if(game.state == ST_RACE && track.id == TRACK_CYBERSTONE) {
        static const struct { int seg; float at; const char *name; } cs_shots[5] = {
            {1, 2.0f, "cs-chicane"}, {6, 30.0f, "cs-crest"}, {6, 90.0f, "cs-drop"}, {11, 2.0f, "cs-national"},
            {17, 40.0f, "cs-luffield"}};
        for(int i = 0; i < 5; i++)
            if(!shot[29 + i] && fabsf(track_delta(track_seg_s(cs_shots[i].seg) + cs_shots[i].at, p->tp.s)) < 3.0f) {
                shot[29 + i] = true;
                take_shot(cs_shots[i].name);
            }
    }
#ifndef DK_HW
    /* Pause mid-race, switch the music off and the performance overlay on,
       grab the menu (the overlay behind it has had time to fill in), then
       step PERF on through FPS back to off and switch the music back. A
       -DDK_PERF_HUD build leaves the overlay alone. */
    static float pause_t;
    if(game.state == ST_RACE && game.race_time > 6.0f && !shot[8] && !game.online) {
        if(!game.paused) { pad.pressed |= BTN_START; pause_t = 0; }
        else {
            pause_t += DK_DT;
            /* The menu as it opens, before the overlay (which Flycast's
               window stops presenting under) goes on. */
            if(pause_t > 0.25f && !shot[64]) { shot[64] = true; take_shot("pause-menu"); }
            game.pause_sel = pause_t < 0.6f ? PAUSE_MUSIC : pause_t < 1.3f ? PAUSE_PERF : PAUSE_NEXT_SONG;
            if(pause_t > 0.3f && audio_music_enabled() && pause_t < 0.6f) pad.pressed |= BTN_A;
#ifndef DK_PERF_HUD
            if(pause_t > 0.7f && perfhud_mode != PERF_OVERLAY && pause_t < 1.0f) pad.pressed |= BTN_A;
#endif
            /* NEXT SONG once, with the music off: the song changes, paused. */
            static bool skipped;
            if(pause_t > 1.4f && !skipped) { skipped = true; pad.pressed |= BTN_A; }
            if(pause_t > 1.8f) { shot[8] = true; take_shot("pause"); }
        }
    }
    if(shot[8] && game.paused && (pause_t += DK_DT) > 2.2f) {
#ifndef DK_PERF_HUD
        if(perfhud_mode != PERF_OFF) { game.pause_sel = PAUSE_PERF; pad.pressed |= BTN_A; }
        else
#endif
        if(!audio_music_enabled()) { game.pause_sel = PAUSE_MUSIC; pad.pressed |= BTN_A; }
        else pad.pressed |= BTN_START;
    }
    /* Squeeze both triggers for a look behind. */
    static float look_t;
    if(game.state == ST_RACE && shot[3] && shot[4] && shot[8] && !game.paused && (look_t += DK_DT) > 1.0f &&
       look_t < 3.0f) {
        if(look_t < 1.0f + DK_DT * 1.5f) pad.pressed |= BTN_L | BTN_R;
        pad.held |= BTN_L | BTN_R;
        if(look_t > 2.0f && !shot[45]) { shot[45] = true; take_shot("lookback"); }
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
    {"-alltr", PROBE_DISTANT | PROBE_FOAM | PROBE_SHADOWS | PROBE_FX | PROBE_HUD}, {"-ta", PROBE_NOTA},
    {"-sheen", PROBE_SHEEN},
#ifdef DK_PERF_HUD
    {"-perfhud", PROBE_PERFHUD},
#endif
};
#define PROBE_CFGS ((int)(sizeof(probe_cfg) / sizeof(probe_cfg[0])))
#define PROBE_FRAMES 24
#define PROBE_SETTLE 4
static struct {
    const char *scene;
    int cfg, frame;
    float sum[PROBE_CFGS];
    int n[PROBE_CFGS];
    float cpu[PROBE_CFGS], world[PROBE_CFGS], karts[PROBE_CFGS];
    bool done[3];
} probe;

/* Returns true while probing (the simulation stays frozen); last_us is
   main.c's latest frame's phase times. */
bool probe_step(const uint32_t *last_us) {
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
        probe.world[probe.cfg] += last_us[1] * 0.001f;
        probe.karts[probe.cfg] += last_us[2] * 0.001f;
        probe.n[probe.cfg]++;
    }
    if(++probe.frame >= PROBE_FRAMES) {
        probe.frame = 0;
        if(++probe.cfg >= PROBE_CFGS) {
            printf("DK GPU %s:", probe.scene);
            for(int i = 0; i < PROBE_CFGS; i++)
                printf(" %s=%.2f", probe_cfg[i].name, (double)(probe.n[i] ? probe.sum[i] / probe.n[i] : 0));
            printf(" (cpu base %.2f)\n", (double)(probe.n[0] ? probe.cpu[0] / probe.n[0] : 0));
            /* CPU ms a frame per configuration: total/world OP/karts. */
            printf("DK CPU %s:", probe.scene);
            for(int i = 0; i < PROBE_CFGS; i++) {
                float n = probe.n[i] ? 1.0f / probe.n[i] : 0;
                printf(" %s=%.2f/%.2f/%.2f", probe_cfg[i].name, (double)(probe.cpu[i] * n), (double)(probe.world[i] * n),
                       (double)(probe.karts[i] * n));
            }
            printf("\n");
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
/* The tests that drive the player's kart themselves: its inputs once its
   CPU driver has set them (hp: the player at the wheel, -1 for a CPU). */
void at_kart_input(Kart *k, int hp, KartInput *in, float dt) {
#ifdef DK_AT_WRONG_WAY
    /* Turn the player round 10 s in (player 2 at 12 s), drive it back
       up the track for 5 s, then turn it round again for the CPU (which
       can't find its way back from facing dead backwards), to check the
       wrong way banner comes and goes. */
    float ww0 = 10.0f + 2.0f * (float)hp;
    if(hp >= 0 && game.race_time >= ww0 && game.race_time - dt < ww0 + 5.0f) {
        if(game.race_time - dt < ww0 || game.race_time >= ww0 + 5.0f) {
            k->heading = wrap_angle(k->heading + DK_PI);
            k->vel = vscale(heading_fwd(k->heading), k->speed);
        }
        if(game.race_time < ww0 + 5.0f) {
            memset(in, 0, sizeof(*in));
            in->accel = true;
        }
    }
#endif
#ifdef DK_AT_FALL
    /* Steer the player off the open edge on the first lap's waves, to
       check the drop back onto the road. */
    if(hp == 0 && track.in_sky && k->lap == 1 && fabsf(track_delta(track_seg_s(2) + 60.0f, k->tp.s)) < 25.0f)
        in->steer = 1.0f;
#endif
#ifdef DK_AT_WALLRIDE
    /* Ride the right wall all of lap 1 and the left all of lap 2, to
       check the rails hold (DK ESCAPE lines in the log if not). */
    if(hp == 0 && k->lap >= 1) in->steer = (k->lap & 1) ? 1.0f : -1.0f;
#endif
#ifdef DK_AT_ZIGZAG
    /* Slam the rails at an angle every couple of seconds. */
    if(hp == 0) in->steer = fmodf(game.time, 3.0f) < 1.5f ? 1.0f : -1.0f;
#endif
#ifdef DK_AT_BRAKE
    /* Stand on the brakes for a second every 6 s, for the screech. */
    if(hp == 0 && fmodf(game.race_time, 6.0f) > 5.0f) {
        in->accel = false;
        in->brake = true;
    }
#endif
}

/* After each race step: the power-ups the item tests hand out, and the
   player's events counted for the verdict. */
void at_race_step(void) {
#ifdef DK_AT_SHIELD_TEST
    /* A Barrier for the player every 12 s, to measure its fill on the console. */
    {
        static float next = 5.0f;
        if(game.state == ST_RACE && game.race_time >= next) {
            next += 12.0f;
            karts[game.player].item = ITEM_SHIELD;
            karts[game.player].item_t = 10.0f;
        }
    }
#endif
#ifdef DK_AT_MINE_TEST
    /* A Mine every 12 s (or every -DDK_AT_MINE_TEST=<s>), for the player
       and the kart ahead of it in turn (the CPU drops it at once): the
       capture looks behind at the one and drives at the other. */
    {
        static float next = 5.0f;
        static int round;
        if(game.state == ST_RACE && game.race_time >= next) {
            next += DK_AT_MINE_TEST > 1 ? (float)DK_AT_MINE_TEST : 12.0f;
            int who = game.player;
            if(round++ & 1) {
                float best = 30.0f;
                for(int i = 0; i < kart_count; i++) {
                    float ahead = track_delta(karts[game.player].tp.s, karts[i].tp.s);
                    if(i != game.player && ahead > 3.0f && ahead < best) { best = ahead; who = i; }
                }
            }
            karts[who].item = ITEM_MINE;
            karts[who].item_t = 10.0f;
        }
    }
#endif
    uint32_t ev = karts[game.player].events;
    if(ev & EV_DRIFT_START) at.drifts++;
    if(ev & EV_BOOST) at.boosts[karts[game.player].boost_stage & 3]++;
    if(ev & EV_WALL) at.walls++;
    if(ev & EV_DASH) at.dash++;
    if(ev & EV_LAP) at.laps++;
    if(ev & EV_JUMP) at.jumps++;
    if(ev & EV_TRICK_BOOST) at.tricks++;
    if(ev & EV_ITEM_GET) at.items++;
    if(ev & EV_ITEM_USE) at.uses++;
    if(ev & EV_SPEAR) at.spears++;
    if(ev & EV_MINE) at.mines++;
    if(ev & EV_SPIN) at.spun++;
}

/* The wrong way banner coming and going (-DDK_AT_WRONG_WAY turns the
   player round to show it). */
void at_player_events(int pi) {
    const Player *pl = &game.pl[pi];
    const Kart *p = &karts[pl->kart];
    static bool shown[MAX_PLAYERS];
    if(shown[pi] != (pl->wrong_way_t > 1.0f)) {
        shown[pi] = !shown[pi];
        printf("DK WRONG WAY P%d %s t=%.1f s=%.0f\n", pi + 1, shown[pi] ? "on" : "off", (double)game.race_time,
               (double)p->tp.s);
    }
#ifdef DK_CAPTURE
    static bool shot[MAX_PLAYERS];
    if(pl->wrong_way_t > 1.6f && !shot[pi]) {
        shot[pi] = true;
        take_shot(pi ? "wrong-way-p2" : "wrong-way");
    }
#endif
}

/* Once a displayed frame: dropped vblanks, physics steps, CPU and PVR
   time, the drawn karts against the drawn road, a timing line every few
   seconds, and the verdict once the race (or the Grand Prix) is over. */
void at_frame(float dt_real, float vbl_dt, int steps) {
    static const char *const state_names[] = {"title", "menu", "tracks", "intro", "countdown", "race", "results",
                                             "main", "online", "karts", "flappy"};
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
        if(steps >= 2) {
            at.win_multi++;
            at.multi++;
        }
        else if(steps == 0) {
            at.win_zero++;
            at.zero++;
        }
        if(game.cpu_ms > at.win_cpu_max) at.win_cpu_max = game.cpu_ms;
        pvr_stats_t ps;
        if(pvr_get_stats(&ps) == 0 && ps.rnd_last_time < 100000000ull) {
            float rnd = (float)ps.rnd_last_time * 1e-6f;
            if(rnd > at.rnd_max_ms) at.rnd_max_ms = rnd;
#ifdef DK_AT_PVRSPIKE
            /* Where a slow render happens, at most one line in 4 s
               (each printf under dcload stalls the console). */
            static float spike_t = -10.0f;
            if(rnd > DK_AT_PVRSPIKE && game.state == ST_RACE && game.race_time - spike_t > 2.5f) {
                spike_t = game.race_time;
                printf("DK PVRSPIKE t=%.1f rnd=%.2fms s=%.0f d=%.1f lap=%d heading=%.2f tris=%d verts=%d\n",
                       (double)game.race_time, (double)rnd, (double)p->tp.s, (double)p->tp.d, p->lap, (double)p->heading,
                       rstats.tris, rstats.verts);
            }
#endif
            if(ps.vtx_buffer_used_max > at.vtx_max) at.vtx_max = ps.vtx_buffer_used_max;
        }
        if(game.state == ST_RACE || game.state == ST_RESULTS) {
            at.cpu_sum += game.cpu_ms;
            at.cpu_n++;
            if(game.cpu_ms > at.cpu_max) at.cpu_max = game.cpu_ms;
            if(game.fps > 0 && game.fps < at.min_fps && game.race_time > 2.0f) at.min_fps = game.fps;
        }
        if(game.state == ST_RACE) {
            for(int i = 0; i < kart_count; i++) {
                const Kart *k = &karts[i];
                if(k->gone || !k->grounded) continue;
                float sink = models_kart_sink(k);
                if(sink < -1e8f) continue;
                if(sink > at.sink_max) at.sink_max = sink;
                at.sink_n++;
                at.sink_hist[sink <= 0.0f ? 0 : sink < 0.01f ? 1 : sink < 0.02f ? 2 : sink < 0.05f ? 3 : sink < 0.10f ? 4 : 5]++;
                if(sink > 0.10f && sink >= at.sink_max)
                    printf("DK AT SINK kart %d s %.1f d %.1f sink %.3f pitch %.2f roll %.2f sq %.2f spd %.1f air %.2f "
                           "y %.2f road %.2f drawn %.2f bank %.3f\n", i, (double)k->tp.s, (double)k->tp.d, (double)sink,
                           (double)k->pitch, (double)k->roll, (double)k->squash, (double)k->speed, (double)k->air_t,
                           (double)k->pos.y, (double)track_road_y(k->tp.s, k->tp.d), (double)world_surface_y(k->tp.s, k->tp.d),
                           (double)track_bank(k->tp.s));
            }
        }
    }
    at.log_t += dt_real;
    if(at.log_t >= AT_LOG_SECS) {
        at.log_t = 0;
        float f = prof_frames ? 1.0f / (prof_frames * 1000.0f) : 0;
        printf("DK AT %s t=%.1f fps=%.1f missed=%d/%d multi=%d zero=%d step=%.3fms cpu=%.2fms max=%.2fms "
               "[upd %.2f world %.2f karts %.2f pt %.2f tr %.2f] pvr=%.2fms tris=%d lap=%d place=%d spd=%.1f "
               "drift=%d stage=%d\n",
               state_names[game.state], (double)game.race_time, (double)game.fps, at.win_missed,
               at.win_frames, at.win_multi, at.win_zero, (double)(vbl_dt * 1000.0f), (double)game.cpu_ms,
               (double)at.win_cpu_max, (double)(prof_us[0] * f),
               (double)(prof_us[1] * f), (double)(prof_us[2] * f), (double)(prof_us[3] * f),
               (double)(prof_us[4] * f), (double)at.rnd_max_ms, rstats.tris, p->lap, p->place,
               (double)p->speed, p->drift, p->stage);
#ifdef DK_PROF
        prof_print();
#endif
#ifdef DK_XF_CHECK
        {
            extern uint32_t xf_checked, xf_mismatch;
            printf("DK XFCHECK checked=%lu mismatch=%lu\n", (unsigned long)xf_checked,
                   (unsigned long)xf_mismatch);
        }
#endif
        memset(prof_us, 0, sizeof(prof_us));
        prof_frames = 0;
        at.win_frames = at.win_missed = at.win_multi = at.win_zero = 0;
        at.win_cpu_max = at.rnd_max_ms = 0;
        at_skip = 2;
    }
#ifdef DK_AT_GP
    /* A Grand Prix: a summary per race, the verdict with the final standings. */
    if(game.state == ST_INTRO && at.done) {
        at.done = false;
        at.drifts = at.walls = at.dash = at.laps = at.jumps = at.tricks = at.items = at.uses = at.spears = at.spun = 0;
        memset(at.boosts, 0, sizeof(at.boosts));
    }
    if(game.state == ST_RESULTS && gp.active && gp_over() && gp.page == 1 && game.time > 2.5f && !at.gp_done) {
        at.gp_done = true;
        /* Audit: every total is the sum of its places' points, every
           race handed out one of each place. */
        static const int pts[MAX_KARTS] = {10, 8, 6, 4, 2, 1};
        bool ok = !at.gp_fail && gp.race == gp.count;
        for(int i = 0; i < kart_count; i++) {
            int sum = 0;
            for(int r = 0; r < gp.race; r++) sum += pts[gp.history[r][i] - 1];
            if(sum != gp.points[i]) ok = false;
            if(i > 0 && gp.points[gp.rank[i]] > gp.points[gp.rank[i - 1]]) ok = false;
        }
        for(int r = 0; r < gp.race; r++) {
            int seen = 0;
            for(int i = 0; i < kart_count; i++) seen |= 1 << (gp.history[r][i] - 1);
            if(seen != (1 << kart_count) - 1) ok = false;
        }
        printf("DK AUTOTEST: grand prix %d races, player %d%s with %d points\n", gp.race,
               gp_standing(game.player), hud_ordinal(gp_standing(game.player)), gp.points[game.player]);
        for(int i = 0; i < kart_count; i++) {
            int k = gp.rank[i];
            printf("DK AUTOTEST: %d. %s %d pts, places", i + 1, karts[k].name, gp.points[k]);
            for(int r = 0; r < gp.race; r++) printf(" %d", gp.history[r][k]);
            printf("\n");
        }
        printf("DK AUTOTEST: %s\n", ok ? "PASS" : "FAIL");
#ifdef DK_HW
        game.quit = true;   /* hand the console back to dcload-ip */
#endif
    }
#endif
    if(game.state == ST_RESULTS && game.time > 3.0f && !at.done && !game.online && (!gp.active || gp.page == 0)) {
        at.done = true;
        printf("DK AUTOTEST: player finish %.2fs place %d laps", (double)p->finish_time, p->place);
        for(int l = 0; l < game.laps; l++) printf(" %.2f", (double)p->lap_times[l]);
        printf("\nDK AUTOTEST: drifts %d boosts blue %d orange %d purple %d dash %d walls %d\n",
               at.drifts, at.boosts[1], at.boosts[2], at.boosts[3], at.dash, at.walls);
        printf("DK AUTOTEST: items %s: player took %d, used %d, fired %d spears, dropped %d mines, spun out %d; "
               "all karts took %d, used %d, fired %d, dropped %d, %d hits, %d spin-outs\n",
               game.items ? "on" : "off", at.items, at.uses, at.spears, at.mines, at.spun, items_got, items_used,
               items_fired, items_dropped, items_hits, items_spins);
        printf("DK AUTOTEST: rumble effects sent %d\n", rumble_sends);
        audio_autotest_report();
        printf("DK AUTOTEST: track %s, ramp jumps %d (player %d, %d barrel rolled), gap rescues %d, falls %d\n",
               track.name, kart_jumps, at.jumps, at.tricks, kart_rescues, kart_falls);
        {
            float n = (float)(at.sink_n ? at.sink_n : 1);
            printf("DK AUTOTEST: tyres vs drawn road: deepest %.3f m under; kart-frames clear %.1f%%, under 1 cm %.1f%%, "
                   "1-2 %.1f%%, 2-5 %.1f%%, 5-10 %.1f%%, over %.1f%%\n", (double)at.sink_max, 100.0 * at.sink_hist[0] / n,
                   100.0 * at.sink_hist[1] / n, 100.0 * at.sink_hist[2] / n, 100.0 * at.sink_hist[3] / n,
                   100.0 * at.sink_hist[4] / n, 100.0 * at.sink_hist[5] / n);
        }
        printf("DK AUTOTEST: fps min %.1f, missed vblanks %d of %d frames, frames with 2+ steps %d, "
               "0 steps %d, cpu avg %.2fms max %.2fms, vertex buffer peak %lu KiB\n", (double)at.min_fps,
               at.missed, at.frames, at.multi, at.zero, (double)(at.cpu_sum / (at.cpu_n ? at.cpu_n : 1)),
               (double)at.cpu_max, (unsigned long)(at.vtx_max / 1024));
        for(int i = 0; i < kart_count; i++)
            printf("DK AUTOTEST: %d. %s %.2f\n", i + 1, karts[game.finish_order[i]].name,
                   (double)karts[game.finish_order[i]].finish_time);
        /* 50 s a lap at 150cc; the slower classes get proportionally longer. */
        float limit = 50.0f * game.laps * cc_params[CC_150].top_speed / cc_params[game.cc].top_speed;
        bool ok = p->finish_time > 0 && p->finish_time < limit && at.drifts > 3 && kart_rescues == 0 &&
                  (!track.has_jump || at.jumps >= game.laps) &&
                  /* Tyres drawn 2 cm or more into the road (a sidewall) in over 10% of kart-frames:
                     the road ribbon no longer follows the road closely enough (Neon Skyway's sharp
                     bump and plunge keep it at about 5% through chassis lag alone). */
                  (at.sink_hist[3] + at.sink_hist[4] + at.sink_hist[5]) * 10 < (at.sink_n ? at.sink_n : 1);
#ifdef DK_AT_GP
        /* A failed race fails the cup there and then. */
        if(ok) printf("DK AUTOTEST: race %d/%d ok\n", gp.race, gp.count);
        else printf("DK AUTOTEST: FAIL (race %d/%d)\n", gp.race, gp.count);
        at.gp_fail |= !ok;
#else
        printf("DK AUTOTEST: %s\n", ok ? "PASS" : "FAIL");
#endif
#if defined(DK_HW) && !defined(DK_AT_GP)
        game.quit = true;   /* hand the console back to dcload-ip */
#endif
    }
}
#endif

#ifdef DK_AT_FIXED
/* A screenshot holds the frame still. Effects draw with frand() (flame
   lengths, drift glow), so each held frame replays the same random state. */
bool at_hold_frame(void) {
    static bool holding;
    static uint32_t hold_rand;
    if(at_hold > 0) {
        at_hold--;
        if(!holding) hold_rand = dk_rand_peek();
        else dk_seed(hold_rand);
        holding = true;
        return true;
    }
    holding = false;
    return false;
}
#endif

#ifdef DK_AT_VIEWS
/* Capture build: Spiral Canyon's rock from outside (=1), the chase camera
   round the lap (=2) or cameras inside the rock (=3). The fixed views are
   a camera position and a point to look at, each in world coordinates
   (seg -1: x, z, y) or track coordinates (segment, metres along it,
   lateral offset, height above the road there). */
static V3 view_pt(int seg, float at, float d, float h) __attribute__((unused));
static V3 view_pt(int seg, float at, float d, float h) {
    if(seg < 0) return v3(at, h, d);   /* world coordinates */
    float s = track_wrap(track_seg_s(seg) + at);
    V3 p = track_point(s, d);
    p.y = track_road_y(s, clampf(d, -9.0f, 9.0f)) + h;
    return p;
}

/* One shot: the camera set, a second's frames so the capture tool's
   screenshot lands well after the marker. */
static void tour_shot(const char *name, V3 pos, V3 look) {
    spectate_set_camera(&game.cam, pos, look, 1.1f);
#ifdef DK_HW
    /* On the console: no screenshots, the render time from this camera
       instead, one line per shot (every 2 s: each printf under dcload
       stalls the console). */
    float rsum = 0, rmax = 0, csum = 0, cmax = 0;
    int rn = 0;
    for(int f = 0; f < 120; f++) {
        pvr_wait_ready();
        world_update(DK_DT);
        uint64_t t0 = timer_us_gettime64();
        game_render();
        float cpu = (float)(timer_us_gettime64() - t0) * 0.001f;
        pvr_stats_t ps;
        if(f >= 30 && pvr_get_stats(&ps) == 0 && ps.rnd_last_time < 100000000ull) {
            float rnd = (float)ps.rnd_last_time * 1e-6f;
            rsum += rnd;
            rmax = fmaxf(rmax, rnd);
            csum += cpu;
            cmax = fmaxf(cmax, cpu);
            rn++;
        }
    }
    printf("DK HWVIEW %s pvr %.2f/%.2f render cpu %.2f/%.2f tris %d\n", name, (double)(rn ? rsum / rn : 0), (double)rmax,
           (double)(rn ? csum / rn : 0), (double)cmax, rstats.tris);
    return;
#endif
    for(int f = 0; f < 45; f++) {
        pvr_wait_ready();
        world_update(DK_DT);
        game_render();
        if(f == 12) printf("DK SHOT %s\n", name);
        if(f == 11) printf("DK SHOTSTATS %s tris=%d verts=%d\n", name, rstats.tris, rstats.verts);
#ifdef DK_CHUNK_STATS
        {
            extern void world_chunk_stats(bool print);
            if(f == 10) world_chunk_stats(false);   /* count one frame */
            if(f == 11) {
                printf("DK SHOTKIND %s ", name);
                world_chunk_stats(true);
            }
        }
#endif
    }
}

void view_tour(void) {
    static const struct { int seg; float at, d, h; int lseg; float lat, ld, lh; } views[] = {
        {-1, 78.0f, -40.0f, 150.0f, -1, 78.0f, -39.0f, 0.0f},   /* 1 straight down on the first mountain */
        {-1, 128.0f, 6.0f, 150.0f, -1, 128.0f, 7.0f, 0.0f},     /* 2 straight down on the second */
        {-1, 20.0f, -150.0f, 60.0f, -1, 78.0f, -40.0f, 10.0f},  /* 3 the first mountain from the south-west, high */
        {-1, 200.0f, 60.0f, 60.0f, -1, 128.0f, 6.0f, 10.0f},    /* 4 the second from the north-east, high */
        {1, 44.0f, 0.0f, 30.0f, 1, 52.0f, 0.0f, 0.0f},          /* 5 straight down on the first tunnel's mouth */
        {5, 118.0f, 0.0f, 30.0f, 5, 126.0f, 0.0f, 0.0f},        /* 6 straight down on the second's */
        {-1, 20.0f, -110.0f, 3.0f, -1, 60.0f, -75.0f, 6.0f},    /* 7 the first mountain's waterfall */
        {-1, 74.0f, 60.0f, 3.0f, -1, 102.0f, 32.0f, 5.0f},      /* 8 the second's */
    };
    const int n = (int)(sizeof(views) / sizeof(views[0]));
    (void)n;
    dk_probe_off |= PROBE_HUD;
#ifdef DK_ROCK_TINT
    dk_probe_off |= PROBE_SKY | PROBE_SEA | PROBE_DISTANT | PROBE_FOAM;   /* the leak test: black beyond the rock */
#endif
#ifdef DK_AT_TRACK
    game_load_track(DK_AT_TRACK);   /* the sweep works on any track */
#else
    game_load_track(TRACK_CANYON);
#endif
    game.state = ST_RESULTS;
#if DK_AT_VIEWS == 2
    /* The sweep: the chase camera every 8 m round the lap, and every 16 m
       a look left and right, so the whole lap is seen as a player does. */
    char name[32];
    int idx = 0;
#ifndef DK_SWEEP_FROM
#define DK_SWEEP_FROM 0
#define DK_SWEEP_TO 100000
#endif
#ifndef DK_SWEEP_STEP
#define DK_SWEEP_STEP 8.0f   /* -DDK_SWEEP_STEP=16 for a quicker look */
#endif
    for(float s = 0; s < track.length; s += DK_SWEEP_STEP, idx++) {
        if(s < DK_SWEEP_FROM || s > DK_SWEEP_TO) continue;
        V3 pos = track_point(track_wrap(s - 6.0f), 0);
        pos.y = track_road_y(track_wrap(s - 6.0f), 0) + 2.6f;
        V3 look = track_point(track_wrap(s + 10.0f), 0);
        look.y = track_road_y(track_wrap(s + 10.0f), 0) + 1.0f;
        snprintf(name, sizeof name, "sweep-%03d-f", idx);
        tour_shot(name, pos, look);
#ifdef DK_HW
        continue;   /* the console: forward only, 128 shots in about four minutes */
#endif
        if(idx & 1) continue;
        for(int side = -1; side <= 1; side += 2) {
            look = track_point(track_wrap(s + 1.0f), side * 16.0f);
            look.y = track_road_y(track_wrap(s + 1.0f), 0) + 2.0f;
            snprintf(name, sizeof name, "sweep-%03d-%c", idx, side < 0 ? 'l' : 'r');
            tour_shot(name, pos, look);
        }
    }
#elif DK_AT_VIEWS == 3
    /* The leak test: cameras inside the rock every 8 m, over each tunnel
       and under the mountainsides, looking along, across and up. With
       -DDK_ROCK_TINT everything beyond the rock is black, so any black
       in a shot is a hole (tools/leakcheck.py finds them). */
    static const float offs[] = {0.0f, -10.6f, 10.6f, -13.5f, 13.5f, -18.5f, 18.5f, -24.5f, 24.5f};
    char name[40];
    int idx = 0;
    /* The list first, with the real rock over each point, so the log
       alone says where the meshes fall short of the height function. */
    for(float s = 0; s < track.length; s += 12.0f) {
        for(int o = 0; o < (int)(sizeof(offs) / sizeof(offs[0])); o++) {
            V3 pos;
            world_leak_point(s, offs[o], &pos);
        }
    }
    printf("DK LEAKLIST done\n");
#ifndef DK_LEAK_FROM
#define DK_LEAK_FROM 0
#define DK_LEAK_TO 100000
#endif
    for(float s = 0; s < track.length; s += 12.0f, idx++) {
        if(s < DK_LEAK_FROM || s > DK_LEAK_TO) continue;   /* -DDK_LEAK_FROM=a -DDK_LEAK_TO=b: a stretch only */
        for(int o = 0; o < (int)(sizeof(offs) / sizeof(offs[0])); o++) {
            V3 pos;
            if(!world_leak_point(s, offs[o], &pos)) continue;
            V3 t = track_tangent(s), r = v3(t.z, 0, -t.x);
            const V3 dirs[4] = {t, vscale(r, -1), r, vadd(vscale(t, 0.27f), v3(0, 1, 0))};   /* f l r u */
            for(int k = 0; k < 4; k++) {
                /* The same shot twice: the real world, then the reference rock. */
                for(int mode = 0; mode < 2; mode++) {
                    world_diag_mode(mode);
                    snprintf(name, sizeof name, "leak-%03d-%d-%c-%c", idx, o, "flru"[k], "ab"[mode]);
                    tour_shot(name, pos, vmad(pos, dirs[k], 10.0f));
                }
            }
        }
    }
    world_diag_mode(0);
#else
    for(int i = 0; i < n; i++) {
        V3 pos = view_pt(views[i].seg, views[i].at, views[i].d, views[i].h);
        V3 look = view_pt(views[i].lseg, views[i].lat, views[i].ld, views[i].lh);
        char name[32];
        snprintf(name, sizeof name, "view-%02d", i + 1);
        tour_shot(name, pos, look);
#ifdef DK_ROCK_TINT
        /* Each again with only the reference rock, to check the reference
           itself. */
        world_diag_mode(1);
        snprintf(name, sizeof name, "view-%02d-ref", i + 1);
        tour_shot(name, pos, look);
        world_diag_mode(0);
#endif
    }
#endif
    printf("DK AUTOTEST: PASS\n");
    for(;;) {
        pvr_wait_ready();
        game_render();
    }
}
#endif

#ifdef DK_AT_CAMTOUR
/* Capture build: every trackside spectator camera on every track, with the
   karts bunched in the middle of the stretch it films, for checking the
   cameras sit somewhere sensible (uv run tools/capture.py --elf ...). */
void camera_tour(void) {
    dk_probe_off |= PROBE_HUD;
    game.state = ST_RESULTS;
    for(int tr = 0; tr < TRACK_COUNT; tr++) {
        game_load_track(tr);
        spectate_begin();
        for(int i = 0; i < spectate_tv_count(); i++) {
            float mid = spectate_tv_preview(&game.cam, i) - 6.0f;
            for(int k = 0; k < kart_count; k++) {
                Kart *kt = &karts[k];
                float s = mid + (float)(k / 2) * 6.0f, d = (k & 1) ? 2.5f : -2.5f;
                kt->pos = track_point(s, d);
                kt->pos.y = kt->gnd = track_road_y(track_wrap(s), d);
                V3 t = track_tangent(s);
                kt->heading = atan2f(t.x, t.z);
                track_locate(kt->pos, -1, &kt->tp);
            }
            spectate_tv_preview(&game.cam, i);
            for(int f = 0; f < 150; f++) {   /* 2.5 s: capture.py's screenshots fall behind at less on a busy Mac */
                pvr_wait_ready();
                game_render();
                if(f == 20) printf("DK SHOT tv-%d-%02d\n", tr, i + 1);
            }
        }
    }
    printf("DK AUTOTEST: PASS\n");
    for(;;) {
        pvr_wait_ready();
        game_render();
    }
}
#endif

#ifdef DK_RECORD
/* Demo races for the lobby server's bots (server/demo): all-CPU races at
   150cc, every kart's wire state 12 times a second over serial, stepped as
   fast as the CPU allows. Each race gives the six CPUs new personalities so
   the demo lobby never drives as one. tools/record_demo.py turns the log
   into server/demo/races/.

   The emeralds are lit from the seed a watcher of the demo lobby derives
   (lobby DK_RECORD_LOBBY, a race id unique to the recording, the track), so
   the server replays each recording under that race id and the gems burst
   where the bots take them. Spear targets go out as kart indices (the
   server maps them to its bots' slots). */
#ifndef DK_RECORD_RACES
#define DK_RECORD_RACES 6
#endif
#ifndef DK_RECORD_LOBBY
#define DK_RECORD_LOBBY 9   /* lobby 10, the server's default demo lobby */
#endif

static float rec_range(float lo, float hi) {
    return lo + (hi - lo) * frand();
}

/* A personality: one of a few driving types, jittered. */
static void record_personality(int i, Kart *k) {
    float look = rec_range(0.9f, 1.1f), sway = rec_range(0.5f, 1.4f), lead = rec_range(-2.0f, 6.0f);
    float grip = rec_range(0.05f, 0.2f), sways = (float)(2 + dk_rand() % 5);
    k->lane = rec_range(-1.8f, 1.8f);
    k->skill = rec_range(0.94f, 1.0f);
    switch(dk_rand() % 6) {
        case 0: grip = 0.0f; lead = rec_range(8.0f, 12.0f); look = rec_range(0.85f, 0.95f); break;  /* drifts early, always */
        case 1: look = rec_range(1.12f, 1.22f); sway = rec_range(0.2f, 0.6f); break;                 /* smooth and tidy */
        case 2: sway = rec_range(1.7f, 2.3f); sways = (float)(5 + dk_rand() % 2); break;             /* weaver */
        case 3: grip = rec_range(0.45f, 0.7f); look = rec_range(1.0f, 1.15f); break;                 /* grips, rarely slides */
        case 4: lead = rec_range(-4.0f, -2.0f); look = rec_range(0.85f, 0.92f); break;               /* late and twitchy */
        default: k->lane = signf(frand_s()) * rec_range(2.0f, 2.6f); break;                          /* wide lines */
    }
    kart_set_ai_style(i, look, sway, sways, lead, grip);
}

void record_races(uint64_t t0) {
    game.cc = CC_150;
    game.mode = MODE_RACE;
    game.num_players = 1;
    game.setup_laps = game.laps = DEFAULT_LAPS;
#ifdef DK_RECORD_ONLY
    /* Record a single track (-DDK_RECORD_ONLY=<id>), e.g. a new one. */
    int total = DK_RECORD_RACES, done = 0;
#else
    int total = TRACK_COUNT * DK_RECORD_RACES, done = 0;
#endif
    for(int tr = 0; tr < TRACK_COUNT; tr++) {
#ifdef DK_RECORD_ONLY
        if(tr != DK_RECORD_ONLY) continue;
#endif
        game_load_track(tr);
        for(int r = 0; r < DK_RECORD_RACES; r++, done++) {
            dk_seed(0x5eed0000u + (uint32_t)tr * 977u + (uint32_t)r * 7919u);
            race_setup();
            int race_id = tr * DK_RECORD_RACES + r + 1;   /* 1-255: never 0, never two tracks alike */
            items_reset(online_items_seed_for(DK_RECORD_LOBBY, race_id, tr));
            for(int i = 0; i < kart_count; i++) {
                karts[i].cpu = true;
                karts[i].net_slot = i;
                record_personality(i, &karts[i]);
            }
            game.state = ST_COUNTDOWN;
            while(game.state == ST_COUNTDOWN) game_countdown_step(DK_DT);
            printf("DKR BEGIN %d %d %d %d %d %d %d\n", tr, r, game.laps, game.cc, kart_count, DK_RECORD_LOBBY, race_id);
            int steps = 0, frame = 0;
            float all_home = -1;
            for(;;) {
                if(steps % 5 == 0) {
                    char line[MAX_KARTS * NET_KART_BYTES * 2 + 1];
                    char *o = line;
                    for(int i = 0; i < kart_count; i++) {
                        NetKart nk;
                        uint8_t b[NET_KART_BYTES];
                        online_pack_kart(&karts[i], &nk);
                        net_encode_kart(b, &nk);
                        for(int j = 0; j < NET_KART_BYTES; j++) o += sprintf(o, "%02x", b[j]);
                    }
                    printf("DKR F %d %s\n", frame++, line);
                }
                game.race_time += DK_DT;
                game_race_step(DK_DT);
                fx_update(DK_DT);
                steps++;
                int home = 0;
                for(int i = 0; i < kart_count; i++) home += karts[i].finished;
                if(home == kart_count && all_home < 0) all_home = game.race_time;
                if((all_home >= 0 && game.race_time > all_home + 12.0f) || game.race_time > 420.0f) break;
                if(steps % 900 == 0) game_boot_screen(t0, (done + game.race_time / 120.0f) / total, "RECORDING DEMO RACES");
            }
            printf("DKR END %d", frame);
            for(int i = 0; i < kart_count; i++) printf(" %.2f", (double)karts[i].finish_time);
            printf("\n");
        }
    }
    printf("DKR DONE\n");
}
#endif
