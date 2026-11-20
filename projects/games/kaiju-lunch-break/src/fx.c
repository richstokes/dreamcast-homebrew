/* Effects (fx.h). Smoke and stars are punch-through sprites, so they keep
   an inked edge and cost the PVR no blending; they leave by shrinking
   rather than fading. */
#include "fx.h"
#include "cel.h"
#include "hud.h"
#include "engine/text.h"

#define PUFFS 200
#define BOOMS 24
#define DEBRIS 72
#define RINGS 6
#define CALLOUTS 6

typedef struct { V3 p, v; float size, age, life, rot; uint32_t col; } Puff;
typedef struct { V3 p; float size, age, rot; } Boom;
typedef struct { V3 p, v; float ax, ay, wx, wy, size, age; uint32_t col; } Debris;
typedef struct { V3 p; float r, age; } Ring;
typedef struct { V3 p; float age, rot; int level, word; } Callout;

static Puff puffs[PUFFS];
static Boom booms[BOOMS];
static Debris debris[DEBRIS];
static Ring rings[RINGS];
static Callout callouts[CALLOUTS];
static int puff_next, boom_next, debris_next, ring_next, callout_next;
static float shake, callout_gap, hitstop;
static bool beam_on;
static V3 beam_a, beam_b;
static float fx_time;
static const pvr_poly_hdr_t *h_puff, *h_boom, *h_ring, *h_fire, *h_beam, *h_glow, *h_pow;

void fx_init(void) {
    h_puff = r_header(TEX_PUFF, HF_PT | HF_CLAMP);
    h_boom = r_header(TEX_BOOM, HF_PT | HF_CLAMP | HF_NOFOG);
    h_ring = r_header(TEX_RING, HF_PT | HF_CLAMP);
    h_fire = r_header(TEX_FIRE, HF_PT | HF_CLAMP);
    h_beam = r_header(TEX_BEAM, HF_TR | HF_NOFOG);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG | HF_CLAMP);
    h_pow = r_header(TEX_POW, HF_TR | HF_NOFOG | HF_CLAMP);
}

void fx_clear(void) {
    memset(puffs, 0, sizeof(puffs));
    memset(booms, 0, sizeof(booms));
    memset(debris, 0, sizeof(debris));
    memset(rings, 0, sizeof(rings));
    memset(callouts, 0, sizeof(callouts));
    shake = hitstop = 0;
    beam_on = false;
}

void fx_puff(V3 p, V3 vel, float size, float life, uint32_t col) {
    Puff *q = &puffs[puff_next];
    puff_next = (puff_next + 1) % PUFFS;
    *q = (Puff){p, vel, size, 0, life, frand_s() * 0.6f, col};
}

void fx_puffs(V3 c, float radius, int n, float size) {
    for(int i = 0; i < n; i++) {
        float a = frand() * ENG_TAU, r = radius * (0.4f + 0.6f * frand());
        V3 d = v3(sinf(a), 0, cosf(a));
        fx_puff(vadd(c, v3(d.x * r, size * 0.4f, d.z * r)), v3(d.x * (8 + frand() * 10), 4 + frand() * 7, d.z * (8 + frand() * 10)),
                size * (0.7f + 0.6f * frand()), 0.8f + frand() * 0.7f, 0xfffaf6ff);
    }
}

void fx_debris(V3 c, int n, float speed, uint32_t tint, float size) {
    for(int i = 0; i < n; i++) {
        Debris *d = &debris[debris_next];
        debris_next = (debris_next + 1) % DEBRIS;
        float a = frand() * ENG_TAU, s = speed * (0.4f + 0.6f * frand());
        *d = (Debris){c, v3(sinf(a) * s, speed * (0.5f + 0.8f * frand()), cosf(a) * s), frand() * 6, frand() * 6,
                      frand_s() * 7, frand_s() * 7, size * (0.6f + 0.8f * frand()), 0,
                      (i & 1) ? tint : color_lerp(tint, 0xff8c8696, 0.6f)};
    }
}

void fx_boom(V3 c, float size) {
    booms[boom_next] = (Boom){c, size, 0, frand() * ENG_TAU};
    boom_next = (boom_next + 1) % BOOMS;
}

void fx_ring(V3 c, float radius) {
    rings[ring_next] = (Ring){c, radius, 0.001f};
    ring_next = (ring_next + 1) % RINGS;
}

void fx_callout(V3 c, int level) {
    /* One at a time unless something bigger comes along. */
    if(game.state != ST_PLAY || (callout_gap > 0 && level < 3) || (level == 0 && (eng_rand() & 1))) return;
    callout_gap = 0.4f;
    /* One word on screen at a time: the new one takes over. */
    for(int i = 0; i < CALLOUTS; i++) callouts[i].age = 0;
    callouts[callout_next] = (Callout){c, 0.001f, frand_s() * 0.22f, level, (int)(eng_rand() % 3)};
    callout_next = (callout_next + 1) % CALLOUTS;
}

void fx_shake(float amount) {
    if(amount > shake) shake = amount;
}

float fx_shake_now(void) { return shake; }

void fx_hitstop(float seconds) {
    if(seconds > hitstop) hitstop = seconds;
}

bool fx_frozen(float dt) {
    if(hitstop <= 0) return false;
    hitstop -= dt;
    return true;
}

void fx_beam(V3 a, V3 b) {
    beam_on = true;
    beam_a = a;
    beam_b = b;
}

void fx_update(float dt) {
    fx_time += dt;
    shake = approach(shake, 0, dt * 2.2f);
    if(callout_gap > 0) callout_gap -= dt;
    for(int i = 0; i < PUFFS; i++) {
        Puff *p = &puffs[i];
        if(p->life <= 0) continue;
        if((p->age += dt) >= p->life) {
            p->life = 0;
            continue;
        }
        p->p = vmad(p->p, p->v, dt);
        float k = 1.0f - 1.8f * dt;
        p->v.x *= k;
        p->v.z *= k;
        p->v.y = p->v.y * k + 1.5f * dt;
    }
    for(int i = 0; i < BOOMS; i++)
        if(booms[i].size > 0 && (booms[i].age += dt) > 0.3f) booms[i].size = 0;
    for(int i = 0; i < RINGS; i++)
        if(rings[i].age > 0 && (rings[i].age += dt) > 0.5f) rings[i].age = 0;
    for(int i = 0; i < CALLOUTS; i++)
        if(callouts[i].age > 0 && (callouts[i].age += dt) > 1.0f) callouts[i].age = 0;
    for(int i = 0; i < DEBRIS; i++) {
        Debris *d = &debris[i];
        if(d->size <= 0) continue;
        if((d->age += dt) > 2.6f) {
            d->size = 0;
            continue;
        }
        d->v.y -= 70.0f * dt;
        d->p = vmad(d->p, d->v, dt);
        d->ax += d->wx * dt;
        d->ay += d->wy * dt;
        if(d->p.y < d->size * 0.5f) {
            d->p.y = d->size * 0.5f;
            d->v.y = -d->v.y * 0.35f;
            d->v.x *= 0.6f;
            d->v.z *= 0.6f;
            d->wx *= 0.5f;
            d->wy *= 0.5f;
        }
    }
}

void fx_draw_op(void) {
    for(int i = 0; i < DEBRIS; i++) {
        const Debris *d = &debris[i];
        if(d->size <= 0 || !r_sphere_visible(d->p, d->size)) continue;
        Xf rx = xf_rot_x(d->ax), ry = xf_rot_y(d->ay);
        Xf x = xf_mul(&ry, &rx);
        x.p = d->p;
        float m[16] __attribute__((aligned(32)));
        float s = d->size * (d->age > 2.1f ? (2.6f - d->age) * 2.0f : 1.0f);
        xf_m4(&x, m);
        V3 l = xf_local(&x, cel_sun);
        m4_scale_apply(m, s, s, s);
        flat_mesh_draw_m(&cel_flat[FM_CUBE], m, l, d->col, false);
    }
    r_load_vp();
}

void fx_flame(V3 p, float size, float phase) {
    float k = 0.85f + 0.2f * sinf(fx_time * 13.0f + phase), w = 0.9f + 0.15f * sinf(fx_time * 17.0f + phase * 2.0f);
    r_hdr(h_fire);
    /* Two frames of flicker: the sprite and its mirror. */
    bool flip = ((int)(fx_time * 9.0f + phase) & 1) != 0;
    r_billboard(vadd(p, v3(0, size * k, 0)), size * 0.8f * w, size * k, 0, flip ? 1 : 0, 0, flip ? 0 : 1, 1, 0xffffffff);
}

void fx_draw_pt(void) {
    r_hdr(h_ring);
    for(int i = 0; i < RINGS; i++) {
        const Ring *g = &rings[i];
        if(g->age <= 0) continue;
        float r = g->r * ease_out(g->age / 0.5f);
        RV v[4] = {{g->p.x - r, 0.4f, g->p.z - r, 0, 0, 0xffffffff}, {g->p.x + r, 0.4f, g->p.z - r, 1, 0, 0xffffffff},
                   {g->p.x - r, 0.4f, g->p.z + r, 0, 1, 0xffffffff}, {g->p.x + r, 0.4f, g->p.z + r, 1, 1, 0xffffffff}};
        r_strip(v, 4);
    }
    r_hdr(h_puff);
    for(int i = 0; i < PUFFS; i++) {
        const Puff *p = &puffs[i];
        if(p->life <= 0) continue;
        float u = p->age / p->life;
        float s = p->size * (u < 0.2f ? 0.4f + 3.0f * u : u > 0.6f ? (1.0f - u) * 2.5f : 1.0f);
        r_billboard(p->p, s, s, p->rot + p->age * 0.6f, 0, 0, 1, 1, p->col);
    }
    r_hdr(h_boom);
    for(int i = 0; i < BOOMS; i++) {
        const Boom *b = &booms[i];
        if(b->size <= 0) continue;
        float u = b->age / 0.3f, s = b->size * (u < 0.35f ? 0.5f + 1.6f * u : 1.06f - (u - 0.35f) * 1.2f);
        r_billboard(b->p, s, s, b->rot, 0, 0, 1, 1, 0xffffffff);
    }
}

static const char *const words[4][3] = {
    {"POW!", "BONK!", "BAM!"},
    {"CRUNCH!", "SMASH!", "WHAM!"},
    {"KRAKOOM!", "CRASH!!", "KABLAM!"},
    {"KA-BOOM!!", "DOOOM!!", "KRA-KOOOM!!"},
};

void fx_draw_tr(void) {
    if(beam_on) {
        /* A ribbon turned to face the camera, in hard bands. */
        V3 d = vsub(beam_b, beam_a), mid = vlerp(beam_a, beam_b, 0.5f);
        V3 side = vnorm(vcross(d, vsub(mid, r_cam.pos)));
        float w = 3.4f + 0.7f * sinf(fx_time * 47.0f);
        float len = vlen(d) / 24.0f, sc = fx_time * 9.0f;
        RV v[4] = {{0, 0, 0, 0, -sc, 0xffffffff}, {0, 0, 0, 1, -sc, 0xffffffff},
                   {0, 0, 0, 0, len - sc, 0xffffffff}, {0, 0, 0, 1, len - sc, 0xffffffff}};
        V3 p[4] = {vmad(beam_a, side, -w * 0.6f), vmad(beam_a, side, w * 0.6f), vmad(beam_b, side, -w), vmad(beam_b, side, w)};
        for(int k = 0; k < 4; k++) {
            v[k].x = p[k].x;
            v[k].y = p[k].y;
            v[k].z = p[k].z;
        }
        r_hdr(h_beam);
        r_strip(v, 4);
        r_hdr(h_glow);
        float g = 9.0f + 3.0f * sinf(fx_time * 31.0f);
        r_billboard(beam_a, g, g, 0, 0, 0, 1, 1, 0xffa0f0ff);
        r_billboard(beam_b, g * 1.8f, g * 1.8f, 0, 0, 0, 1, 1, 0xffc0f8ff);
        beam_on = false;
    }
    const EngFont *f = hud_font();
    for(int i = 0; i < CALLOUTS; i++) {
        const Callout *c = &callouts[i];
        float sx, sy, iw;
        if(c->age <= 0 || !r_project(c->p, &sx, &sy, &iw)) continue;
        /* Pops in past its size, settles, then drops away. */
        float u = c->age, k = u < 0.12f ? u / 0.12f * 1.25f : u < 0.2f ? 1.25f - (u - 0.12f) * 3.0f : u > 0.8f ? (1.0f - u) * 5.0f : 1.0f;
        float size = (0.62f + 0.2f * (float)c->level) * k;
        sx = clampf(sx, 90, 550);
        sy = clampf(sy - u * 30.0f, 170, 390);
        const char *w = words[c->level][c->word];
        float half = text_width(f, w, size) * 0.5f + 26.0f * size;
        float cs = cosf(c->rot), sn = sinf(c->rot), hh = 46.0f * size;
        const float xy[8] = {sx - half * cs + hh * sn, sy - half * sn - hh * cs, sx + half * cs + hh * sn, sy + half * sn - hh * cs,
                             sx - half * cs - hh * sn, sy - half * sn + hh * cs, sx + half * cs - hh * sn, sy + half * sn + hh * cs};
        static const float uv[8] = {0, 0, 1, 0, 0, 1, 1, 1};
        static const uint32_t burst[4] = {0xffffffff, 0xfffff0a0, 0xffffd060, 0xffff9060};
        uint32_t col[4] = {burst[c->level], burst[c->level], burst[c->level], burst[c->level]};
        r_hdr(h_pow);
        r_quad2d_pts(xy, uv, col, 18.0f);
        static const uint32_t ink_of[4] = {0xffe84040, 0xffe84040, 0xffd02848, 0xffb01838};
        text_draw_c(f, sx, sy, size, ALIGN_CENTER, ink_of[c->level], w);
    }
}
