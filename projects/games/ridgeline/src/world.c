/* Ridgeline world (world.h).

   The look: flat colour fields with soft painted shading. Nothing here is
   lit per frame; every vertex colour is computed when the scenery is
   built, from the hour's sun and sky, with a darker base and a lighter
   crown on each tree, and each vertex keeps the albedo, normal and
   occlusion it was lit from so the clock can relight it (tod.c). Distance
   does the rest: the PVR's fog fades everything toward the haze colour,
   and the far ranges are flat silhouettes stepping lighter with distance. */
#include "world.h"
#include "tod.h"
#include "engine/sky.h"
#include "terrain.h"
#include "water.h"
#include "campfire.h"
#include "glow.h"
#include "truck.h"
#include "lamps.h"
#include "engine/prof.h"
#include <sh4zam/shz_sh4zam.h>

Scenery scenery;
V3 sun_dir;

static const pvr_poly_hdr_t *h_flat, *h_flat_nofog, *h_rock, *h_bark, *h_crown, *h_sun, *h_moon, *h_stars, *h_glow_add,
    *h_ring_add, *h_cloud;

/* ---- Lighting ---- */

static inline uint32_t lit_with(const Palette *p, V3 L, uint32_t albedo, V3 n, float ao) {
    float d = vdot(n, L);
    if(d < 0) d = 0;
    float up = 0.5f + 0.5f * n.y;
    int ch[3];
    for(int i = 0; i < 3; i++) {
        float k = (p->amb_k[i] + p->sky_k[i] * up + p->sun_k[i] * d * 1.15f) * ao;
        int c = (int)(((albedo >> (16 - 8 * i)) & 255) * k);
        ch[i] = c > 255 ? 255 : c;
    }
    return 0xff000000u | (uint32_t)ch[0] << 16 | (uint32_t)ch[1] << 8 | (uint32_t)ch[2];
}

static inline uint32_t lit(uint32_t albedo, V3 n, float ao) {
    return lit_with(pal, sun_dir, albedo, n, ao);
}

/* What each scenery vertex was lit from (world.h), kept beside the pool
   for relighting and the headlights (lamps.c). */
LightRec *world_lrec;
uint32_t world_lrec_first;      /* pool index of record 0 */
static uint32_t lrec_marked;   /* the next pool index to record */
uint32_t world_alb[64];
static int alb_n;

static uint8_t alb_index(uint32_t albedo) {
    for(int i = 0; i < alb_n; i++)
        if(world_alb[i] == albedo) return (uint8_t)i;
    if(alb_n < 64) world_alb[alb_n++] = albedo;
    return (uint8_t)(alb_n - 1);
}

static void rec_mark(LightRec r) {
    if(!world_lrec) return;
    for(uint32_t i = lrec_marked; i < scenery.pool_n; i++) world_lrec[i - world_lrec_first] = r;
    /* A join appends the previous vertex again before this one twice. */
    if(scenery.pool_n - lrec_marked == 3) world_lrec[lrec_marked - world_lrec_first] = world_lrec[lrec_marked - 1 - world_lrec_first];
    lrec_marked = scenery.pool_n;
}

static LightRec rec_of(uint32_t albedo, V3 n, float ao) {
    int a = (int)(ao * 100.0f + 0.5f);
    return (LightRec){(uint8_t)(alb_index(albedo) | (n.y < 0 ? 0x40 : 0)), (int8_t)(n.x * 127.0f), (int8_t)(n.z * 127.0f),
                      (uint8_t)(a > 255 ? 255 : a)};
}

/* A lit vertex into the open strip (lj joined on to it), and one in a
   colour used as is. */
static void lv(V3 p, float u, float v, uint32_t albedo, V3 n, float ao) {
    sc_vert(&scenery, p, u, v, lit(albedo, n, ao));
    rec_mark(rec_of(albedo, n, ao));
}

static void lj(V3 p, float u, float v, uint32_t albedo, V3 n, float ao) {
    sc_join(&scenery, p, u, v, lit(albedo, n, ao));
    rec_mark(rec_of(albedo, n, ao));
}

static void lc(V3 p, float u, float v, uint32_t col) {
    sc_vert(&scenery, p, u, v, col);
    rec_mark((LightRec){(uint8_t)(alb_index(col) | 0x80), 0, 0, 100});
}

/* The pass's light as the per-channel weights of 1, the normal's y and
   its dot with the light: k = a + b * ny + c * d. */
static float rl_a[3], rl_b[3], rl_c[3];
static V3 rl_L;

void world_relight_setup(const Palette *p, V3 light) {
    for(int i = 0; i < 3; i++) {
        rl_a[i] = p->amb_k[i] + 0.5f * p->sky_k[i];
        rl_b[i] = 0.5f * p->sky_k[i];
        rl_c[i] = 1.15f * p->sun_k[i];
    }
    rl_L = light;
}

int world_relight_n(void) {
    return world_lrec ? (int)(scenery.pool_n - world_lrec_first) : 0;
}

void world_relight(int from, int to) {
    RV *pool = scenery.pool + world_lrec_first;
    for(int k = from; k < to; k++) {
        LightRec r = world_lrec[k];
        uint32_t albedo = world_alb[r.alb & 0x3f];
        if(r.alb & 0x80) {
            pool[k].c = albedo;
            continue;
        }
        float nx = r.nx * (1.0f / 127.0f), nz = r.nz * (1.0f / 127.0f);
        float ny = sqrtf(fmaxf(1.0f - nx * nx - nz * nz, 0.0f));
        if(r.alb & 0x40) ny = -ny;
        float d = nx * rl_L.x + ny * rl_L.y + nz * rl_L.z;
        if(d < 0) d = 0;
        float ao = r.ao * 0.01f;
        int cr = (int)(((albedo >> 16) & 255) * (rl_a[0] + rl_b[0] * ny + rl_c[0] * d) * ao);
        int cg = (int)(((albedo >> 8) & 255) * (rl_a[1] + rl_b[1] * ny + rl_c[1] * d) * ao);
        int cb = (int)((albedo & 255) * (rl_a[2] + rl_b[2] * ny + rl_c[2] * d) * ao);
        if(cr > 255) cr = 255;
        if(cg > 255) cg = 255;
        if(cb > 255) cb = 255;
        pool[k].c = 0xff000000u | (uint32_t)cr << 16 | (uint32_t)cg << 8 | (uint32_t)cb;
    }
}

/* ---- Tree, rock and prop builders ---- */

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static inline float hrand(uint32_t *s) {
    *s = hash32(*s + 0x9e3779b9u);
    return (float)(*s & 0xffff) / 65535.0f;
}

static bool cone_lamps;   /* each cone its own object for the headlights (lamps.c) */

/* Texture on a cone (near trees only): us repeats round it, vs metres
   of its side a repeat, v running from its top down; 0 for none. */
static float cone_us, cone_vs;

/* A frustum (a cone with a small flat top) round a vertical axis: k sides,
   radii r0 at y0 and r1 at y1, joined on to the strip so far. Colours from
   the albedo, lit on the sun's side, darker at the base. */

static void cone(V3 base, float r0, float r1, float y0, float y1, int k, float yaw, uint32_t albedo, float ao0,
                 float ao1, bool join) {
    uint32_t f0 = scenery.pool_n;
    float slope = (r0 - r1) / fmaxf(y1 - y0, 0.01f);
    float vb = cone_vs > 0 ? sqrtf((r0 - r1) * (r0 - r1) + (y1 - y0) * (y1 - y0)) / cone_vs : 0;
    for(int s = 0; s <= k; s++) {
        float u = cone_us * (float)s / k;
        float a = yaw + ENG_TAU * (float)(s % k) / k;
        float ca = cosf(a), sa = sinf(a);
        V3 n = vnorm(v3(ca, slope, sa));
        V3 pt = v3(base.x + ca * r1, base.y + y1, base.z + sa * r1);
        V3 pb = v3(base.x + ca * r0, base.y + y0, base.z + sa * r0);
        V3 nt = vnorm(v3(ca * 0.6f, 1.0f, sa * 0.6f));
        if(s == 0 && join) lj(pt, u, 0, albedo, nt, ao1);
        else lv(pt, u, 0, albedo, nt, ao1);
        lv(pb, u, vb, albedo, n, ao0);
    }
    /* (joined on, it starts with the last cone's vertex again) */
    if(cone_lamps) lamps_add(join ? f0 + 1 : f0, scenery.pool_n);
}

static const uint32_t fir_col = 0xff2f6a3c, fir_col2 = 0xff3f7a44, larch_col = 0xffe4b23a, maple_col = 0xffd8642e,
                      trunk_col = 0xff5a4030;

/* One tree at a level of detail (0 near .. 3 far) into the open strip.
   Near, trunk and crown take the painted strokes (one texture, so a
   chunk of trees stays one strip); further off a tree is flat colour,
   the strokes being a few pixels there. */
static void tree(const lvl_tree_t *t, int lod, bool first) {
    V3 b = v3(t->x, t->y - 0.2f, t->z);
    float s = t->scale;
    uint32_t seed = (uint32_t)(t->x * 131.0f) ^ (uint32_t)(t->z * 977.0f);
    float yaw = t->yaw;
    uint32_t col;
    if(t->kind == 1) col = larch_col;
    else if(t->kind == 2) col = maple_col;
    else col = hrand(&seed) < 0.5f ? fir_col : fir_col2;
    bool j = !first;
    cone_us = lod == 0 ? 1.0f : 0;   /* the trunk */
    cone_vs = lod == 0 ? 1.6f : 0;
    float crown_us = lod == 0 ? 2.0f : 0, crown_vs = lod == 0 ? 3.0f * s : 0;
    if(t->kind == 2) {
        /* A vine maple: a round crown in two cones on a short trunk. */
        int k = lod == 0 ? 6 : lod < 3 ? 5 : 4;
        if(lod == 0) cone(b, 0.18f * s, 0.14f * s, 0, 2.2f * s, 4, yaw, trunk_col, 0.7f, 0.8f, j);
        else if(lod == 1) cone(b, 0.18f * s, 0.16f * s, 0, 1.9f * s, 3, yaw, trunk_col, 0.7f, 0.75f, j);
        j = j || lod < 2;   /* past the middle the crown hides the trunk */
        cone_us = crown_us;
        cone_vs = crown_vs;
        cone(b, 0.5f * s, 2.6f * s, 1.6f * s, 3.6f * s, k, yaw, col, 0.62f, 0.95f, j);
        cone(b, 2.6f * s, 0.4f * s, 3.6f * s, 6.0f * s, k, yaw, col, 0.95f, 1.15f, true);
        return;
    }
    float slim = t->kind == 1 ? 0.78f : 1.0f;
    static const float tiers[3][4] = {{3.0f, 1.2f, 6.8f, 0.30f}, {2.3f, 4.6f, 9.6f, 0.24f}, {1.5f, 8.0f, 12.6f, 0.12f}};
    if(lod < 2) {
        /* Out to ~105 m a tree is the near one's three tiers on five
           sides over a three-sided stump of trunk, so the change between
           them is hard to see. */
        if(lod == 0) cone(b, 0.26f * s, 0.2f * s, 0, 3.0f * s, 4, yaw, trunk_col, 0.7f, 0.8f, j);
        else cone(b, 0.26f * s, 0.24f * s, 0, 1.6f * s, 3, yaw, trunk_col, 0.7f, 0.75f, j);
        j = true;
        cone_us = crown_us;
        cone_vs = crown_vs;
        for(int i = 0; i < 3; i++) {
            float ao = 0.62f + 0.14f * i;
            cone(b, tiers[i][0] * s * slim, tiers[i][3] * s, tiers[i][1] * s, tiers[i][2] * s, lod ? 5 : 6, yaw + i * 0.5f,
                 col, ao, ao + 0.42f, j);
            j = true;
        }
    }
    else if(lod == 2) {
        /* Then the bottom tier and one cone over the top two, the same
           outline less a notch and as light at the top. */
        cone(b, tiers[0][0] * s * slim, tiers[0][3] * s, tiers[0][1] * s, tiers[0][2] * s, 5, yaw, col, 0.62f, 1.04f, j);
        cone(b, tiers[1][0] * s * slim, tiers[2][3] * s, tiers[1][1] * s, tiers[2][2] * s, 5, yaw + 0.5f, col, 0.76f, 1.32f,
             true);
    }
    else {
        cone(b, 2.8f * s * slim, 0.05f * s, 0.6f * s, 12.4f * s, 3, yaw, col, 0.66f, 1.1f, j);
    }
}

/* A boulder: rings round a vertical axis, jittered so it reads faceted. */
static void boulder(V3 c, float r, uint32_t seed, int lod) {
    int k = lod ? 5 : 7;
    static const float ring_y[4] = {-0.45f, 0.15f, 0.7f, 1.0f}, ring_r[4] = {0.95f, 1.0f, 0.62f, 0.08f};
    float jit[4][8];
    for(int i = 0; i < 4; i++)
        for(int s = 0; s < k; s++) jit[i][s] = 0.82f + 0.3f * hrand(&seed);
    float squash = 0.7f + 0.25f * hrand(&seed);
    for(int i = 0; i < 3; i++) {
        sc_strip_begin(&scenery, h_rock);
        for(int s = 0; s <= k; s++) {
            int sm = s % k;
            float a = ENG_TAU * sm / k;
            for(int e = 1; e >= 0; e--) {
                int ri = i + e;
                float rr = r * ring_r[ri] * jit[ri][sm];
                V3 p = v3(c.x + cosf(a) * rr, c.y + r * ring_y[ri] * squash, c.z + sinf(a) * rr);
                V3 n = vnorm(v3(cosf(a) * (1.0f - ring_y[ri] * 0.5f), ring_y[ri] + 0.3f, sinf(a) * (1.0f - ring_y[ri] * 0.5f)));
                lv(p, a * 0.6f, ring_y[ri] * 1.2f, 0xff9aa0a6, n, 0.8f + 0.25f * ring_y[ri]);
            }
        }
        sc_strip_end(&scenery);
    }
}

/* The height of boulder i's crown as boulder() draws it near (lod 0): the
   same seed, the same jitter draws, then the squash. */
float world_boulder_top(int i) {
    const lvl_sphere_t *s = &lvl_spheres[i];
    uint32_t seed = (uint32_t)i * 2654435761u;
    for(int k = 0; k < 4 * 7; k++) hrand(&seed);
    float squash = 0.7f + 0.25f * hrand(&seed);
    return s->y + s->r * 1.04f * squash;
}

/* A box from its centre, half extents and turn: sides then top. */
static void box(const pvr_poly_hdr_t *h, V3 c, float hx, float hy, float hz, float yaw, uint32_t albedo, float uv) {
    float cy = cosf(yaw), sy = sinf(yaw);
    V3 ax = v3(cy * hx, 0, -sy * hx), az = v3(sy * hz, 0, cy * hz);
    V3 corner[4] = {vadd(vadd(c, ax), az), vsub(vadd(c, ax), az), vsub(vsub(c, ax), az), vadd(vsub(c, ax), az)};
    sc_strip_begin(&scenery, h);
    for(int s = 0; s <= 4; s++) {
        V3 p = corner[s & 3];
        V3 nrm = vnorm(vsub(vadd(corner[s & 3], corner[(s + 3) & 3]), vscale(c, 2)));
        float u = (float)s * (hx + hz) * uv;
        lv(v3(p.x, c.y + hy, p.z), u, 0, albedo, nrm, 0.95f);
        lv(v3(p.x, c.y - hy, p.z), u, hy * 2 * uv, albedo, nrm, 0.95f * 0.75f);
    }
    sc_strip_end(&scenery);
    sc_strip_begin(&scenery, h);
    V3 up = v3(0, 1, 0);
    lv(v3(corner[0].x, c.y + hy, corner[0].z), 0, 0, albedo, up, 1.0f);
    lv(v3(corner[1].x, c.y + hy, corner[1].z), hz * 2 * uv, 0, albedo, up, 1.0f);
    lv(v3(corner[3].x, c.y + hy, corner[3].z), 0, hx * 2 * uv, albedo, up, 1.0f);
    lv(v3(corner[2].x, c.y + hy, corner[2].z), hz * 2 * uv, hx * 2 * uv, albedo, up, 1.0f);
    sc_strip_end(&scenery);
}

/* box() with its sides cut into columns about seg metres wide and, when
   taller than 2.4 m, two rows: a headlight lights vertices, and a wall
   with only its corners would stay dark with the beam on its middle. */
static void box_seg(const pvr_poly_hdr_t *h, V3 c, float hx, float hy, float hz, float yaw, uint32_t albedo, float uv,
                    float seg) {
    float cy = cosf(yaw), sy = sinf(yaw);
    V3 ax = v3(cy * hx, 0, -sy * hx), az = v3(sy * hz, 0, cy * hz);
    V3 corner[4] = {vadd(vadd(c, ax), az), vsub(vadd(c, ax), az), vsub(vsub(c, ax), az), vadd(vsub(c, ax), az)};
    V3 nrm[4];
    for(int s = 0; s < 4; s++) nrm[s] = vnorm(vsub(vadd(corner[s], corner[(s + 3) & 3]), vscale(c, 2)));
    int rows = hy > 1.2f ? 2 : 1;
    for(int r = 0; r < rows; r++) {
        float t0 = (float)r / rows, t1 = (float)(r + 1) / rows;   /* down from the top */
        float y0 = c.y + hy - 2 * hy * t0, y1 = c.y + hy - 2 * hy * t1;
        float ao0 = 0.95f * (1.0f - 0.25f * t0), ao1 = 0.95f * (1.0f - 0.25f * t1);
        sc_strip_begin(&scenery, h);
        for(int s = 0; s < 4; s++) {
            V3 p0 = corner[s], p1 = corner[(s + 1) & 3];
            int n = (int)ceilf(vlen(vsub(p1, p0)) / seg);
            if(n < 1) n = 1;
            for(int i = s ? 1 : 0; i <= n; i++) {
                float t = (float)i / n;
                V3 p = vadd(p0, vscale(vsub(p1, p0), t));
                V3 nn = vnorm(vadd(vscale(nrm[s], 1.0f - t), vscale(nrm[(s + 1) & 3], t)));
                float u = (s + t) * (hx + hz) * uv;
                lv(v3(p.x, y0, p.z), u, 2 * hy * t0 * uv, albedo, nn, ao0);
                lv(v3(p.x, y1, p.z), u, 2 * hy * t1 * uv, albedo, nn, ao1);
            }
        }
        sc_strip_end(&scenery);
    }
    sc_strip_begin(&scenery, h);
    V3 up = v3(0, 1, 0);
    lv(v3(corner[0].x, c.y + hy, corner[0].z), 0, 0, albedo, up, 1.0f);
    lv(v3(corner[1].x, c.y + hy, corner[1].z), hz * 2 * uv, 0, albedo, up, 1.0f);
    lv(v3(corner[3].x, c.y + hy, corner[3].z), 0, hx * 2 * uv, albedo, up, 1.0f);
    lv(v3(corner[2].x, c.y + hy, corner[2].z), hz * 2 * uv, hx * 2 * uv, albedo, up, 1.0f);
    sc_strip_end(&scenery);
}

/* A square timber from its foot up to its top, half width w, sides only:
   the foot is in the ground and the top under a deck. */
static void leg(V3 a, V3 b, float w, uint32_t albedo) {
    V3 c[4] = {v3(w, 0, w), v3(w, 0, -w), v3(-w, 0, -w), v3(-w, 0, w)};
    float len = vlen(vsub(b, a));
    sc_strip_begin(&scenery, h_bark);
    for(int s = 0; s <= 4; s++) {
        V3 o = c[s & 3];
        V3 nrm = vnorm(vadd(o, c[(s + 3) & 3]));
        float u = s * 2 * w;
        lv(vadd(b, o), u, 0, albedo, nrm, 0.95f);
        lv(vadd(a, o), u, len, albedo, nrm, 0.95f * 0.75f);
    }
    sc_strip_end(&scenery);
}

/* A log: a six-sided cylinder along the capsule, bark round it. */
static void log_mesh(const lvl_capsule_t *cp) {
    V3 a = v3(cp->ax, cp->ay, cp->az), b = v3(cp->bx, cp->by, cp->bz);
    V3 ax = vnorm(vsub(b, a));
    V3 side = vnorm(vcross(ax, v3(0, 1, 0)));
    V3 up = vcross(side, ax);
    float r = cp->r, len = vlen(vsub(b, a));
    sc_strip_begin(&scenery, h_bark);
    for(int s = 0; s <= 6; s++) {
        float ang = ENG_TAU * (s % 6) / 6.0f;
        V3 n = vadd(vscale(side, cosf(ang)), vscale(up, sinf(ang)));
        lv(vmad(a, n, r), s / 3.0f, 0, 0xff7a5a3e, n, 0.9f);
        lv(vmad(b, n, r), s / 3.0f, len / 1.5f, 0xff7a5a3e, n, 0.9f);
    }
    sc_strip_end(&scenery);
    for(int e = 0; e < 2; e++) {
        V3 c = e ? b : a;
        sc_strip_begin(&scenery, h_flat);
        V3 en = vscale(ax, e ? 1.0f : -1.0f);
        static const int order[6] = {0, 1, 5, 2, 4, 3};
        for(int s = 0; s < 6; s++) {
            float ang = ENG_TAU * order[s] / 6.0f;
            lv(vmad(c, vadd(vscale(side, cosf(ang)), vscale(up, sinf(ang))), r * 0.98f), 0, 0, 0xffd8b88a, en, 1.0f);
        }
        sc_strip_end(&scenery);
    }
}

static void prop(const lvl_prop_t *p) {
    V3 c = v3(p->x, p->y, p->z);
    float cy = cosf(p->yaw), sy = sinf(p->yaw);
    switch(p->kind) {
        case PROP_CABIN: {
            box_seg(h_bark, v3(c.x, c.y + 1.4f, c.z), 4.2f, 1.6f, 3.2f, p->yaw, 0xffa0703e, 0.4f, 1.5f);
            /* The roof: a ridge along the cabin's length. */
            V3 ax = v3(cy * 4.8f, 0, -sy * 4.8f), az = v3(sy * 3.8f, 0, cy * 3.8f);
            V3 e0 = vadd(c, az), e1 = vsub(c, az);
            float eave = c.y + 3.0f, ridge = c.y + 5.0f;
            uint32_t rc = 0xff9a3a2a;
            for(int side = -1; side <= 1; side += 2) {
                V3 n = vnorm(v3(cy * side, 0.9f, -sy * side));
                sc_strip_begin(&scenery, h_flat);
                V3 o = vscale(ax, (float)side);
                lv(v3(e0.x + o.x, eave, e0.z + o.z), 0, 0, rc, n, 1.0f);
                lv(v3(e1.x + o.x, eave, e1.z + o.z), 0, 0, rc, n, 1.0f);
                lv(v3(e0.x, ridge, e0.z), 0, 0, rc, n, 1.0f);
                lv(v3(e1.x, ridge, e1.z), 0, 0, rc, n, 1.0f);
                sc_strip_end(&scenery);
            }
            for(int end = -1; end <= 1; end += 2) {
                V3 o = vscale(az, (float)end * 0.84f), n = vnorm(o);
                sc_strip_begin(&scenery, h_flat);
                lv(v3(c.x + o.x - ax.x * 0.88f, eave, c.z + o.z - ax.z * 0.88f), 0, 0, 0xffb88850, n, 0.95f);
                lv(v3(c.x + o.x + ax.x * 0.88f, eave, c.z + o.z + ax.z * 0.88f), 0, 0, 0xffb88850, n, 0.95f);
                lv(v3(c.x + o.x, ridge - 0.1f, c.z + o.z), 0, 0, 0xffb88850, n, 0.95f);
                sc_strip_end(&scenery);
            }
            box(h_rock, v3(c.x + cy * 2.6f, c.y + 3.6f, c.z - sy * 2.6f), 0.4f, 1.6f, 0.4f, p->yaw, 0xff8a8a90, 0.5f);
            break;
        }
        case PROP_SIGN:
            box(h_bark, v3(c.x, c.y + 1.0f, c.z), 0.1f, 1.0f, 0.1f, p->yaw, 0xff6a4a30, 1.0f);
            box(h_flat, v3(c.x, c.y + 1.9f, c.z), 0.9f, 0.35f, 0.06f, p->yaw, 0xff3c6a44, 1.0f);
            box(h_flat, v3(c.x, c.y + 1.9f, c.z), 0.75f, 0.06f, 0.07f, p->yaw, 0xfff0e0b0, 1.0f);
            break;
        case PROP_TABLE:
            box(h_bark, v3(c.x, c.y + 0.75f, c.z), 0.9f, 0.05f, 0.4f, p->yaw, 0xffa07850, 1.0f);
            box(h_bark, v3(c.x, c.y + 0.45f, c.z), 0.9f, 0.04f, 0.15f, p->yaw + 0.0f, 0xff8a6440, 1.0f);
            box(h_bark, v3(c.x, c.y + 0.37f, c.z), 0.7f, 0.37f, 0.06f, p->yaw, 0xff6a4a30, 1.0f);
            break;
        case PROP_CAIRN: {
            uint32_t seed = (uint32_t)(p->x * 31.0f) ^ (uint32_t)(p->z * 7.0f);
            float y = c.y;
            for(int i = 0; i < 4; i++) {
                float r = 0.55f - i * 0.11f;
                boulder(v3(c.x + (hrand(&seed) - 0.5f) * 0.1f, y + r * 0.45f, c.z), r, seed, 1);
                y += r * 0.85f;
            }
            box(h_bark, v3(c.x + 0.1f, y + 0.6f, c.z), 0.04f, 0.8f, 0.04f, 0, 0xff6a4a30, 1.0f);
            box(h_flat, v3(c.x + 0.45f, y + 1.15f, c.z), 0.35f, 0.2f, 0.02f, 0, 0xffe04a30, 1.0f);
            break;
        }
        case PROP_BRIDGE: {
            /* A plank deck on two log stringers, posts holding the rail
               logs (the rails and the deck are the level's obstacles). */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            box(h_bark, v3(c.x, c.y - 0.15f, c.z), LVL_BRIDGE_HX, 0.15f, LVL_BRIDGE_HZ, p->yaw, 0xffb08a5c, 0.7f);
            for(int side = -1; side <= 1; side += 2) {
                V3 m = vadd(c, vscale(r, side * (LVL_BRIDGE_HX - 0.7f)));
                V3 a = vsub(m, vscale(f, LVL_BRIDGE_HZ + 0.6f)), b = vadd(m, vscale(f, LVL_BRIDGE_HZ + 0.6f));
                lvl_capsule_t st = {a.x, c.y - 0.55f, a.z, b.x, c.y - 0.55f, b.z, 0.26f};
                log_mesh(&st);
                for(float t = -LVL_BRIDGE_HZ + 0.6f; t <= LVL_BRIDGE_HZ - 0.5f; t += 3.2f) {
                    V3 q = vadd(vadd(c, vscale(r, side * (LVL_BRIDGE_HX - 0.15f))), vscale(f, t));
                    box(h_bark, v3(q.x, c.y + 0.2f, q.z), 0.09f, 0.22f, 0.09f, p->yaw, 0xff6a4a30, 1.0f);
                }
            }
            break;
        }
        case PROP_WRECK: {
            /* An old pickup left to rust: cab, hood, bed, three wheels. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            const uint32_t rust = 0xff8e5232, teal = 0xff5f7f78;
            box(h_flat, vadd(c, v3(f.x * 0.3f, 1.35f, f.z * 0.3f)), 0.82f, 0.48f, 0.62f, p->yaw, teal, 1.0f);
            box(h_flat, vadd(c, v3(f.x * 0.94f, 1.52f, f.z * 0.94f)), 0.74f, 0.26f, 0.03f, p->yaw, 0xff3a4246, 1.0f);
            box(h_flat, vadd(c, v3(f.x * 1.55f, 0.95f, f.z * 1.55f)), 0.8f, 0.3f, 0.6f, p->yaw, rust, 1.0f);
            box(h_flat, vadd(c, v3(-f.x * 1.15f, 0.8f, -f.z * 1.15f)), 0.84f, 0.32f, 1.05f, p->yaw, rust, 1.0f);
            box(h_flat, vadd(c, v3(f.x * 0.2f, 0.6f, f.z * 0.2f)), 0.86f, 0.14f, 2.1f, p->yaw, 0xff4a3a30, 1.0f);
            for(int k = 0; k < 3; k++) {
                float along = k == 0 ? 1.4f : -1.3f, side = k == 2 ? -1.0f : 1.0f;
                V3 w = vadd(vadd(c, vscale(f, along)), vscale(r, side * 0.86f));
                V3 a = vsub(w, vscale(r, 0.14f)), b = vadd(w, vscale(r, 0.14f));
                lvl_capsule_t wh = {a.x, c.y + 0.36f, a.z, b.x, c.y + 0.36f, b.z, 0.38f};
                log_mesh(&wh);
            }
            break;
        }
        case PROP_ARCH: {
            /* Two stone pillars and a lintel high enough to drive under. */
            V3 r = v3(cy, 0, -sy);
            uint32_t seed = (uint32_t)(p->x * 17.0f) ^ (uint32_t)(p->z * 29.0f);
            for(int side = -1; side <= 1; side += 2) {
                V3 q = vadd(c, vscale(r, side * 3.4f));
                box_seg(h_rock, v3(q.x, c.y + 2.05f, q.z), 0.9f, 2.25f, 1.1f, p->yaw + side * 0.08f, 0xff9a9488, 0.35f, 1.5f);
            }
            box_seg(h_rock, v3(c.x, c.y + 4.75f, c.z), 4.4f, 0.55f, 1.2f, p->yaw, 0xffa49e92, 0.35f, 1.5f);
            boulder(v3(c.x + r.x * 1.5f, c.y + 5.4f, c.z + r.z * 1.5f), 0.7f, seed, 0);
            boulder(v3(c.x - r.x * 2.2f, c.y + 5.3f, c.z - r.z * 2.2f), 0.5f, seed * 3u, 0);
            break;
        }
        case PROP_TENT: {
            /* A ridge tent, a fire ring in front of it. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            V3 fr = vadd(c, vscale(f, 1.1f)), bk = vsub(c, vscale(f, 1.1f));
            for(int side = -1; side <= 1; side += 2) {
                V3 n = vnorm(v3(r.x * side, 0.8f, r.z * side));
                sc_strip_begin(&scenery, h_flat);
                lv(v3(fr.x + r.x * side * 0.95f, c.y, fr.z + r.z * side * 0.95f), 0, 0, 0xffd8642a, n, 1.0f);
                lv(v3(bk.x + r.x * side * 0.95f, c.y, bk.z + r.z * side * 0.95f), 0, 0, 0xffd8642a, n, 1.0f);
                lv(v3(fr.x, c.y + 1.35f, fr.z), 0, 0, 0xffd8642a, n, 1.0f);
                lv(v3(bk.x, c.y + 1.35f, bk.z), 0, 0, 0xffd8642a, n, 1.0f);
                sc_strip_end(&scenery);
            }
            for(int end = 0; end < 2; end++) {
                V3 e = end ? fr : bk, n = vscale(f, -1.0f);
                V3 q[3] = {v3(e.x - r.x * 0.95f, c.y, e.z - r.z * 0.95f), v3(e.x + r.x * 0.95f, c.y, e.z + r.z * 0.95f),
                           v3(e.x, c.y + 1.35f, e.z)};
                sc_strip_begin(&scenery, h_flat);
                for(int k = 0; k < 3; k++) {
                    /* The open end is the dark inside of the tent. */
                    if(end) lc(q[k], 0, 0, 0xff3a2a20);
                    else lv(q[k], 0, 0, 0xffc0582a, n, 0.9f);
                }
                sc_strip_end(&scenery);
            }
            /* The ring of stones on the ground where the fire burns
               (campfire.c), embers in it, split logs leant
               together over them. */
            V3 fire = campfire_pos();
            uint32_t seed = (uint32_t)(p->x * 13.0f) ^ (uint32_t)(p->z * 7.0f);
            for(int k = 0; k < 8; k++) {
                float a = k * ENG_TAU / 8;
                float sx = fire.x + sinf(a) * 0.62f, sz = fire.z + cosf(a) * 0.62f;
                boulder(v3(sx, terrain_height(sx, sz) + 0.05f, sz), 0.16f, seed + k, 1);
            }
            sc_strip_begin(&scenery, h_flat);
            for(int k = 0; k < 4; k++)
                lc(v3(fire.x + (k & 1 ? 0.3f : -0.3f), fire.y + 0.05f, fire.z + (k & 2 ? 0.3f : -0.3f)), 0, 0, 0xffb8431a);
            sc_strip_end(&scenery);
            for(int k = 0; k < 5; k++) {
                float a = k * ENG_TAU / 5 + 0.3f;
                V3 foot = v3(fire.x + sinf(a) * 0.4f, fire.y + 0.02f, fire.z + cosf(a) * 0.4f);
                leg(foot, v3(fire.x - sinf(a) * 0.05f, fire.y + 0.3f, fire.z - cosf(a) * 0.05f), 0.045f, 0xff4a3426);
            }
            break;
        }
        case PROP_TOWER: {
            /* A fire lookout: four splayed legs (the level's posts), the
               cab on top, a pyramid roof. */
            for(int k = 0; k < LVL_POSTS; k++) {
                const lvl_post_t *q = &lvl_posts[k];
                if(fabsf(q->x1 - c.x) > 3.0f || fabsf(q->z1 - c.z) > 3.0f) continue;
                leg(v3(q->x0, q->y0, q->z0), v3(q->x1, q->y1, q->z1), 0.24f, 0xff6a4a30);
            }
            box(h_bark, v3(c.x, c.y + 6.0f, c.z), 2.2f, 0.12f, 2.2f, 0, 0xff8a6440, 0.5f);
            box_seg(h_flat, v3(c.x, c.y + 7.2f, c.z), 1.9f, 1.1f, 1.9f, 0, 0xffe8dcc0, 1.0f, 1.3f);
            V3 rn = vnorm(v3(-0.6f, 1, 0.2f));
            sc_strip_begin(&scenery, h_flat);
            V3 apex = v3(c.x, c.y + 9.8f, c.z);
            static const float cx[5] = {-2.4f, 2.4f, 2.4f, -2.4f, -2.4f}, cz[5] = {-2.4f, -2.4f, 2.4f, 2.4f, -2.4f};
            for(int k = 0; k < 5; k++) {
                lv(v3(c.x + cx[k], c.y + 8.3f, c.z + cz[k]), 0, 0, 0xff3c6a44, rn, 0.8f + 0.1f * (k & 1));
                lv(apex, 0, 0, 0xff3c6a44, rn, 1.0f);
            }
            sc_strip_end(&scenery);
            /* Lanterns (glow.c lights them): one hung under each corner
               of the roof, one under the deck. A brass cap, amber glass
               in a colour used as is, so it shows lit by day too. */
            const V3 *lp;
            int ln = glow_tower_lanterns(&lp);
            for(int k = 0; k < ln; k++) {
                V3 q = vadd(c, lp[k]);
                float s = k < 4 ? 0.09f : 0.13f;
                box(h_flat, v3(q.x, q.y + s * 1.7f, q.z), s * 1.25f, s * 0.25f, s * 1.25f, 0, 0xff5a4a30, 1.0f);
                leg(v3(q.x, q.y + s * 1.9f, q.z), v3(q.x, q.y + s * 1.9f + (k < 4 ? 0.42f : 0.35f), q.z), 0.012f, 0xff3a3028);
                sc_strip_begin(&scenery, h_flat);
                for(int e = 0; e <= 4; e++) {
                    float ex = ((e + 1) & 2) ? -s : s, ez = (e & 2) ? -s : s;
                    lc(v3(q.x + ex, q.y + s * 1.45f, q.z + ez), 0, 0, 0xffe6a84c);
                    lc(v3(q.x + ex * 0.8f, q.y - s * 1.3f, q.z + ez * 0.8f), 0, 0, 0xffc8802c);
                }
                sc_strip_end(&scenery);
            }
            break;
        }
    }
}

/* ---- Distant ranges and sky ---- */

#define SKY_SEG 16
#define RANGES 3
#define RANGE_SEG 48
static SkyDome sky;   /* engine/sky.h: 6 rings of SKY_SEG */
static float range_h[RANGES][RANGE_SEG + 1];
static uint32_t range_top[RANGES][2], range_bot[RANGES][2];   /* away from the sun, toward it */
/* Clouds: a dozen cumulus (TEX_CLOUD, four shapes) carried across the sky
   on the wind, each forming and clearing on its own slow cycle. Their
   positions are kept about the camera (along the wind and across it), so
   driving never moves them: they are far away. */
#define CLOUDS 12
typedef struct {
    float along, across;   /* metres from the camera, in the wind's frame */
    float y, w;            /* height and half width */
    int shape;             /* column of the atlas */
    float phase, period;   /* its forming and clearing cycle, seconds */
} Cloud;
static Cloud clouds[CLOUDS];
static float cloud_t;
#define CLOUD_FAR 1350.0f
static const float sky_el[6] = {-0.25f, 0.0f, 0.08f, 0.22f, 0.55f, 1.5707f};
static float range_ca[RANGE_SEG + 1][RANGES], range_sa[RANGE_SEG + 1][RANGES];   /* each range vertex's bearing */

/* ---- The haze toward the sun, and the cloud on the summit ----

   The sky low toward the sun takes the sunrise or sunset glow, so the
   haze the hills fade into does too: the fog's colour warms as the camera
   turns toward the sun (the PVR has one fog colour for the screen, so it
   follows the view's heading), and the bottom of the dome and the far
   ranges warm by bearing to match.

   A cap of cloud sits on the upper mountain, thick in the morning and
   the evening, thin by early afternoon: from outside a soft collar of
   mist wrapped round the peak, from inside a whiteout (the fog pulled in
   to tens of metres and the sky gone to the cloud's colour). The summit
   stands above it, so the summit trail climbs through it and comes out
   into the sun. It goes by the hour alone, which an online lobby shares. */
#define HAZE_GLOW 0.85f       /* the fog toward the sun: this share of the horizon's glow */
#define CAP_BASE 108.0f
#define CAP_TOP 129.0f
#define CAP_SOFT 6.0f         /* the cloud thins over this either side of the base and top */
#define CAP_R 115.0f          /* and out to here from the summit (horizontally) */
#define CAP_R_SOFT 35.0f
#define CAP_FOG_FAR 55.0f     /* the fog's far distance deep in it */
/* The collar: rings round the peak, each a height and how far out from
   the mountain's side it sits there. Clear at the top and the bottom
   rings, thick between; the mountain narrows as it
   climbs, so a straight run between two rings stays off the slope. */
#define CAP_SEG 32
#define CAP_RINGS 4
static const float cap_ring_y[CAP_RINGS] = {CAP_TOP + 5.0f, CAP_TOP - 3.0f, CAP_BASE + 5.0f, CAP_BASE - 4.0f};
static const float cap_ring_out[CAP_RINGS] = {4.0f, 12.0f, 18.0f, 26.0f};
static V3 cap_v[CAP_RINGS][CAP_SEG + 1];
static float cap_sa[CAP_SEG + 1], cap_ca[CAP_SEG + 1];
static bool cap_built;
static unsigned atmo_tick;   /* frames: slow colours are worked out every fourth */
#define CAP_NOISE 64
static float cap_noise_tab[CAP_NOISE];   /* where the mist is thick round the collar */
static float cap_k;           /* how much cloud this hour, 0..1 */
static float cap_in;          /* how deep in it the camera is, 0..1 */
static uint32_t fog_col, cap_col;
static float fog_near_m, fog_far_m;   /* the fog table's, for what is fogged by hand */
static const pvr_poly_hdr_t *h_cap;

/* By the hour: thick from first light to mid morning, burning off to a
   thin cap by early afternoon, back as the evening cools. */
static float cap_amount(float hour) {
    static const float k[][2] = {{0, 0.85f}, {5.0f, 1.0f}, {10.0f, 1.0f}, {13.0f, 0.55f}, {15.5f, 0.6f}, {18.5f, 1.0f}, {24.0f, 0.85f}};
    int i = 0;
    while(hour > k[i + 1][0]) i++;
    float t = (hour - k[i][0]) / (k[i + 1][0] - k[i][0]);
    t = t * t * (3 - 2 * t);
    return k[i][1] + (k[i + 1][1] - k[i][1]) * t;
}

static inline float smooth01(float x) {
    x = clampf(x, 0, 1);
    return x * x * (3 - 2 * x);
}

/* How far into the cap a point is, 0..1, before the hour's amount. */
static float cap_depth(V3 p) {
    float dx = p.x - LEVEL_SUMMIT_X, dz = p.z - LEVEL_SUMMIT_Z;
    float r = sqrtf(dx * dx + dz * dz);
    float v = smooth01((p.y - (CAP_BASE - CAP_SOFT)) / (2 * CAP_SOFT)) * smooth01(((CAP_TOP + CAP_SOFT) - p.y) / (2 * CAP_SOFT));
    return v * smooth01((CAP_R + CAP_R_SOFT - r) / (2 * CAP_R_SOFT));
}

/* How far out from the summit the mountain's side passes height y, along
   bearing (sa, ca). */
static float mountain_r(float sa, float ca, float y) {
    float r = 4.0f;
    while(r < CAP_R + CAP_R_SOFT && terrain_height(LEVEL_SUMMIT_X + sa * r, LEVEL_SUMMIT_Z + ca * r) > y) r += 2.0f;
    return r;
}

static void cap_build(void) {
    for(int i = 0; i <= CAP_SEG; i++) {
        float a = ENG_TAU * i / CAP_SEG;
        cap_sa[i] = sinf(a);
        cap_ca[i] = cosf(a);
        for(int k = 0; k < CAP_RINGS; k++) {
            /* Lumpy: the top swells and dips round the peak, the sides bulge. */
            uint32_t hs = hash32((uint32_t)(i % CAP_SEG) * 977u + 13u);
            float lump = (hs & 1023) * (1.0f / 1023), bulge = (hs >> 10 & 1023) * (1.0f / 1023);
            float y = cap_ring_y[k] + (k == 0 ? 5.0f * lump - 2.0f : k == 1 ? 3.0f * lump : 0);
            float r = mountain_r(cap_sa[i], cap_ca[i], y) + cap_ring_out[k] * (0.7f + 0.6f * bulge);
            cap_v[k][i] = v3(LEVEL_SUMMIT_X + cap_sa[i] * r, y, LEVEL_SUMMIT_Z + cap_ca[i] * r);
        }
    }
    for(int i = 0; i < CAP_NOISE; i++) cap_noise_tab[i] = (hash32((uint32_t)i * 2654435761u) & 1023) * (1.0f / 1023);
    cap_built = true;
}

/* Integer lerp of two colours, t in 0..256 (the dome and ranges do a few
   hundred a frame). */
static inline uint32_t mix256(uint32_t a, uint32_t b, int t) {
    uint32_t rb = ((a & 0xff00ffu) * (uint32_t)(256 - t) + (b & 0xff00ffu) * (uint32_t)t) >> 8;
    uint32_t ag = (((a >> 8) & 0xff00ffu) * (uint32_t)(256 - t) + ((b >> 8) & 0xff00ffu) * (uint32_t)t) >> 8;
    return (rb & 0xff00ffu) | ((ag & 0xff00ffu) << 8);
}

/* The glow toward the sun at a bearing (sin, cos), 0..glow_k. */
static float glow_cg, glow_sg;
static inline float glow_at(float sa, float ca) {
    float t = 0.5f + 0.5f * (sa * glow_sg + ca * glow_cg);
    return t * t * t * pal->glow_k;
}

static void haze_update(void) {
    glow_sg = sinf(tod.glow_az);
    glow_cg = cosf(tod.glow_az);
    cap_k = cap_amount(tod.hour);
    cap_in = cap_depth(cam.pos) * smooth01(cap_k * 1.6f);
    /* The cloud from inside: the palette's cloud a shade down, in the haze. */
    cap_col = color_scale(color_lerp(pal->haze, pal->cloud_col, 0.65f), 0.94f);
    V3 f = cam.fwd;
    float fl = sqrtf(f.x * f.x + f.z * f.z);
    float w = fl > 0.01f ? glow_at(f.x / fl, f.z / fl) : 0;
    fog_col = color_lerp(color_lerp(pal->haze, pal->glow_col, w * HAZE_GLOW), cap_col, cap_in);
}

/* Stars: patches of them (TEX_STARS) on fixed directions, drawn as the
   sky darkens. A patch each is far cheaper than a sprite a star. */
#define STARS 14
static V3 star_dir[STARS];
static float star_rot[STARS], star_b[STARS];

/* The dome's colours for the hour: the palette's gradient, warmed low in
   the sky toward the sun (the moon at night), the haze at the horizon. */
static void sky_colours(void) {
    static float sky_sa[SKY_SEG + 1], sky_ca[SKY_SEG + 1];
    if(sky_ca[0] == 0)
        for(int i = 0; i <= SKY_SEG; i++) {
            sky_sa[i] = sinf(ENG_TAU * i / SKY_SEG);
            sky_ca[i] = cosf(ENG_TAU * i / SKY_SEG);
        }
    /* Each ring's gradient colour and how much glow it takes; below the
       horizon the haze, warmed as the fog is. */
    uint32_t ring_c[6];
    float ring_g[6];
    for(int e = 0; e < 6; e++) {
        float t = clampf(sky_el[e] / 0.9f, 0, 1);
        ring_c[e] = e ? color_lerp(pal->sky_horizon, pal->sky_top, sqrtf(t)) : pal->haze;
        ring_g[e] = (e ? 1.0f - t : HAZE_GLOW) * 256.0f;
    }
    int in = (int)(cap_in * 256.0f);
    for(int i = 0; i <= SKY_SEG; i++) {
        float g = glow_at(sky_sa[i], sky_ca[i]);
        for(int e = 0; e < 6; e++) {
            uint32_t c = mix256(ring_c[e], pal->glow_col, (int)(g * ring_g[e]));
            /* Inside the cap the sky goes to the cloud. */
            if(in) c = mix256(c, cap_col, in);
            sky_set_color(&sky, e, i, c | 0xff000000u);
        }
    }
    /* Further ranges sit lighter and bluer in the haze; all of them go
       dark with the night. Each has a colour away from the sun and one
       toward it, mixed by bearing as they are drawn. */
    uint32_t base = color_lerp(0xff4e6a70, 0xff0a1424, pal->dark);
    uint32_t warm = color_lerp(pal->haze, pal->glow_col, HAZE_GLOW);
    for(int r = 0; r < RANGES; r++) {
        float k = 0.35f + 0.22f * r;
        range_top[r][0] = color_lerp(base, pal->haze, k);
        range_bot[r][0] = color_lerp(range_top[r][0], pal->haze, 0.55f);
        range_top[r][1] = color_lerp(base, warm, k);
        range_bot[r][1] = color_lerp(range_top[r][1], warm, 0.55f);
        for(int s = 0; s < 2 && cap_in > 0; s++) {
            range_top[r][s] = color_lerp(range_top[r][s], cap_col, cap_in);
            range_bot[r][s] = color_lerp(range_bot[r][s], cap_col, cap_in);
        }
    }
}

/* A cloud's shape, size, height and where across the wind it rides:
   never close overhead, where one would fill the sky. */
static void cloud_roll(Cloud *c, uint32_t *seed) {
    float side = hrand(seed) < 0.5f ? -1.0f : 1.0f;
    c->across = side * (650.0f + 550.0f * hrand(seed));
    c->y = 240.0f + 180.0f * hrand(seed);
    c->w = 120.0f + 140.0f * hrand(seed);
    c->shape = (int)(hrand(seed) * 3.999f);
    c->period = 180.0f + 200.0f * hrand(seed);
    c->phase = 0;
}

static void sky_setup(void) {
    sky_init(&sky, sky_el, 6, SKY_SEG, 1900.0f, h_flat_nofog);
    uint32_t seed = 4242;
    for(int r = 0; r < RANGES; r++) {
        float base = 60.0f + 50.0f * r;
        for(int i = 0; i < RANGE_SEG; i++) {
            float x = (float)i / RANGE_SEG * ENG_TAU;
            float h = base + 70.0f * (0.5f + 0.5f * sinf(x * (2 + r) + r)) + 60.0f * hrand(&seed) + 40.0f * hrand(&seed);
            range_h[r][i] = h;
        }
        range_h[r][RANGE_SEG] = range_h[r][0];
        for(int i = 0; i <= RANGE_SEG; i++) {
            float a = ENG_TAU * i / RANGE_SEG + r * 0.37f;
            range_sa[i][r] = sinf(a);
            range_ca[i][r] = cosf(a);
        }
    }
    for(int i = 0; i < CLOUDS; i++) {
        cloud_roll(&clouds[i], &seed);
        clouds[i].along = -CLOUD_FAR + 2 * CLOUD_FAR * (i + hrand(&seed)) / CLOUDS;
        clouds[i].phase = clouds[i].period * hrand(&seed);
    }
    /* Star patches round the sky: a ring low, a ring high, one overhead,
       each turned its own way so the pattern never repeats. */
    seed = 777;
    for(int i = 0; i < STARS; i++) {
        float a, el;
        if(i < 8) {
            a = ENG_TAU * (i + 0.5f * hrand(&seed)) / 8;
            el = 0.22f + 0.12f * hrand(&seed);
        }
        else if(i < 13) {
            a = ENG_TAU * (i - 8 + 0.5f * hrand(&seed)) / 5;
            el = 0.62f + 0.15f * hrand(&seed);
        }
        else {
            a = 0;
            el = 1.5f;
        }
        star_dir[i] = v3(sinf(a) * cosf(el), sinf(el), cosf(a) * cosf(el));
        star_rot[i] = ENG_TAU * hrand(&seed);
        star_b[i] = 0.75f + 0.25f * hrand(&seed);
    }
    sky_colours();
}

/* The ranges go round the camera in arcs, so those behind it are skipped
   whole (a ring in one strip had half its triangles across the near
   plane, each clipped on the CPU). */
#define RANGE_ARC 6   /* segments an arc */
void world_draw_sky(void) {
    V3 c = r_cam.pos;
    sky_draw(&sky);
    r_hdr(h_flat_nofog);
    V3 f = r_cam.fwd;
    float fl = sqrtf(f.x * f.x + f.z * f.z);
    float fx = fl > 0.01f ? f.x / fl : 0, fz = fl > 0.01f ? f.z / fl : 0;
    /* The ranges, furthest first (the PVR sorts opaque depth anyway). */
    for(int r = RANGES - 1; r >= 0; r--) {
        float rad = 1050.0f + 260.0f * r;
        for(int a0 = 0; a0 < RANGE_SEG; a0 += RANGE_ARC) {
            /* Off the screen: the arc's middle more than 78 degrees off the
               view's heading (its near end 55 out, past the view's ~40 even
               at the corners). */
            int mid = a0 + RANGE_ARC / 2;
            if(fl > 0.01f && range_sa[mid][r] * fx + range_ca[mid][r] * fz < 0.2f) continue;
            RV v[2 * (RANGE_ARC + 1)];
            int n = 0;
            for(int i = a0; i <= a0 + RANGE_ARC; i++) {
                float sa = range_sa[i][r], ca = range_ca[i][r];
                int g = (int)(glow_at(sa, ca) * 256.0f);
                v[n++] = (RV){c.x + sa * rad, range_h[r][i] - 40.0f + c.y * 0.15f, c.z + ca * rad, 0, 0,
                              mix256(range_top[r][0], range_top[r][1], g) | 0xff000000u};
                v[n++] = (RV){c.x + sa * rad, -60.0f, c.z + ca * rad, 0, 0, mix256(range_bot[r][0], range_bot[r][1], g) | 0xff000000u};
            }
            r_strip(v, n);
        }
    }
}

/* ---- Grass tufts near the camera ---- */

/* Tufts sit on a jittered 2 m grid, thinned into clumps by a slow noise
   so the meadow has lush patches and bare ones. Every tuft is drawn out
   to TUFT_NEAR; past it every other one carries on, growing a little to
   fill the gap, out to TUFT_FAR, the rest shrinking away as they near
   TUFT_NEAR (the eye reads it as the same meadow, as with far trees).
   Working out a tuft (surface, water, height) is a handful of cache
   misses into the level, so tufts are worked out once per 8 m block and
   kept while the block stays near: a frame only reads them back. Each
   tuft's lit colour is read from the ground as it is drawn, so they
   follow the time of day. */
#define TUFT_CELL 2.0f
#define TUFT_NEAR 24.0f
#define TUFT_FAR 44.0f
#define TUFT_MAX 560
#define TB_CELLS 4                          /* cells a block side */
#define TB_SIZE (TUFT_CELL * TB_CELLS)
#define TB_RAD (TB_SIZE * 0.7072f + 1.2f)   /* a block's tufts lie within this of its centre */
#define TB_SLOTS 16                         /* cached blocks a side; > 2 * TUFT_FAR / TB_SIZE + 2 */
#define TB_FILLS 10                         /* blocks worked out a frame at most (after a camera cut) */
#define TB_EMPTY INT16_MIN

typedef struct {
    float x, y, z;
    uint32_t g;          /* the lvl_ground vertex it takes its colour from */
    uint8_t var;         /* which of the four tuft textures */
    uint8_t flags;       /* 1: mirrored, 2: carries on past TUFT_NEAR */
    uint8_t size;        /* half width, cm */
    uint8_t tint;        /* brightness, 0.8 + tint / 850 */
} Tuft;

typedef struct {
    int16_t bi, bj;
    uint8_t n;
    Tuft t[TB_CELLS * TB_CELLS];
} TuftBlock;

static TuftBlock tblocks[TB_SLOTS * TB_SLOTS];
static float tuft_wind_t;
int world_tufts;   /* drawn last frame, for the bench */

/* Height of each texture over the half width: tall seed grass, a low
   wide spray, the broad clumps in between. */
static const float tuft_aspect[4] = {1.25f, 1.6f, 1.0f, 1.3f};

/* Tufts sit on a fixed grid, so one under a truck would poke up through
   its bed: leave out any whose billboard (half width w, mostly blades)
   reaches under a truck's hull footprint. */
static V3 tuft_hull[MAX_TRUCKS];

static bool under_truck(float px, float pz, float w) {
    for(int i = 0; i < game.ntrucks; i++) {
        const Truck *t = &trucks[i];
        float dx = px - tuft_hull[i].x, dz = pz - tuft_hull[i].z;
        if(dx * dx + dz * dz > 64.0f) continue;
        if(fabsf(dx * t->ax[0].x + dz * t->ax[0].z) < t->spec.hull_h[0] + w &&
           fabsf(dx * t->ax[2].x + dz * t->ax[2].z) < t->spec.hull_h[2] + w)
            return true;
    }
    return false;
}

/* Value noise on a 14 m lattice, 0..1: where the grass grows thick. */
static float tuft_patch(float x, float z) {
    float gx = x * (1.0f / 14.0f), gz = z * (1.0f / 14.0f);
    int i = (int)floorf(gx), j = (int)floorf(gz);
    float fx = gx - i, fz = gz - j;
    fx = fx * fx * (3 - 2 * fx);
    fz = fz * fz * (3 - 2 * fz);
#define TP(a, b) ((hash32((uint32_t)(a) * 2654435761u ^ (uint32_t)(b) * 40503u ^ 0x5bd1e995u) & 1023) * (1.0f / 1023))
    float a = TP(i, j), b = TP(i + 1, j), c = TP(i, j + 1), d = TP(i + 1, j + 1);
#undef TP
    return a + (b - a) * fx + (c - a) * fz + (a - b - c + d) * fx * fz;
}

static void tuft_fill(TuftBlock *b, int bi, int bj) {
    b->bi = (int16_t)bi;
    b->bj = (int16_t)bj;
    b->n = 0;
    for(int cj = 0; cj < TB_CELLS; cj++)
        for(int ci = 0; ci < TB_CELLS; ci++) {
            int i = bi * TB_CELLS + ci, j = bj * TB_CELLS + cj;
            uint32_t s = hash32((uint32_t)i * 73856093u ^ (uint32_t)j * 19349663u);
            float px = (i + (s & 255) / 255.0f) * TUFT_CELL, pz = (j + ((s >> 8) & 255) / 255.0f) * TUFT_CELL;
            int sf = surface_at(px, pz);
            if(sf != SURF_GRASS && !(sf == SURF_FOREST && (s >> 20 & 3) == 0)) continue;
            if(water_at(px, pz) > -999.0f) continue;
            uint32_t s2 = hash32(s ^ 0x68e31da4u);
            float y = terrain_height(px, pz);
            /* Up the mountain the grass thins and stays short: from the
               meadow's edge (30 m) to near the scree (160 m) a tuft
               grows a third as often and a third smaller. */
            float alp = clampf((y - 30.0f) * (1.0f / 130.0f), 0, 1);
            alp = alp * alp * (3 - 2 * alp);
            float dens = tuft_patch(px, pz) * (1.0f - 0.35f * alp);
            if((s2 & 255) * (1.0f / 255) > (0.4f + dens) * (1.0f - 0.55f * alp)) continue;
            Tuft *t = &b->t[b->n++];
            t->x = px;
            t->y = y - 0.05f;
            t->z = pz;
            int gi = lvl_clampi((int)((px - LEVEL_ORG) / LEVEL_D + 0.5f)), gj = lvl_clampi((int)((pz - LEVEL_ORG) / LEVEL_D + 0.5f));
            t->g = (uint32_t)(gj * LEVEL_N + gi);
            static const uint8_t var_pick[8] = {0, 0, 0, 1, 1, 2, 2, 3};
            t->var = var_pick[s2 >> 8 & 7];
            t->flags = (uint8_t)((s >> 24 & 1) | (s2 >> 10 & 2));
            float w = (0.55f + ((s >> 16) & 15) / 30.0f) * (0.8f + 0.35f * dens) * (1.0f - 0.3f * alp);
            t->size = (uint8_t)fminf(w * 100.0f, 255.0f);
            t->tint = (uint8_t)(s2 >> 16);
        }
}

typedef struct {
    float d2;
    int16_t bi, bj;
} TuftDraw;

#define TUFT_BUF 192
static RV tuft_v[4][TUFT_BUF + 6];
static int tuft_n[4];
static const pvr_poly_hdr_t *h_tufts[4];

static void tuft_flush(int k) {
    if(!tuft_n[k]) return;
    r_hdr(h_tufts[k]);
    r_strip(tuft_v[k], tuft_n[k]);
    tuft_n[k] = 0;
}

void world_draw_pt(void) {
    for(int i = 0; i < game.ntrucks; i++) {
        const float *hc = trucks[i].spec.hull_c;
        tuft_hull[i] = truck_to_world(&trucks[i], v3(hc[0], hc[1], hc[2]));
    }
    V3 c = r_cam.pos;
    float tx, ty;
    r_frustum_slopes(&tx, &ty);
    float sec = sqrtf(1.0f + tx * tx);
    V3 rt = vnorm(v3(r_cam.right.x, 0, r_cam.right.z));

    /* The blocks in reach and in view, nearest first, so a full buffer
       drops the furthest tufts. */
    static TuftDraw vis[TB_SLOTS * TB_SLOTS];
    int nvis = 0, fills = 0;
    int bi0 = (int)floorf((c.x - TUFT_FAR) / TB_SIZE), bi1 = (int)floorf((c.x + TUFT_FAR) / TB_SIZE);
    int bj0 = (int)floorf((c.z - TUFT_FAR) / TB_SIZE), bj1 = (int)floorf((c.z + TUFT_FAR) / TB_SIZE);
    for(int bj = bj0; bj <= bj1; bj++)
        for(int bi = bi0; bi <= bi1; bi++) {
            float dx = (bi + 0.5f) * TB_SIZE - c.x, dz = (bj + 0.5f) * TB_SIZE - c.z;
            float d2 = dx * dx + dz * dz;
            if(d2 > (TUFT_FAR + TB_RAD) * (TUFT_FAR + TB_RAD)) continue;
            float fz = dx * r_cam.fwd.x + dz * r_cam.fwd.z;
            float fx = dx * r_cam.right.x + dz * r_cam.right.z;
            if(fz < -TB_RAD || fabsf(fx) > fz * tx + TB_RAD * sec) continue;
            int k = nvis++;
            while(k > 0 && vis[k - 1].d2 > d2) {
                vis[k] = vis[k - 1];
                k--;
            }
            vis[k] = (TuftDraw){d2, (int16_t)bi, (int16_t)bj};
        }
    int count = 0;
    float sway_t = tuft_wind_t;
    for(int v = 0; v < nvis && count < TUFT_MAX; v++) {
        int bi = vis[v].bi, bj = vis[v].bj;
        TuftBlock *b = &tblocks[(bj & (TB_SLOTS - 1)) * TB_SLOTS + (bi & (TB_SLOTS - 1))];
        /* New blocks are worked out nearest first, a few a frame; the
           rest wait (they only pile up after a camera cut). */
        if(b->bi != bi || b->bj != bj) {
            if(fills >= TB_FILLS) continue;
            tuft_fill(b, bi, bj);
            fills++;
        }
        if(!b->n) continue;
        float bx = (bi + 0.5f) * TB_SIZE, bz = (bj + 0.5f) * TB_SIZE;
        bool near_truck = false;
        for(int i = 0; i < game.ntrucks; i++) {
            float dx = bx - tuft_hull[i].x, dz = bz - tuft_hull[i].z;
            if(dx * dx + dz * dz < (TB_RAD + 8.0f) * (TB_RAD + 8.0f)) near_truck = true;
        }
        for(int k = 0; k < b->n && count < TUFT_MAX; k++) {
            const Tuft *t = &b->t[k];
            float dx = t->x - c.x, dz = t->z - c.z, d2 = dx * dx + dz * dz;
            bool far = t->flags & 2;
            if(d2 > (far ? TUFT_FAR * TUFT_FAR : TUFT_NEAR * TUFT_NEAR)) continue;
            /* In view? (by the vertical frustum slope only roughly) */
            float fz = dx * r_cam.fwd.x + dz * r_cam.fwd.z;
            float fx = dx * r_cam.right.x + dz * r_cam.right.z;
            if(fz < -1.0f || fabsf(fx) > fz * tx + 2.0f) continue;
            float d = sqrtf(d2), w = t->size * 0.01f;
            if(far) {
                w *= (1.0f + 0.45f * clampf((d - (TUFT_NEAR - 8.0f)) * (1.0f / 8.0f), 0, 1)) *
                     fminf(1.0f, (TUFT_FAR - d) * (1.0f / 8.0f));
            }
            else w *= fminf(1.0f, (TUFT_NEAR - d) * (1.0f / 8.0f));
            if(near_truck && under_truck(t->x, t->z, w * 0.6f)) continue;
            float h = w * tuft_aspect[t->var];
            uint32_t col = color_scale(lvl_ground[t->g].c, 1.35f * (0.8f + t->tint * (1.0f / 850)));
            uint32_t root = color_scale(col, 0.7f);
            /* The tips lean downwind and back, a slow gust rolling across
               the meadow with a quicker flutter of each tuft's own. */
            float along = t->x * WIND_X + t->z * WIND_Z;
            float lean = h * (0.07f + 0.06f * sinf(sway_t * 1.3f - along * 0.11f) +
                              0.035f * sinf(sway_t * 3.1f + (float)(t->tint & 63)));
            float u0 = (t->flags & 1) ? 0.0f : 1.0f, u1 = 1.0f - u0;
            V3 base = v3(t->x, t->y, t->z);
            V3 l = vmad(base, rt, -w), r = vmad(base, rt, w);
            V3 top = v3(WIND_X * lean, h, WIND_Z * lean);
            int bk = t->var;
            RV *o = tuft_v[bk] + tuft_n[bk];
            if(tuft_n[bk]) {
                o[0] = o[-1];
                o[1] = (RV){l.x + top.x, l.y + top.y, l.z + top.z, u0, 0, col};
                o += 2;
            }
            o[0] = (RV){l.x + top.x, l.y + top.y, l.z + top.z, u0, 0, col};
            o[1] = (RV){r.x + top.x, r.y + top.y, r.z + top.z, u1, 0, col};
            o[2] = (RV){l.x, l.y, l.z, u0, 1, root};
            o[3] = (RV){r.x, r.y, r.z, u1, 1, root};
            tuft_n[bk] = (int)(o + 4 - tuft_v[bk]);
            count++;
            if(tuft_n[bk] > TUFT_BUF) tuft_flush(bk);
        }
    }
    for(int k = 0; k < 4; k++) tuft_flush(k);
    world_tufts = count;
}

/* ---- Sun, clouds and flare ---- */

static float flare_vis;   /* 0..1: how clearly the camera sees the sun */

static bool sun_occluded(void) {
    V3 p = r_cam.pos;
    for(int k = 1; k <= 30; k++) {
        float t = k * k * 0.9f + 4.0f;
        V3 q = vmad(p, tod.sun, t);
        if(fabsf(q.x) > 383 || fabsf(q.z) > 383) break;
        if(terrain_height(q.x, q.z) > q.y) return true;
    }
    return false;
}

float world_mist(void) {
    return cap_in;
}

float world_fog_far(void) {
    return fog_far_m;
}

uint32_t world_lit(uint32_t albedo, V3 n) {
    return lit(albedo, n, 1.0f);
}

void world_update(float dt) {
    haze_update();
    /* Whether the hills hide the sun: a march over the terrain, every
       fourth frame (the flare eases in and out over a quarter second). */
    static float flare_target;
    if((atmo_tick & 3) == 1) flare_target = tod.sun_el > 0 && cap_in < 0.3f && !sun_occluded() ? 1.0f : 0.0f;
    flare_vis = approach(flare_vis, flare_target, dt * 4.0f);
    /* The dome and the ranges change over seconds: every fourth frame. */
    if(!(atmo_tick++ & 3)) sky_colours();
    /* The clouds blow downwind; one that has gone over the far side comes
       back round upwind as another cloud. */
    cloud_t += dt;
    tuft_wind_t += dt;
    static uint32_t cloud_seed = 9001;
    for(int i = 0; i < CLOUDS; i++) {
        Cloud *c = &clouds[i];
        c->along += WIND_SPEED * dt;
        if(c->along > CLOUD_FAR) {
            cloud_roll(c, &cloud_seed);
            c->along -= 2 * CLOUD_FAR;
            c->phase = -cloud_t;   /* arrives forming */
        }
    }
    /* The fog and the clear colour follow the haze (warmed toward the
       sun, gone to cloud in the cap); the fog table is rewritten only
       when its distances move. Deep in the cap the far distance closes
       in by the same share at each step, so the thinning reads evenly. */
    static float fog_near, fog_far;
    float r = ((fog_col >> 16) & 255) / 255.0f, g = ((fog_col >> 8) & 255) / 255.0f, b = (fog_col & 255) / 255.0f;
    pvr_fog_table_color(1.0f, r, g, b);
    pvr_set_bg_color(r, g, b);
    float want_near = lerpf(pal->fog_near, 2.0f, cap_in), want_far = pal->fog_far * powf(CAP_FOG_FAR / pal->fog_far, cap_in);
    if(fabsf(fog_near - want_near) > 0.5f || fabsf(fog_far - want_far) > 0.5f) {
        fog_near = want_near;
        fog_far = want_far;
        fog_near_m = fog_near;
        fog_far_m = fog_far;
        pvr_fog_table_linear(fog_near, fog_far);
    }
}

/* A disc in the sky fades out over the last few degrees before it sets. */
static float horizon_alpha(float el) {
    return clampf((el + 0.06f) / 0.06f, 0, 1);
}

/* Slow value noise round the collar: where the mist is thick and where
   it thins, drifting round with the wind. */
static float cap_noise(float x) {
    int i = (int)x;   /* x >= 0 */
    float f = x - i;
    f = f * f * (3 - 2 * f);
    float a = cap_noise_tab[i & (CAP_NOISE - 1)], b = cap_noise_tab[(i + 1) & (CAP_NOISE - 1)];
    return a + (b - a) * f;
}

/* The collar, seen from outside the cap: lit on top and shaded under
   (warmed on the sun's side), thinning near the camera (inside, the fog
   takes over, and big translucent sheets are what costs the PVR) and
   fogged by hand like the hills behind it (the PVR's fog would fog its
   clear edges too). */
static uint32_t cap_c[CAP_RINGS][CAP_SEG + 1];

/* Lit and fogged a segment at a time, from the thick middle of the
   collar (the rings are within metres of each other). */
static void cap_colours(uint32_t shade, float out) {
    V3 c = r_cam.pos;
    float fog_k = fog_far_m > fog_near_m ? 128.0f / (fog_far_m - fog_near_m) : 0;
    float drift = cloud_t * 0.035f;
    float inv_gk = 1.0f / fmaxf(pal->glow_k, 0.01f);
    for(int i = 0; i <= CAP_SEG; i++) {
        int seg = i == CAP_SEG ? 0 : i;   /* the seam: the same mist both sides */
        float n1 = 0.25f + 0.75f * cap_noise(seg * 0.45f + drift), n2 = 0.25f + 0.75f * cap_noise(seg * 0.45f + drift + 17.3f);
        V3 p = cap_v[1][i];
        float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float a = out * clampf((d - 12.0f) * (1.0f / 30.0f), 0, 1);
        int fog = (int)clampf((d - fog_near_m) * fog_k, 0, 128);   /* half fogged: a cloud stays brighter than the hills */
        float g = clampf(glow_at(cap_sa[i], cap_ca[i]) * inv_gk, 0, 1);
        uint32_t under = mix256(shade, pal->glow_col, (int)(256.0f * clampf(pal->glow_k * (0.15f + 0.75f * g), 0, 1)));
        uint32_t top = mix256(pal->cloud_col, fog_col, fog) & 0xffffffu;
        under = mix256(under, fog_col, fog) & 0xffffffu;
        cap_c[0][i] = top;
        cap_c[1][i] = top | (uint32_t)(int)(255.0f * a * n1) << 24;
        cap_c[2][i] = under | (uint32_t)(int)(255.0f * a * n2) << 24;
        cap_c[3][i] = under;
    }
}

static void cap_draw(uint32_t shade) {
    static bool fresh;
    float out = (1.0f - cap_in) * cap_k;
    if(out < 0.02f || !cap_built ||
       !r_sphere_visible(v3(LEVEL_SUMMIT_X, (CAP_BASE + CAP_TOP) * 0.5f, LEVEL_SUMMIT_Z), CAP_R + 20.0f)) {
        fresh = false;
        return;
    }
    /* Its colours drift slowly too: every fourth frame, or as it comes
       back into view. */
    if((atmo_tick & 3) == 2 || !fresh) cap_colours(shade, out);
    fresh = true;
    r_hdr(h_cap);
    for(int k = 0; k + 1 < CAP_RINGS; k++) {
        RV v[2 * (CAP_SEG + 1)];
        int n = 0;
        for(int i = 0; i <= CAP_SEG; i++) {
            V3 a = cap_v[k][i], b = cap_v[k + 1][i];
            v[n++] = (RV){a.x, a.y, a.z, 0, 0, cap_c[k][i]};
            v[n++] = (RV){b.x, b.y, b.z, 0, 0, cap_c[k + 1][i]};
        }
        r_strip(v, n);
    }
}

/* ---- Valley mist ----

   At dawn, and less at dusk and by moonlight, mist lies over the low
   ground: the meadow and the pond, the east meadow, the boulder field, up
   the creek's valley and on the lake. Each patch is a soft sheet a couple
   of metres up, drifting with the wind, cut into a coarse grid so the
   cells about the camera are never drawn (a translucent sheet filling the
   screen is what costs the PVR; driving into the mist, the fog does the
   rest). Fogged by hand, as the PVR's fog would fog its clear texels. */
#define MIST_N 6              /* cells a side, near */
#define MIST_FAR_N 3          /* and far off */
typedef struct { float x, z, r, y; } Mist;
static Mist mists[] = {
    {-40, -255, 55, 0}, {20, -228, 55, 0}, {140, -210, 50, 0}, {-130, -250, 45, 0},
    {-70, -180, 40, 0}, {-105, -100, 40, 0}, {-115, -20, 40, 0}, {-42, 66, 38, 0},
};
#define MISTS (int)(sizeof(mists) / sizeof(mists[0]))
static const pvr_poly_hdr_t *h_mist;
/* A sheet's soft round edge, for each grid: the same for every sheet. */
static float mist_edge[MIST_N + 1][MIST_N + 1], mist_edge_far[MIST_FAR_N + 1][MIST_FAR_N + 1];

static float mist_amount(float hour) {
    static const float k[][2] = {{0, 0.3f}, {4.5f, 0.55f}, {5.5f, 1.0f}, {7.8f, 1.0f}, {9.5f, 0},
                                 {18.0f, 0}, {19.5f, 0.6f}, {21.5f, 0.35f}, {24.0f, 0.3f}};
    int i = 0;
    while(hour > k[i + 1][0]) i++;
    float t = (hour - k[i][0]) / (k[i + 1][0] - k[i][0]);
    t = t * t * (3 - 2 * t);
    return k[i][1] + (k[i + 1][1] - k[i][1]) * t;
}

/* Each sheet lies a little over the lowest ground near its middle. */
static void mist_build(void) {
    for(int m = 0; m < MISTS; m++) {
        Mist *s = &mists[m];
        float lo = 1e9f;
        for(int k = 0; k < 9; k++) {
            float a = ENG_TAU * k / 8, r = k ? s->r * 0.35f : 0;
            lo = fminf(lo, terrain_height(s->x + sinf(a) * r, s->z + cosf(a) * r));
        }
        float w = water_at(s->x, s->z);
        if(w > lo) lo = w;
        s->y = lo + 2.2f;
    }
    for(int j = 0; j <= MIST_N; j++)
        for(int i = 0; i <= MIST_N; i++) {
            float ex = 2.0f * i / MIST_N - 1.0f, ez = 2.0f * j / MIST_N - 1.0f;
            mist_edge[j][i] = clampf(1.0f - (ex * ex + ez * ez), 0, 1);
        }
    for(int j = 0; j <= MIST_FAR_N; j++)
        for(int i = 0; i <= MIST_FAR_N; i++) {
            float ex = 2.0f * i / MIST_FAR_N - 1.0f, ez = 2.0f * j / MIST_FAR_N - 1.0f;
            mist_edge_far[j][i] = clampf(1.0f - 0.8f * (ex * ex + ez * ez), 0, 1);
        }
}

/* One sheet as a grid of n cells a side: alpha a[j * (n + 1) + i], below
   zero for a corner behind the camera. A run of cells with mist on all
   four corners goes in one strip; any other cell ends it. */
static void mist_sheet(const Mist *s, int n, const float *a, uint32_t col, float u0, float v0) {
    float cell = 2.0f * s->r / n, x0 = s->x - s->r, z0 = s->z - s->r, uk = cell * (1.0f / 30.0f);
    int w = n + 1;
    for(int j = 0; j < n; j++) {
        RV v[2 * (MIST_N + 1)];
        int k = 0;
        float za = z0 + j * cell, zb = za + cell, va = j * uk + v0, vb = va + uk;
        for(int i = 0; i <= n; i++) {
            const float *r0 = a + j * w, *r1 = r0 + w;
            bool lr = i < n && r0[i] >= 0 && r0[i + 1] >= 0 && r1[i] >= 0 && r1[i + 1] >= 0 &&
                      r0[i] + r0[i + 1] + r1[i] + r1[i + 1] > 0.02f;
            bool ll = i > 0 && r0[i - 1] >= 0 && r0[i] >= 0 && r1[i - 1] >= 0 && r1[i] >= 0 &&
                      r0[i - 1] + r0[i] + r1[i - 1] + r1[i] > 0.02f;
            if(!lr && !ll) {
                if(k >= 4) r_strip(v, k);
                k = 0;
                continue;
            }
            float x = x0 + i * cell, u = i * uk + u0;
            v[k++] = (RV){x, s->y, zb, u, vb, color_alpha(col, r1[i])};
            v[k++] = (RV){x, s->y, za, u, va, color_alpha(col, r0[i])};
        }
        if(k >= 4) r_strip(v, k);
    }
}

static void mist_draw(void) {
    float k = mist_amount(tod.hour) * (1.0f - cap_in);
    if(k < 0.02f) return;
    k *= 0.85f;
    V3 c = r_cam.pos, f = r_cam.fwd;
    uint32_t col = mix256(fog_col, pal->cloud_col, 110) & 0xffffffu;
    float fog_k = fog_far_m > fog_near_m ? 128.0f / (fog_far_m - fog_near_m) : 0;
    float drift = cloud_t * 0.25f;   /* metres downwind */
    float u0 = -(drift * WIND_X) * (1.0f / 30.0f), v0 = -(drift * WIND_Z) * (1.0f / 30.0f);
    u0 -= floorf(u0);
    v0 -= floorf(v0);
    r_hdr(h_mist);
    for(int m = 0; m < MISTS; m++) {
        const Mist *s = &mists[m];
        V3 sc = v3(s->x, s->y, s->z);
        float sd = vlen(vsub(sc, c));
        if(sd > 450.0f || !r_sphere_visible(sc, s->r)) continue;
        /* Fogged as a whole, by its middle's distance. */
        uint32_t sheet_col = mix256(col, fog_col, (int)clampf((sd - fog_near_m) * fog_k, 0, 128)) & 0xffffffu;
        if(sd - s->r > 50.0f) {
            /* Far off: a coarse grid, the edge fade alone. */
            float a[(MIST_FAR_N + 1) * (MIST_FAR_N + 1)];
            for(int q = 0; q < (MIST_FAR_N + 1) * (MIST_FAR_N + 1); q++) a[q] = k * (&mist_edge_far[0][0])[q];
            mist_sheet(s, MIST_FAR_N, a, sheet_col, u0, v0);
            continue;
        }
        /* Near: clear about the camera, and no corner behind it (that
           cell would be clipped on the CPU, for mist the clear hole about
           the camera hides anyway). */
        float a[(MIST_N + 1) * (MIST_N + 1)];
        float cell = 2.0f * s->r / MIST_N, dy = s->y - c.y;
        for(int j = 0; j <= MIST_N; j++) {
            float dz = s->z - s->r + j * cell - c.z;
            for(int i = 0; i <= MIST_N; i++) {
                float dx = s->x - s->r + i * cell - c.x, d2 = dx * dx + dy * dy + dz * dz;
                float hole = clampf((d2 - 14.0f * 14.0f) * (1.0f / (32.0f * 32.0f - 14.0f * 14.0f)), 0, 1);
                a[j * (MIST_N + 1) + i] = dx * f.x + dy * f.y + dz * f.z < 3.0f ? -1.0f : k * mist_edge[j][i] * hole;
            }
        }
        mist_sheet(s, MIST_N, a, sheet_col, u0, v0);
    }
}

void world_draw_tr(void) {
    water_draw(&scenery);
    mist_draw();
    V3 c = r_cam.pos;
    /* Stars, once the sky is dark enough; only the patches ahead. */
    float clear = 1.0f - cap_in;
    if(pal->stars * clear > 0.02f) {
        r_hdr(h_stars);
        for(int i = 0; i < STARS; i++) {
            V3 d = star_dir[i];
            if(vdot(d, r_cam.fwd) < -0.1f) continue;
            float tw = 0.88f + 0.12f * sinf(game.time * 1.9f + i * 1.7f);
            r_billboard(vmad(c, d, 1850.0f), 560.0f, 560.0f, star_rot[i], 0, 0, 1, 1,
                        color_alpha(0xffffffff, pal->stars * clear * star_b[i] * tw));
        }
    }
    /* Clouds, far to near so they overlap right: each an upright quad
       facing the camera, lit on top by the palette and shaded underneath,
       the underside taking the sunrise or sunset glow on the sun's side.
       Fewer, fainter by night. */
    int order[CLOUDS];
    float dist[CLOUDS];
    for(int i = 0; i < CLOUDS; i++) {
        const Cloud *cl = &clouds[i];
        float d2 = cl->along * cl->along + cl->across * cl->across;
        int k = i;
        while(k > 0 && dist[k - 1] < d2) {
            order[k] = order[k - 1];
            dist[k] = dist[k - 1];
            k--;
        }
        order[k] = i;
        dist[k] = d2;
    }
    float cover = 0.92f * (0.55f + 0.45f * (1.0f - pal->dark)) * clear;
    uint32_t shade = color_scale(pal->cloud_col, 0.72f);
    r_hdr(h_cloud);
    for(int k = 0; k < CLOUDS; k++) {
        const Cloud *cl = &clouds[order[k]];
        float cyc = (cloud_t + cl->phase) / cl->period;
        cyc -= floorf(cyc);
        float a = clampf(sinf(ENG_PI * cyc) * 1.7f, 0, 1) * cover;
        if(a < 0.02f) continue;
        float dx = cl->along * WIND_X - cl->across * WIND_Z, dz = cl->along * WIND_Z + cl->across * WIND_X;
        V3 p = v3(c.x + dx, cl->y, c.z + dz);
        float inv = 1.0f / sqrtf(dx * dx + dz * dz);
        V3 rt = v3(dz * inv, 0, -dx * inv);
        float toward = 0.5f + 0.5f * cosf(atan2f(dx, dz) - tod.glow_az);
        uint32_t top = color_alpha(pal->cloud_col, a);
        uint32_t base = color_alpha(color_lerp(shade, pal->glow_col, clampf(pal->glow_k * (0.15f + 0.75f * toward), 0, 1)), a);
        float w = cl->w, h = cl->w * 0.42f;
        float u0 = cl->shape * 0.25f, u1 = u0 + 0.25f;
        V3 l = vmad(p, rt, -w), r = vmad(p, rt, w);
        RV q[4] = {{l.x, l.y + h, l.z, u0, 0, top}, {r.x, r.y + h, r.z, u1, 0, top},
                   {l.x, l.y - h, l.z, u0, 1, base}, {r.x, r.y - h, r.z, u1, 1, base}};
        r_strip(q, 4);
    }
    cap_draw(shade);
    /* The moon: a disc, faint by day, with a glow at night. */
    float ma = horizon_alpha(tod.moon_el) * (1.0f - 0.85f * cap_in);
    if(ma > 0) {
        V3 mp = vmad(c, tod.moon, 1600.0f);
        if(pal->dark > 0.05f) {
            r_hdr(h_glow_add);
            r_billboard(mp, 220.0f, 220.0f, 0, 0, 0, 1, 1, color_alpha(0xff8090b8, 0.3f * pal->dark * ma));
        }
        r_hdr(h_moon);
        r_billboard(mp, 44.0f, 44.0f, 0, 0, 0, 1, 1, color_alpha(0xffffffff, ma * (0.35f + 0.65f * pal->dark)));
    }
    /* The sun: a big flat disc with a soft glow. */
    float sa = horizon_alpha(tod.sun_el) * (1.0f - 0.8f * cap_in);
    if(sa > 0) {
        V3 sp = vmad(c, tod.sun, 1600.0f);
        r_hdr(h_glow_add);
        r_billboard(sp, 320.0f, 320.0f, 0, 0, 0, 1, 1, color_alpha(pal->sun_col, 0.375f * sa));
        r_hdr(h_sun);
        r_billboard(sp, 62.0f, 62.0f, 0, 0, 0, 1, 1, color_alpha(pal->sun_col, sa));
    }
}

void world_draw_flare(void) {
    if(flare_vis <= 0.01f) return;
    float sx, sy;
    if(!sky_sun_screen(tod.sun, 1600.0f, &sx, &sy)) return;
    float facing = vdot(r_cam.fwd, tod.sun);
    if(facing < 0.5f) return;
    /* Fades as the sun leaves the screen. */
    float edge = fminf(fminf(sx + 40.0f, 680.0f - sx), fminf(sy + 40.0f, 520.0f - sy));
    float k = flare_vis * clampf((facing - 0.5f) / 0.4f, 0, 1) * clampf(edge / 80.0f, 0, 1);
    if(k <= 0.01f) return;
    /* A wash over the screen when looking into the sun. */
    r_hdr(h_glow_add);
    r_quad2d(sx - 260, sy - 260, sx + 260, sy + 260, 0, 0, 1, 1, color_alpha(0xffffe8c0, 0.35f * k), 30.0f);
    /* The ring texture's left half is a disc, its right a hexagon. */
    static const SkyGhost ghosts[] = {
        {0.32f, 26.0f, 0xffffc890, 0, 0.5f}, {0.55f, 14.0f, 0xff9fe0d0, 0.5f, 1}, {0.78f, 40.0f, 0xffffb0c8, 0, 0.5f},
        {1.15f, 22.0f, 0xffc8e0ff, 0.5f, 1}, {1.42f, 60.0f, 0xffffe0a0, 0, 0.5f}, {1.8f, 18.0f, 0xffa0ffd8, 0.5f, 1},
    };
    sky_flare(sx, sy, h_ring_add, ghosts, 6, 0.33f * k, 30.0f);
}

/* ---- Building ---- */

void world_init(void) {
    h_flat = r_header(TEX_NONE, HF_OP);
    h_flat_nofog = r_header(TEX_NONE, HF_OP | HF_NOFOG);
    h_rock = r_header(TEX_ROCK, HF_OP);
    h_bark = r_header(TEX_BARK, HF_OP);
    h_crown = r_header(TEX_CROWN, HF_OP);
    for(int k = 0; k < 4; k++) h_tufts[k] = r_header(TEX_TUFT0 + k, HF_PT);
    h_sun = r_header(TEX_SUN, HF_TR | HF_NOFOG | HF_ADD);
    h_moon = r_header(TEX_MOON, HF_TR | HF_NOFOG);
    h_stars = r_header(TEX_STARS, HF_TR | HF_NOFOG | HF_ADD);
    h_glow_add = r_header(TEX_GLOW, HF_TR | HF_NOFOG | HF_ADD | HF_NOZTEST);
    h_ring_add = r_header(TEX_RING, HF_TR | HF_NOFOG | HF_ADD | HF_NOZTEST);
    h_cloud = r_header(TEX_CLOUD, HF_TR | HF_NOFOG);
    h_cap = r_header(TEX_NONE, HF_TR | HF_NOFOG);
    h_mist = r_header(TEX_MIST, HF_TR | HF_NOFOG);
    sky_setup();
    if(!sc_init(&scenery, 250000, 7000, 2400)) printf("Ridgeline world: no memory for the scenery\n");
}

/* Rocks, props and trees are grouped into 48 m cells. */
#define TCELL 48.0f
#define TCELLS 16

/* Trees: a cell picks its version by its middle's distance. Nearer than
   110 m each 24 m quarter of it picks the near tree (to 70 m) or the
   middle one by the quarter's middle, so no tree changes nearer than
   ~53 m, and the middle tree has the near one's three tiers. Past 110 m
   (the nearest tree ~76 m off) a tree is two cones, past 170 m every
   other tree is one cone, a little bigger, which the eye reads as the
   same forest. The trees' chunks sit past scenery.chunk_n, so the
   scenery's own walks (sc_draw, the water's) never visit them. */
#define TREE_NEAR_M 70.0f
#define TREE_SPLIT_M 110.0f
#define TREE_MID_M 170.0f
#define TREE_FAR_M 430.0f
#define TREE_NONE 0xffff
typedef struct {
    V3 lp;
    uint16_t quarter[4][2];   /* near, middle */
    uint16_t far[2];          /* two cones, one */
} TreeCell;
static TreeCell tcells[TCELLS * TCELLS];
static int tcell_n;

/* One chunk of trees into the open strip, as the near tree or the rest. */
static uint16_t tree_chunk(const uint16_t *order, int from, int to, float qx, float qz, int q, V3 lp, int lod) {
    static const float range[4][2] = {{0, TREE_NEAR_M}, {TREE_NEAR_M, 1000.0f}, {TREE_SPLIT_M, TREE_MID_M},
                                      {TREE_MID_M, TREE_FAR_M}};
    uint32_t at = scenery.chunk_n;
    sc_chunk_begin(&scenery, HF_OP, range[lod][0], range[lod][1], TAG_TREES);
    sc_chunk_lod_point(&scenery, lp);
    lamps_lod(lp, TREE_NEAR_M);
    sc_strip_begin(&scenery, lod ? h_flat : h_crown);
    bool firstt = true;
    for(int k = from; k < to; k++) {
        lvl_tree_t t = lvl_trees[order[k]];
        if(q >= 0 && q != ((t.x >= qx) | (t.z >= qz) << 1)) continue;
        if(lod == 3) {
            if(order[k] & 1) continue;
            t.scale *= 1.2f;
        }
        cone_lamps = lod == 0;
        tree(&t, lod, firstt);
        cone_lamps = false;
        firstt = false;
    }
    sc_strip_end(&scenery);
    sc_chunk_end(&scenery);
    return scenery.chunk_n > at ? (uint16_t)at : TREE_NONE;
}

static void trees_build(void) {
    /* Bucket the trees by cell. */
    static uint16_t order[LVL_TREES];
    static uint16_t first[TCELLS * TCELLS + 1];
    int cnt[TCELLS * TCELLS] = {0};
    for(int i = 0; i < LVL_TREES; i++) {
        int ci = clampf((lvl_trees[i].x - LEVEL_ORG) / TCELL, 0, TCELLS - 1), cj = clampf((lvl_trees[i].z - LEVEL_ORG) / TCELL, 0, TCELLS - 1);
        cnt[cj * TCELLS + ci]++;
    }
    first[0] = 0;
    for(int c = 0; c < TCELLS * TCELLS; c++) first[c + 1] = first[c] + cnt[c];
    memset(cnt, 0, sizeof(cnt));
    for(int i = 0; i < LVL_TREES; i++) {
        int ci = clampf((lvl_trees[i].x - LEVEL_ORG) / TCELL, 0, TCELLS - 1), cj = clampf((lvl_trees[i].z - LEVEL_ORG) / TCELL, 0, TCELLS - 1);
        int c = cj * TCELLS + ci;
        order[first[c] + cnt[c]++] = (uint16_t)i;
    }
    tcell_n = 0;
    for(int c = 0; c < TCELLS * TCELLS; c++) {
        if(first[c + 1] == first[c]) continue;
        TreeCell *tc = &tcells[tcell_n++];
        float mx = LEVEL_ORG + ((c % TCELLS) + 0.5f) * TCELL, mz = LEVEL_ORG + ((c / TCELLS) + 0.5f) * TCELL;
        tc->lp = v3(mx, terrain_height(mx, mz), mz);
        for(int q = 0; q < 4; q++) {
            float x = mx + ((q & 1) ? 0.25f : -0.25f) * TCELL, z = mz + ((q & 2) ? 0.25f : -0.25f) * TCELL;
            V3 lp = v3(x, terrain_height(x, z), z);
            for(int l = 0; l < 2; l++) tc->quarter[q][l] = tree_chunk(order, first[c], first[c + 1], mx, mz, q, lp, l);
        }
        for(int l = 0; l < 2; l++) tc->far[l] = tree_chunk(order, first[c], first[c + 1], mx, mz, -1, tc->lp, 2 + l);
    }
}

/* A tree chunk if it is in range and in view; headers only on a change. */
static void tree_draw_chunk(uint16_t i, const pvr_poly_hdr_t **last) {
    if(i == TREE_NONE) return;
    const ScChunk *c = &scenery.chunks[i];
    V3 d = vsub(c->lc, r_cam.pos);
    float d2 = vdot(d, d);
    if(d2 < c->near2 || d2 >= c->far2 || !r_sphere_visible(c->c, c->r)) return;
    const ScStrip *t = &scenery.strips[c->first_strip];
    for(int k = 0; k < c->nstrips; k++, t++) {
        if(t->hdr != *last) {
            r_hdr(t->hdr);
            *last = t->hdr;
        }
        r_strip_bounded(scenery.pool + t->first, t->count, t->c, t->r);
    }
}

static void trees_draw(void) {
    const pvr_poly_hdr_t *last = NULL;
    V3 cam = r_cam.pos;
    for(int i = 0; i < tcell_n; i++) {
        const TreeCell *tc = &tcells[i];
        V3 d = vsub(tc->lp, cam);
        float d2 = vdot(d, d);
        if(d2 < TREE_SPLIT_M * TREE_SPLIT_M) {
            for(int q = 0; q < 4; q++) {
                tree_draw_chunk(tc->quarter[q][0], &last);
                tree_draw_chunk(tc->quarter[q][1], &last);
            }
        }
        else if(d2 < TREE_FAR_M * TREE_FAR_M) {
            tree_draw_chunk(tc->far[0], &last);
            tree_draw_chunk(tc->far[1], &last);
        }
    }
}

void world_build(void) {
    for(int i = 0; i < TB_SLOTS * TB_SLOTS; i++) tblocks[i].bi = TB_EMPTY;
    sc_clear(&scenery);
    lamps_clear();
    glow_place();
    terrain_build(&scenery);
    int terrain_v = scenery.pool_n;
    world_lrec_first = lrec_marked = scenery.pool_n;
    alb_n = 0;
    if(!world_lrec) world_lrec = malloc(sizeof(LightRec) * (scenery.pool_max - scenery.pool_n));
    if(!world_lrec) printf("Ridgeline world: no memory to relight the scenery\n");
    cap_build();
    mist_build();
    /* Boulders, logs and ledges: near and far versions in one chunk per cell. */
    for(int c = 0; c < TCELLS * TCELLS; c++) {
        float x0 = LEVEL_ORG + (c % TCELLS) * TCELL, z0 = LEVEL_ORG + (c / TCELLS) * TCELL;
        V3 lp = v3(x0 + TCELL / 2, 0, z0 + TCELL / 2);
        lp.y = terrain_height(lp.x, lp.z);
        for(int lod = 0; lod < 2; lod++) {
            sc_chunk_begin(&scenery, HF_OP, lod ? 90.0f : 0.0f, lod ? 360.0f : 90.0f, TAG_ROCKS);
            sc_chunk_lod_point(&scenery, lp);
            lamps_lod(lp, 90.0f);
            for(int i = 0; i < LVL_SPHERES; i++) {
                const lvl_sphere_t *s = &lvl_spheres[i];
                if(s->x < x0 || s->x >= x0 + TCELL || s->z < z0 || s->z >= z0 + TCELL) continue;
                if(lod && s->r < 0.6f) continue;
                uint32_t f0 = scenery.pool_n;
                boulder(v3(s->x, s->y, s->z), s->r * 1.04f, (uint32_t)i * 2654435761u, lod);
                if(lod == 0) lamps_add(f0, scenery.pool_n);
            }
            for(int i = 0; i < LVL_CAPSULES; i++) {
                const lvl_capsule_t *cp = &lvl_capsules[i];
                float mx = (cp->ax + cp->bx) * 0.5f, mz = (cp->az + cp->bz) * 0.5f;
                if(mx < x0 || mx >= x0 + TCELL || mz < z0 || mz >= z0 + TCELL) continue;
                uint32_t f0 = scenery.pool_n;
                log_mesh(cp);
                if(lod == 0) lamps_add(f0, scenery.pool_n);
            }
            for(int i = 0; i < LVL_ROCK_BOXES; i++) {    /* the rest are props', drawn as the prop */
                const lvl_box_t *b = &lvl_boxes[i];
                if(b->x < x0 || b->x >= x0 + TCELL || b->z < z0 || b->z >= z0 + TCELL) continue;
                uint32_t f0 = scenery.pool_n;
                box(h_rock, v3(b->x, b->y, b->z), b->hx, b->hy, b->hz, b->yaw, 0xff8c9298, 0.35f);
                if(lod == 0) lamps_add(f0, scenery.pool_n);
            }
            sc_chunk_end(&scenery);
        }
    }
    lamps_lod(v3(0, 0, 0), 0);
    for(int i = 0; i < LVL_PROPS; i++) {
        sc_chunk_begin(&scenery, HF_OP, 0, 600.0f, TAG_PROPS);
        uint32_t f0 = scenery.pool_n;
        prop(&lvl_props[i]);
        lamps_add(f0, scenery.pool_n);
        sc_chunk_end(&scenery);
    }
    uint32_t tree_c = scenery.chunk_n, tree_p = scenery.pool_n;
    trees_build();
    lamps_index();
    printf("Ridgeline world: %lu vertices (terrain %d, trees %lu), %lu strips, %lu chunks%s\n",
           (unsigned long)scenery.pool_n, terrain_v, (unsigned long)(scenery.pool_n - tree_p),
           (unsigned long)scenery.strip_n, (unsigned long)scenery.chunk_n,
           scenery.full ? " - FULL, some scenery missing" : "");
    scenery.chunk_n = tree_c;
}

uint64_t world_us[3];

void world_draw_op(void) {
    PROF_BEGIN(pw);
    uint64_t t0 = timer_us_gettime64();
    terrain_draw_near();
#ifdef RL_AUTOTEST
    extern int ter_count[4];
    ter_count[3] += sc_draw(&scenery, HF_OP, 1u << TAG_TERRAIN);
    ter_count[2] += terrain_draw_far_skirts(&scenery);
#else
    sc_draw(&scenery, HF_OP, 1u << TAG_TERRAIN);
    terrain_draw_far_skirts(&scenery);
#endif
    PROF_LAP(PS_TERRAIN, pw);
    uint64_t t1 = timer_us_gettime64();
    trees_draw();
    sc_draw(&scenery, HF_OP, (1u << TAG_ROCKS) | (1u << TAG_PROPS));
    PROF_LAP(PS_TREES, pw);
    uint64_t t2 = timer_us_gettime64();
    world_us[0] += t1 - t0;
    world_us[1] += t2 - t1;
}
