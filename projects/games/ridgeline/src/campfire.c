/* Ridgeline: the campfires (campfire.h).

   A fire is all additive glow: a handful of tongues, each a tapered
   strip turned about the vertical to face the camera, growing up from
   the logs and fading as it tops out, over a hot core; sparks going up
   on the draught, and thin smoke above. From across the valley only a
   point of light is left, held a few pixels wide so it still shows.

   Its light on the ground, the scenery and the trucks is glow.c's,
   lamps.c's and truckdraw.c's, all from its glow_level (GLOW_FIRE and
   on), set here, so they flicker as one. An area has one by its tent
   and may have another in a fire pit; each flickers in its own time. */
#include "campfire.h"
#include "glow.h"
#include "world.h"
#include "terrain.h"

#define TONGUES 7
#define T_ROWS 5
#define SPARKS 14
#define SMOKE 6
#define FLAME_FAR 260.0f      /* nothing of it drawn past this */

typedef struct {
    V3 p, v;
    float age, life, phase, size;
    bool live;
} Bit;

typedef struct {
    V3 p;
    bool lit;             /* the area has it */
    float flick;
    Bit sparks[SPARKS], smoke[SMOKE];
    float spark_acc, smoke_acc;
} Fire;
static Fire fires[CAMPFIRES];
static float fire_t;
static const pvr_poly_hdr_t *h_add, *h_smoke;

static uint32_t seed = 0xc4f1e5u;
static inline float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8 & 0xffff) * (1.0f / 65535.0f);
}
static inline float rnd_s(void) { return rnd() * 2.0f - 1.0f; }

bool campfire_of(const lvl_prop_t *p, V3 *at) {
    if(p->kind != PROP_TENT && p->kind != PROP_FIREPIT) return false;
    /* A tent's burns four metres out from its open end. */
    float out = p->kind == PROP_TENT ? 4.0f : 0.0f;
    V3 f = v3(p->x + sinf(p->yaw) * out, 0, p->z + cosf(p->yaw) * out);
    f.y = terrain_height(f.x, f.z);
    *at = f;
    return true;
}

void campfire_place(void) {
    /* The first tent's and the first pit's. */
    for(int k = 0; k < CAMPFIRES; k++) fires[k].lit = false;
    for(int i = 0; i < LVL_PROPS; i++) {
        const lvl_prop_t *p = &lvl_props[i];
        Fire *f = &fires[p->kind == PROP_TENT ? 0 : 1];
        if(!f->lit) f->lit = campfire_of(p, &f->p);
    }
}

bool campfire_lit(int i, V3 *at) {
    if(fires[i].lit && at) *at = fires[i].p;
    return fires[i].lit;
}

void campfire_init(void) {
    h_add = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    h_smoke = r_header(TEX_PUFF, HF_TR);
    for(int k = 0; k < CAMPFIRES; k++) {
        fires[k].flick = 1.0f;
        for(int i = 0; i < SPARKS; i++) fires[k].sparks[i].live = false;
        for(int i = 0; i < SMOKE; i++) fires[k].smoke[i].live = false;
    }
}

static void fire_update(Fire *f, int k, float dt) {
    V3 fire = f->p;
    float t = fire_t + k * 7.3f;
    /* A fire's flicker: slow breathing with a fast shiver on top. */
    f->flick = clampf(0.88f + 0.10f * sinf(t * 3.7f) * sinf(t * 1.3f + 1.0f) + 0.06f * sinf(t * 11.3f) +
                          0.05f * sinf(t * 23.1f + 2.0f), 0.65f, 1.1f);
    glow_level[GLOW_FIRE + k] = clampf(pal->dark * 1.15f, 0, 1) * f->flick;
    /* Sparks and smoke only when someone is near enough to see them. */
    V3 dc = vsub(fire, cam.pos);
    bool near = vdot(dc, dc) < 90.0f * 90.0f;
    f->spark_acc += dt * 4.5f;
    f->smoke_acc += dt * 1.4f;
    if(!near) f->spark_acc = f->smoke_acc = 0;
    for(int i = 0; i < SPARKS; i++) {
        Bit *b = &f->sparks[i];
        if(!b->live) {
            if(f->spark_acc < 1.0f) continue;
            f->spark_acc -= 1.0f;
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
        Bit *b = &f->smoke[i];
        if(!b->live) {
            if(f->smoke_acc < 1.0f) continue;
            f->smoke_acc -= 1.0f;
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

void campfire_update(float dt) {
    bool frozen = game_frozen();
    if(!frozen) fire_t += dt;
    for(int k = 0; k < CAMPFIRES; k++) {
        if(!fires[k].lit) glow_level[GLOW_FIRE + k] = 0;
        else if(!frozen) fire_update(&fires[k], k, dt);
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

/* One tongue of flame: how far through its rise it is (0 just lit, 1
   topping out), from its foot, across the screen along side. */
static void tongue(V3 fire, float ft, float flick, int i, V3 side, float fade) {
    static const uint32_t ramp[T_ROWS] = {0xffffe6a0, 0xffffc24a, 0xffff8a22, 0xffe2480e, 0xff8c1c06};
    static const float ramp_a[T_ROWS] = {0.9f, 1.0f, 0.85f, 0.5f, 0.0f};
    float t = ft * (0.75f + 0.11f * i) + i * 0.37f;
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
        float sway = (0.09f * sinf(ft * 5.3f + i * 1.7f + s * 3.0f) + 0.05f * sinf(ft * 9.1f + i * 2.9f + s * 5.0f)) * s;
        float curl = 0.07f * sinf(ft * 4.1f + i * 0.9f + s * 4.0f) * s;
        V3 c = v3(foot.x + side.x * sway + side.z * curl + WIND_X * 0.12f * s * s, foot.y + h * s,
                  foot.z + side.z * sway - side.x * curl + WIND_Z * 0.12f * s * s);
        float ww = w * (1.0f - s * s) + 0.015f;
        uint32_t col = color_alpha(ramp[k], ramp_a[k] * a);
        v[2 * k] = (RV){c.x - side.x * ww, c.y, c.z - side.z * ww, 0, 0.5f, col};
        v[2 * k + 1] = (RV){c.x + side.x * ww, c.y, c.z + side.z * ww, 1, 0.5f, col};
    }
    r_strip(v, 2 * T_ROWS);
}

static void fire_draw(const Fire *f, int k) {
    V3 fire = f->p;
    float flick = f->flick;
    V3 dc = vsub(fire, r_cam.pos);
    float dist = vlen(dc);
    if(dist > FLAME_FAR || !r_sphere_visible(fire, 6.0f)) return;
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
            const Bit *b = &f->smoke[i];
            if(!b->live) continue;
            float t = b->age / b->life;
            float a = 0.2f * fminf(t * 5.0f, 1.0f) * (1.0f - t) * fog;
            if(a < 0.01f) continue;
            float s = 0.22f + 0.9f * t;
            r_billboard(b->p, s, s, b->phase + b->age * 0.4f, 0, 0, 1, 1, color_alpha(grey, a));
        }
    }
    r_hdr(h_add);
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
    for(int i = 0; i < TONGUES; i++) tongue(fire, fire_t + k * 7.3f, flick, i, side, fog);
    if(dist > 70.0f) return;
    for(int i = 0; i < SPARKS; i++) {
        const Bit *b = &f->sparks[i];
        if(!b->live) continue;
        float t = b->age / b->life;
        /* Yellow off the fire, red as it cools, winking as it tumbles. */
        float a = (1.0f - t * t) * (0.7f + 0.3f * sinf(b->phase * 3.0f)) * fog;
        sprite(b->p, b->size, color_alpha(color_lerp(0xffffd060, 0xffff4a10, t), a));
    }
}

void campfire_draw_tr(void) {
    for(int k = 0; k < CAMPFIRES; k++)
        if(fires[k].lit) fire_draw(&fires[k], k);
}
