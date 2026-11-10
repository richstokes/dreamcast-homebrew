/* Ridgeline: shared types and game state. */
#ifndef RL_H
#define RL_H

#include <kos.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "engine/vecmath.h"
#include "engine/rand.h"
#include "engine/input.h"
#include "engine/render.h"
#include "texture_assets.h"

#define RL_DT ENG_DT
#define MAX_TRUCKS 4
#define TRUCK_MODELS 5   /* RL_TRUCK_COUNT in trucks.h (truckdraw.c checks) */
#define GRAVITY 9.81f
#define MPS_TO_MPH 2.23694f
#define M_TO_FT 3.28084f

enum {
    ST_TITLE,     /* logo over a flyover of the mountain */
    ST_SELECT,    /* truck, livery and company */
    ST_DRIVE,     /* out on the mountain (paused or not) */
    ST_SOUND,     /* the sound test, over the title's flyover */
    ST_ONLINE     /* online: the name, connecting and the lobby browser (online.c) */
};

/* Drive modes, stepped with the D-pad: rear-wheel drive; all-wheel drive
   with the centre locked and open axle diffs; low range with both axle
   diffs locked as well. */
enum { DRIVE_2WD, DRIVE_4WD, DRIVE_LOCK, DRIVE_COUNT };

typedef struct {
    int state;
    float time;           /* seconds in this state */
    bool paused;
    int pause_sel;
    bool quit;
    /* The run's choices. */
    int truck_model;      /* RL_TRUCK_* */
    int livery;
    int companions;       /* CPU trucks: 0 solo .. 3 */
    int ntrucks;
    /* Progress, for the HUD. */
    float best_y;         /* highest the player has been */
    float summit_frac;
    uint16_t views_found; /* bit per viewpoint */
    int8_t last_view;     /* the latest found, -1 none yet */
    int8_t view_sel;      /* the pause menu's viewpoint to go back to */
    bool summit_reached;
    float fps, cpu_ms;
    float clock;          /* seconds driving */
    int cam_mode;
    int map_mode;         /* X: 0 corner map, 1 large map, 2 no map */
    bool hud_hidden;
    bool show_fps;        /* the frame rate counter (pause menu) */
    bool photo;           /* photo mode: paused, no HUD, the camera orbits */
    float photo_t;        /* seconds in photo mode, for its hint */
    bool cam_scripted;    /* test builds' scenery tour holds the camera */
    bool demo;            /* demo mode (demo.h): four CPU trucks, the camera cuts between them */
    int demo_truck;       /* the truck it is watching */
    int start_tod;        /* when the drive starts: TOD_DAWN .. (tod.h) */
    float photo_time_t;   /* seconds since photo mode last moved the clock, for its readout */
    bool online;          /* playing online (online.h): the select and the drive are a lobby's */
} Game;

extern Game game;
extern Camera cam;

/* The direction the world is lit from (toward the sun, or the moon at
   night; tod.c sets it). */
extern V3 sun_dir;

/* The pause menu stops the world, except online, where it runs on. */
static inline bool game_frozen(void) {
    return game.paused && !game.online;
}

void game_set_state(int state);
void game_start_drive(void);

#endif
