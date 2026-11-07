/* Ridgeline birds (birds.h).

   Perched: Steller's jays and juncos in twos and threes along some of the
   fallen logs, the picnic table and the trail signs, the odd one on top of
   a big boulder, and ravens on the cabin's roof, the cairns, the ledges and
   the lookout. They hop about and peck while they wait. A truck coming
   near (or a horn) flushes the group: each bird bursts off away from it,
   climbing, and is gone over the trees. They come back once every truck
   and the camera are well clear, out of sight.

   Overhead: a red-tailed hawk circles above the meadow and each of two
   bluffs, and now and then a skein of geese crosses the sky in a V, ahead
   of the camera so it passes through the view.

   Cosmetic only: nothing here touches a truck, and the randomness is a
   hash and a generator of its own, so the game's sequence is left alone. */
#include "birds.h"
#include "terrain.h"
#include "truck.h"
#include "world.h"
#define ENG_BIRD_IMPL
#include "engine/bird.h"

enum { SP_JAY, SP_JUNCO, SP_RAVEN, SP_HAWK, SP_GOOSE, SP_COUNT };

typedef struct {
    uint32_t pal[BIRD_COLS];
    float scale;
    float flush;          /* how near a moving truck may come, m */
    float beat;           /* wing beat, radians a second */
    float range;          /* drawn within, m */
    float out, rise, len; /* the flight away: distance, climb, seconds */
} Species;

/* Bigger than life (a jay is 0.3 m long), so they read from the chase
   camera. */
static const Species species[SP_COUNT] = {
    [SP_JAY] = {{0xff2c4c8c, 0xff22386a, 0xff3466b4, 0xff141820, 0xff1a1a1e}, 0.62f, 14.0f, 34.0f, 65.0f, 50, 14, 5.0f},
    [SP_JUNCO] = {{0xff50545c, 0xffe6e2da, 0xff40444c, 0xff24262a, 0xffe2c4b4}, 0.55f, 12.0f, 38.0f, 60.0f, 45, 11, 4.5f},
    [SP_RAVEN] = {{0xff202228, 0xff16171b, 0xff26282f, 0xff0e0f12, 0xff151518}, 1.05f, 22.0f, 15.0f, 120.0f, 95, 24, 9.0f},
    [SP_HAWK] = {{0xff7a5838, 0xffe6dac4, 0xff6c4c30, 0xff2a2018, 0xffd8b040}, 1.7f, 0, 0, 420.0f, 0, 0, 0},
    [SP_GOOSE] = {{0xff7c6e5e, 0xffd8d0c2, 0xff62564a, 0xff1c1a18, 0xff141414}, 3.0f, 0, 15.0f, 420.0f, 0, 0, 0},
};

enum { B_PERCHED, B_FLYING, B_GONE };

typedef struct {
    V3 home;              /* where its feet are */
    float yaw;            /* facing on the perch, before it fidgets */
    V3 from, dir;         /* takeoff point and heading away */
    float t, delay;       /* seconds since the flush, and its own lag */
    float side, seed;
    uint8_t sp, state, group;
} Bird;

typedef struct {
    V3 c;                 /* centre, for the flush */
    uint8_t first, n, sp;
} Group;

#define MAX_BIRDS 96
#define MAX_GROUPS 40
static Bird birds[MAX_BIRDS];
static int bird_n;
static Group groups[MAX_GROUPS];
static int group_n;

/* Hawks circling: centre, radius, height over the ground there. */
typedef struct { float x, z, r, h, phase, dir; } Hawk;
static const Hawk hawks[] = {
    {10, -235, 34, 42, 0.0f, 1},     /* over the meadow */
    {118, 36, 28, 38, 2.0f, -1},     /* Eagle Bluff */
    {-80, 84, 40, 46, 4.0f, 1},      /* above Cedar Falls */
};
#define HAWK_N (int)(sizeof(hawks) / sizeof(hawks[0]))
static float hawk_y[HAWK_N];

/* A skein of geese. */
#define GEESE 9
#define SKEIN_SPEED 14.0f
#define SKEIN_HALF 260.0f     /* metres either side of where it crosses */
static struct {
    bool on;
    V3 c, dir;            /* where it crosses the view, heading */
    float s;              /* along the path from -SKEIN_HALF */
    float wait;           /* seconds to the next */
    float seed;
} skein;

static float bird_clock;
static uint32_t rng = 0x5eed1234u;
static const pvr_poly_hdr_t *h_bird;

static float hash01(int i, int k) {
    uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)k * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (float)(h & 0xffff) / 65535.0f;
}

static float brand(void) {
    rng = rng * 1664525u + 1013904223u;
    return (float)(rng >> 16) / 65535.0f;
}

static void add_bird(int sp, V3 feet, float yaw) {
    if(bird_n >= MAX_BIRDS) return;
    int i = bird_n++;
    birds[i] = (Bird){.home = feet, .yaw = yaw, .sp = (uint8_t)sp, .state = B_PERCHED, .group = (uint8_t)group_n,
                      .seed = hash01(i, 1) * 10.0f, .side = hash01(i, 2) - 0.5f};
}

static void group_begin(int sp) {
    groups[group_n] = (Group){.first = (uint8_t)bird_n, .sp = (uint8_t)sp};
}

static void group_end(void) {
    Group *g = &groups[group_n];
    g->n = (uint8_t)(bird_n - g->first);
    if(!g->n) return;
    V3 c = v3(0, 0, 0);
    for(int i = g->first; i < bird_n; i++) c = vadd(c, birds[i].home);
    g->c = vscale(c, 1.0f / g->n);
    if(group_n < MAX_GROUPS - 1) group_n++;
}

void birds_init(void) {
    h_bird = r_header(TEX_NONE, HF_OP);
    bird_n = group_n = 0;
    /* Logs: two to four small birds along the top of most of them. */
    for(int i = 0; i < LVL_CAPSULES; i++) {
        if(hash01(i, 10) > 0.7f) continue;
        const lvl_capsule_t *cp = &lvl_capsules[i];
        int sp = hash01(i, 11) < 0.5f ? SP_JAY : SP_JUNCO;
        int n = 2 + (int)(hash01(i, 12) * 2.99f);
        V3 a = v3(cp->ax, cp->ay + cp->r, cp->az), b = v3(cp->bx, cp->by + cp->r, cp->bz);
        float along = atan2f(b.x - a.x, b.z - a.z);
        float u0 = 0.2f + 0.3f * hash01(i, 13);
        group_begin(sp);
        for(int k = 0; k < n; k++) {
            float u = u0 + 0.12f * k + 0.04f * hash01(i, 20 + k);
            add_bird(sp, vadd(a, vscale(vsub(b, a), u)), along + (hash01(i, 30 + k) < 0.5f ? 0.5f : -0.5f) * ENG_PI);
        }
        group_end();
    }
    /* Big boulders: a junco or two on some, a raven on the odd one. */
    for(int i = 0; i < LVL_SPHERES; i++) {
        const lvl_sphere_t *s = &lvl_spheres[i];
        if(s->r < 0.9f || hash01(i, 40) > 0.4f) continue;
        bool raven = hash01(i, 41) < 0.3f;
        int sp = raven ? SP_RAVEN : SP_JUNCO;
        int n = raven ? 1 : 1 + (hash01(i, 42) > 0.5f);
        group_begin(sp);
        for(int k = 0; k < n; k++) {
            float a = hash01(i, 43 + k) * ENG_TAU, d = n > 1 ? 0.12f * s->r : 0;
            V3 p = v3(s->x + cosf(a) * d, world_boulder_top(i) - 0.03f, s->z + sinf(a) * d);
            add_bird(sp, p, hash01(i, 45 + k) * ENG_TAU);
        }
        group_end();
    }
    /* Rock ledges: a raven on the near end of some. */
    for(int i = 0; i < LVL_BOXES; i++) {
        const lvl_box_t *b = &lvl_boxes[i];
        if(b->hx > 3.9f && b->hy > 1.5f) continue;   /* a building's footprint */
        if(hash01(i, 50) > 0.6f) continue;
        float off = (hash01(i, 51) - 0.5f) * b->hx;
        V3 p = v3(b->x + cosf(b->yaw) * off, b->y + b->hy, b->z - sinf(b->yaw) * off);
        group_begin(SP_RAVEN);
        add_bird(SP_RAVEN, p, hash01(i, 52) * ENG_TAU);
        group_end();
    }
    /* The props (world.c builds them): the cabin's ridge, the sign boards,
       the picnic table, the cairns' poles and the lookout's roof. */
    for(int i = 0; i < LVL_PROPS; i++) {
        const lvl_prop_t *p = &lvl_props[i];
        float cy = cosf(p->yaw), sy = sinf(p->yaw);
        V3 c = v3(p->x, p->y, p->z);
        switch(p->kind) {
            case PROP_CABIN: {
                group_begin(SP_RAVEN);
                V3 along = v3(sy, 0, cy);
                add_bird(SP_RAVEN, vadd(v3(c.x, c.y + 5.0f, c.z), vscale(along, -1.8f)), p->yaw + ENG_PI * 0.5f);
                add_bird(SP_RAVEN, vadd(v3(c.x, c.y + 5.0f, c.z), vscale(along, 0.9f)), p->yaw - ENG_PI * 0.5f);
                group_end();
                break;
            }
            case PROP_SIGN:
                if(hash01(i, 60) > 0.6f) break;
                group_begin(SP_JAY);
                add_bird(SP_JAY, v3(c.x + cy * 0.4f, c.y + 2.25f, c.z - sy * 0.4f), p->yaw);
                group_end();
                break;
            case PROP_TABLE:
                group_begin(SP_JUNCO);
                add_bird(SP_JUNCO, v3(c.x + cy * 0.5f, c.y + 0.8f, c.z - sy * 0.5f), p->yaw);
                add_bird(SP_JUNCO, v3(c.x - cy * 0.2f, c.y + 0.8f, c.z + sy * 0.2f), p->yaw + 2.5f);
                add_bird(SP_JUNCO, v3(c.x - cy * 0.6f + sy * 0.15f, c.y + 0.8f, c.z + sy * 0.6f + cy * 0.15f), p->yaw + 4.0f);
                group_end();
                break;
            case PROP_CAIRN:
                /* The pole's top: the four stones stacked, then the pole. */
                group_begin(SP_RAVEN);
                add_bird(SP_RAVEN, v3(c.x + 0.1f, c.y + 1.309f + 1.4f, c.z), hash01(i, 61) * ENG_TAU);
                group_end();
                break;
            case PROP_TOWER:
                group_begin(SP_RAVEN);
                add_bird(SP_RAVEN, v3(c.x, c.y + 9.72f, c.z), 1.0f);
                group_end();
                break;
        }
    }
    for(int i = 0; i < HAWK_N; i++) hawk_y[i] = terrain_height(hawks[i].x, hawks[i].z) + hawks[i].h;
    printf("Ridgeline birds: %d perched in %d groups\n", bird_n, group_n);
    birds_reset();
}

void birds_reset(void) {
    for(int i = 0; i < bird_n; i++) birds[i].state = B_PERCHED;
    skein.on = false;
    skein.wait = 12.0f + 12.0f * brand();
}

/* Away from the takeoff point: a burst up, then out and climbing, curving
   a little to one side. u runs 0 to 1. */
static V3 flight_pos(const Bird *b, float t, float *u_out) {
    const Species *sp = &species[b->sp];
    float u = t / sp->len;
    *u_out = u;
    float d = sp->out * u * (0.55f + 0.45f * u);
    V3 side = v3(-b->dir.z, 0, b->dir.x);
    V3 p = vmad(b->from, b->dir, d);
    p = vmad(p, side, b->side * 0.4f * d * u);
    p.y = b->from.y + 0.8f * clampf(t / 0.3f, 0, 1) + sp->rise * powf(u, 0.8f);
    float g = terrain_height(p.x, p.z) + 1.0f + 6.0f * u;
    if(p.y < g) p.y = g;
    return p;
}

static void flush(Group *g, V3 from) {
    for(int i = g->first; i < g->first + g->n; i++) {
        Bird *b = &birds[i];
        if(b->state != B_PERCHED) continue;
        V3 away = vsub(b->home, from);
        away.y = 0;
        float a = atan2f(away.x, away.z) + (hash01(i, (int)bird_clock) - 0.5f) * 1.4f;
        b->dir = v3(sinf(a), 0, cosf(a));
        b->from = b->home;
        b->t = 0;
        b->delay = hash01(i, 70 + (int)bird_clock) * (b->sp == SP_RAVEN ? 0.6f : 0.3f);
        b->state = B_FLYING;
    }
}

static void skein_update(float dt) {
    if(skein.on) {
        skein.s += SKEIN_SPEED * dt;
        if(skein.s > SKEIN_HALF) {
            skein.on = false;
            skein.wait = 35.0f + 40.0f * brand();
        }
        return;
    }
    if((skein.wait -= dt) > 0) return;
    /* Across the camera's view ahead, a quarter turn either way from
       straight across, in the top part of the picture (a chase camera
       looks down a little), or over the ground if that is higher. */
    V3 f = cam.fwd;
    f.y = 0;
    f = vnorm(f);
    float ahead = 90.0f + 60.0f * brand();
    V3 c = vmad(cam.pos, f, ahead);
    float a = atan2f(f.x, f.z) + (brand() < 0.5f ? 1 : -1) * (ENG_PI * 0.5f + (brand() - 0.5f) * ENG_PI * 0.5f);
    float pitch = asinf(clampf(cam.fwd.y, -1, 1)) + 0.25f + 0.08f * brand();
    c.y = fmaxf(cam.pos.y + ahead * tanf(pitch), terrain_height(c.x, c.z) + 18.0f);
    skein.c = c;
    skein.dir = v3(sinf(a), 0, cosf(a));
    skein.s = -SKEIN_HALF;
    skein.seed = brand() * 10.0f;
    skein.on = true;
}

void birds_update(float dt) {
    if(game.state == ST_DRIVE && game.paused) return;
    bird_clock += dt;
    skein_update(dt);
    int nt = game.state == ST_DRIVE || game.state == ST_SELECT ? game.ntrucks : 0;
    for(int gi = 0; gi < group_n; gi++) {
        Group *g = &groups[gi];
        const Species *sp = &species[g->sp];
        bool any_perched = false, all_gone = true;
        for(int i = g->first; i < g->first + g->n; i++) {
            Bird *b = &birds[i];
            if(b->state == B_PERCHED) any_perched = true;
            if(b->state != B_GONE) all_gone = false;
            if(b->state == B_FLYING && (b->t += dt) - b->delay > sp->len) b->state = B_GONE;
        }
        if(any_perched) {
            for(int k = 0; k < nt; k++) {
                const Truck *t = &trucks[k];
                V3 d = vsub(g->c, t->pos);
                float q = vdot(d, d), r = t->horn ? 40.0f : fabsf(t->speed) > 1.0f ? sp->flush : 5.0f;
                if(q < r * r) {
                    flush(g, t->pos);
                    break;
                }
            }
        }
        else if(all_gone) {
            /* Back when nobody is near enough to see them land. */
            float clear = sp->range + 10.0f;
            V3 d = vsub(g->c, cam.pos);
            bool ok = vdot(d, d) > clear * clear;
            for(int k = 0; k < nt && ok; k++) {
                V3 e = vsub(g->c, trucks[k].pos);
                ok = vdot(e, e) > clear * clear;
            }
            if(ok)
                for(int i = g->first; i < g->first + g->n; i++) birds[i].state = B_PERCHED;
        }
    }
}

static bool in_range(V3 p, float range, float radius) {
    float dx = p.x - r_cam.pos.x, dz = p.z - r_cam.pos.z;
    return dx * dx + dz * dz < range * range && r_sphere_visible(p, radius);
}

static void draw_perched(int i, const Bird *b, const Species *sp) {
    /* Fidgeting: every couple of seconds (ravens less often) a hop round
       to face somewhere new, and the odd peck in between. */
    float rate = b->sp == SP_RAVEN ? 0.18f : 0.55f;
    float ph = bird_clock * rate * (0.8f + 0.04f * b->seed) + b->seed;
    int step = (int)floorf(ph);
    float frac = ph - step;
    float yaw = b->yaw + (hash01(i, step) - 0.5f) * (b->sp == SP_RAVEN ? 1.6f : 3.0f);
    V3 pos = b->home;
    pos.y += 0.17f * sp->scale;
    float hop = frac < 0.12f ? sinf(frac / 0.12f * ENG_PI) : 0;
    pos.y += hop * 0.06f * sp->scale;
    float peck = fmaxf(0.0f, sinf((frac - 0.3f) * 9.0f)) * (frac > 0.3f && frac < 0.65f && hash01(i, step + 99) < 0.6f);
    float pitch = 0.15f - 0.7f * peck;
    eng_bird_draw(pos, yaw, pitch, 0, hop * 0.6f, 0.5f * hop, sp->scale, sp->pal, true);
}

static void draw_flying(const Bird *b, const Species *sp) {
    float t = b->t - b->delay, u;
    if(t < 0) {
        /* Waiting its turn: a crouch. */
        V3 pos = b->home;
        pos.y += 0.14f * sp->scale;
        eng_bird_draw(pos, atan2f(b->dir.x, b->dir.z), 0.2f, 0, 0, 0, sp->scale, sp->pal, true);
        return;
    }
    V3 pos = flight_pos(b, t, &u);
    if(!in_range(pos, sp->range * 1.4f, sp->scale)) return;
    float u2;
    V3 vel = vsub(flight_pos(b, t + 0.05f, &u2), pos);
    float hz = sqrtf(vel.x * vel.x + vel.z * vel.z);
    float pitch = clampf(atan2f(vel.y, hz), -0.4f, 0.8f);
    float roll = -b->side * 0.8f;
    /* Small birds bound: bursts of beats with the wings shut between,
       after the frantic takeoff. Ravens row steadily. */
    float open = clampf(t / 0.1f, 0, 1), beat = sp->beat * t + b->seed;
    if(b->sp != SP_RAVEN && t > 0.8f && fmodf(t + b->seed, 0.7f) > 0.45f) open = 0.15f;
    float flap = 0.15f + 0.8f * sinf(beat);
    /* Shrinks away to nothing over the last stretch, far off by then. */
    float s = sp->scale * clampf((1.0f - u) / 0.15f, 0, 1);
    if(s <= 0.01f) return;
    eng_bird_draw(pos, atan2f(vel.x, vel.z), pitch, roll, open, flap, s, sp->pal, false);
}

static void draw_hawks(void) {
    const Species *sp = &species[SP_HAWK];
    for(int i = 0; i < HAWK_N; i++) {
        const Hawk *h = &hawks[i];
        /* About 7 m/s round the circle, rising and sinking on the air. */
        float a = h->phase + h->dir * bird_clock * 7.0f / h->r;
        V3 pos = v3(h->x + sinf(a) * h->r, hawk_y[i] + 4.0f * sinf(bird_clock * 0.13f + h->phase), h->z + cosf(a) * h->r);
        if(!in_range(pos, sp->range, 1.5f)) continue;
        float yaw = a + h->dir * ENG_PI * 0.5f;
        /* Gliding with the wings a little raised, and every so often a few
           slow beats. */
        float burst = fmodf(bird_clock + h->phase * 3.0f, 11.0f);
        float flap = burst < 1.6f ? 0.15f + 0.55f * sinf(burst * 9.0f) : 0.12f;
        eng_bird_draw(pos, yaw, 0, h->dir * 0.35f, 1, flap, sp->scale, sp->pal, false);
    }
}

static void draw_skein(void) {
    const Species *sp = &species[SP_GOOSE];
    V3 side = v3(-skein.dir.z, 0, skein.dir.x);
    float yaw = atan2f(skein.dir.x, skein.dir.z);
    for(int k = 0; k < GEESE; k++) {
        /* The leader, then alternate arms of the V, one slightly longer. */
        int rank = (k + 1) / 2;
        float arm = k == 0 ? 0 : (k & 1) ? 1.0f : -1.0f;
        float back = rank * 5.0f + 0.8f * hash01(k, 80);
        V3 pos = vmad(skein.c, skein.dir, skein.s - back);
        pos = vmad(pos, side, arm * rank * 4.2f);
        pos.y += rank * 0.5f + 0.4f * sinf(bird_clock * 0.9f + k);
        /* Up and over a ridge in the way. */
        pos.y = fmaxf(pos.y, terrain_height(pos.x, pos.z) + 18.0f);
        if(!in_range(pos, sp->range, 2.0f)) continue;
        float flap = 0.1f + 0.6f * sinf(bird_clock * sp->beat + k * 0.7f + skein.seed);
        eng_bird_draw(pos, yaw, 0, 0, 1, flap, sp->scale, sp->pal, false);
    }
}

void birds_draw(void) {
    r_hdr(h_bird);
    for(int i = 0; i < bird_n; i++) {
        const Bird *b = &birds[i];
        const Species *sp = &species[b->sp];
        if(b->state == B_GONE) continue;
        if(b->state == B_FLYING) {
            draw_flying(b, sp);
            continue;
        }
        if(!in_range(b->home, sp->range, sp->scale)) continue;
        draw_perched(i, b, sp);
    }
    draw_hawks();
    if(skein.on) draw_skein();
    r_load_vp();
}
