/* Kaiju Lunch Break: the game's shared types. */
#ifndef KL_H
#define KL_H

#include "engine/render.h"
#include "engine/rand.h"
#include "engine/input.h"
#include "texture_assets.h"
#include "sound_assets.h"
#include <kos.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

enum { ST_TITLE, ST_PLAY, ST_RESULTS };
enum { PAUSE_RESUME, PAUSE_RESTART, PAUSE_MUSIC, PAUSE_FPS, PAUSE_QUIT, PAUSE_ITEMS };

#define LUNCH_SECONDS 180.0f   /* the hour, 12:00 to 1:00, at twenty to one */
/* Overtime: a bill this big (thousands of dollars, the running combo
   counted) when the bell goes buys OVERTIME_SECONDS more, once. */
#ifndef OVERTIME_BILL
#define OVERTIME_BILL 20000000ull
#endif
#define OVERTIME_SECONDS 120.0f

typedef struct {
    int state;
    float time;            /* seconds in this state */
    bool paused;
    int pause_sel;
    bool quit;
    float fps, cpu_ms;
    bool show_fps;         /* the pause menu's: frames a second and the frame's CPU time, on the HUD */
    float clock;           /* seconds of lunch left */
    float intro;           /* counts down the "LUNCH TIME!" card */
    float outro;           /* counts up once the bell has gone */
    float extra;           /* seconds of overtime added to this lunch */
    float ot_card;         /* counts down the "OVERTIME!" card */
    uint32_t frame;        /* displayed frames, from 1 */
} Game;
extern Game game;
extern Camera cam;
/* The frame's phases for the test builds' log (engine/prof.h): the update,
   then the city, the traffic and aircraft, the kaiju, the effects and
   sprites, and the overlay with the scene's end. */
enum { PH_UPD, PH_CITY, PH_LIFE, PH_KAIJU, PH_FX, PH_TR };
void game_set_state(int state);
void game_start(void);

/* What knocked something down: the combo counts how many of these a chain
   has used, and a kind repeated too often goes stale (score.c). */
enum {
    SRC_CLAW, SRC_TAIL, SRC_STOMP, SRC_BEAM, SRC_RAM, SRC_DOMINO, SRC_BLAST, SRC_FLOP, SRC_THROWN,
    SRC_EAT, SRC_COUNT
};

/* A frame's commands for the kaiju: the player's pad, or the autotest. */
typedef struct {
    float mx, mz;          /* where to walk, in the world, length up to 1 */
    bool claw, tail, jump, chomp, roar;   /* pressed this step */
    bool breath;           /* held */
    bool dash;             /* held: the charge */
    bool grab;             /* the chomp button still down: carry what the jaws close on */
} Ctl;

/* A rigid transform: a right-handed basis (x right, y up, z forward) and
   an origin. Segments chain with xf_mul(parent, local). */
typedef struct { V3 x, y, z, p; } Xf;

static inline Xf xf_identity(void) {
    return (Xf){{1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0, 0, 0}};
}
static inline V3 xf_dir(const Xf *a, V3 d) {
    return vadd(vadd(vscale(a->x, d.x), vscale(a->y, d.y)), vscale(a->z, d.z));
}
static inline V3 xf_point(const Xf *a, V3 p) {
    return vadd(a->p, xf_dir(a, p));
}
/* A world direction in a's own axes. */
static inline V3 xf_local(const Xf *a, V3 d) {
    return v3(vdot(d, a->x), vdot(d, a->y), vdot(d, a->z));
}
static inline Xf xf_mul(const Xf *a, const Xf *b) {
    return (Xf){xf_dir(a, b->x), xf_dir(a, b->y), xf_dir(a, b->z), xf_point(a, b->p)};
}
static inline Xf xf_trans(V3 p) {
    Xf r = xf_identity();
    r.p = p;
    return r;
}
/* About X: positive tips the top forward. About Y: positive turns right, as
   a heading does. About Z: positive rolls to the left. */
static inline Xf xf_rot_x(float a) {
    float s = sinf(a), c = cosf(a);
    return (Xf){{1, 0, 0}, {0, c, s}, {0, -s, c}, {0, 0, 0}};
}
static inline Xf xf_rot_y(float a) {
    float s = sinf(a), c = cosf(a);
    return (Xf){{c, 0, -s}, {0, 1, 0}, {s, 0, c}, {0, 0, 0}};
}
static inline Xf xf_rot_z(float a) {
    float s = sinf(a), c = cosf(a);
    return (Xf){{c, s, 0}, {-s, c, 0}, {0, 0, 1}, {0, 0, 0}};
}
/* A segment hanging from p toward q (its -Y axis), turned so its Z faces
   as near to fwd as it can. */
static inline Xf xf_hang(V3 p, V3 q, V3 fwd) {
    Xf r;
    r.y = vnorm(vsub(p, q));
    r.x = vnorm(vcross(r.y, fwd));
    r.z = vcross(r.x, r.y);
    r.p = p;
    return r;
}
static inline void xf_m4(const Xf *a, float *m) {
    m[0] = a->x.x; m[1] = a->x.y; m[2] = a->x.z; m[3] = 0;
    m[4] = a->y.x; m[5] = a->y.y; m[6] = a->y.z; m[7] = 0;
    m[8] = a->z.x; m[9] = a->z.y; m[10] = a->z.z; m[11] = 0;
    m[12] = a->p.x; m[13] = a->p.y; m[14] = a->p.z; m[15] = 1;
}

static inline float ease_out(float t) { return 1.0f - (1.0f - t) * (1.0f - t); }
static inline float ease_io(float t) { return t * t * (3.0f - 2.0f * t); }
static inline float dist2_xz(V3 a, V3 b) {
    float dx = a.x - b.x, dz = a.z - b.z;
    return dx * dx + dz * dz;
}

#endif
