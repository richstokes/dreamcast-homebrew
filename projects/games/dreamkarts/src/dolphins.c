/* Final-lap dolphins.

   On the final lap two dolphins surface in the sea and race alongside: they
   swim west at the kart's speed, keeping a set distance ahead, making up to
   three leaps each and splashing in and out. A leap that would land past
   the stage's west limit is skipped, so at boost speed you see two leaps
   rather than a dolphin diving into the sand. On Emerald Beach that is out of
   turn 1, beside the boardwalk (about 26 m ahead, 15-20 m right); on Coral
   Crossing, where the sea is further off, up leg B and over the west loop
   (about 80 m ahead, larger and leaping higher); on Palm Harbour, from
   the seafront straight through the chicane and down the beach straight
   (about 60 m ahead); on Temple Hill, along the beach straight out of the
   start line. A small original mesh
   (tapered body, beak, dorsal fin, flippers and flukes), drawn only while
   airborne: about 150 triangles each for ~4.5 seconds a race. */
#include "dolphins.h"
#include "audio.h"
#include "fx.h"
#include "kart.h"
#include "render.h"
#include "track.h"
#include "world.h"

#define WATER_Y    (-0.35f)
#define LEAP_TIME  1.1f
#define LEAP_DEPTH (-1.1f)    /* path starts and ends below the surface */
#define FIRST_LEAP 0.3f       /* show time of the first leap */
#define LEAP_GAP   1.4f       /* between leap starts */
#define LEAPS      3
#define SHOW_LEN   4.9f

/* Per-track staging. Both courses run the pod west (-X) along the sea. */
typedef struct {
    float trigger;     /* lap distance where the show starts */
    float lane[2];     /* z of each dolphin's lane */
    float ahead;       /* how far west of the kart the pod keeps */
    float scale;       /* cartoon-sized so they read at distance */
    float peak;        /* leap rise from the start depth */
    float splash;
    float west;        /* re-entry x must stay east of this */
} Stage;

static Stage stage;
static bool staged;
static int staged_id = -1;
#define MAX_SHOWS  2

#define RINGS 8
#define RADIAL 8
#define DOLPHIN_MAX_VERTS (RINGS * RADIAL + 2 + 16)   /* body, end caps, fins */
#define DOLPHIN_MAX_IDX ((RINGS - 1) * RADIAL * 6 + RADIAL * 6 + 36)

static RV verts[DOLPHIN_MAX_VERTS] __attribute__((aligned(32)));
static uint16_t idx[DOLPHIN_MAX_IDX];
static int nverts, nidx;
static const pvr_poly_hdr_t *h_dolphin;

typedef struct {
    bool active;
    float t;
    float prev_y[2];
    V3 last[2];
    float x, speed;    /* pod position (x) and westward swimming speed */
    int kart;          /* the kart they race alongside */
    uint8_t dry[2];    /* per dolphin, bit k: leap k skipped */
} Show;

static Show shows[MAX_SHOWS];
static bool seen[MAX_PLAYERS];
static float prev_s[MAX_PLAYERS];
static float newest_t = -1;

static const float stagger_t[2] = {0.0f, 0.18f};
static const float stagger_x[2] = {0.0f, 2.5f};

static uint32_t shade(uint32_t c, V3 n) {
    V3 l = vnorm(v3(0.25f, 0.9f, 0.3f));
    float k = 0.62f + 0.45f * fmaxf(0.0f, vdot(vnorm(n), l));
    return color_scale(c, k);
}

static int add_vert(V3 p, uint32_t c) {
    if(nverts >= DOLPHIN_MAX_VERTS) return nverts - 1;
    verts[nverts] = (RV){p.x, p.y, p.z, 0, 0, c};
    return nverts++;
}

static void add_tri(int a, int b, int c) {
    if(nidx + 3 > DOLPHIN_MAX_IDX) return;
    idx[nidx++] = (uint16_t)a;
    idx[nidx++] = (uint16_t)b;
    idx[nidx++] = (uint16_t)c;
}

void dolphins_init(void) {
    /* z along the body (+ = nose), half-heights, half-widths, centre y. */
    static const float ring[RINGS][4] = {
        {-1.45f, 0.07f, 0.06f, 0.00f}, {-1.05f, 0.16f, 0.13f, 0.02f},
        {-0.45f, 0.33f, 0.29f, 0.04f}, {0.20f, 0.40f, 0.35f, 0.04f},
        {0.80f, 0.34f, 0.30f, 0.02f}, {1.15f, 0.24f, 0.22f, -0.02f},
        {1.32f, 0.13f, 0.12f, -0.08f}, {1.62f, 0.05f, 0.05f, -0.10f},
    };
    const uint32_t back = 0xff42647e, side = 0xff7d9cb0, belly = 0xffdce5e8;
    nverts = nidx = 0;
    for(int r = 0; r < RINGS; r++)
        for(int j = 0; j < RADIAL; j++) {
            float a = (float)j / RADIAL * DK_TAU;
            float ca = cosf(a), sa = sinf(a);      /* a = 0 is the top */
            V3 n = v3(sa, ca, 0);
            V3 p = v3(sa * ring[r][2], ring[r][3] + ca * ring[r][1], ring[r][0]);
            uint32_t c = ca > 0.35f ? back : ca > -0.45f ? side : belly;
            add_vert(p, shade(c, n));
        }
    int tail = add_vert(v3(0, 0, -1.55f), shade(back, v3(0, 0, -1)));
    int nose = add_vert(v3(0, -0.10f, 1.70f), shade(side, v3(0, 0, 1)));
    for(int r = 0; r + 1 < RINGS; r++)
        for(int j = 0; j < RADIAL; j++) {
            int a = r * RADIAL + j, b = r * RADIAL + (j + 1) % RADIAL;
            int c = a + RADIAL, d = b + RADIAL;
            add_tri(a, b, c);
            add_tri(c, b, d);
        }
    for(int j = 0; j < RADIAL; j++) {
        add_tri(tail, (j + 1) % RADIAL, j);
        add_tri(nose, (RINGS - 1) * RADIAL + j, (RINGS - 1) * RADIAL + (j + 1) % RADIAL);
    }
    /* Dorsal fin, two flippers and the tail flukes (drawn double-sided). */
    uint32_t fin = shade(back, v3(0, 1, 0)), finl = shade(side, v3(0, -1, 0));
    int f0 = add_vert(v3(0, 0.40f, 0.35f), fin), f1 = add_vert(v3(0, 0.80f, -0.30f), fin);
    int f2 = add_vert(v3(0, 0.36f, -0.25f), fin);
    add_tri(f0, f1, f2);
    for(int s = -1; s <= 1; s += 2) {
        int p0 = add_vert(v3(0.26f * s, -0.12f, 0.75f), finl);
        int p1 = add_vert(v3(0.62f * s, -0.42f, 0.30f), finl);
        int p2 = add_vert(v3(0.24f * s, -0.18f, 0.45f), finl);
        add_tri(p0, p1, p2);
    }
    int t0 = add_vert(v3(0, 0, -1.40f), fin);
    int tl = add_vert(v3(-0.58f, 0.02f, -1.88f), fin), tr = add_vert(v3(0.58f, 0.02f, -1.88f), fin);
    int tn = add_vert(v3(0, 0, -1.68f), fin);
    add_tri(t0, tl, tn);
    add_tri(t0, tn, tr);
    h_dolphin = r_header(TEX_NONE, HF_OP);
}

/* Emerald Beach: just beyond the boardwalk's outer rail (z 66), out of
   turn 1. West of x -62 the outer wall widens towards turn 2 and the
   lanes run onto the sand shoulder, so leaps must land by x -57. Coral
   Crossing: the sea lies north of the loops (waterline near
   z 102), so the pod leaps just past the shallows, well ahead, as karts
   run up leg B from the crossing and over the top of the west loop. Palm
   Harbour's sea is the same distance off, north of the seafront. */
#define NO_LIMIT (-1.0e6f)

static void stage_for_track(void) {
    staged_id = track.id;
    staged = true;
    if(track.id == TRACK_BEACH) {
        stage = (Stage){track.back_start - 8.0f, {69.5f, 72.2f}, 26.0f, 1.4f, 4.4f, 1.0f, -57.0f};
        return;
    }
    if(track.id == TRACK_FIGURE8) {
        /* Leg B passes the centre crossing heading north-west. */
        float cross_b = -1.0f;
        for(int i = 0; i < TRACK_SAMPLES; i++) {
            const TrackSample *t = &track.smp[i];
            if(t->t.x < 0 && t->p.x * t->p.x + t->p.z * t->p.z < 9.0f) cross_b = t->s;
        }
        if(cross_b >= 0) {
            stage = (Stage){track_wrap(cross_b + 15.0f), {106.0f, 109.5f}, 78.0f, 2.5f, 7.5f, 2.2f, NO_LIMIT};
            return;
        }
    }
    if(track.id == TRACK_HARBOUR) {
        /* From the seafront straight, through the chicane and down the
           beach straight, out past the shallows. */
        stage = (Stage){track_wrap(track_seg_s(6) + 2.0f), {108.0f, 111.5f}, 60.0f, 2.3f, 6.5f, 2.0f, NO_LIMIT};
        return;
    }
    if(track.id == TRACK_TEMPLE) {
        /* Out of the start line along the beach straight. */
        stage = (Stage){6.0f, {108.0f, 111.5f}, 48.0f, 2.2f, 6.5f, 2.0f, NO_LIMIT};
        return;
    }
    staged = false;
}

void dolphins_reset(void) {
    stage_for_track();
    memset(shows, 0, sizeof(shows));
    memset(seen, 0, sizeof(seen));
    for(int p = 0; p < MAX_PLAYERS; p++) prev_s[p] = -1;
    newest_t = -1;
}

/* Where dolphin i is at show time t; false while it is under water. */
static bool dolphin_pose(const Show *sh, int i, float t, V3 *pos, float *pitch) {
    float tau = t - stagger_t[i];
    float x = sh->x + sh->speed * stagger_t[i] + stagger_x[i];
    for(int k = 0; k < LEAPS; k++) {
        float u = (tau - (FIRST_LEAP + k * LEAP_GAP)) / LEAP_TIME;
        if(u < 0 || u > 1 || (sh->dry[i] & (1u << k))) continue;
        float y = LEAP_DEPTH + stage.peak * 4.0f * u * (1.0f - u);
        *pos = v3(x, y, stage.lane[i]);
        float vy = stage.peak * 4.0f * (1.0f - 2.0f * u) / LEAP_TIME;
        *pitch = atan2f(vy, sh->speed);
        return true;
    }
    return false;
}

static void start_show(const Kart *k) {
    for(int i = 0; i < MAX_SHOWS; i++)
        if(!shows[i].active) {
            Show *sh = &shows[i];
            sh->active = true;
            sh->t = 0;
            sh->kart = (int)(k - karts);
            sh->dry[0] = sh->dry[1] = 0;
            sh->speed = clampf(fabsf(k->speed), 12.0f, 33.0f);
            sh->x = fminf(k->pos.x, 90.0f) - stage.ahead;
            sh->prev_y[0] = sh->prev_y[1] = LEAP_DEPTH;
            sh->last[0] = v3(sh->x, WATER_Y, stage.lane[0]);
            sh->last[1] = v3(sh->x, WATER_Y, stage.lane[1]);
            return;
        }
}

void dolphins_update(float dt) {
    newest_t = -1;
    if(staged_id != track.id) stage_for_track();
    if(!staged) return;
    /* Trigger as each human reaches the staging point on the final lap; a
       second human arriving within a few seconds shares the first show. */
    float trigger = stage.trigger;
    for(int p = 0; p < game.num_players; p++) {
        const Kart *k = &karts[game.pl[p].kart];
        float s = k->tp.s;
        if(!seen[p] && k->lap == game.laps && prev_s[p] >= 0 && track_delta(prev_s[p], trigger) > 0 &&
           track_delta(trigger, s) >= 0 && track_delta(prev_s[p], s) < 20.0f) {
            seen[p] = true;
            bool recent = false;
            for(int i = 0; i < MAX_SHOWS; i++)
                if(shows[i].active && shows[i].t < 2.5f) recent = true;
            if(!recent) start_show(k);
        }
        prev_s[p] = s;
    }
    for(int i = 0; i < MAX_SHOWS; i++) {
        Show *sh = &shows[i];
        if(!sh->active) continue;
        float was = sh->t;
        sh->t += dt;
        /* Pace the kart: match its westward speed (never below 12 m/s, so
           a crash doesn't leave them hovering) and ease back to AHEAD. */
        const Kart *k = &karts[sh->kart];
        float target = k->pos.x - stage.ahead;
        sh->speed = clampf(-k->vel.x + (sh->x - target) * 0.8f, 12.0f, 35.0f);
        sh->x -= sh->speed * dt;
        if(sh->t > SHOW_LEN) {
            sh->active = false;
            continue;
        }
        if(newest_t < 0 || sh->t < newest_t) newest_t = sh->t;
        /* As each leap begins, skip it if it would land west of the limit
           at the current pace. */
        for(int d = 0; d < 2; d++)
            for(int k = 0; k < LEAPS; k++) {
                float at = stagger_t[d] + FIRST_LEAP + k * LEAP_GAP;
                if(was >= at || sh->t < at) continue;
                float x = sh->x + sh->speed * stagger_t[d] + stagger_x[d];
                if(x - sh->speed * LEAP_TIME < stage.west) sh->dry[d] |= (uint8_t)(1u << k);
            }
        /* Splashes where each dolphin breaks or re-enters the surface. */
        for(int d = 0; d < 2; d++) {
            V3 pos = sh->last[d];
            float pitch;
            float y = LEAP_DEPTH;
            if(dolphin_pose(sh, d, sh->t, &pos, &pitch)) {
                y = pos.y;
                sh->last[d] = pos;
            }
            bool up = sh->prev_y[d] < WATER_Y && y >= WATER_Y;
            bool down = sh->prev_y[d] >= WATER_Y && y < WATER_Y;
            if(up || down) {
                V3 at = v3(pos.x, WATER_Y, pos.z);
                fx_splash(at, (up ? 1.0f : 1.3f) * stage.splash);
                V3 rel = vsub(at, karts[game.pl[0].kart].pos);
                float dist = vlen(rel);
                int vol = (int)(170.0f * clampf(1.0f - dist / 150.0f, 0, 1));
                int pan = (int)clampf(128.0f + vdot(vnorm(rel), game.pl[0].cam.right) * 90.0f, 0, 255);
                audio_sfx_pitch(DK_SFX_WHOOSH, vol, pan, up ? 0.62f : 0.5f);
            }
            sh->prev_y[d] = y;
        }
    }
}

void dolphins_draw(void) {
    if(!staged) return;
    for(int i = 0; i < MAX_SHOWS; i++) {
        if(!shows[i].active) continue;
        for(int d = 0; d < 2; d++) {
            V3 pos;
            float pitch;
            if(!dolphin_pose(&shows[i], d, shows[i].t, &pos, &pitch)) continue;
            if(!r_sphere_visible(pos, 3.0f)) continue;
            float m[16];
            /* Swimming west (-X): heading -90 degrees; a little roll. */
            float roll = 0.12f * sinf(shows[i].t * 3.0f + d);
            m4_translate_rotate(m, pos, -DK_PI * 0.5f, pitch, roll);
            m4_scale_apply(m, stage.scale, stage.scale, stage.scale);
            r_load_model(m);
            r_hdr(h_dolphin);
            r_mesh(verts, nverts, idx, nidx, false);
        }
    }
}

float dolphins_show_time(void) {
    return newest_t;
}
