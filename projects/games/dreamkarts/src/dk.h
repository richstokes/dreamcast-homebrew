/* DreamKarts: shared types, math helpers and game state. */
#ifndef DK_H
#define DK_H

#include <kos.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DK_PI      ENG_PI
#define DK_TAU     ENG_TAU
#define DK_DT      ENG_DT
#define MAX_KARTS  6
#define DEFAULT_LAPS 3
#define MAX_LAPS   10   /* online lobbies race 3, 5 or 10 laps */

/* The shared engine (projects/common/engine): vectors, colours and the
   camera, the generator, controllers. */
#include "engine/vecmath.h"
#include "engine/rand.h"
#include "engine/input.h"

#define MAX_PLAYERS ENG_PADS

typedef struct {
    float steer;         /* -1 left .. +1 right */
    bool accel, brake;
    bool drift;          /* hop/drift button held */
    bool look_back;
    bool wave;           /* start a wave this step */
    bool item;           /* use the held power-up this step */
    bool item_back;      /* ...fired backwards (down held) */
} KartInput;

/* ---- Game state ---- */
enum { CC_50, CC_100, CC_150, CC_COUNT };
/* Grand Prix: every course back to back on points; Single Race: one course
   against the rivals; Time Trial (Versus with two players); Free Roam: no
   laps or places, just the Night Meet lot to drive around (roam.h). */
enum { MODE_GP, MODE_RACE, MODE_TT, MODE_ROAM, MODE_COUNT };
enum {
    ST_TITLE, ST_MENU, ST_TRACKS, ST_INTRO, ST_COUNTDOWN, ST_RACE, ST_RESULTS,
    ST_MAIN,     /* single player / two player / online */
    ST_ONLINE,   /* online menus: name, connecting, lobbies, lobby room */
    ST_KARTS,    /* two-player kart select, split screen */
    ST_FLAPPY    /* FlappyKarts, the hidden mini game (flappy.c) */
};

/* A human player: their kart, chase camera and per-race state. */
typedef struct {
    int kart;
    int colour;          /* kart_colours[] preset */
    Camera cam;
    float cam_yaw, cam_height, cam_dist, fov;
    float accel_press_t; /* rocket start timing */
    bool final_lap_seen;
    float wrong_way_t;   /* seconds spent driving against the race direction */
} Player;

typedef struct {
    int state;
    int mode, cc;
    int track;           /* TRACK_* (the loaded track and its scenery) */
    float time;          /* seconds in current state */
    float race_time;     /* seconds since GO */
    float countdown;     /* seconds since countdown began */
    bool paused;
    int pause_sel;
    bool hud_hidden;     /* Y on the pause menu: no pause panel or HUD, for clean screenshots */
    int menu_sel;
    int results_sel;     /* RESULTS_* choice after a single race */
    float fps, cpu_ms;
    int player;          /* player 1's kart index */
    int num_players;     /* 1 or 2 (split screen) */
    Player pl[MAX_PLAYERS];
    float humans_done_t; /* seconds since the first human finished */
    int finish_order[MAX_KARTS];
    int finished_count;
    float best_lap;
    int laps;            /* laps in this race */
    int setup_laps;      /* laps picked in the race setup menu (offline races) */
    bool online;         /* in the online menus or an online race */
    bool items;          /* power-ups in this race */
    bool setup_items;    /* the race setup menu's ITEMS switch */
    bool final_lap_announced;
    Camera cam;          /* full-screen camera: title, intro, results */
    int pause_owner;
    float title_t;
    bool quit;
    bool ios;            /* running in the DreamKarts iOS app (dk_on_ios) */
} Game;

extern Game game;

/* GPU probe (hardware benchmark): feature groups switched off for timing. */
enum {
    PROBE_SKY = 1 << 0, PROBE_SEA = 1 << 1, PROBE_WORLD = 1 << 2, PROBE_KARTS = 1 << 3,
    PROBE_PT = 1 << 4, PROBE_DISTANT = 1 << 5, PROBE_FOAM = 1 << 6, PROBE_SHADOWS = 1 << 7,
    PROBE_FX = 1 << 8, PROBE_HUD = 1 << 9, PROBE_AUTOSORT = 1 << 10,
    PROBE_NOTA = 1 << 11,  /* vertices to a cached scratch line, not the TA */
    PROBE_PERFHUD = 1 << 12,
    PROBE_SHEEN = 1 << 13   /* the sun's sheen on the asphalt */
};
extern uint32_t dk_probe_off;

/* The iOS app stamps DKiOS\1 into the last 6 bytes of the console ID (the first
   2 are a checksum). False on a real Dreamcast and in stock Flycast. */
bool dk_on_ios(void);

void game_set_state(int state);
void race_setup(void);
/* Reset the race clock, cameras and HUD for the karts already lined up. */
void race_reset(void);
/* Switch to another track: rebuild its scenery and line up the grid. */
void game_load_track(int id);

#endif
