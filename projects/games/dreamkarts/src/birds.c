/* Birds: the beach gulls, and Temple Hill's canopy flock.

   Gulls stand in small groups beside the sea, on Emerald Beach at the
   water's edge on the outside of turn 1, walking with the tide as it comes
   and goes, and on Coral Crossing along the barrier rail on the beach side
   of both loops (on the sand beyond it, the barrier hides them), there
   only on the first and final laps so they stay a treat; on Bay City
   Hills along the pier's low sea-side rail, out and back. A
   kart coming near flushes a group: each bird lifts off away from it, out
   over the sea in a wide loop, and lands back where it was 5-7 s later (or
   goes round again if a kart is still close).

   On Temple Hill a flock of parrots roosts in the palms beside the first
   hairpin and bursts out of the canopy as a human player comes up to it
   on the first lap, scattering out over the road ahead.

   Cosmetic only: they never touch a kart, and their randomness is a fixed
   hash, so the race RNG is left alone. */
#include "birds.h"
#include "kart.h"
#include "render.h"
#include "tide.h"
#include "track.h"
#include "world.h"
#include "world_int.h"

#define GULL_N      16
#define GULL_SCALE  2.4f       /* cartoon-sized so they read at speed */
#define GULL_Y      0.16f      /* body height off the sand */
#define WALK_SPEED  1.2f       /* m/s, keeping ahead of the tide */

#define FLOCK_N     16
#define FLOCK_SCALE 2.6f
#define FLOCK_LEN   7.0f       /* seconds in sight after the burst */

typedef struct {
    float s, d, d_off;    /* home along the track; d follows the waterline */
    float yaw;            /* facing while on the ground */
    float air_t, air_len; /* seconds into the flight, < 0 on the ground */
    float flush;          /* how near a kart may come */
    float out, side;      /* the loop: reach out to sea and sideways sweep */
    float height, seed;
    float sea;            /* which side the sea is: +1 right, -1 left */
    bool tidal;           /* stands at the tide's edge (Emerald Beach), else on the rail */
    float rail_h;         /* the rail's top above the road */
    V3 home, dir;         /* takeoff point and heading out */
} Gull;

typedef struct {
    V3 perch, dir;        /* start in the canopy, heading away */
    float delay, speed, climb, seed;
    int pal;
} Parrot;

static Gull gulls[GULL_N];
static int gull_n;
static Parrot parrots[FLOCK_N];
static bool flock_on;
static float flock_t = -1.0f, flock_trigger;
static float prev_s[MAX_PLAYERS];
static const pvr_poly_hdr_t *h_bird;

/* Body, belly/shade, wing, wing tip and beak. */
static const uint32_t gull_pal[5] = {0xfff4f4f0, 0xffc9ccce, 0xffa3adb8, 0xff2c3036, 0xffffc23a};
static const uint32_t parrot_pal[4][5] = {
    {0xffe0302a, 0xffb82420, 0xffe0302a, 0xff2a5cd8, 0xfff2ead8},   /* scarlet macaw */
    {0xff2f74e0, 0xfff2c632, 0xff2f74e0, 0xff1a3c8a, 0xff2a2a2a},   /* blue and gold */
    {0xff3cb84a, 0xff2c8c3a, 0xff3cb84a, 0xff2a5cd8, 0xfff2c632},   /* green parrot */
    {0xfff2c632, 0xffe08a20, 0xff3cb84a, 0xff2a5cd8, 0xff2a2a2a},   /* sun conure */
};

static float hash01(int i, int k) {
    uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)k * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (float)(h & 0xffff) / 65535.0f;
}

static const TrackSample *sample_at(float s) {
    return &track.smp[(int)(track_wrap(s) / track.ds) % TRACK_SAMPLES];
}

/* Where a standing gull wants to be. On the tide: just above the
   waterline, or on the sand a few metres past the curb when the sea is
   out (or there is no tide), near enough the road to be seen. */
static float ground_d(const Gull *g) {
    if(!g->tidal) return g->d_off;
    const TrackSample *t = sample_at(g->s);
    float edge = tide_on() ? tide_edge(g->s) : 100.0f;
    return fminf(edge + 1.3f, fminf(t->half + t->curb + 4.0f, t->wall_out - 2.5f)) + g->d_off;
}

static V3 ground_pos(const Gull *g) {
    V3 p = track_point(g->s, g->d);
    p.y = track_height(g->s) + (g->tidal ? 0.0f : g->rail_h) + GULL_Y;
    return p;
}

void birds_init(void) {
    h_bird = r_header(TEX_NONE, HF_OP);
}

typedef struct {
    float s;
    int n;
    int side;             /* 0: at the tide's edge; else on that side's barrier */
} GullGroup;

static void add_group(const GullGroup *gg) {
    for(int j = 0; j < gg->n && gull_n < GULL_N; j++) {
        int i = gull_n++;
        Gull *g = &gulls[i];
        g->tidal = gg->side == 0;
        g->rail_h = track.id == TRACK_SF ? SF_PIER_RAIL_H : f8m.wall_h;
        g->sea = gg->side < 0 ? -1.0f : 1.0f;
        g->s = gg->s + ((float)j - (gg->n - 1) * 0.5f) * (g->tidal ? 2.4f : 3.0f) + (hash01(i, 1) - 0.5f) * 1.0f;
        const TrackSample *t = sample_at(g->s);
        g->d_off = g->tidal ? (hash01(i, 2) - 0.5f) * 2.2f : g->sea * (g->sea > 0 ? t->wall_out : t->wall_in);
        g->d = ground_d(g);
        g->yaw = hash01(i, 3) * DK_TAU;
        g->flush = (g->tidal ? 24.0f : 18.0f) + hash01(i, 4) * 6.0f;
        g->out = 14.0f + hash01(i, 5) * 10.0f;
        g->side = (hash01(i, 6) < 0.5f ? -1.0f : 1.0f) * (8.0f + hash01(i, 7) * 8.0f);
        g->height = 6.0f + hash01(i, 8) * 5.0f;
        g->air_len = 5.5f + hash01(i, 9) * 2.0f;
        g->seed = hash01(i, 10) * 10.0f;
        g->air_t = -1.0f;
    }
}

/* Temple Hill: the palms nearest the outside of the first hairpin. */
static void flock_build(void) {
    float apex = track_seg_s(5) + 26.7f;
    V3 c = track_point(apex, -24.0f);
    int near[3] = {-1, -1, -1};
    float best[3] = {1e9f, 1e9f, 1e9f};
    for(int i = 0; i < palm_n; i++) {
        V3 d = vsub(palms[i].base, c);
        float q = d.x * d.x + d.z * d.z;
        for(int k = 0; k < 3; k++)
            if(q < best[k]) {
                for(int m = 2; m > k; m--) {
                    best[m] = best[m - 1];
                    near[m] = near[m - 1];
                }
                best[k] = q;
                near[k] = i;
                break;
            }
    }
    V3 mid = track_point(apex, 0);
    for(int i = 0; i < FLOCK_N; i++) {
        Parrot *b = &parrots[i];
        int pi = near[i % 3];
        V3 top = pi >= 0 && best[i % 3] < 60.0f * 60.0f ? vadd(palms[pi].base, v3(0, palms[pi].height, 0))
                                                         : vadd(c, v3(0, track_height(apex) + 9.0f, 0));
        b->perch = vadd(top, v3((hash01(i, 21) - 0.5f) * 3.0f, (hash01(i, 22) - 0.5f) * 1.5f, (hash01(i, 23) - 0.5f) * 3.0f));
        /* Out over the road ahead of the player, fanned out. */
        V3 over = vsub(mid, b->perch);
        float a = atan2f(over.x, over.z) + (hash01(i, 24) - 0.5f) * 1.2f;
        b->dir = v3(sinf(a), 0, cosf(a));
        b->delay = hash01(i, 25) * 0.6f;
        b->speed = 9.0f + hash01(i, 26) * 5.0f;
        b->climb = 1.5f + hash01(i, 27) * 2.0f;
        b->seed = hash01(i, 28) * 10.0f;
        b->pal = i % 4;
    }
    flock_trigger = track_seg_s(5) - 25.0f;
    flock_on = true;
}

void birds_build(void) {
    gull_n = 0;
    flock_on = false;
    flock_t = -1.0f;
    for(int p = 0; p < MAX_PLAYERS; p++) prev_s[p] = -1.0f;
    if(track.id == TRACK_BEACH) {
        /* Along the outside of turn 1 and its exit. */
        static const GullGroup g[] = {{150, 3, 0}, {185, 4, 0}, {212, 4, 0}, {238, 3, 0}};
        for(unsigned i = 0; i < sizeof(g) / sizeof(g[0]); i++) add_group(&g[i]);
    }
    else if(track.id == TRACK_FIGURE8) {
        /* The north beach, outside the top of each loop: the sea is to the
           right round the west loop and to the left round the east. */
        static const GullGroup g[] = {{128, 4, 1}, {160, 4, 1}, {515, 4, -1}, {548, 4, -1}};
        for(unsigned i = 0; i < sizeof(g) / sizeof(g[0]); i++) add_group(&g[i]);
    }
    else if(track.id == TRACK_TEMPLE)
        flock_build();
    else if(track.id == TRACK_SF) {
        /* The pier's lanes out and back, the sea on their right. */
        float out = track_seg_s(SF_PIER_SEG + 1), back = track_seg_s(SF_PIER_SEG + 3);
        const GullGroup g[] = {{out + 18, 4, 1}, {out + 52, 3, 1}, {back + 16, 3, 1}, {back + 48, 4, 1}};
        for(unsigned i = 0; i < sizeof(g) / sizeof(g[0]); i++) add_group(&g[i]);
    }
}

/* The gull's flight: out and up away from the kart, round, and back down
   onto the takeoff point. u runs 0 to 1. */
static V3 flight_pos(const Gull *g, float u) {
    float a = DK_PI * u, rise = powf(sinf(a), 0.55f);
    V3 side = v3(-g->dir.z, 0, g->dir.x);
    V3 p = vmad(g->home, g->dir, g->out * sinf(a));
    p = vmad(p, side, g->side * 0.5f * (1.0f - cosf(2.0f * a)));
    p.y = g->home.y + g->height * rise;
    return p;
}

/* A parrot t seconds after leaving the canopy: a burst up and out,
   levelling off as it goes, with a little weave. */
static V3 parrot_pos(const Parrot *b, float t) {
    V3 side = v3(-b->dir.z, 0, b->dir.x);
    V3 p = vmad(b->perch, b->dir, b->speed * t);
    p = vmad(p, side, 1.5f * sinf(t * 1.7f + b->seed));
    p.y += b->climb * 4.0f * (1.0f - expf(-t * 0.8f)) + 1.5f * t;
    return p;
}

static int humans(void) {
    return game.num_players > 0 ? (game.num_players < MAX_PLAYERS ? game.num_players : MAX_PLAYERS) : 1;
}

/* Coral Crossing's gulls only while a human is on the first or final lap. */
static bool gulls_shown(void) {
    if(track.id != TRACK_FIGURE8) return true;
    for(int p = 0; p < humans(); p++) {
        int lap = karts[game.pl[p].kart].lap;
        if(lap <= 1 || lap >= game.laps) return true;
    }
    return false;
}

static void flock_update(float dt) {
    if(flock_t >= 0 && (flock_t += dt) > FLOCK_LEN) flock_t = -1.0f;
    for(int p = 0; p < humans(); p++) {
        const Kart *k = &karts[game.pl[p].kart];
        float s = k->tp.s;
        if(flock_t < 0 && k->lap <= 1 && prev_s[p] >= 0 && track_delta(prev_s[p], flock_trigger) > 0 &&
           track_delta(flock_trigger, s) >= 0 && track_delta(prev_s[p], s) < 20.0f)
            flock_t = 0;
        prev_s[p] = s;
    }
}

void birds_update(float dt) {
    if(flock_on) flock_update(dt);
    if(!gulls_shown()) {
        /* Back on their perches for when they next appear. */
        for(int i = 0; i < gull_n; i++) gulls[i].air_t = -1.0f;
        return;
    }
    for(int i = 0; i < gull_n; i++) {
        Gull *g = &gulls[i];
        if(g->air_t >= 0) {
            g->air_t += dt;
            if(g->air_t < g->air_len) continue;
            g->air_t = -1.0f;
            g->yaw = atan2f(g->dir.x, g->dir.z) + DK_PI + (hash01(i, (int)world_time) - 0.5f);
        }
        /* Walk with the water. */
        float want = ground_d(g), step = WALK_SPEED * dt;
        g->d += clampf(want - g->d, -step, step);
        V3 p = ground_pos(g);
        for(int k = 0; k < MAX_KARTS; k++) {
            const Kart *kt = &karts[k];
            float dx = p.x - kt->pos.x, dz = p.z - kt->pos.z;
            if(fabsf(kt->speed) < 3.0f || dx * dx + dz * dz > g->flush * g->flush) continue;
            /* Away from the kart, swung out towards the sea. */
            V3 away = vnorm(v3(dx, 0, dz));
            V3 r = vscale(sample_at(g->s)->r, 1.5f * g->sea);
            g->dir = vnorm(v3(away.x + r.x, 0, away.z + r.z));
            g->home = p;
            g->air_t = 0;
            break;
        }
    }
}

/* One bird in its own frame (+z forward, +y up): body, crown, beak, tail,
   two wings of two panels each and, standing, two legs, coloured from pal
   (body, shade, wing, tip, beak). open 0 folds the wings along the back, 1
   spreads them; flap raises (+) or lowers (-) them. */
static void draw_bird(V3 pos, float yaw, float pitch, float roll, float open, float flap, float scale,
                      const uint32_t *pal, bool legs) {
    /* Body 0-5 (head, tail end, left, right, back, belly), beak 6-8,
       tail 9-11, then each wing: root front, root back, elbow front, elbow
       back, tip. */
    static const uint16_t idx[] = {
        0, 2, 4, 0, 4, 3, 0, 3, 5, 0, 5, 2, 1, 4, 2, 1, 3, 4, 1, 5, 3, 1, 2, 5,
        6, 7, 8, 9, 10, 11,
        12, 13, 14, 13, 15, 14, 14, 15, 16,
        17, 18, 19, 18, 20, 19, 19, 20, 21,
        22, 0, 2, 22, 3, 0, 22, 4, 3, 22, 2, 4,      /* crown */
        23, 24, 25, 26, 27, 28,                      /* legs */
    };
    const uint32_t body = pal[0], shade = pal[1], wing = pal[2], tip = pal[3], beak = pal[4];
    RV v[29] __attribute__((aligned(32)));
    int n = 0;
#define P(x, y, z, c) (v[n++] = (RV){(x), (y), (z), 0, 0, (c)})
    P(0, 0.07f, 0.30f, body);
    P(0, 0.02f, -0.26f, shade);
    P(-0.08f, 0, 0, body);
    P(0.08f, 0, 0, body);
    P(0, 0.10f, 0.02f, body);
    P(0, -0.07f, 0, shade);
    P(-0.015f, 0.06f, 0.30f, beak);
    P(0.015f, 0.06f, 0.30f, beak);
    P(0, 0.05f, 0.42f, beak);
    P(-0.07f, 0.03f, -0.22f, shade);
    P(0.07f, 0.03f, -0.22f, shade);
    P(0, 0.02f, -0.38f, body);
    /* Spread, the elbow sits 0.3 out and the tip 0.32 beyond, the outer
       panel bending further than the inner; folded, both lie along the
       back towards the tail. */
    float c0 = cosf(flap), s0 = sinf(flap), c1 = cosf(flap * 1.5f), s1 = sinf(flap * 1.5f);
    float ex = lerpf(0.09f, 0.30f * c0, open), ey = lerpf(0.07f, 0.30f * s0 + 0.04f, open);
    float tx = lerpf(0.06f, 0.30f * c0 + 0.32f * c1, open), ty = lerpf(0.07f, ey + 0.32f * s1, open);
    float ez = lerpf(-0.10f, 0.04f, open), tz = lerpf(-0.36f, -0.14f, open);
    for(int sd = -1; sd <= 1; sd += 2) {
        P(sd * 0.06f, 0.05f, 0.10f, wing);
        P(sd * 0.06f, 0.05f, -0.10f, wing);
        P(sd * ex, ey, ez, wing);
        P(sd * ex, ey, ez - 0.14f, wing);
        P(sd * tx, ty, tz, tip);
    }
    P(0, 0.16f, 0.24f, body);
    for(int sd = -1; sd <= 1; sd += 2) {
        P(sd * 0.035f, -0.04f, 0.04f, beak);
        P(sd * 0.035f, -0.04f, -0.02f, beak);
        P(sd * 0.035f, -0.17f, 0.02f, beak);
    }
#undef P
    float m[16];
    m4_translate_rotate(m, pos, yaw, pitch, roll);
    m4_scale_apply(m, scale, scale, scale);
    r_load_model(m);
    int ni = (int)(sizeof(idx) / sizeof(idx[0]));
    r_mesh(v, n, idx, legs ? ni : ni - 6, false);
}

static bool in_range(V3 p, float range, float radius) {
    float dx = p.x - r_cam.pos.x, dz = p.z - r_cam.pos.z;
    return dx * dx + dz * dz < range * range && r_sphere_visible(p, radius);
}

static void draw_flock(void) {
    for(int i = 0; i < FLOCK_N; i++) {
        const Parrot *b = &parrots[i];
        float t = flock_t - b->delay;
        if(t < 0) continue;
        V3 pos = parrot_pos(b, t);
        if(!in_range(pos, 220.0f, 1.2f)) continue;
        V3 vel = vsub(parrot_pos(b, t + 0.02f), pos);
        float hz = sqrtf(vel.x * vel.x + vel.z * vel.z);
        float pitch = clampf(atan2f(vel.y, hz), -0.3f, 0.9f);
        float roll = 0.4f * cosf(t * 1.7f + b->seed);
        /* Frantic out of the trees, settling to a steadier beat. */
        float rate = lerpf(22.0f, 14.0f, clampf(t / 2.0f, 0, 1));
        float flap = 0.1f + 0.8f * sinf(t * rate + b->seed);
        draw_bird(pos, atan2f(vel.x, vel.z), pitch, roll, clampf(t / 0.12f, 0, 1), flap, FLOCK_SCALE,
                  parrot_pal[b->pal], false);
    }
}

void birds_draw(void) {
    int n = gulls_shown() ? gull_n : 0;
    if(!n && flock_t < 0) return;
    r_hdr(h_bird);
    if(flock_t >= 0) draw_flock();
    for(int i = 0; i < n; i++) {
        const Gull *g = &gulls[i];
        V3 pos;
        float yaw, pitch = 0, roll = 0, open, flap;
        if(g->air_t < 0) {
            pos = ground_pos(g);
            float t = world_time + g->seed;
            /* Chest up, an odd peck, and a little bob while walking. */
            pitch = 0.2f - 0.6f * fmaxf(0.0f, sinf(t * 0.9f) - 0.85f) * 6.0f;
            pos.y += 0.03f * fabsf(sinf(t * 7.0f)) * fminf(fabsf(ground_d(g) - g->d) * 4.0f, 1.0f);
            yaw = g->yaw;
            open = 0;
            flap = 0;
        }
        else {
            float u = g->air_t / g->air_len;
            pos = flight_pos(g, u);
            V3 ahead = flight_pos(g, fminf(u + 0.01f, 1.0f)), back = flight_pos(g, fmaxf(u - 0.01f, 0.0f));
            V3 vel = vsub(ahead, back);
            yaw = atan2f(vel.x, vel.z);
            float hz = sqrtf(vel.x * vel.x + vel.z * vel.z);
            pitch = clampf(atan2f(vel.y, hz), -0.5f, 0.7f);
            roll = -0.5f * g->side / 16.0f * cosf(2.0f * DK_PI * u);
            /* Wings out within the first 0.15 s, folded over the last 0.2. */
            open = clampf(g->air_t / 0.15f, 0, 1) * clampf((g->air_len - g->air_t) / 0.2f, 0, 1);
            /* Hard flapping on the climb, a glide with the odd beat after. */
            float beat = vel.y > 0 ? 1.0f : 0.25f + 0.25f * (sinf(g->air_t * 0.8f + g->seed) > 0.6f);
            flap = 0.15f + 0.75f * beat * sinf(g->air_t * 17.0f + g->seed);
        }
        /* Bay City Hills' pier is in view from the start straight, the
           busiest view on the lap, where they are specks anyway. */
        if(!in_range(pos, track.id == TRACK_SF ? 70.0f : 220.0f, 1.2f)) continue;
        draw_bird(pos, yaw, pitch, roll, open, flap, GULL_SCALE, gull_pal, g->air_t < 0);
    }
    r_load_vp();
}

float birds_flock_time(void) {
    return flock_t;
}
