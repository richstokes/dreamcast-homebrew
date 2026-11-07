/* Ridgeline world (world.h).

   The look: flat colour fields with soft painted shading. Nothing here is
   lit at run time; every vertex colour is baked when the scenery is built,
   from the scene's sun and sky, with a darker base and a lighter crown on
   each tree. Distance does the rest: the PVR's fog fades everything toward
   the haze colour, and the far ranges are flat silhouettes stepping lighter
   with distance. */
#include "world.h"
#include "engine/sky.h"
#include "terrain.h"
#include "truck.h"
#include "engine/prof.h"
#include <sh4zam/shz_sh4zam.h>

Scenery scenery;
V3 sun_dir;

/* Autumn: mustard gold, forest green, cool grey, pale sky. A late sun in
   the west-north-west, low enough to throw long shadows and backlight the
   trees when driving up the mountain. */
static const Palette autumn = {
    .sky_top = 0xff8fbcd8, .sky_horizon = 0xffdce9ee, .haze = 0xffd6e2e8,
    .sun_col = 0xfffff2d8,
    .fog_near = 140.0f, .fog_far = 760.0f,
    .sun_az = 300.0f * ENG_PI / 180.0f, .sun_el = 22.0f * ENG_PI / 180.0f,
    .sun_k = {0.80f, 0.70f, 0.55f}, .sky_k = {0.42f, 0.47f, 0.54f}, .amb_k = {0.22f, 0.22f, 0.24f},
};
const Palette *pal = &autumn;

static const pvr_poly_hdr_t *h_flat, *h_flat_nofog, *h_rock, *h_bark, *h_tuft, *h_sun, *h_glow_add, *h_ring_add,
    *h_cloud;

/* ---- Baked lighting ---- */

static inline uint32_t lit(uint32_t albedo, V3 n, float ao) {
    float d = vdot(n, sun_dir);
    if(d < 0) d = 0;
    float up = 0.5f + 0.5f * n.y;
    int ch[3];
    for(int i = 0; i < 3; i++) {
        float k = (pal->amb_k[i] + pal->sky_k[i] * up + pal->sun_k[i] * d * 1.15f) * ao;
        int c = (int)(((albedo >> (16 - 8 * i)) & 255) * k);
        ch[i] = c > 255 ? 255 : c;
    }
    return 0xff000000u | (uint32_t)ch[0] << 16 | (uint32_t)ch[1] << 8 | (uint32_t)ch[2];
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

/* A frustum (a cone with a small flat top) round a vertical axis: k sides,
   radii r0 at y0 and r1 at y1, joined on to the strip so far. Colours from
   the albedo, lit on the sun's side, darker at the base. */
static void cone(V3 base, float r0, float r1, float y0, float y1, int k, float yaw, uint32_t albedo, float ao0,
                 float ao1, bool join) {
    float slope = (r0 - r1) / fmaxf(y1 - y0, 0.01f);
    for(int s = 0; s <= k; s++) {
        float a = yaw + ENG_TAU * (float)(s % k) / k;
        float ca = cosf(a), sa = sinf(a);
        V3 n = vnorm(v3(ca, slope, sa));
        V3 pt = v3(base.x + ca * r1, base.y + y1, base.z + sa * r1);
        V3 pb = v3(base.x + ca * r0, base.y + y0, base.z + sa * r0);
        uint32_t ct = lit(albedo, vnorm(v3(ca * 0.6f, 1.0f, sa * 0.6f)), ao1);
        uint32_t cb = lit(albedo, n, ao0);
        if(s == 0 && join) sc_join(&scenery, pt, 0, 0, ct);
        else sc_vert(&scenery, pt, 0, 0, ct);
        sc_vert(&scenery, pb, 0, 0, cb);
    }
}

static const uint32_t fir_col = 0xff2f6a3c, fir_col2 = 0xff3f7a44, larch_col = 0xffe4b23a, maple_col = 0xffd8642e,
                      trunk_col = 0xff5a4030;

/* One tree at a level of detail (0 near .. 2 far) into the open strip. */
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
    if(t->kind == 2) {
        /* A vine maple: a round crown in two cones on a short trunk. */
        int k = lod == 0 ? 6 : lod == 1 ? 5 : 4;
        if(lod == 0) {
            cone(b, 0.18f * s, 0.14f * s, 0, 2.2f * s, 4, yaw, trunk_col, 0.7f, 0.8f, j);
            j = true;
        }
        cone(b, 0.5f * s, 2.6f * s, 1.6f * s, 3.6f * s, k, yaw, col, 0.62f, 0.95f, j);
        cone(b, 2.6f * s, 0.4f * s, 3.6f * s, 6.0f * s, k, yaw, col, 0.95f, 1.15f, true);
        return;
    }
    float slim = t->kind == 1 ? 0.78f : 1.0f;
    if(lod == 0) {
        cone(b, 0.26f * s, 0.2f * s, 0, 3.0f * s, 4, yaw, trunk_col, 0.7f, 0.8f, j);
        j = true;
        static const float tiers[3][4] = {{3.0f, 1.2f, 6.8f, 0.30f}, {2.3f, 4.6f, 9.6f, 0.24f}, {1.5f, 8.0f, 12.6f, 0.12f}};
        for(int i = 0; i < 3; i++) {
            float ao = 0.62f + 0.14f * i;
            cone(b, tiers[i][0] * s * slim, tiers[i][3] * s, tiers[i][1] * s, tiers[i][2] * s, 6, yaw + i * 0.5f, col,
                 ao, ao + 0.42f, j);
        }
    }
    else if(lod == 1) {
        cone(b, 2.9f * s * slim, 0.4f * s, 1.0f * s, 7.6f * s, 5, yaw, col, 0.64f, 1.0f, j);
        cone(b, 1.9f * s * slim, 0.1f * s, 5.6f * s, 12.6f * s, 5, yaw + 0.6f, col, 0.78f, 1.12f, true);
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
                uint32_t col = lit(0xff9aa0a6, n, 0.8f + 0.25f * ring_y[ri]);
                sc_vert(&scenery, p, a * 0.6f, ring_y[ri] * 1.2f, col);
            }
        }
        sc_strip_end(&scenery);
    }
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
        uint32_t col = lit(albedo, nrm, 0.95f);
        float u = (float)s * (hx + hz) * uv;
        sc_vert(&scenery, v3(p.x, c.y + hy, p.z), u, 0, col);
        sc_vert(&scenery, v3(p.x, c.y - hy, p.z), u, hy * 2 * uv, color_scale(col, 0.75f));
    }
    sc_strip_end(&scenery);
    sc_strip_begin(&scenery, h);
    uint32_t top = lit(albedo, v3(0, 1, 0), 1.0f);
    sc_vert(&scenery, v3(corner[0].x, c.y + hy, corner[0].z), 0, 0, top);
    sc_vert(&scenery, v3(corner[1].x, c.y + hy, corner[1].z), hz * 2 * uv, 0, top);
    sc_vert(&scenery, v3(corner[3].x, c.y + hy, corner[3].z), 0, hx * 2 * uv, top);
    sc_vert(&scenery, v3(corner[2].x, c.y + hy, corner[2].z), hz * 2 * uv, hx * 2 * uv, top);
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
        uint32_t col = lit(0xff7a5a3e, n, 0.9f);
        sc_vert(&scenery, vmad(a, n, r), s / 3.0f, 0, col);
        sc_vert(&scenery, vmad(b, n, r), s / 3.0f, len / 1.5f, col);
    }
    sc_strip_end(&scenery);
    for(int e = 0; e < 2; e++) {
        V3 c = e ? b : a;
        sc_strip_begin(&scenery, h_flat);
        uint32_t col = lit(0xffd8b88a, vscale(ax, e ? 1.0f : -1.0f), 1.0f);
        static const int order[6] = {0, 1, 5, 2, 4, 3};
        for(int s = 0; s < 6; s++) {
            float ang = ENG_TAU * order[s] / 6.0f;
            sc_vert(&scenery, vmad(c, vadd(vscale(side, cosf(ang)), vscale(up, sinf(ang))), r * 0.98f), 0, 0, col);
        }
        sc_strip_end(&scenery);
    }
}

static void prop(const lvl_prop_t *p) {
    V3 c = v3(p->x, p->y, p->z);
    float cy = cosf(p->yaw), sy = sinf(p->yaw);
    switch(p->kind) {
        case PROP_CABIN: {
            box(h_bark, v3(c.x, c.y + 1.4f, c.z), 4.2f, 1.6f, 3.2f, p->yaw, 0xffa0703e, 0.4f);
            /* The roof: a ridge along the cabin's length. */
            V3 ax = v3(cy * 4.8f, 0, -sy * 4.8f), az = v3(sy * 3.8f, 0, cy * 3.8f);
            V3 e0 = vadd(c, az), e1 = vsub(c, az);
            float eave = c.y + 3.0f, ridge = c.y + 5.0f;
            uint32_t rc = 0xff9a3a2a;
            for(int side = -1; side <= 1; side += 2) {
                V3 n = vnorm(v3(ax.x * side * 0.2f, 1.0f, ax.z * side * 0.2f));
                uint32_t col = lit(rc, vnorm(v3(cy * side, 0.9f, -sy * side)), 1.0f);
                (void)n;
                sc_strip_begin(&scenery, h_flat);
                V3 o = vscale(ax, (float)side);
                sc_vert(&scenery, v3(e0.x + o.x, eave, e0.z + o.z), 0, 0, col);
                sc_vert(&scenery, v3(e1.x + o.x, eave, e1.z + o.z), 0, 0, col);
                sc_vert(&scenery, v3(e0.x, ridge, e0.z), 0, 0, col);
                sc_vert(&scenery, v3(e1.x, ridge, e1.z), 0, 0, col);
                sc_strip_end(&scenery);
            }
            for(int end = -1; end <= 1; end += 2) {
                V3 o = vscale(az, (float)end * 0.84f);
                uint32_t col = lit(0xffb88850, vnorm(o), 0.95f);
                sc_strip_begin(&scenery, h_flat);
                sc_vert(&scenery, v3(c.x + o.x - ax.x * 0.88f, eave, c.z + o.z - ax.z * 0.88f), 0, 0, col);
                sc_vert(&scenery, v3(c.x + o.x + ax.x * 0.88f, eave, c.z + o.z + ax.z * 0.88f), 0, 0, col);
                sc_vert(&scenery, v3(c.x + o.x, ridge - 0.1f, c.z + o.z), 0, 0, col);
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
                box(h_rock, v3(q.x, c.y + 2.05f, q.z), 0.9f, 2.25f, 1.1f, p->yaw + side * 0.08f, 0xff9a9488, 0.35f);
            }
            box(h_rock, v3(c.x, c.y + 4.75f, c.z), 4.4f, 0.55f, 1.2f, p->yaw, 0xffa49e92, 0.35f);
            boulder(v3(c.x + r.x * 1.5f, c.y + 5.4f, c.z + r.z * 1.5f), 0.7f, seed, 0);
            boulder(v3(c.x - r.x * 2.2f, c.y + 5.3f, c.z - r.z * 2.2f), 0.5f, seed * 3u, 0);
            break;
        }
        case PROP_TENT: {
            /* A ridge tent, a fire ring in front of it. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            V3 fr = vadd(c, vscale(f, 1.1f)), bk = vsub(c, vscale(f, 1.1f));
            for(int side = -1; side <= 1; side += 2) {
                uint32_t col = lit(0xffd8642a, vnorm(v3(r.x * side, 0.8f, r.z * side)), 1.0f);
                sc_strip_begin(&scenery, h_flat);
                sc_vert(&scenery, v3(fr.x + r.x * side * 0.95f, c.y, fr.z + r.z * side * 0.95f), 0, 0, col);
                sc_vert(&scenery, v3(bk.x + r.x * side * 0.95f, c.y, bk.z + r.z * side * 0.95f), 0, 0, col);
                sc_vert(&scenery, v3(fr.x, c.y + 1.35f, fr.z), 0, 0, col);
                sc_vert(&scenery, v3(bk.x, c.y + 1.35f, bk.z), 0, 0, col);
                sc_strip_end(&scenery);
            }
            for(int end = 0; end < 2; end++) {
                V3 e = end ? fr : bk;
                uint32_t col = end ? 0xff3a2a20 : lit(0xffc0582a, vscale(f, -1.0f), 0.9f);
                sc_strip_begin(&scenery, h_flat);
                sc_vert(&scenery, v3(e.x - r.x * 0.95f, c.y, e.z - r.z * 0.95f), 0, 0, col);
                sc_vert(&scenery, v3(e.x + r.x * 0.95f, c.y, e.z + r.z * 0.95f), 0, 0, col);
                sc_vert(&scenery, v3(e.x, c.y + 1.35f, e.z), 0, 0, col);
                sc_strip_end(&scenery);
            }
            V3 fire = vadd(c, vscale(f, 4.0f));
            uint32_t seed = (uint32_t)(p->x * 13.0f) ^ (uint32_t)(p->z * 7.0f);
            for(int k = 0; k < 8; k++) {
                float a = k * ENG_TAU / 8;
                boulder(v3(fire.x + sinf(a) * 0.62f, c.y + 0.05f, fire.z + cosf(a) * 0.62f), 0.16f, seed + k, 1);
            }
            box(h_flat, v3(fire.x, c.y + 0.03f, fire.z), 0.42f, 0.03f, 0.42f, 0, 0xff302824, 1.0f);
            break;
        }
        case PROP_TOWER: {
            /* A fire lookout: four legs, the cab on top, a pyramid roof. */
            for(int k = 0; k < 4; k++) {
                float lx = (k & 1) ? 1.8f : -1.8f, lz = (k & 2) ? 1.8f : -1.8f;
                box(h_bark, v3(c.x + lx, c.y + 3.0f, c.z + lz), 0.12f, 3.0f, 0.12f, 0, 0xff6a4a30, 1.0f);
            }
            box(h_bark, v3(c.x, c.y + 6.0f, c.z), 2.2f, 0.12f, 2.2f, 0, 0xff8a6440, 0.5f);
            box(h_flat, v3(c.x, c.y + 7.2f, c.z), 1.9f, 1.1f, 1.9f, 0, 0xffe8dcc0, 1.0f);
            uint32_t rc = lit(0xff3c6a44, vnorm(v3(-0.6f, 1, 0.2f)), 1.0f);
            sc_strip_begin(&scenery, h_flat);
            V3 apex = v3(c.x, c.y + 9.8f, c.z);
            static const float cx[5] = {-2.4f, 2.4f, 2.4f, -2.4f, -2.4f}, cz[5] = {-2.4f, -2.4f, 2.4f, 2.4f, -2.4f};
            for(int k = 0; k < 5; k++) {
                sc_vert(&scenery, v3(c.x + cx[k], c.y + 8.3f, c.z + cz[k]), 0, 0, color_scale(rc, 0.8f + 0.1f * (k & 1)));
                sc_vert(&scenery, apex, 0, 0, rc);
            }
            sc_strip_end(&scenery);
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
static uint32_t range_top[RANGES], range_bot[RANGES];
static V3 cloud_p[8];
static float cloud_w[8];

static void sky_setup(void) {
    static const float el[6] = {-0.25f, 0.0f, 0.08f, 0.22f, 0.55f, 1.5707f};
    sky_init(&sky, el, 6, SKY_SEG, 1900.0f, h_flat_nofog);
    for(int e = 0; e < 6; e++)
        for(int i = 0; i <= SKY_SEG; i++) {
            float a = ENG_TAU * i / SKY_SEG;
            float t = clampf(el[e] / 0.9f, 0, 1);
            uint32_t c = color_lerp(pal->sky_horizon, pal->sky_top, sqrtf(t));
            if(e == 0) c = pal->haze;
            /* Warmer low in the sky toward the sun. */
            float toward = 0.5f + 0.5f * cosf(a - pal->sun_az);
            c = color_lerp(c, 0xfff6e2c4, toward * toward * toward * (1.0f - t) * 0.55f);
            sky_set_color(&sky, e, i, c);
        }
    uint32_t seed = 4242;
    for(int r = 0; r < RANGES; r++) {
        float base = 60.0f + 50.0f * r;
        for(int i = 0; i < RANGE_SEG; i++) {
            float x = (float)i / RANGE_SEG * ENG_TAU;
            float h = base + 70.0f * (0.5f + 0.5f * sinf(x * (2 + r) + r)) + 60.0f * hrand(&seed) + 40.0f * hrand(&seed);
            range_h[r][i] = h;
        }
        range_h[r][RANGE_SEG] = range_h[r][0];
        /* Further ranges sit lighter and bluer in the haze. */
        float k = 0.35f + 0.22f * r;
        range_top[r] = color_lerp(0xff4e6a70, pal->haze, k);
        range_bot[r] = color_lerp(range_top[r], pal->haze, 0.55f);
    }
    for(int i = 0; i < 8; i++) {
        float a = ENG_TAU * (i + hrand(&seed) * 0.6f) / 8;
        float d = 950.0f + 350.0f * hrand(&seed);
        cloud_p[i] = v3(sinf(a) * d, 260.0f + 160.0f * hrand(&seed), cosf(a) * d);
        cloud_w[i] = 110.0f + 120.0f * hrand(&seed);
    }
}

void world_draw_sky(void) {
    V3 c = r_cam.pos;
    sky_draw(&sky);
    r_hdr(h_flat_nofog);
    /* The ranges, furthest first (the PVR sorts opaque depth anyway). */
    for(int r = RANGES - 1; r >= 0; r--) {
        float rad = 1050.0f + 260.0f * r;
        RV v[2 * (RANGE_SEG + 1)];
        int n = 0;
        for(int i = 0; i <= RANGE_SEG; i++) {
            float a = ENG_TAU * i / RANGE_SEG + r * 0.37f;
            float sx = sinf(a) * rad, sz = cosf(a) * rad;
            v[n++] = (RV){c.x + sx, range_h[r][i] - 40.0f + c.y * 0.15f, c.z + sz, 0, 0, range_top[r]};
            v[n++] = (RV){c.x + sx, -60.0f, c.z + sz, 0, 0, range_bot[r]};
        }
        r_strip(v, n);
    }
}

/* ---- Grass tufts near the camera ---- */

#define TUFT_R 26.0f
#define TUFT_CELL 2.0f
#define TUFT_MAX 220

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

void world_draw_pt(void) {
    static RV v[TUFT_MAX * 6];
    for(int i = 0; i < game.ntrucks; i++) {
        const float *hc = trucks[i].spec.hull_c;
        tuft_hull[i] = truck_to_world(&trucks[i], v3(hc[0], hc[1], hc[2]));
    }
    int n = 0;
    V3 c = r_cam.pos;
    float tx, ty;
    r_frustum_slopes(&tx, &ty);
    V3 rt = vnorm(v3(r_cam.right.x, 0, r_cam.right.z));
    int i0 = (int)floorf((c.x - TUFT_R) / TUFT_CELL), i1 = (int)floorf((c.x + TUFT_R) / TUFT_CELL);
    int j0 = (int)floorf((c.z - TUFT_R) / TUFT_CELL), j1 = (int)floorf((c.z + TUFT_R) / TUFT_CELL);
    int count = 0;
    for(int j = j0; j <= j1 && count < TUFT_MAX; j++)
        for(int i = i0; i <= i1 && count < TUFT_MAX; i++) {
            uint32_t s = hash32((uint32_t)i * 73856093u ^ (uint32_t)j * 19349663u);
            float px = (i + (s & 255) / 255.0f) * TUFT_CELL, pz = (j + ((s >> 8) & 255) / 255.0f) * TUFT_CELL;
            float dx = px - c.x, dz = pz - c.z, d2 = dx * dx + dz * dz;
            if(d2 > TUFT_R * TUFT_R) continue;
            /* In view? (by the vertical frustum slope only roughly) */
            float fz = dx * r_cam.fwd.x + dz * r_cam.fwd.z;
            float fx = dx * r_cam.right.x + dz * r_cam.right.z;
            if(fz < -1.0f || fabsf(fx) > fz * tx + 2.0f) continue;
            int sf = surface_at(px, pz);
            if(sf != SURF_GRASS && !(sf == SURF_FOREST && (s >> 20 & 3) == 0)) continue;
            if(water_at(px, pz) > -999.0f) continue;
            float y = terrain_height(px, pz);
            float k = 1.0f - d2 / (TUFT_R * TUFT_R);
            float w = (0.55f + ((s >> 16) & 15) / 30.0f) * fminf(1.0f, k * 3.0f);
            float h = w * 1.25f;
            if(under_truck(px, pz, w * 0.6f)) continue;
            int gi = lvl_clampi((int)((px - LEVEL_ORG) / LEVEL_D + 0.5f)), gj = lvl_clampi((int)((pz - LEVEL_ORG) / LEVEL_D + 0.5f));
            uint32_t col = color_scale(lvl_ground[gj * LEVEL_N + gi].c, 1.35f);
            float u0 = (s >> 24 & 1) ? 0.0f : 1.0f, u1 = 1.0f - u0;
            V3 l = vmad(v3(px, y - 0.05f, pz), rt, -w), r = vmad(v3(px, y - 0.05f, pz), rt, w);
            if(n) {
                v[n] = v[n - 1];
                n++;
                v[n++] = (RV){l.x, l.y + h, l.z, u0, 0, col};
            }
            v[n++] = (RV){l.x, l.y + h, l.z, u0, 0, col};
            v[n++] = (RV){r.x, r.y + h, r.z, u1, 0, col};
            v[n++] = (RV){l.x, l.y, l.z, u0, 1, color_scale(col, 0.7f)};
            v[n++] = (RV){r.x, r.y, r.z, u1, 1, color_scale(col, 0.7f)};
            count++;
            if(n > 480) {
                r_hdr(h_tuft);
                r_strip(v, n);
                n = 0;
            }
        }
    if(n) {
        r_hdr(h_tuft);
        r_strip(v, n);
    }
}

/* ---- Sun, clouds and flare ---- */

static float flare_vis;   /* 0..1: how clearly the camera sees the sun */

static bool sun_occluded(void) {
    V3 p = r_cam.pos;
    for(int k = 1; k <= 30; k++) {
        float t = k * k * 0.9f + 4.0f;
        V3 q = vmad(p, sun_dir, t);
        if(fabsf(q.x) > 383 || fabsf(q.z) > 383) break;
        if(terrain_height(q.x, q.z) > q.y) return true;
    }
    return false;
}

void world_update(float dt) {
    float target = sun_occluded() ? 0.0f : 1.0f;
    flare_vis = approach(flare_vis, target, dt * 4.0f);
}

void world_draw_tr(void) {
    terrain_draw_water(&scenery);
    V3 c = r_cam.pos;
    /* Clouds, far and flat. */
    r_hdr(h_cloud);
    for(int i = 0; i < 8; i++) {
        V3 p = vadd(c, cloud_p[i]);
        p.y = cloud_p[i].y;
        r_billboard(p, cloud_w[i], cloud_w[i] * 0.42f, 0, 0, 0, 1, 1, color_alpha(0xfff8fbfc, 0.92f));
    }
    /* The sun: a big flat disc with a soft glow. */
    V3 sp = vmad(c, sun_dir, 1600.0f);
    r_hdr(h_glow_add);
    r_billboard(sp, 320.0f, 320.0f, 0, 0, 0, 1, 1, 0x60fff0d0);
    r_hdr(h_sun);
    r_billboard(sp, 62.0f, 62.0f, 0, 0, 0, 1, 1, pal->sun_col);
}

void world_draw_flare(void) {
    if(flare_vis <= 0.01f) return;
    float sx, sy;
    if(!sky_sun_screen(sun_dir, 1600.0f, &sx, &sy)) return;
    float facing = vdot(r_cam.fwd, sun_dir);
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
    float ce = cosf(pal->sun_el);
    sun_dir = v3(sinf(pal->sun_az) * ce, sinf(pal->sun_el), cosf(pal->sun_az) * ce);
    h_flat = r_header(TEX_NONE, HF_OP);
    h_flat_nofog = r_header(TEX_NONE, HF_OP | HF_NOFOG);
    h_rock = r_header(TEX_ROCK, HF_OP);
    h_bark = r_header(TEX_BARK, HF_OP);
    h_tuft = r_header(TEX_TUFT, HF_PT);
    h_sun = r_header(TEX_SUN, HF_TR | HF_NOFOG | HF_ADD);
    h_glow_add = r_header(TEX_GLOW, HF_TR | HF_NOFOG | HF_ADD | HF_NOZTEST);
    h_ring_add = r_header(TEX_RING, HF_TR | HF_NOFOG | HF_ADD | HF_NOZTEST);
    h_cloud = r_header(TEX_CLOUD, HF_TR | HF_NOFOG);
    pvr_fog_table_color(1.0f, ((pal->haze >> 16) & 255) / 255.0f, ((pal->haze >> 8) & 255) / 255.0f,
                        (pal->haze & 255) / 255.0f);
    pvr_fog_table_linear(pal->fog_near, pal->fog_far);
    pvr_set_bg_color(((pal->haze >> 16) & 255) / 255.0f, ((pal->haze >> 8) & 255) / 255.0f, (pal->haze & 255) / 255.0f);
    sky_setup();
    if(!sc_init(&scenery, 170000, 6000, 1400)) printf("Ridgeline world: no memory for the scenery\n");
}

/* Trees, rocks and props are grouped into 48 m cells, a chunk per cell
   and level of detail. Far away only every other tree is drawn, a little
   bigger, which the eye reads as the same forest. */
#define TCELL 48.0f
#define TCELLS 16
static const float tree_far[3] = {70.0f, 170.0f, 430.0f};

void world_build(void) {
    sc_clear(&scenery);
    terrain_build(&scenery);
    int terrain_v = scenery.pool_n;
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
    for(int c = 0; c < TCELLS * TCELLS; c++) {
        if(first[c + 1] == first[c]) continue;
        V3 lp = v3(LEVEL_ORG + ((c % TCELLS) + 0.5f) * TCELL, 0, LEVEL_ORG + ((c / TCELLS) + 0.5f) * TCELL);
        lp.y = terrain_height(lp.x, lp.z);
        for(int lod = 0; lod < 3; lod++) {
            sc_chunk_begin(&scenery, HF_OP, lod ? tree_far[lod - 1] : 0.0f, tree_far[lod], TAG_TREES);
            sc_chunk_lod_point(&scenery, lp);
            sc_strip_begin(&scenery, h_flat);
            bool firstt = true;
            for(int k = first[c]; k < first[c + 1]; k++) {
                lvl_tree_t t = lvl_trees[order[k]];
                if(lod == 2) {
                    if(order[k] & 1) continue;
                    t.scale *= 1.2f;
                }
                tree(&t, lod, firstt);
                firstt = false;
            }
            sc_strip_end(&scenery);
            sc_chunk_end(&scenery);
        }
    }
    int tree_v = scenery.pool_n - terrain_v;
    /* Boulders, logs and ledges: near and far versions in one chunk per cell. */
    for(int c = 0; c < TCELLS * TCELLS; c++) {
        float x0 = LEVEL_ORG + (c % TCELLS) * TCELL, z0 = LEVEL_ORG + (c / TCELLS) * TCELL;
        V3 lp = v3(x0 + TCELL / 2, 0, z0 + TCELL / 2);
        lp.y = terrain_height(lp.x, lp.z);
        for(int lod = 0; lod < 2; lod++) {
            sc_chunk_begin(&scenery, HF_OP, lod ? 90.0f : 0.0f, lod ? 360.0f : 90.0f, TAG_ROCKS);
            sc_chunk_lod_point(&scenery, lp);
            for(int i = 0; i < LVL_SPHERES; i++) {
                const lvl_sphere_t *s = &lvl_spheres[i];
                if(s->x < x0 || s->x >= x0 + TCELL || s->z < z0 || s->z >= z0 + TCELL) continue;
                if(lod && s->r < 0.6f) continue;
                boulder(v3(s->x, s->y, s->z), s->r * 1.04f, (uint32_t)i * 2654435761u, lod);
            }
            for(int i = 0; i < LVL_CAPSULES; i++) {
                const lvl_capsule_t *cp = &lvl_capsules[i];
                float mx = (cp->ax + cp->bx) * 0.5f, mz = (cp->az + cp->bz) * 0.5f;
                if(mx < x0 || mx >= x0 + TCELL || mz < z0 || mz >= z0 + TCELL) continue;
                log_mesh(cp);
            }
            for(int i = 0; i < LVL_ROCK_BOXES; i++) {    /* the rest are props', drawn as the prop */
                const lvl_box_t *b = &lvl_boxes[i];
                if(b->x < x0 || b->x >= x0 + TCELL || b->z < z0 || b->z >= z0 + TCELL) continue;
                box(h_rock, v3(b->x, b->y, b->z), b->hx, b->hy, b->hz, b->yaw, 0xff8c9298, 0.35f);
            }
            sc_chunk_end(&scenery);
        }
    }
    for(int i = 0; i < LVL_PROPS; i++) {
        sc_chunk_begin(&scenery, HF_OP, 0, 600.0f, TAG_PROPS);
        prop(&lvl_props[i]);
        sc_chunk_end(&scenery);
    }
    printf("Ridgeline world: %lu vertices (terrain %d, trees %d), %lu strips, %lu chunks%s\n",
           (unsigned long)scenery.pool_n, terrain_v, tree_v, (unsigned long)scenery.strip_n,
           (unsigned long)scenery.chunk_n, scenery.full ? " - FULL, some scenery missing" : "");
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
    sc_draw(&scenery, HF_OP, (1u << TAG_TREES) | (1u << TAG_ROCKS) | (1u << TAG_PROPS));
    PROF_LAP(PS_TREES, pw);
    uint64_t t2 = timer_us_gettime64();
    world_us[0] += t1 - t0;
    world_us[1] += t2 - t1;
}
