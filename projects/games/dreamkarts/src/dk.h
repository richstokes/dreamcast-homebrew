/* DreamKarts: shared types, math helpers and game state. */
#ifndef DK_H
#define DK_H

#include <kos.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DK_PI      3.14159265f
#define DK_TAU     6.28318531f
#define DK_DT      (1.0f / 60.0f)
#define MAX_KARTS  6
#define DEFAULT_LAPS 3
#define MAX_LAPS   10   /* online lobbies race 3, 5 or 10 laps */

/* World: +X east, +Y up, +Z north, metres. A heading of 0 faces +Z and a
   positive heading change turns right (forward = (sin h, 0, cos h)). */
typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { return (V3){x, y, z}; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vscale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline V3 vmad(V3 a, V3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static inline V3 vnorm(V3 a) {
    float l = vlen(a);
    return l > 1e-6f ? vscale(a, 1.0f / l) : v3(0, 0, 1);
}
static inline V3 vlerp(V3 a, V3 b, float t) { return vmad(a, vsub(b, a), t); }
static inline float clampf(float a, float lo, float hi) { return a < lo ? lo : (a > hi ? hi : a); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float signf(float a) { return a < 0 ? -1.0f : 1.0f; }
static inline float wrap_angle(float a) {
    while(a > DK_PI) a -= DK_TAU;
    while(a < -DK_PI) a += DK_TAU;
    return a;
}
/* Move a toward b by at most step. */
static inline float approach(float a, float b, float step) {
    if(a < b) return a + step > b ? b : a + step;
    return a - step < b ? b : a - step;
}
/* Frame-rate independent exponential smoothing factor. */
static inline float smooth_k(float rate, float dt) {
    float k = rate * dt;
    return k > 1.0f ? 1.0f : k;
}
static inline V3 heading_fwd(float h) { return v3(sinf(h), 0, cosf(h)); }
static inline V3 heading_right(float h) { return v3(cosf(h), 0, -sinf(h)); }

static inline uint32_t argb(int a, int r, int g, int b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static inline uint32_t color_scale(uint32_t c, float s) {
    int r = (int)(((c >> 16) & 255) * s), g = (int)(((c >> 8) & 255) * s), b = (int)((c & 255) * s);
    if(r > 255) r = 255;
    if(g > 255) g = 255;
    if(b > 255) b = 255;
    return (c & 0xff000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static inline uint32_t color_lerp(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for(int s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        r |= (uint32_t)(ca + (cb - ca) * t) << s;
    }
    return r;
}
static inline uint32_t color_alpha(uint32_t c, float a) {
    int ai = (int)(clampf(a, 0, 1) * 255.0f);
    return (c & 0x00ffffffu) | ((uint32_t)ai << 24);
}

/* Small deterministic RNG for effects and AI variety. */
uint32_t dk_rand(void);
void dk_seed(uint32_t seed);
uint32_t dk_rand_peek(void);   /* the generator's state, for QA fingerprints */
static inline float frand(void) { return (float)(dk_rand() & 0xffff) / 65535.0f; }
static inline float frand_s(void) { return frand() * 2.0f - 1.0f; }

/* ---- Input ---- */
typedef struct {
    bool connected;
    int port;                 /* maple port of the controller, -1 if none */
    uint32_t held, pressed, released;
    float stick_x, stick_y;   /* -1..1 with dead zone */
    float ltrig, rtrig;       /* 0..1 */
} Pad;

enum {
    BTN_A = 1 << 0, BTN_B = 1 << 1, BTN_X = 1 << 2, BTN_Y = 1 << 3,
    BTN_START = 1 << 4, BTN_UP = 1 << 5, BTN_DOWN = 1 << 6,
    BTN_LEFT = 1 << 7, BTN_RIGHT = 1 << 8, BTN_L = 1 << 9, BTN_R = 1 << 10
};

#define MAX_PLAYERS 2
/* pad is player 1 (first controller), pad2 player 2 (second controller). */
extern Pad pad, pad2;
Pad *input_pad(int player);
void input_init(void);
void input_update(void);
/* Menu helpers with key repeat for held directions (player 1). */
bool input_menu_up(void);
bool input_menu_down(void);
bool input_menu_left(void);
bool input_menu_right(void);
/* Directions with repeat for any player. */
bool input_up(int player);
bool input_down(int player);
bool input_left(int player);
bool input_right(int player);
#if defined(DK_FILM) || defined(DK_CAPTURE)
void input_film_tap(int player, uint32_t btns);
#endif

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
   against the rivals; Time Trial (Versus with two players). */
enum { MODE_GP, MODE_RACE, MODE_TT, MODE_COUNT };
enum {
    ST_TITLE, ST_MENU, ST_TRACKS, ST_INTRO, ST_COUNTDOWN, ST_RACE, ST_RESULTS,
    ST_MAIN,     /* single player / two player / online */
    ST_ONLINE,   /* online menus: name, connecting, lobbies, lobby room */
    ST_KARTS     /* two-player kart select, split screen */
};

typedef struct {
    V3 pos, fwd, right, up;
    float fov;           /* horizontal field of view, radians */
    float shake;
} Camera;

/* A human player: their kart, chase camera and per-race state. */
typedef struct {
    int kart;
    int colour;          /* kart_colours[] preset */
    Camera cam;
    float cam_yaw, cam_height, cam_dist, fov;
    float accel_press_t; /* rocket start timing */
    bool final_lap_seen;
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
} Game;

extern Game game;

/* GPU probe (hardware benchmark): feature groups switched off for timing. */
enum {
    PROBE_SKY = 1 << 0, PROBE_SEA = 1 << 1, PROBE_WORLD = 1 << 2, PROBE_KARTS = 1 << 3,
    PROBE_PT = 1 << 4, PROBE_DISTANT = 1 << 5, PROBE_FOAM = 1 << 6, PROBE_SHADOWS = 1 << 7,
    PROBE_FX = 1 << 8, PROBE_HUD = 1 << 9, PROBE_AUTOSORT = 1 << 10,
    PROBE_NOTA = 1 << 11   /* vertices to a cached scratch line, not the TA */
};
extern uint32_t dk_probe_off;

void game_set_state(int state);
void race_setup(void);
/* Reset the race clock, cameras and HUD for the karts already lined up. */
void race_reset(void);
/* Switch to another track: rebuild its scenery and line up the grid. */
void game_load_track(int id);

#endif
