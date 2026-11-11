/* Ridgeline deer (deer.h), built each frame from a few square tubes in
   world space: ~70 vertices a deer. */
#include "deer.h"
#include "truck.h"
#include "terrain.h"
#include "world.h"

static const pvr_poly_hdr_t *h;
static uint32_t seed = 0x5eed1e5u;

static float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8) * (1.0f / 16777216.0f);
}

static float rnd_s(void) {
    return rnd() * 2.0f - 1.0f;
}

/* How far the nearest truck is, and whether the player's horn is going. */
static float truck_near(V3 p, V3 *from) {
    float best = 1e9f;
    for(int i = 0; i < game.ntrucks; i++) {
        V3 d = vsub(p, trucks[i].pos);
        d.y = 0;
        float l = vlen(d);
        if(trucks[i].horn && i == 0) l *= 0.35f;    /* a horn carries */
        if(l < best) {
            best = l;
            if(from) *from = trucks[i].pos;
        }
    }
    return best;
}

/* ---- Drawing ---- */

/* A square tube from a to b, hw wide and hh tall, lit round its sides. */
static void prism(V3 a, V3 b, float hw, float hh, uint32_t col) {
    V3 d = vsub(b, a);
    float len = vlen(d);
    if(len < 1e-4f) return;
    d = vscale(d, 1.0f / len);
    V3 ref = fabsf(d.y) > 0.9f ? v3(1, 0, 0) : v3(0, 1, 0);
    V3 s = vnorm(vcross(d, ref)), u = vcross(s, d);
    static const float cs[4] = {1, -1, -1, 1}, cu[4] = {1, 1, -1, -1};
    RV v[10];
    for(int k = 0; k <= 4; k++) {
        V3 o = vadd(vscale(s, cs[k & 3] * hw), vscale(u, cu[k & 3] * hh));
        uint32_t c = world_lit(col, vnorm(o));
        V3 p = vadd(a, o), q = vadd(b, o);
        v[k * 2] = (RV){p.x, p.y, p.z, 0, 0, c};
        v[k * 2 + 1] = (RV){q.x, q.y, q.z, 0, 0, c};
    }
    r_strip(v, 10);
}

/* ---- Deer ---- */

enum { GRAZE, ALERT, FLEE };
#define NDEER 6

typedef struct {
    V3 p, home;
    float yaw, speed, want_speed, phase, t, head;
    int state;
} Deer;

static Deer deer[NDEER];
static int ndeer;     /* none where the area has nowhere for them */

static void deer_update(Deer *d, float dt) {
    V3 tp = d->p;
    float dist = truck_near(d->p, &tp);
    d->t += dt;
    if(d->state == GRAZE) {
        if(dist < 45.0f) {
            d->state = ALERT;
            d->t = 0;
        }
        else if(d->t > 3.0f + 4.0f * rnd()) {
            /* Stand and graze, or wander a few steps. */
            d->t = 0;
            d->want_speed = rnd() < 0.4f ? 0.6f : 0.0f;
            d->yaw += rnd_s() * 1.2f;
        }
    }
    else if(d->state == ALERT) {
        d->want_speed = 0;
        if(dist < 26.0f) {
            d->state = FLEE;
            d->t = 0;
        }
        else if(dist > 55.0f) d->state = GRAZE;
    }
    else {
        /* Away from the truck, bounding. */
        float away = atan2f(d->p.x - tp.x, d->p.z - tp.z);
        d->yaw += wrap_angle(away - d->yaw) * smooth_k(3.0f, dt);
        d->want_speed = 9.0f;
        if(dist > 70.0f && d->t > 4.0f) {
            d->state = GRAZE;
            d->t = 0;
        }
    }
    /* Stay near home and out of the water. */
    V3 hd = vsub(d->home, d->p);
    if(vlen(v3(hd.x, 0, hd.z)) > 90.0f) d->yaw += wrap_angle(atan2f(hd.x, hd.z) - d->yaw) * smooth_k(2.0f, dt);
    V3 f = v3(sinf(d->yaw), 0, cosf(d->yaw));
    if(water_at(d->p.x + f.x * 4, d->p.z + f.z * 4) > -999.0f) d->yaw += 1.6f * dt * 3.0f;
    d->speed += (d->want_speed - d->speed) * smooth_k(d->want_speed > d->speed ? 3.0f : 2.0f, dt);
    d->p = vmad(d->p, f, d->speed * dt);
    d->p.y = terrain_height(d->p.x, d->p.z);
    d->phase += dt * (d->speed > 3.0f ? 9.0f : 4.0f * d->speed / 0.6f);
    float head_up = d->state == GRAZE && d->speed < 0.1f ? 0.0f : 1.0f;
    d->head += (head_up - d->head) * smooth_k(3.0f, dt);
}

static void deer_draw(const Deer *d) {
    float sy = sinf(d->yaw), cy = cosf(d->yaw);
    V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
    float bound = d->speed > 3.0f ? fabsf(sinf(d->phase)) * 0.28f : 0.0f;
    V3 base = vadd(d->p, v3(0, bound, 0));
#define L(x, y, z) vadd(vadd(vadd(base, vscale(r, (x))), v3(0, (y), 0)), vscale(f, (z)))
    const uint32_t tan = 0xff9a6a42, dark = 0xff5a3e28, pale = 0xffe8dcc8;
    prism(L(0, 0.95f, -0.45f), L(0, 1.0f, 0.45f), 0.19f, 0.22f, tan);
    V3 nb = L(0, 1.1f, 0.4f);
    V3 up_end = L(0, 1.55f, 0.62f), down_end = L(0, 0.72f, 0.82f);
    V3 ne = vadd(vscale(down_end, 1.0f - d->head), vscale(up_end, d->head));
    prism(nb, ne, 0.07f, 0.09f, tan);
    prism(ne, vadd(ne, vadd(vscale(f, 0.3f), v3(0, -0.06f, 0))), 0.07f, 0.08f, dark);
    prism(L(0, 1.05f, -0.46f), L(0, 0.98f, -0.6f), 0.05f, 0.04f, pale);
    float stride = clampf(d->speed / 3.0f, 0, 1) * 0.32f;
    for(int k = 0; k < 4; k++) {
        float sx = (k & 1) ? 0.12f : -0.12f, sz = (k & 2) ? 0.36f : -0.36f;
        float sw = sinf(d->phase + (k == 0 || k == 3 ? 0.0f : 3.14f)) * stride;
        V3 hip = L(sx, 0.82f, sz), foot = L(sx, 0.02f - bound, sz + sw);
        prism(hip, foot, 0.035f, 0.035f, dark);
    }
#undef L
}

/* ---- ---- */

void deer_reset(void) {
    h = r_header(TEX_NONE, HF_OP);
    ndeer = lvl->nherds ? NDEER : 0;
    for(int i = 0; i < ndeer; i++) {
        /* Four on the first home, two on the second. */
        const float *hm = lvl->herds[i < 4 || lvl->nherds < 2 ? 0 : 1];
        Deer *d = &deer[i];
        d->home = v3(hm[0], 0, hm[1]);
        d->p = v3(hm[0] + rnd_s() * 8.0f, 0, hm[1] + rnd_s() * 8.0f);
        d->p.y = terrain_height(d->p.x, d->p.z);
        d->yaw = rnd() * ENG_TAU;
        d->speed = d->want_speed = 0;
        d->state = GRAZE;
        d->t = rnd() * 3.0f;
        d->head = 0;
        d->phase = rnd() * 6.0f;
    }
}

void deer_update_all(float dt) {
    if(!h) deer_reset();
    for(int i = 0; i < ndeer; i++) deer_update(&deer[i], dt);
}

void deer_draw_all(void) {
    if(!h) return;
    r_load_vp();
    r_hdr(h);
    for(int i = 0; i < ndeer; i++) {
        V3 d = vsub(deer[i].p, r_cam.pos);
        if(vdot(d, d) < 130.0f * 130.0f && r_sphere_visible(vadd(deer[i].p, v3(0, 1, 0)), 1.6f)) deer_draw(&deer[i]);
    }
}
