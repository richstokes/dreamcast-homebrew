/* DreamKarts film builds: footage for trailers, recorded from Flycast.

   make VARIANT=film dreamkarts-film.elf EXTRA_CFLAGS=-DDK_FILM_SPECTATE
   goes online and watches the demo lobby for as long as it runs, with the
   TV director on AUTO. EXTRA_CFLAGS=-DDK_FILM_SHOW instead plays the title,
   the menus (stepping through the colours and the track select) and a set
   of single-player and split-screen races with the CPU driving every
   kart, then starts the set again.

   Every interesting moment is logged as a "DK FILM" line (camera cuts,
   overtakes, jumps, falls, purple boosts, laps, finishes) so a recording
   script can timestamp them and pick the highlights. */
#include "film.h"
#include "kart.h"
#include "spectate.h"
#include "track.h"
#include "dolphins.h"

#ifdef DK_FILM

static const char *const state_names[] = {"title", "menu", "tracks", "intro", "countdown", "race", "results",
                                          "main", "online"};

/* What happened since the last step, for the highlight log. */
static void film_log(void) {
    static int last_state = -1, last_track = -1, last_lap, last_mode = -1, last_target = -1;
    static int order[MAX_KARTS];
    static bool air[MAX_KARTS], fell[MAX_KARTS], done[MAX_KARTS], dolphins;
    static float order_t;
    if(game.state != last_state || game.track != last_track) {
        if(game.state == ST_INTRO || game.state == ST_COUNTDOWN || game.track != last_track) {
            memset(air, 0, sizeof(air));
            memset(fell, 0, sizeof(fell));
            memset(done, 0, sizeof(done));
            memset(order, 0xff, sizeof(order));
            last_lap = 0;
            dolphins = false;
        }
        printf("DK FILM state %s track %d %s players %d\n", state_names[game.state], game.track, track.name,
               game.num_players);
        last_state = game.state;
        last_track = game.track;
    }
    if(game.state != ST_RACE && game.state != ST_COUNTDOWN && game.state != ST_RESULTS) return;
    if(spectate_on()) {
        int m = spectate_shot_mode(), t = spectate_target();
        if(m != last_mode || t != last_target) {
            static const char *const cams[CAM_MODES] = {"auto", "tv", "chase", "heli", "front", "blimp"};
            printf("DK FILM cam %s %s\n", cams[m], karts[t].name ? karts[t].name : "?");
            last_mode = m;
            last_target = t;
        }
    }
    if(game.state != ST_RACE) return;
    int lead_lap = 0;
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->lap > lead_lap) lead_lap = k->lap;
        float h = k->pos.y - track_road_y(k->tp.s, 0);
        bool up = !k->grounded && (k->big_air || h > 1.5f);
        if(up && !air[i]) printf("DK FILM air %s %.1f\n", k->name, (double)h);
        air[i] = up;
        if(k->over_edge && !fell[i]) printf("DK FILM fall %s\n", k->name);
        fell[i] = k->over_edge;
        if(k->finished && !done[i]) printf("DK FILM finish %s place %d\n", k->name, k->place);
        done[i] = k->finished;
        if((k->events & EV_BOOST) && k->boost_stage >= 3) printf("DK FILM purple %s\n", k->name);
        if(k->events & EV_KART_BUMP) printf("DK FILM bump %s\n", k->name);
        if(k->events & EV_JUMP) printf("DK FILM jump %s\n", k->name);
        if(k->events & EV_FALL) printf("DK FILM fall %s\n", k->name);
    }
    if(lead_lap != last_lap && lead_lap > 0) {
        printf("DK FILM lap %d of %d\n", lead_lap, game.laps);
        last_lap = lead_lap;
    }
    if(!dolphins && dolphins_show_time() > 0.2f) {
        dolphins = true;
        printf("DK FILM dolphins\n");
    }
    /* Overtakes: the running order, checked a few times a second so a
       pair side by side doesn't flicker. */
    order_t += DK_DT;
    if(order_t < 0.25f) return;
    order_t = 0;
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->place < 1 || k->place > MAX_KARTS || k->finished) continue;
        int was = order[i];
        if(was > 0 && k->place < was && game.race_time > 4.0f)
            printf("DK FILM pass %s to %d\n", k->name, k->place);
        order[i] = k->place;
    }
}

#ifdef DK_FILM_SHOW
/* The set of races: players, track, each player's colour, and how many
   colours to step through first so the menu kart changes on camera. */
static const struct { int players, track, colour[2], browse; } plan[] = {
    {1, TRACK_BEACH, {6, 0}, 4},
    {2, TRACK_FIGURE8, {2, 5}, 2},
    {1, TRACK_SKYWAY, {5, 0}, 2},
    {2, TRACK_TEMPLE, {6, 3}, 1},
    {1, TRACK_HARBOUR, {3, 0}, 2},
    {2, TRACK_SKYWAY, {0, 7}, 1},
};
#define PLAN_N ((int)(sizeof(plan) / sizeof(plan[0])))

/* -DDK_FILM_FIRST=n starts the set at race n, skipping the track tour. */
#ifndef DK_FILM_FIRST
#define DK_FILM_FIRST 0
#endif
static int race_i = DK_FILM_FIRST, step, taps;
static float wait_t;
static bool tracks_toured = DK_FILM_FIRST > 0;

/* One scripted action every `gap` seconds of the current state. */
static bool due(float gap) {
    if(game.time < wait_t + gap) return false;
    wait_t = game.time;
    return true;
}

static void show_script(void) {
    static int last_state = -1;
    if(game.state != last_state) {
        last_state = game.state;
        step = taps = 0;
        wait_t = 0;
    }
    int r = race_i % PLAN_N;
    switch(game.state) {
        case ST_TITLE:
            if(due(race_i == DK_FILM_FIRST ? 9.0f : 3.0f)) pad.pressed |= BTN_START;
            break;
        case ST_MAIN:
            /* Single player is the top row; two player the next one down. */
            if(step == 0 && due(1.4f)) {
                if(plan[r].players > 1) input_film_tap(0, BTN_DOWN);
                step = 1;
            }
            else if(step == 1 && due(0.9f)) {
                pad.pressed |= BTN_A;
                step = 2;
            }
            break;
        case ST_MENU: {
            /* Rows: mode, cc, colour, (player 2's colour), start. 150cc,
               then step through some colours, landing on the planned one. */
            int start = (plan[r].colour[0] - plan[r].browse + 8) % 8;
            if(step == 0 && due(1.2f)) {
                input_film_tap(0, BTN_DOWN);
                step = 1;
            }
            else if(step == 1 && due(0.5f)) {
                if(game.cc < CC_150) input_film_tap(0, BTN_RIGHT);
                else step = 2;
            }
            else if(step == 2 && due(0.5f)) {
                input_film_tap(0, BTN_DOWN);
                /* Already on screen, so stepping lands on the plan. A colour
                   the other player holds is skipped, so player 2 starts one
                   short of theirs and off player 1's path. */
                game.pl[0].colour = start;
                if(plan[r].players > 1) {
                    int c2 = (plan[r].colour[1] + 7) % 8;
                    for(int i = 0; i <= plan[r].browse; i++)
                        if((start + i) % 8 == c2) c2 = plan[r].colour[1];
                    game.pl[1].colour = c2;
                }
                race_setup();
                step = 3;
            }
            else if(step == 3 && due(0.8f)) {
                if(game.pl[0].colour != plan[r].colour[0] && taps++ < 10) input_film_tap(0, BTN_RIGHT);
                else step = 4, taps = 0;
            }
            else if(step == 4 && due(0.6f)) {
                if(plan[r].players > 1 && game.pl[1].colour != plan[r].colour[1] && taps++ < 10)
                    input_film_tap(1, BTN_RIGHT);
                else step = 5;
            }
            else if(step == 5 && due(1.2f)) {
                pad.pressed |= BTN_START;
                step = 6;
            }
            break;
        }
        case ST_TRACKS:
            /* The first time round, fly over every course; later, go
               straight to the planned one. */
            if(step == 0 && due(2.6f)) {
                if(!tracks_toured && taps < TRACK_COUNT) {
                    input_film_tap(0, BTN_RIGHT);
                    taps++;
                }
                else if(game.track != plan[r].track) {
                    input_film_tap(0, BTN_RIGHT);
                }
                else step = 1;
            }
            else if(step == 1 && due(1.5f)) {
                tracks_toured = true;
                pad.pressed |= BTN_START;
                step = 2;
            }
            break;
        case ST_COUNTDOWN:
            /* A wave on the grid; the nearest CPU waves back. */
            if(step == 0 && game.countdown > 0.4f) {
                pad.pressed |= BTN_Y;
                if(game.num_players > 1) pad2.pressed |= BTN_Y;
                step = 1;
            }
            break;
        case ST_RESULTS:
            if(step == 0 && due(7.0f)) {
                pad.pressed |= BTN_B;
                race_i++;
                printf("DK FILM next %d\n", race_i);
                step = 1;
            }
            break;
    }
}
#endif

void film_step(float dt) {
    (void)dt;
#ifdef DK_FILM_SHOW
    show_script();
#endif
    film_log();
}

#endif
