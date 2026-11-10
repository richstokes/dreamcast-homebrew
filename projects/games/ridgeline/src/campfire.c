/* Ridgeline: the campfire (campfire.h).

   The fire is all additive glow: a handful of tongues, each a tapered
   strip turned about the vertical to face the camera, growing up from
   the logs and fading as it tops out, over a hot core; sparks going up
   on the draught, and thin smoke above. From across the valley only a
   point of light is left, held a few pixels wide so it still shows.

   Its light on the ground is a fan of rings laid on the terrain round
   the ring of stones, brightest by the fire and gone by CAMPFIRE_REACH,
   every vertex weighted once (the ground doesn't move) and only scaled
   by the flicker each frame. The scenery's share is in lamps.c and the
   trucks' in truckdraw.c, all from campfire_level() so they flicker as
   one. Nothing but the flames and smoke is drawn by day. */
#include "campfire.h"
#include "world.h"
#include "terrain.h"

#define TONGUES 7
#define T_ROWS 5
#define SPARKS 14
#define SMOKE 6
#define RINGS 10
#define SPOKES 16
#define FLAME_FAR 260.0f      /* nothing of it drawn past this */

static V3 fire;
static bool placed;
static float fire_t, level, flick = 1.0f;
static const pvr_poly_hdr_t *h_add, *h_smoke;

typedef struct {
    V3 p, v;
    float age, life, phase, size;
    bool live;
} Bit;
static Bit sparks[SPARKS], smoke[SMOKE];
static float spark_acc, smoke_acc;

/* The ground's fan: where each vertex is and how much light it gets. */
static const float ring_r[RINGS] = {0, 0.7f, 1.5f, 2.6f, 4.0f, 6.0f, 8.5f, 11.5f, 15.0f, CAMPFIRE_REACH};
static V3 gnd_p[RINGS][SPOKES];
static float gnd_k[RINGS][SPOKES];

static uint32_t seed = 0xc4f1e5u;
static inline float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8 & 0xffff) * (1.0f / 65535.0f);
}
static inline float rnd_s(void) { return rnd() * 2.0f - 1.0f; }

V3 campfire_pos(void) {
    if(!placed) {
        for(int i = 0; i < LVL_PROPS; i++) {
            const lvl_prop_t *p = &lvl_props[i];
            if(p->kind != PROP_TENT) continue;
            fire = v3(p->x + sinf(p->yaw) * 4.0f, 0, p->z + cosf(p->yaw) * 4.0f);
            fire.y = terrain_height(fire.x, fire.z);
            break;
        }
        placed = true;
    }
    return fire;
}

void campfire_init(void) {
    h_add = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    h_smoke = r_header(TEX_PUFF, HF_TR);
    V3 f = campfire_pos();
    /* The light comes from a little above the flames, so the flat ground
       further out still catches some of it. */
    V3 lamp = v3(f.x, f.y + 2.2f, f.z);
    for(int r = 0; r < RINGS; r++)
        for(int s = 0; s < SPOKES; s++) {
            float a = ENG_TAU * (s + 0.5f * (r & 1)) / SPOKES;
            V3 n, p = v3(f.x + sinf(a) * ring_r[r], 0, f.z + cosf(a) * ring_r[r]);
            p.y = terrain_height_n(p.x, p.z, &n);
            V3 to = vsub(lamp, p);
            float d2 = ring_r[r] * ring_r[r] + (p.y - f.y) * (p.y - f.y);
            float lam = clampf(0.35f + 0.65f * vdot(n, vnorm(to)), 0, 1);
            gnd_k[r][s] = r == RINGS - 1 ? 0 : campfire_falloff(d2) * lam;
            /* Lifted clear of the ground, more where the fan is coarse
               (and by then nearly nothing). */
            p.y += 0.06f + 0.03f * ring_r[r];
            gnd_p[r][s] = p;
        }
    for(int i = 0; i < SPARKS; i++) sparks[i].live = false;
    for(int i = 0; i < SMOKE; i++) smoke[i].live = false;
}

float campfire_level(void) {
    return level;
}

float campfire_light(V3 p, V3 *from) {
    if(level <= 0) return 0;
    V3 d = vsub(v3(fire.x, fire.y + 0.8f, fire.z), p);
    float d2 = vdot(d, d);
    if(d2 >= CAMPFIRE_REACH * CAMPFIRE_REACH) return 0;
    if(from) *from = d2 > 1e-4f ? vscale(d, 1.0f / sqrtf(d2)) : v3(0, 1, 0);
    return level * campfire_falloff(d2);
}

void campfire_update(float dt) {
    if(game_frozen()) return;
    fire_t += dt;
    float t = fire_t;
    /* A fire's flicker: slow breathing with a fast shiver on top. */
    flick = clampf(0.88f + 0.10f * sinf(t * 3.7f) * sinf(t * 1.3f + 1.0f) + 0.06f * sinf(t * 11.3f) +
                       0.05f * sinf(t * 23.1f + 2.0f), 0.65f, 1.1f);
    level = clampf(pal->dark * 1.15f, 0, 1) * flick;
    /* Sparks and smoke only when someone is near enough to see them. */
    V3 dc = vsub(fire, cam.pos);
    bool near = vdot(dc, dc) < 90.0f * 90.0f;
    spark_acc += dt * 4.5f;
    smoke_acc += dt * 1.4f;
    if(!near) spark_acc = smoke_acc = 0;
    for(int i = 0; i < SPARKS; i++) {
        Bit *b = &sparks[i];
        if(!b->live) {
            if(spark_acc < 1.0f) continue;
            spark_acc -= 1.0f;
            b->p = v3(fire.x + rnd_s() * 0.15f, fire.y + 0.35f + 0.3f * rnd(), fire.z + rnd_s() * 0.15f);
            b->v = v3(rnd_s() * 0.35f, 1.3f + 1.3f * rnd(), rnd_s() * 0.35f);
            b->age = 0;
            b->life = 0.9f + 1.4f * rnd();
            b->phase = rnd() * ENG_TAU;
            b->size = 0.022f + 0.02f * rnd();
            b->live = true;
            continue;
        }
        b->age += dt;
        if(b->age > b->life) {
            b->live = false;
            continue;
        }
        /* Up on the draught, slowing, wandering as it goes. */
        b->phase += dt * 6.0f;
        b->v.x += (0.5f * sinf(b->phase) + WIND_X * 0.25f - b->v.x) * dt * 2.0f;
        b->v.z += (0.5f * cosf(b->phase * 0.8f) + WIND_Z * 0.25f - b->v.z) * dt * 2.0f;
        b->v.y -= b->v.y * 0.5f * dt;
        b->p = vmad(b->p, b->v, dt);
    }
    for(int i = 0; i < SMOKE; i++) {
        Bit *b = &smoke[i];
        if(!b->live) {
            if(smoke_acc < 1.0f) continue;
            smoke_acc -= 1.0f;
            b->p = v3(fire.x + rnd_s() * 0.1f, fire.y + 0.9f, fire.z + rnd_s() * 0.1f);
            b->v = v3(rnd_s() * 0.1f, 0.9f + 0.3f * rnd(), rnd_s() * 0.1f);
            b->age = 0;
            b->life = 3.6f + 0.8f * rnd();
            b->phase = rnd() * ENG_TAU;
            b->live = true;
            continue;
        }
        b->age += dt;
        if(b->age > b->life) {
            b->live = false;
            continue;
        }
        /* Rising straight at first, then leaning off downwind. */
        float lean = clampf(b->age * 0.4f, 0, 1);
        b->v.x += (WIND_X * 0.9f * lean - b->v.x) * dt;
        b->v.z += (WIND_Z * 0.9f * lean - b->v.z) * dt;
        b->p = vmad(b->p, b->v, dt);
    }
}

/* ---- Drawing ---- */

static void sprite(V3 p, float s, uint32_t col) {
    V3 rx = vscale(r_cam.right, s), ry = vscale(r_cam.up, s);
    RV q[4] = {{p.x - rx.x + ry.x, p.y - rx.y + ry.y, p.z - rx.z + ry.z, 0, 0, col},
               {p.x + rx.x + ry.x, p.y + rx.y + ry.y, p.z + rx.z + ry.z, 1, 0, col},
               {p.x - rx.x - ry.x, p.y - rx.y - ry.y, p.z - rx.z - ry.z, 0, 1, col},
               {p.x + rx.x - ry.x, p.y + rx.y - ry.y, p.z + rx.z - ry.z, 1, 1, col}};
    r_strip(q, 4);
}

/* The firelight on the ground: a strip between each pair of rings. */
static void ground_draw(float k) {
    static const uint32_t warm = 0xffff8c34;
    uint32_t col[RINGS][SPOKES];
    for(int r = 0; r < RINGS; r++)
        for(int s = 0; s < SPOKES; s++) col[r][s] = color_alpha(warm, clampf(gnd_k[r][s] * k, 0, 1));
    for(int r = 0; r + 1 < RINGS; r++) {
        RV st[2 * (SPOKES + 1)];
        for(int s = 0; s <= SPOKES; s++) {
            int m = s % SPOKES;
            V3 a = gnd_p[r][m], b = gnd_p[r + 1][m];
            st[2 * s] = (RV){a.x, a.y, a.z, 0.5f, 0.5f, col[r][m]};
            st[2 * s + 1] = (RV){b.x, b.y, b.z, 0.5f, 0.5f, col[r + 1][m]};
        }
        r_strip_bounded(st, 2 * (SPOKES + 1), fire, ring_r[r + 1] + 3.0f);
    }
}

/* One tongue of flame: how far through its rise it is (0 just lit, 1
   topping out), from its foot, across the screen along side. */
static void tongue(int i, V3 side, float fade) {
    static const uint32_t ramp[T_ROWS] = {0xffffe6a0, 0xffffc24a, 0xffff8a22, 0xffe2480e, 0xff8c1c06};
    static const float ramp_a[T_ROWS] = {0.9f, 1.0f, 0.85f, 0.5f, 0.0f};
    float t = fire_t * (0.75f + 0.11f * i) + i * 0.37f;
    float p = t - floorf(t);
    float ang = i * 2.4f;
    float rad = i ? 0.2f : 0;                      /* one in the middle, the rest round it */
    float h = (0.8f + 0.65f * (float)((i * 5) % TONGUES) / TONGUES) * (0.3f + 0.7f * p) * (0.85f + 0.15f * flick);
    float w = (0.26f + 0.04f * (i % 3)) * (1.0f - 0.35f * p);
    float a = fade * (1.0f - p * p * p) * fminf(p * 8.0f, 1.0f);
    V3 foot = v3(fire.x + sinf(ang) * rad, fire.y + 0.1f, fire.z + cosf(ang) * rad);
    RV v[2 * T_ROWS];
    for(int k = 0; k < T_ROWS; k++) {
        float s = (float)k / (T_ROWS - 1);
        /* It leans and curls more the higher it gets. */
        float sway = (0.09f * sinf(fire_t * 5.3f + i * 1.7f + s * 3.0f) + 0.05f * sinf(fire_t * 9.1f + i * 2.9f + s * 5.0f)) * s;
        float curl = 0.07f * sinf(fire_t * 4.1f + i * 0.9f + s * 4.0f) * s;
        V3 c = v3(foot.x + side.x * sway + side.z * curl + WIND_X * 0.12f * s * s, foot.y + h * s,
                  foot.z + side.z * sway - side.x * curl + WIND_Z * 0.12f * s * s);
        float ww = w * (1.0f - s * s) + 0.015f;
        uint32_t col = color_alpha(ramp[k], ramp_a[k] * a);
        v[2 * k] = (RV){c.x - side.x * ww, c.y, c.z - side.z * ww, 0, 0.5f, col};
        v[2 * k + 1] = (RV){c.x + side.x * ww, c.y, c.z + side.z * ww, 1, 0.5f, col};
    }
    r_strip(v, 2 * T_ROWS);
}

void campfire_draw_tr(void) {
    V3 dc = vsub(fire, r_cam.pos);
    float dist = vlen(dc);
    if(dist > FLAME_FAR || !r_sphere_visible(fire, CAMPFIRE_REACH)) return;
    /* The fog takes it with everything else (these lists aren't fogged). */
    float fog = clampf(1.0f - dist / world_fog_far(), 0, 1);
    if(fog <= 0) return;
    float dark = clampf(pal->dark * 1.15f, 0, 1);
    r_load_vp();
    /* Smoke first, under the glow: grey by day, lit from below at night. */
    if(dist < 90.0f) {
        uint32_t grey = color_lerp(world_lit(0xff8c8884, v3(0, 1, 0)), 0xff70402a, 0.5f * dark);
        r_hdr(h_smoke);
        for(int i = 0; i < SMOKE; i++) {
            const Bit *b = &smoke[i];
            if(!b->live) continue;
            float k = b->age / b->life;
            float a = 0.2f * fminf(k * 5.0f, 1.0f) * (1.0f - k) * fog;
            if(a < 0.01f) continue;
            float s = 0.22f + 0.9f * k;
            r_billboard(b->p, s, s, b->phase + b->age * 0.4f, 0, 0, 1, 1, color_alpha(grey, a));
        }
    }
    r_hdr(h_add);
    if(level > 0.02f && dist < 140.0f) ground_draw(level * 0.55f * fog);
    /* The glow round the fire, wider in the dark; from far off a point
       of light that never gets smaller than a few pixels. */
    V3 mid = v3(fire.x, fire.y + 0.45f, fire.z);
    float halo = fmaxf(1.5f + 1.3f * dark, dist * 0.014f) * (0.9f + 0.1f * flick);
    sprite(mid, halo, color_alpha(0xffff7a28, (0.10f + 0.24f * dark) * flick * fog));
    sprite(v3(fire.x, fire.y + 0.3f, fire.z), fmaxf(0.55f, dist * 0.006f), color_alpha(0xffffd070, 0.8f * flick * fog));
    if(dist > 120.0f) return;
    /* The tongues, turned about the vertical to face the camera. */
    V3 side = v3(r_cam.right.x, 0, r_cam.right.z);
    float sl = vlen(side);
    side = sl > 0.01f ? vscale(side, 1.0f / sl) : v3(1, 0, 0);
    for(int i = 0; i < TONGUES; i++) tongue(i, side, fog);
    if(dist > 70.0f) return;
    for(int i = 0; i < SPARKS; i++) {
        const Bit *b = &sparks[i];
        if(!b->live) continue;
        float k = b->age / b->life;
        /* Yellow off the fire, red as it cools, winking as it tumbles. */
        float a = (1.0f - k * k) * (0.7f + 0.3f * sinf(b->phase * 3.0f)) * fog;
        sprite(b->p, b->size, color_alpha(color_lerp(0xffffd060, 0xffff4a10, k), a));
    }
}
