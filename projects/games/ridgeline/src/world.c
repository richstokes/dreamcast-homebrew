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
#include "outland.h"
#include "rain.h"
#include "water.h"
#include "campfire.h"
#include "glow.h"
#include "truck.h"
#include "lamps.h"
#include "dunes.h"
#include "engine/prof.h"
#include <sh4zam/shz_sh4zam.h>

Scenery scenery;
V3 sun_dir;

static const pvr_poly_hdr_t *h_marks, *h_glyphs;
static const pvr_poly_hdr_t *h_flat, *h_flat_nofog, *h_rock, *h_bark, *h_crown, *h_sun, *h_moon, *h_stars, *h_glow_add,
    *h_ring_add, *h_cloud, *h_stone;

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
static uint32_t lrec_max;      /* records there is room for */
uint32_t world_alb[64];
static int alb_n;
static uint32_t alb_over[64];   /* colours there was no room for */
static int alb_over_n;

static uint8_t alb_index(uint32_t albedo) {
    for(int i = 0; i < alb_n; i++)
        if(world_alb[i] == albedo) return (uint8_t)i;
    if(alb_n < 64) world_alb[alb_n++] = albedo;
    else {
        /* No room: drawn as the last one. Counted, for the log. */
        int k = 0;
        while(k < alb_over_n && alb_over[k] != albedo) k++;
        if(k == alb_over_n && alb_over_n < 64) alb_over[alb_over_n++] = albedo;
    }
    return (uint8_t)(alb_n - 1);
}

static void rec_mark(LightRec r) {
    if(!world_lrec || scenery.pool_n - world_lrec_first > lrec_max) return;
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
    uint32_t n = scenery.pool_n - world_lrec_first;
    return world_lrec ? (int)(n < lrec_max ? n : lrec_max) : 0;
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
                      trunk_col = 0xff5a4030, snag_col = 0xffb4aea2;

/* One tree at a level of detail (0 near .. 3 far) into the open strip.
   Near, trunk and crown take the painted strokes (one texture, so a
   chunk of trees stays one strip); further off a tree is flat colour,
   the strokes being a few pixels there. */
/* The desert's trees: a juniper (kind 0) is a squat round crown leaning
   off a twisted trunk, a pinyon (2) a small dark pine, a cottonwood (1) a
   tall pale trunk under a broad crown, green or already gold. */
static void tree_desert(const lvl_tree_t *t, int lod, bool first) {
    V3 b = v3(t->x, t->y - 0.2f, t->z);
    float s = t->scale, yaw = t->yaw;
    uint32_t seed = (uint32_t)(t->x * 131.0f) ^ (uint32_t)(t->z * 977.0f);
    float pick = hrand(&seed);
    bool j = !first;
    int k = lod == 0 ? 6 : lod < 3 ? 5 : 4;
    float crown_us = lod == 0 ? 2.0f : 0, crown_vs = lod == 0 ? 2.4f * s : 0;
    cone_us = lod == 0 ? 1.0f : 0;
    cone_vs = lod == 0 ? 1.6f : 0;
    if(t->kind == 1) {
        uint32_t col = pick < 0.45f ? 0xffe0b63a : pick < 0.7f ? 0xffc4b83c : 0xff98b040;
        const uint32_t bark = 0xffb4a48c;
        if(lod == 0) cone(b, 0.36f * s, 0.24f * s, 0, 3.6f * s, 5, yaw, bark, 0.72f, 0.85f, j);
        else if(lod < 3) cone(b, 0.36f * s, 0.26f * s, 0, 3.2f * s, 3, yaw, bark, 0.72f, 0.8f, j);
        j = j || lod < 3;
        cone_us = crown_us;
        cone_vs = crown_vs;
        if(lod == 3) {
            cone(b, 3.0f * s, 0.6f * s, 2.8f * s, 8.4f * s, 4, yaw, col, 0.7f, 1.12f, j);
            return;
        }
        /* A broad round head: out, up, and in again. */
        cone(b, 1.0f * s, 3.1f * s, 2.6f * s, 4.2f * s, k, yaw, col, 0.58f, 0.86f, j);
        cone(b, 3.1f * s, 3.3f * s, 4.2f * s, 6.2f * s, k, yaw + 0.3f, col, 0.86f, 1.04f, true);
        cone(b, 3.3f * s, 0.9f * s, 6.2f * s, 8.4f * s, k, yaw + 0.6f, col, 1.04f, 1.18f, true);
        return;
    }
    if(t->kind == 2) {
        uint32_t col = pick < 0.5f ? 0xff40603e : 0xff4a6a42;
        if(lod < 2) cone(b, 0.2f * s, 0.16f * s, 0, 1.2f * s, lod ? 3 : 4, yaw, 0xff5a4636, 0.7f, 0.8f, j);
        j = j || lod < 2;
        cone_us = crown_us;
        cone_vs = crown_vs;
        if(lod == 3) {
            cone(b, 1.7f * s, 0.06f * s, 0.6f * s, 4.4f * s, 3, yaw, col, 0.66f, 1.1f, j);
            return;
        }
        cone(b, 1.75f * s, 0.7f * s, 0.7f * s, 2.6f * s, k, yaw, col, 0.6f, 0.98f, j);
        cone(b, 1.2f * s, 0.08f * s, 2.2f * s, 4.5f * s, k, yaw + 0.5f, col, 0.8f, 1.16f, true);
        return;
    }
    /* Juniper: the crown sits off to one side of where the trunk roots. */
    uint32_t col = pick < 0.5f ? 0xff5c7040 : 0xff50643a;
    float la = hrand(&seed) * ENG_TAU, lean = 0.45f * s;
    V3 c = v3(b.x + cosf(la) * lean, b.y, b.z + sinf(la) * lean);
    if(lod == 0) {
        /* The trunk in two bends, grey and stringy. */
        V3 m = v3(b.x + cosf(la) * lean * 0.2f, b.y, b.z + sinf(la) * lean * 0.2f);
        cone(b, 0.3f * s, 0.2f * s, 0, 0.8f * s, 4, yaw, 0xff8a7866, 0.7f, 0.8f, j);
        cone(m, 0.2f * s, 0.15f * s, 0.8f * s, 1.5f * s, 4, yaw + 0.6f, 0xff8a7866, 0.8f, 0.85f, true);
        j = true;
    }
    else if(lod == 1) {
        cone(b, 0.28f * s, 0.18f * s, 0, 1.3f * s, 3, yaw, 0xff8a7866, 0.7f, 0.78f, j);
        j = true;
    }
    cone_us = crown_us;
    cone_vs = crown_vs;
    if(lod == 3) {
        cone(c, 2.0f * s, 0.5f * s, 0.7f * s, 3.5f * s, 4, yaw, col, 0.66f, 1.1f, j);
        return;
    }
    cone(c, 0.8f * s, 2.15f * s, 0.8f * s, 1.9f * s, k, yaw, col, 0.56f, 0.9f, j);
    cone(c, 2.15f * s, 1.5f * s, 1.9f * s, 2.9f * s, k, yaw + 0.3f, col, 0.9f, 1.08f, true);
    cone(c, 1.5f * s, 0.3f * s, 2.9f * s, 3.7f * s, k, yaw + 0.6f, col, 1.08f, 1.2f, true);
}

/* The dunes' trees, all of them where roots reach water: a Russian olive
   (kind 0) is a low spreading crown of silver-grey leaves in two heaps
   on a dark leaning trunk, a willow (2) a thicket with no trunk to see,
   a cottonwood (1) the desert's. */
static void tree_dunes(const lvl_tree_t *t, int lod, bool first) {
    if(t->kind == 1) {
        tree_desert(t, lod, first);
        return;
    }
    V3 b = v3(t->x, t->y - 0.2f, t->z);
    float s = t->scale, yaw = t->yaw;
    uint32_t seed = (uint32_t)(t->x * 131.0f) ^ (uint32_t)(t->z * 977.0f);
    float pick = hrand(&seed);
    bool j = !first;
    int k = lod == 0 ? 6 : lod < 3 ? 5 : 4;
    float crown_us = lod == 0 ? 2.0f : 0, crown_vs = lod == 0 ? 2.4f * s : 0;
    cone_us = lod == 0 ? 1.0f : 0;
    cone_vs = lod == 0 ? 1.6f : 0;
    if(t->kind == 2) {
        /* Green going to yellow, as willows do first. */
        uint32_t col = pick < 0.4f ? 0xff96a44c : pick < 0.75f ? 0xffb0ac48 : 0xffc8b044;
        cone_us = crown_us;
        cone_vs = crown_vs;
        if(lod == 3) {
            cone(b, 2.3f * s, 0.5f * s, 0.1f * s, 3.4f * s, 4, yaw, col, 0.62f, 1.1f, j);
            return;
        }
        cone(b, 1.5f * s, 2.5f * s, 0.0f, 1.3f * s, k, yaw, col, 0.5f, 0.84f, j);
        cone(b, 2.5f * s, 1.7f * s, 1.3f * s, 2.6f * s, k, yaw + 0.3f, col, 0.84f, 1.04f, true);
        cone(b, 1.7f * s, 0.25f * s, 2.6f * s, 3.5f * s, k, yaw + 0.6f, col, 1.04f, 1.2f, true);
        return;
    }
    uint32_t col = pick < 0.5f ? 0xffaebca0 : 0xff9cae96;
    const uint32_t bark = 0xff4e4036;
    float la = hrand(&seed) * ENG_TAU, lean = 0.7f * s;
    V3 c = v3(b.x + cosf(la) * lean, b.y, b.z + sinf(la) * lean);
    if(lod == 0) {
        V3 m = v3(b.x + cosf(la) * lean * 0.4f, b.y, b.z + sinf(la) * lean * 0.4f);
        cone(b, 0.3f * s, 0.22f * s, 0, 1.2f * s, 4, yaw, bark, 0.7f, 0.8f, j);
        cone(m, 0.22f * s, 0.16f * s, 1.2f * s, 2.3f * s, 4, yaw + 0.6f, bark, 0.8f, 0.85f, true);
        j = true;
    }
    else if(lod == 1) {
        cone(b, 0.28f * s, 0.2f * s, 0, 2.0f * s, 3, yaw, bark, 0.7f, 0.78f, j);
        j = true;
    }
    cone_us = crown_us;
    cone_vs = crown_vs;
    if(lod == 3) {
        cone(c, 2.3f * s, 0.5f * s, 1.3f * s, 5.0f * s, 4, yaw, col, 0.64f, 1.1f, j);
        return;
    }
    /* The main heap, and at hand a second, lower, to the other side. */
    cone(c, 0.7f * s, 2.3f * s, 1.7f * s, 2.8f * s, k, yaw, col, 0.54f, 0.88f, j);
    cone(c, 2.3f * s, 1.7f * s, 2.8f * s, 4.0f * s, k, yaw + 0.3f, col, 0.88f, 1.06f, true);
    cone(c, 1.7f * s, 0.3f * s, 4.0f * s, 5.0f * s, k, yaw + 0.6f, col, 1.06f, 1.22f, true);
    if(lod < 2) {
        V3 c2 = v3(b.x - cosf(la) * lean * 2.1f, b.y, b.z - sinf(la) * lean * 2.1f);
        cone(c2, 0.6f * s, 1.6f * s, 1.1f * s, 2.0f * s, 5, yaw + 1.0f, col, 0.52f, 0.84f, true);
        cone(c2, 1.6f * s, 0.3f * s, 2.0f * s, 3.4f * s, 5, yaw + 1.4f, col, 0.84f, 1.16f, true);
    }
}

/* The Highlands' trees: a Scots pine (kind 0) is a tall bare trunk,
   grey below and orange above, under a flat dark crown with a bough or
   two below it; a birch (1) a thin white stem in a narrow crown gone
   gold; a rowan (2) a small round tree, red with the autumn. */
/* A pad of foliage: a rounded lump k-sided, r across and tall high, its
   foot at y over c. */
static void leaf_pad(V3 c, float y, float r, float tall, int k, float yaw, uint32_t col, float ao, bool join) {
    cone(c, r * 0.5f, r, y, y + tall * 0.34f, k, yaw, col, ao - 0.28f, ao - 0.06f, join);
    cone(c, r, r * 0.74f, y + tall * 0.34f, y + tall * 0.7f, k, yaw + 0.3f, col, ao - 0.06f, ao + 0.1f, true);
    cone(c, r * 0.74f, r * 0.1f, y + tall * 0.7f, y + tall, k, yaw + 0.6f, col, ao + 0.1f, ao + 0.24f, true);
}

static void tree_highland(const lvl_tree_t *t, int lod, bool first) {
    V3 b = v3(t->x, t->y - 0.2f, t->z);
    float s = t->scale, yaw = t->yaw;
    uint32_t seed = (uint32_t)(t->x * 131.0f) ^ (uint32_t)(t->z * 977.0f);
    float pick = hrand(&seed);
    bool j = !first;
    int k = lod == 0 ? 6 : lod < 3 ? 5 : 4;
    float crown_us = lod == 0 ? 2.0f : 0, crown_vs = lod == 0 ? 2.0f * s : 0;
    cone_us = lod == 0 ? 1.0f : 0;
    cone_vs = lod == 0 ? 1.6f : 0;
    if(t->kind == 0) {
        /* A Scots pine: the trunk leaning a little, then pads of needles
           at the ends of its boughs, the biggest on top. */
        static const uint32_t greens[3] = {0xff3e5e48, 0xff48684c, 0xff3e5e48};
        const uint32_t grey = 0xff6e5a4c, orange = 0xffb06a3c;
        float la = hrand(&seed) * ENG_TAU, lean = 0.7f * s, ca = cosf(la), sa = sinf(la);
        V3 mid = v3(b.x + ca * lean * 0.4f, b.y, b.z + sa * lean * 0.4f), top = v3(b.x + ca * lean, b.y, b.z + sa * lean);
        if(lod == 0) {
            cone(b, 0.3f * s, 0.23f * s, 0, 4.4f * s, 5, yaw, grey, 0.7f, 0.85f, j);
            cone(mid, 0.23f * s, 0.14f * s, 4.4f * s, 8.6f * s, 4, yaw + 0.4f, orange, 0.85f, 0.9f, true);
        }
        else cone(b, 0.28f * s, 0.15f * s, 0, 8.2f * s, 3, yaw, lod == 1 ? grey : orange, 0.72f, 0.88f, j);
        cone_us = crown_us;
        cone_vs = crown_vs;
        if(lod == 3) {
            cone(top, 2.3f * s, 0.5f * s, 7.4f * s, 10.4f * s, 4, yaw, greens[0], 0.66f, 1.1f, true);
            return;
        }
        leaf_pad(top, 8.0f * s, 2.0f * s, 2.7f * s, k, yaw, greens[pick < 0.5f], 0.92f, true);
        /* Lower pads out on their boughs: two near, one from further off. */
        int pads = lod == 0 ? 3 : lod == 1 ? 2 : 1;
        for(int i = 0; i < pads; i++) {
            float a = la + 2.1f * i + 1.2f + hrand(&seed), out = (1.5f + 0.5f * hrand(&seed)) * s;
            V3 q = v3(mid.x + cosf(a) * out, b.y, mid.z + sinf(a) * out);
            leaf_pad(q, (6.5f - 0.7f * i + 0.6f * hrand(&seed)) * s, (1.35f - 0.12f * i) * s, 1.9f * s, k - 1, yaw + i,
                     greens[(i + 1) % 3], 0.82f - 0.06f * i, true);
        }
        return;
    }
    if(t->kind == 1) {
        /* A birch: a white stem, three small heads of gold up it, each a
           little off the last. */
        static const uint32_t golds[4] = {0xffe0b43a, 0xffd2ba40, 0xffc8a236, 0xffe0b43a};
        const uint32_t bark = 0xffe6e2d6;
        if(lod == 0) cone(b, 0.16f * s, 0.07f * s, 0, 5.6f * s, 5, yaw, bark, 0.74f, 0.92f, j);
        else if(lod < 3) cone(b, 0.16f * s, 0.1f * s, 0, 3.6f * s, 3, yaw, bark, 0.74f, 0.86f, j);
        j = j || lod < 3;
        cone_us = crown_us;
        cone_vs = crown_vs;
        if(lod == 3) {
            cone(b, 1.5f * s, 0.1f * s, 2.4f * s, 7.2f * s, 4, yaw, golds[0], 0.7f, 1.12f, j);
            return;
        }
        int ci = (int)(pick * 3.99f);
        for(int i = 0; i < 3; i++) {
            float a = hrand(&seed) * ENG_TAU, out = (0.25f + 0.2f * hrand(&seed)) * s * (i < 2 ? 1.0f : 0.4f);
            V3 q = v3(b.x + cosf(a) * out, b.y, b.z + sinf(a) * out);
            leaf_pad(q, (2.4f + 1.35f * i) * s, (1.25f - 0.2f * i) * s, 2.6f * s, lod ? 4 : 5, yaw + i, golds[(ci + i) & 3],
                     0.84f + 0.08f * i, j || i);
        }
        return;
    }
    /* A rowan: small, round, red with the autumn. */
    static const uint32_t reds[3] = {0xffc8562a, 0xffd06c2e, 0xffc8562a};
    if(lod == 0) cone(b, 0.14f * s, 0.1f * s, 0, 2.2f * s, 4, yaw, 0xff6e5a4c, 0.7f, 0.8f, j);
    else if(lod == 1) cone(b, 0.14f * s, 0.12f * s, 0, 1.9f * s, 3, yaw, 0xff6e5a4c, 0.7f, 0.75f, j);
    j = j || lod < 2;
    cone_us = crown_us;
    cone_vs = crown_vs;
    if(lod == 3) {
        cone(b, 1.7f * s, 0.2f * s, 1.6f * s, 4.4f * s, 4, yaw, reds[0], 0.7f, 1.1f, j);
        return;
    }
    int ci = (int)(pick * 2.99f);
    float a = hrand(&seed) * ENG_TAU;
    leaf_pad(v3(b.x + cosf(a) * 0.5f * s, b.y, b.z + sinf(a) * 0.5f * s), 1.7f * s, 1.5f * s, 1.9f * s, lod ? 4 : 5, yaw, reds[ci], 0.84f, j);
    leaf_pad(v3(b.x - cosf(a) * 0.4f * s, b.y, b.z - sinf(a) * 0.4f * s), 2.5f * s, 1.25f * s, 1.9f * s, lod ? 4 : 5, yaw + 1.0f,
        reds[(ci + 1) % 3], 0.94f, true);
}

static void tree(const lvl_tree_t *t, int lod, bool first) {
    if(lvl_area == AREA_MOAB) {
        tree_desert(t, lod, first);
        return;
    }
    if(lvl_area == AREA_DUNES) {
        tree_dunes(t, lod, first);
        return;
    }
    if(lvl_area == AREA_LOWLAND) {
        tree_highland(t, lod, first);
        return;
    }
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
    if(t->kind == 3) {
        /* A snag, where the mountain burned: a bare spar weathered
           silver, black at the foot, broken off short or kinked where
           its top went. */
        float lean = 0.6f * s * (hrand(&seed) - 0.3f), top = (4.5f + 6.5f * hrand(&seed)) * s;
        /* (the short ones snapped off: a blunt top, not a point) */
        float tip = top < 7.0f * s ? 0.13f * s : 0.05f * s;
        V3 b2 = v3(b.x + cosf(yaw) * lean, b.y, b.z + sinf(yaw) * lean);
        int k = lod == 0 ? 5 : lod == 1 ? 4 : 3;
        if(lod < 2) {
            cone(b, 0.36f * s, 0.22f * s, 0, top * 0.55f, k, yaw, snag_col, 0.38f, 0.9f, j);
            cone(b2, 0.22f * s, tip, top * 0.55f, top, k, yaw + 0.6f, snag_col, 0.9f, 1.1f, true);
        }
        else cone(b, 0.36f * s, tip, 0, top, 3, yaw, snag_col, 0.5f, 1.05f, j);
        return;
    }
    if(t->kind == 4) {
        /* Old growth: a trunk a truck wide and bare for three storeys,
           the crown far overhead. What stands along the extreme trail,
           where the trail runs between the trunks and under the boughs. */
        static const float old[3][4] = {{3.5f, 6.4f, 12.4f, 0.42f}, {2.7f, 9.8f, 16.0f, 0.3f}, {1.8f, 13.6f, 19.6f, 0.12f}};
        if(lod < 2) {
            cone(b, 0.36f * s, 0.27f * s, 0, 7.4f * s, lod ? 4 : 5, yaw, trunk_col, 0.6f, 0.82f, j);
            cone_us = crown_us;
            cone_vs = crown_vs;
            for(int i = 0; i < 3; i++) {
                float ao = 0.58f + 0.15f * i;
                cone(b, old[i][0] * s, old[i][3] * s, old[i][1] * s, old[i][2] * s, 5, yaw + i * 0.5f, col, ao, ao + 0.42f,
                     true);
            }
        }
        else if(lod == 2) {
            cone(b, 0.36f * s, 0.3f * s, 0, 6.8f * s, 3, yaw, trunk_col, 0.6f, 0.78f, j);
            cone(b, old[0][0] * s, old[2][3] * s, old[0][1] * s, old[2][2] * s, 5, yaw, col, 0.6f, 1.3f, true);
        }
        else cone(b, 3.2f * s, 0.05f * s, 5.6f * s, 19.4f * s, 3, yaw, col, 0.62f, 1.1f, j);
        return;
    }
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
    uint32_t albedo = look->boulder[seed % 3];
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
                lv(p, a * 0.6f, ring_y[ri] * 1.2f, albedo, n, 0.8f + 0.25f * ring_y[ri]);
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

/* A log: a six-sided cylinder along the capsule, bark round it and pale
   cut ends (or a wheel, in a colour of its own). */
static void log_mesh_in(const lvl_capsule_t *cp, uint32_t round, uint32_t ends) {
    V3 a = v3(cp->ax, cp->ay, cp->az), b = v3(cp->bx, cp->by, cp->bz);
    V3 ax = vnorm(vsub(b, a));
    V3 side = vnorm(vcross(ax, v3(0, 1, 0)));
    V3 up = vcross(side, ax);
    float r = cp->r, len = vlen(vsub(b, a));
    sc_strip_begin(&scenery, h_bark);
    for(int s = 0; s <= 6; s++) {
        float ang = ENG_TAU * (s % 6) / 6.0f;
        V3 n = vadd(vscale(side, cosf(ang)), vscale(up, sinf(ang)));
        lv(vmad(a, n, r), s / 3.0f, 0, round, n, 0.9f);
        lv(vmad(b, n, r), s / 3.0f, len / 1.5f, round, n, 0.9f);
    }
    sc_strip_end(&scenery);
    for(int e = 0; e < 2; e++) {
        V3 c = e ? b : a;
        sc_strip_begin(&scenery, h_flat);
        V3 en = vscale(ax, e ? 1.0f : -1.0f);
        static const int order[6] = {0, 1, 5, 2, 4, 3};
        for(int s = 0; s < 6; s++) {
            float ang = ENG_TAU * order[s] / 6.0f;
            lv(vmad(c, vadd(vscale(side, cosf(ang)), vscale(up, sinf(ang))), r * 0.98f), 0, 0, ends, en, 1.0f);
        }
        sc_strip_end(&scenery);
    }
}

static void log_mesh(const lvl_capsule_t *cp) {
    log_mesh_in(cp, 0xff7a5a3e, 0xffd8b88a);
}

/* The scenery has 64 colours to light (alb_index), and the desert uses
   most of them: these share what is there already where they can. */
#define PANEL_VARNISH 0xff4c2e28   /* the petroglyph slab's face */
#define MARK_BLACK 0xff0e0a0a      /* the mine's dark, a black diamond */
#define MARK_PALE 0xfff0e0b0       /* trail paint and pecked rock: the signs' cream */

/* A drum on its side from a to b: eight flat sides, a cap each end. */
static void drum(V3 a, V3 b, float r, uint32_t col, uint32_t cap) {
    V3 ax = vnorm(vsub(b, a));
    V3 side = vnorm(vcross(ax, v3(0, 1, 0)));
    V3 up = vcross(side, ax);
    sc_strip_begin(&scenery, h_flat);
    for(int s = 0; s <= 8; s++) {
        float ang = ENG_TAU * (s % 8) / 8.0f;
        V3 n = vadd(vscale(side, cosf(ang)), vscale(up, sinf(ang)));
        lv(vmad(a, n, r), 0, 0, col, n, 0.9f);
        lv(vmad(b, n, r), 0, 0, col, n, 0.9f);
    }
    sc_strip_end(&scenery);
    for(int e = 0; e < 2; e++) {
        V3 c = e ? b : a, en = vscale(ax, e ? 1.0f : -1.0f);
        static const int order[8] = {0, 1, 7, 2, 6, 3, 5, 4};
        sc_strip_begin(&scenery, h_flat);
        for(int s = 0; s < 8; s++) {
            float ang = ENG_TAU * order[s] / 8.0f;
            lv(vmad(c, vadd(vscale(side, cosf(ang)), vscale(up, sinf(ang))), r * 0.72f), 0, 0, cap, en, 1.0f);
        }
        sc_strip_end(&scenery);
    }
}

/* A steel tube from a to b, half width w, whichever way it runs. */
static __attribute__((noinline)) void bar(V3 a, V3 b, float w, uint32_t albedo) {
    V3 ax = vnorm(vsub(b, a));
    /* Across it: level, unless it stands upright. */
    V3 u = fabsf(ax.y) > 0.9f ? v3(0, 0, 1) : vnorm(v3(ax.z, 0, -ax.x)), v = vcross(ax, u);
    V3 c[4] = {vscale(vadd(u, v), w), vscale(vsub(u, v), w), vscale(vadd(u, v), -w), vscale(vsub(v, u), w)};
    sc_strip_begin(&scenery, h_flat);
    for(int s = 0; s <= 4; s++) {
        V3 o = c[s & 3];
        V3 nrm = vnorm(vadd(o, c[(s + 3) & 3]));
        lv(vadd(b, o), 0, 0, albedo, nrm, 1.0f);
        lv(vadd(a, o), 0, 0, albedo, nrm, 0.9f);
    }
    sc_strip_end(&scenery);
}

/* A fire's ring of stones on the ground where it burns (campfire.c),
   embers in it, split logs leant together over them. */
static void fire_ring(V3 fire, uint32_t seed) {
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
}

/* A whip and its pennant, as every rig in the dunes carries to be seen
   over a crest (dunes.c lights it after dark). It leans with the wind. */
static void whip(const lvl_prop_t *p) {
    V3 base, top;
    if(!dunes_whip_of(p, &base, &top)) return;
    leg(base, top, 0.022f, 0xffd8d6cc);
    V3 n = v3(-WIND_Z, 0, WIND_X);
    sc_strip_begin(&scenery, h_flat);
    lv(top, 0, 0, 0xffff6a1c, n, 1.0f);
    lv(v3(top.x - WIND_X * 0.03f, top.y - 0.4f, top.z - WIND_Z * 0.03f), 0, 0, 0xffff6a1c, n, 0.9f);
    lv(v3(top.x + WIND_X * 0.62f, top.y - 0.24f, top.z + WIND_Z * 0.62f), 0, 0, 0xffff7a2a, n, 1.0f);
    sc_strip_end(&scenery);
}

/* The four colours a trailer's stripe, a quad's plastics or a buggy's
   frame come in. */
static const uint32_t camp_paint[4] = {0xff2f8a96, 0xffc8542a, 0xff3a5a9a, 0xff7a8a3a};
#define CAMP_TYRE 0xff26262a
#define CAMP_STEEL 0xff3a3a3c
#define CAMP_ALLOY 0xffb8b8b4

#define AT(right, up, fwd) v3(c.x + r.x * (right) + f.x * (fwd), c.y + (up), c.z + r.z * (right) + f.z * (fwd))

/* A folding camp chair standing at q, facing along yaw. */
static __attribute__((noinline)) void camp_chair(V3 q, float yaw, uint32_t cloth) {
    float sy = sinf(yaw), cy = cosf(yaw);
    box(h_flat, v3(q.x, q.y + 0.42f, q.z), 0.26f, 0.02f, 0.24f, yaw, cloth, 1.0f);
    box(h_flat, v3(q.x - sy * 0.25f, q.y + 0.72f, q.z - cy * 0.25f), 0.26f, 0.2f, 0.02f, yaw, cloth, 1.0f);
    /* Its four legs: the back two run on up to carry the back. (A table,
       not `k & 1 ? a : -a` in the loop: GCC 15 for the SH-4 dies in
       gen_reg_rtx on that here.) */
    static const float legs[4][3] = {{-0.25f, -0.23f, 0.92f}, {0.25f, -0.23f, 0.92f}, {-0.25f, 0.23f, 0.44f}, {0.25f, 0.23f, 0.44f}};
    for(int k = 0; k < 4; k++) {
        V3 foot = v3(q.x + cy * legs[k][0] + sy * legs[k][1], q.y - 0.03f, q.z - sy * legs[k][0] + cy * legs[k][1]);
        bar(foot, v3(foot.x, q.y + legs[k][2], foot.z), 0.013f, CAMP_ALLOY);
    }
}

static __attribute__((noinline)) void prop_firepit(const lvl_prop_t *p) {
    /* A fire in its ring, two folding chairs pulled up to it and a
       cooler between them. */
    V3 c = v3(p->x, p->y, p->z);
    uint32_t seed = (uint32_t)(p->x * 13.0f) ^ (uint32_t)(p->z * 7.0f);
    fire_ring(c, seed);
    for(int k = 0; k < 2; k++) {
        /* Each a little round the ring from straight behind it, turned
           to face the fire. */
        float a = p->yaw + ENG_PI + (k ? 0.62f : -0.62f);
        V3 q = v3(c.x + sinf(a) * 1.5f, 0, c.z + cosf(a) * 1.5f);
        q.y = terrain_height(q.x, q.z);
        camp_chair(q, a + ENG_PI, camp_paint[(hash32(seed) >> (2 * k)) & 3]);
    }
    V3 q = v3(c.x - sinf(p->yaw) * 1.75f, 0, c.z - cosf(p->yaw) * 1.75f);
    q.y = terrain_height(q.x, q.z);
    box(h_flat, v3(q.x, q.y + 0.17f, q.z), 0.3f, 0.17f, 0.19f, p->yaw + 0.3f, camp_paint[2], 1.0f);
    box(h_flat, v3(q.x, q.y + 0.37f, q.z), 0.31f, 0.035f, 0.2f, p->yaw + 0.3f, 0xffe6e2d6, 1.0f);
}

static __attribute__((noinline)) void prop_quad(const lvl_prop_t *p) {
    V3 c = v3(p->x, p->y, p->z);
    float cy = cosf(p->yaw), sy = sinf(p->yaw);
    /* A quad bike: four fat tyres, the engine between them, a
       mudguard over each pair, tank and seat, the bars. */
    V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
    uint32_t paint = camp_paint[hash32((uint32_t)(p->x * 19.0f) ^ (uint32_t)(p->z * 23.0f)) & 3];
    for(int k = 0; k < 4; k++) {
        float side = k & 1 ? 1.0f : -1.0f, along = k & 2 ? 0.62f : -0.6f;
        V3 w = AT(side * 0.46f, 0.29f, along);
        drum(vsub(w, vscale(r, 0.13f)), vadd(w, vscale(r, 0.13f)), 0.29f, CAMP_TYRE, CAMP_ALLOY);
    }
    box(h_flat, AT(0, 0.42f, 0), 0.24f, 0.15f, 0.72f, p->yaw, CAMP_STEEL, 1.0f);
    box(h_flat, AT(0, 0.66f, 0.62f), 0.56f, 0.05f, 0.33f, p->yaw, paint, 1.0f);
    box(h_flat, AT(0, 0.66f, -0.62f), 0.56f, 0.05f, 0.35f, p->yaw, paint, 1.0f);
    box(h_flat, AT(0, 0.73f, 0.14f), 0.19f, 0.13f, 0.3f, p->yaw, paint, 1.0f);
    box(h_flat, AT(0, 0.8f, -0.4f), 0.19f, 0.07f, 0.34f, p->yaw, CAMP_TYRE, 1.0f);
    bar(AT(0, 0.74f, 0.44f), AT(0, 1.04f, 0.34f), 0.025f, CAMP_STEEL);
    bar(AT(-0.38f, 1.04f, 0.32f), AT(0.38f, 1.04f, 0.32f), 0.022f, CAMP_STEEL);
    /* A rack on the front guard. */
    box(h_flat, AT(0, 0.74f, 0.7f), 0.36f, 0.015f, 0.2f, p->yaw, CAMP_STEEL, 1.0f);
    whip(p);
}

static __attribute__((noinline)) void prop_rail(const lvl_prop_t *p) {
    V3 c = v3(p->x, p->y, p->z);
    float cy = cosf(p->yaw), sy = sinf(p->yaw);
    /* A sand rail: a bare tube frame and cage, two seats, the
       engine hung out behind them, paddle tyres at the back and
       thin ones on the front beam. */
    V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
    uint32_t paint = camp_paint[hash32((uint32_t)(p->x * 19.0f) ^ (uint32_t)(p->z * 23.0f)) & 3];
    for(int side = -1; side <= 1; side += 2) {
        V3 w = AT(side * 0.84f, 0.43f, -1.05f);
        drum(vsub(w, vscale(r, 0.2f)), vadd(w, vscale(r, 0.2f)), 0.43f, CAMP_TYRE, CAMP_ALLOY);
        w = AT(side * 0.74f, 0.3f, 1.42f);
        drum(vsub(w, vscale(r, 0.08f)), vadd(w, vscale(r, 0.08f)), 0.3f, CAMP_TYRE, CAMP_ALLOY);
        float s = (float)side;
        /* The frame's side: floor rail, nose, hoop, roof, stay. */
        bar(AT(s * 0.3f, 0.4f, 1.6f), AT(s * 0.54f, 0.32f, 0.55f), 0.03f, paint);
        bar(AT(s * 0.54f, 0.32f, 0.55f), AT(s * 0.54f, 0.32f, -0.72f), 0.03f, paint);
        bar(AT(s * 0.54f, 0.32f, -0.72f), AT(s * 0.4f, 0.52f, -1.55f), 0.03f, paint);
        bar(AT(s * 0.3f, 0.4f, 1.6f), AT(s * 0.5f, 0.8f, 0.55f), 0.03f, paint);
        bar(AT(s * 0.54f, 0.32f, 0.55f), AT(s * 0.5f, 0.8f, 0.55f), 0.03f, paint);
        bar(AT(s * 0.5f, 0.8f, 0.55f), AT(s * 0.44f, 1.5f, 0.08f), 0.03f, paint);
        bar(AT(s * 0.44f, 1.5f, 0.08f), AT(s * 0.44f, 1.5f, -0.62f), 0.03f, paint);
        bar(AT(s * 0.54f, 0.32f, -0.72f), AT(s * 0.44f, 1.5f, -0.62f), 0.03f, paint);
        bar(AT(s * 0.44f, 1.5f, -0.62f), AT(s * 0.4f, 0.52f, -1.55f), 0.03f, paint);
        /* A seat: its squab and its back. */
        box(h_flat, AT(s * 0.26f, 0.5f, -0.1f), 0.2f, 0.05f, 0.22f, p->yaw, CAMP_TYRE, 1.0f);
        box(h_flat, AT(s * 0.26f, 0.84f, -0.36f), 0.2f, 0.32f, 0.05f, p->yaw, CAMP_TYRE, 1.0f);
    }
    bar(AT(-0.44f, 1.5f, 0.08f), AT(0.44f, 1.5f, 0.08f), 0.03f, paint);
    bar(AT(-0.44f, 1.5f, -0.62f), AT(0.44f, 1.5f, -0.62f), 0.03f, paint);
    bar(AT(-0.4f, 0.52f, -1.55f), AT(0.4f, 0.52f, -1.55f), 0.03f, paint);
    bar(AT(-0.74f, 0.3f, 1.42f), AT(0.74f, 0.3f, 1.42f), 0.035f, CAMP_STEEL);
    bar(AT(-0.84f, 0.43f, -1.05f), AT(0.84f, 0.43f, -1.05f), 0.04f, CAMP_STEEL);
    box(h_flat, AT(0, 0.3f, -0.05f), 0.52f, 0.02f, 0.72f, p->yaw, CAMP_STEEL, 1.0f);
    /* The engine, and its two pipes up past the cage. */
    box(h_flat, AT(0, 0.62f, -1.12f), 0.3f, 0.2f, 0.3f, p->yaw, 0xff8a8a88, 1.0f);
    for(int side = -1; side <= 1; side += 2)
        bar(AT(side * 0.2f, 0.8f, -1.3f), AT(side * 0.24f, 1.25f, -1.72f), 0.035f, CAMP_ALLOY);
    whip(p);
#undef AT
}
#undef AT

#define PROP_NEAR_M 250.0f   /* a prop's detail is drawn to here */

/* A trailer or a powerline frame from past PROP_NEAR_M: what of it still
   shows. The same boxes and poles as prop()'s, so nothing moves at the
   change. */
static void prop_far(const lvl_prop_t *p) {
    V3 c = v3(p->x, p->y, p->z);
    float cy = cosf(p->yaw), sy = sinf(p->yaw);
    V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
    if(p->kind == PROP_CAMPER) {
        uint32_t stripe = camp_paint[hash32((uint32_t)(p->x * 19.0f) ^ (uint32_t)(p->z * 23.0f)) & 3];
        box_seg(h_flat, v3(c.x, c.y + 1.62f, c.z), 1.15f, 1.02f, 2.9f, p->yaw, 0xffe6e2d6, 1.0f, 1.6f);
        box(h_flat, v3(c.x, c.y + 1.2f, c.z), 1.17f, 0.13f, 2.92f, p->yaw, stripe, 1.0f);
        V3 in0 = vadd(c, v3(r.x * 1.16f + f.x * 1.9f, 2.6f, r.z * 1.16f + f.z * 1.9f)),
           in1 = vadd(c, v3(r.x * 1.16f - f.x * 1.3f, 2.6f, r.z * 1.16f - f.z * 1.3f));
        V3 o0 = vadd(in0, v3(r.x * 2.3f, -0.55f, r.z * 2.3f)), o1 = vadd(in1, v3(r.x * 2.3f, -0.55f, r.z * 2.3f));
        V3 an = vnorm(v3(r.x * 0.24f, 1.0f, r.z * 0.24f));
        sc_strip_begin(&scenery, h_flat);
        lv(in0, 0, 0, stripe, an, 1.0f);
        lv(in1, 0, 0, stripe, an, 1.0f);
        lv(o0, 0, 0, stripe, an, 0.92f);
        lv(o1, 0, 0, stripe, an, 0.92f);
        sc_strip_end(&scenery);
    }
    else if(p->kind == PROP_PYLON) {
        const uint32_t wood = 0xff6c5a48;
        for(int side = -1; side <= 1; side += 2) {
            V3 q = vadd(c, vscale(r, side * 2.6f));
            leg(v3(q.x, terrain_height(q.x, q.z) - 0.5f, q.z), v3(q.x, c.y + 15.0f, q.z), 0.19f, wood);
        }
        box(h_bark, v3(c.x, c.y + 13.7f, c.z), 4.7f, 0.13f, 0.11f, p->yaw, wood, 0.5f);
    }
}

static void prop(const lvl_prop_t *p) {
    V3 c = v3(p->x, p->y, p->z);
    float cy = cosf(p->yaw), sy = sinf(p->yaw);
    switch(p->kind) {
        case PROP_CABIN: {
            /* Logs and a red roof on the mountain; in the desert a line
               cabin's bleached boards under rusted tin; in the Highlands
               a bothy, its stone whitewashed under a red tin roof. */
            bool dry = lvl_area == AREA_MOAB, bothy = lvl_area == AREA_LOWLAND;
            box_seg(bothy ? h_rock : h_bark, v3(c.x, c.y + 1.4f, c.z), 4.2f, 1.6f, 3.2f, p->yaw,
                    dry ? 0xffb09a78 : bothy ? 0xffe6e2d6 : 0xffa0703e, 0.4f, 1.5f);
            /* The roof: a ridge along the cabin's length. */
            V3 ax = v3(cy * 4.8f, 0, -sy * 4.8f), az = v3(sy * 3.8f, 0, cy * 3.8f);
            V3 e0 = vadd(c, az), e1 = vsub(c, az);
            float eave = c.y + 3.0f, ridge = c.y + (dry ? 4.3f : 5.0f);
            uint32_t rc = dry ? 0xff8e5232 : 0xff9a3a2a;
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
                uint32_t gc = dry ? 0xffb09a78 : bothy ? 0xffe6e2d6 : 0xffb88850;
                lv(v3(c.x + o.x - ax.x * 0.88f, eave, c.z + o.z - ax.z * 0.88f), 0, 0, gc, n, 0.95f);
                lv(v3(c.x + o.x + ax.x * 0.88f, eave, c.z + o.z + ax.z * 0.88f), 0, 0, gc, n, 0.95f);
                lv(v3(c.x + o.x, ridge - 0.1f, c.z + o.z), 0, 0, gc, n, 0.95f);
                sc_strip_end(&scenery);
            }
            /* A stone chimney, or a stovepipe. */
            if(dry) box(h_flat, v3(c.x + cy * 2.6f, c.y + 4.3f, c.z - sy * 2.6f), 0.14f, 0.9f, 0.14f, p->yaw, 0xff3a4246, 1.0f);
            else box(h_rock, v3(c.x + cy * 2.6f, c.y + 3.6f, c.z - sy * 2.6f), 0.4f, 1.6f, 0.4f, p->yaw, 0xff8a8a90, 0.5f);
            break;
        }
        case PROP_SIGN:
            box(h_bark, v3(c.x, c.y + 1.0f, c.z), 0.1f, 1.0f, 0.1f, p->yaw, 0xff6a4a30, 1.0f);
            /* Forest green on the mountain, the desert's routed brown, a
               road sign's white in the Highlands. */
            box(h_flat, v3(c.x, c.y + 1.9f, c.z), 0.9f, 0.35f, 0.06f, p->yaw,
                lvl_area == AREA_CASCADE ? 0xff3c6a44 : lvl_area == AREA_LOWLAND ? 0xffe6e2d6 : 0xff74482c, 1.0f);
            box(h_flat, v3(c.x, c.y + 1.9f, c.z), 0.75f, 0.06f, 0.07f, p->yaw, lvl_area == AREA_LOWLAND ? 0xff2c2c30 : 0xfff0e0b0, 1.0f);
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
            V3 fire;
            if(campfire_of(p, &fire)) fire_ring(fire, (uint32_t)(p->x * 13.0f) ^ (uint32_t)(p->z * 7.0f));
            break;
        }
        case PROP_FIREPIT: prop_firepit(p); break;
        case PROP_QUAD: prop_quad(p); break;
        case PROP_RAIL: prop_rail(p); break;
        case PROP_KIOSK: {
            /* A trailhead notice board: two posts, the board with its
               map, a strip of roof to keep the sun off it. */
            V3 r = v3(cy, 0, -sy);
            for(int side = -1; side <= 1; side += 2)
                box(h_bark, v3(c.x + r.x * side * 1.0f, c.y + 1.2f, c.z + r.z * side * 1.0f), 0.08f, 1.3f, 0.08f, p->yaw,
                    0xff6a4a30, 1.0f);
            box(h_flat, v3(c.x, c.y + 1.55f, c.z), 1.0f, 0.62f, 0.05f, p->yaw, 0xff7a5638, 1.0f);
            box(h_flat, v3(c.x, c.y + 1.55f, c.z), 0.86f, 0.5f, 0.06f, p->yaw, 0xffeadfc0, 1.0f);
            box(h_flat, v3(c.x - r.x * 0.3f, c.y + 1.62f, c.z - r.z * 0.3f), 0.42f, 0.3f, 0.07f, p->yaw, 0xffd8905c, 1.0f);
            box(h_bark, v3(c.x, c.y + 2.56f, c.z), 1.3f, 0.05f, 0.42f, p->yaw, 0xff8a6440, 0.8f);
            break;
        }
        case PROP_RAMADA: {
            /* Shade for a picnic table: four posts and a slatted roof. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            for(int k = 0; k < 4; k++) {
                float sx = (k & 1 ? 1.0f : -1.0f) * 1.9f, sz = (k & 2 ? 1.0f : -1.0f) * 1.3f;
                box(h_bark, v3(c.x + r.x * sx + f.x * sz, c.y + 1.25f, c.z + r.z * sx + f.z * sz), 0.07f, 1.3f, 0.07f, p->yaw,
                    0xff6a4a30, 1.0f);
            }
            box(h_bark, v3(c.x, c.y + 2.6f, c.z), 2.3f, 0.06f, 1.7f, p->yaw, 0xffa07850, 0.9f);
            box(h_bark, v3(c.x, c.y + 0.75f, c.z), 0.9f, 0.05f, 0.4f, p->yaw, 0xffa07850, 1.0f);
            box(h_bark, v3(c.x, c.y + 0.45f, c.z), 0.9f, 0.04f, 0.15f, p->yaw, 0xff8a6440, 1.0f);
            box(h_bark, v3(c.x, c.y + 0.37f, c.z), 0.7f, 0.37f, 0.06f, p->yaw, 0xff6a4a30, 1.0f);
            break;
        }
        case PROP_PYLON: {
            /* A powerline's H-frame: two poles (the level's posts), a
               crossarm past them both ways, braces crossed between, an
               insulator for each wire (dunes.c hangs them). */
            V3 r = v3(cy, 0, -sy);
            const uint32_t wood = 0xff6c5a48;
            for(int side = -1; side <= 1; side += 2) {
                V3 q = vadd(c, vscale(r, side * 2.6f));
                leg(v3(q.x, terrain_height(q.x, q.z) - 0.5f, q.z), v3(q.x, c.y + 15.0f, q.z), 0.19f, wood);
                V3 lo = vadd(c, vscale(r, side * 2.6f)), hi = vadd(c, vscale(r, -side * 2.6f));
                leg(v3(lo.x, c.y + 7.0f, lo.z), v3(hi.x, c.y + 12.6f, hi.z), 0.07f, wood);
            }
            box(h_bark, v3(c.x, c.y + 13.7f, c.z), 4.7f, 0.13f, 0.11f, p->yaw, wood, 0.5f);
            for(int k = -1; k <= 1; k++)
                box(h_flat, v3(c.x + r.x * k * 4.2f, c.y + 13.3f, c.z + r.z * k * 4.2f), 0.07f, 0.28f, 0.07f, p->yaw, 0xff6a8088, 1.0f);
            break;
        }
        case PROP_CAMPER: {
            /* A travel trailer parked up for the weekend: the box, its
               stripe, windows and door, the hitch, an awning out over
               the door side. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            uint32_t seed = (uint32_t)(p->x * 19.0f) ^ (uint32_t)(p->z * 23.0f);
            uint32_t stripe = camp_paint[hash32(seed) & 3];
            const uint32_t shell = 0xffe6e2d6, glass = 0xff2a363c;
            box_seg(h_flat, v3(c.x, c.y + 1.62f, c.z), 1.15f, 1.02f, 2.9f, p->yaw, shell, 1.0f, 1.6f);
            box(h_flat, v3(c.x, c.y + 1.2f, c.z), 1.17f, 0.13f, 2.92f, p->yaw, stripe, 1.0f);
            box(h_flat, v3(c.x, c.y + 0.5f, c.z), 0.95f, 0.12f, 2.6f, p->yaw, 0xff3a3a3c, 1.0f);
            box(h_flat, vadd(c, v3(f.x * 1.2f, 2.0f, f.z * 1.2f)), 1.18f, 0.27f, 0.62f, p->yaw, glass, 1.0f);
            box(h_flat, vadd(c, v3(-f.x * 1.5f, 2.0f, -f.z * 1.5f)), 1.18f, 0.27f, 0.4f, p->yaw, glass, 1.0f);
            box(h_flat, vadd(c, v3(f.x * 2.9f, 2.0f, f.z * 2.9f)), 0.7f, 0.27f, 0.03f, p->yaw, glass, 1.0f);
            box(h_flat, vadd(c, v3(r.x * 1.16f - f.x * 0.3f, 1.52f, r.z * 1.16f - f.z * 0.3f)), 0.02f, 0.88f, 0.34f, p->yaw, 0xffcfcabb, 1.0f);
            box(h_flat, vadd(c, v3(-f.x * 0.6f, 2.74f, -f.z * 0.6f)), 0.42f, 0.11f, 0.5f, p->yaw, 0xffd0d0cc, 1.0f);
            for(int side = -1; side <= 1; side += 2) {
                V3 w = vadd(c, v3(r.x * side * 1.0f - f.x * 0.5f, 0.38f, r.z * side * 1.0f - f.z * 0.5f));
                drum(vsub(w, vscale(r, 0.13f)), vadd(w, vscale(r, 0.13f)), 0.38f, CAMP_TYRE, CAMP_ALLOY);
                V3 a = vadd(c, v3(f.x * 2.9f + r.x * side * 0.75f, 0.58f, f.z * 2.9f + r.z * side * 0.75f));
                leg(a, vadd(c, v3(f.x * 4.2f, 0.55f, f.z * 4.2f)), 0.04f, 0xff3a3a3c);
            }
            box(h_flat, vadd(c, v3(f.x * 4.1f, 0.28f, f.z * 4.1f)), 0.04f, 0.3f, 0.04f, p->yaw, 0xff8a8a88, 1.0f);
            /* The awning, and the two poles under its outer edge. */
            V3 in0 = vadd(c, v3(r.x * 1.16f + f.x * 1.9f, 2.6f, r.z * 1.16f + f.z * 1.9f)),
               in1 = vadd(c, v3(r.x * 1.16f - f.x * 1.3f, 2.6f, r.z * 1.16f - f.z * 1.3f));
            V3 o0 = vadd(in0, v3(r.x * 2.3f, -0.55f, r.z * 2.3f)), o1 = vadd(in1, v3(r.x * 2.3f, -0.55f, r.z * 2.3f));
            V3 an = vnorm(v3(r.x * 0.24f, 1.0f, r.z * 0.24f));
            sc_strip_begin(&scenery, h_flat);
            lv(in0, 0, 0, stripe, an, 1.0f);
            lv(in1, 0, 0, stripe, an, 1.0f);
            lv(o0, 0, 0, stripe, an, 0.92f);
            lv(o1, 0, 0, stripe, an, 0.92f);
            sc_strip_end(&scenery);
            leg(v3(o0.x, terrain_height(o0.x, o0.z) - 0.1f, o0.z), o0, 0.025f, 0xffb8b8b4);
            leg(v3(o1.x, terrain_height(o1.x, o1.z) - 0.1f, o1.z), o1, 0.025f, 0xffb8b8b4);
            break;
        }
        case PROP_FLAG: {
            /* A whip stuck in the sand to mark a place. */
            whip(p);
            break;
        }
        case PROP_STONEBRIDGE: {
            /* An arch of stone over the burn: the deck (p->y its top, the
               level's box under the tyres), a parapet each side (its
               capsules), the arch and the walls over it, an abutment
               into each bank. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            const uint32_t stone = 0xff8e8880, pale = 0xffa8a49c;
            const float hx = LVL_BRIDGE_HX, hz = LVL_BRIDGE_HZ, span = 4.4f, rise = 2.3f, crown = c.y - 0.55f;
            box(h_rock, v3(c.x, c.y - 0.3f, c.z), hx, 0.3f, hz, p->yaw, pale, 0.4f);
            for(int side = -1; side <= 1; side += 2) {
                V3 m = vadd(c, vscale(r, side * (hx - 0.25f)));
                box_seg(h_rock, v3(m.x, c.y + 0.4f, m.z), 0.27f, 0.42f, hz + 0.9f, p->yaw, stone, 0.5f, 2.0f);
                V3 e = vadd(c, vscale(f, side * (hz - 1.1f)));
                box(h_rock, v3(e.x, c.y - 2.3f, e.z), hx + 0.15f, 1.9f, 1.3f, p->yaw, stone, 0.4f);
            }
            /* The arch: its soffit, then the wall over it either side. */
            enum { ARC = 8 };
            V3 arc[ARC + 1];
            for(int i = 0; i <= ARC; i++) {
                float a = ENG_PI * i / ARC;
                arc[i] = v3(-cosf(a) * span, crown - rise + sinf(a) * rise, 0);   /* x: along the bridge */
            }
            sc_strip_begin(&scenery, h_rock);
            for(int i = 0; i <= ARC; i++) {
                float a = ENG_PI * i / ARC;
                V3 n = vadd(vscale(f, cosf(a) * 0.6f), v3(0, -sinf(a), 0));
                V3 m = vadd(c, vscale(f, arc[i].x));
                lv(v3(m.x - r.x * hx, arc[i].y, m.z - r.z * hx), i * 0.5f, 0, stone, n, 0.62f);
                lv(v3(m.x + r.x * hx, arc[i].y, m.z + r.z * hx), i * 0.5f, 2.0f, stone, n, 0.62f);
            }
            sc_strip_end(&scenery);
            for(int side = -1; side <= 1; side += 2) {
                sc_strip_begin(&scenery, h_rock);
                for(int k = 0; k <= ARC; k++) {
                    int i = side > 0 ? k : ARC - k;   /* wound to face outward */
                    V3 m = vadd(vadd(c, vscale(f, arc[i].x)), vscale(r, side * hx));
                    lv(v3(m.x, c.y - 0.6f, m.z), i * 0.5f, 0, stone, vscale(r, (float)side), 0.92f);
                    lv(v3(m.x, arc[i].y, m.z), i * 0.5f, (c.y - 0.6f - arc[i].y) * 0.4f, stone, vscale(r, (float)side), 0.78f);
                }
                sc_strip_end(&scenery);
            }
            break;
        }
        case PROP_RUIN: {
            /* A shieling: what is left of its walls, one gable standing. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            const uint32_t stone = 0xff8e8880, moss = 0xff6c6660;
            box_seg(h_rock, vadd(c, v3(-r.x * 2.8f, 0.55f, -r.z * 2.8f)), 0.36f, 0.95f, 2.3f, p->yaw, stone, 0.5f, 1.6f);
            box_seg(h_rock, vadd(c, v3(r.x * 2.8f, 0.2f, r.z * 2.8f)), 0.36f, 0.6f, 2.3f, p->yaw, moss, 0.5f, 1.6f);
            V3 g = vadd(c, vscale(f, 2.2f));
            box_seg(h_rock, v3(g.x, c.y + 0.9f, g.z), 3.1f, 1.3f, 0.36f, p->yaw, stone, 0.5f, 1.6f);
            for(int face = -1; face <= 1; face += 2) {
                V3 o = vscale(f, face * 0.36f), n = vscale(f, (float)face);
                sc_strip_begin(&scenery, h_rock);
                V3 a = vadd(g, vadd(o, vscale(r, -3.1f * face))), b = vadd(g, vadd(o, vscale(r, 3.1f * face)));
                lv(v3(a.x, c.y + 2.2f, a.z), 0, 1.0f, stone, n, 0.95f);
                lv(v3(b.x, c.y + 2.2f, b.z), 3.0f, 1.0f, stone, n, 0.95f);
                lv(v3(g.x + o.x, c.y + 3.9f, g.z + o.z), 1.5f, 0, stone, n, 1.0f);
                sc_strip_end(&scenery);
            }
            for(int side = -1; side <= 1; side += 2) {
                V3 q = vadd(vsub(c, vscale(f, 2.2f)), vscale(r, side * 2.0f));
                box(h_rock, v3(q.x, c.y + 0.25f, q.z), 1.0f, 0.55f + 0.2f * side, 0.36f, p->yaw, side > 0 ? moss : stone, 0.5f);
            }
            break;
        }
        case PROP_MAST: {
            /* A relay mast: four legs drawing together (the level's
               posts) in red and white, braced, a dish or two near the
               top, the hut its gear lives in beside it. */
            const uint32_t red = 0xffe04a30, white = 0xffe6e2d6;
            V3 foot[4], top[4];
            int n = 0;
            for(int k = 0; k < LVL_POSTS && n < 4; k++) {
                const lvl_post_t *q = &lvl_posts[k];
                if(fabsf(q->x1 - c.x) > 2.0f || fabsf(q->z1 - c.z) > 2.0f) continue;
                foot[n] = v3(q->x0, q->y0, q->z0);
                top[n++] = v3(q->x1, q->y1, q->z1);
            }
            enum { BAYS = 5 };
            /* (the posts come round the square: south-west, south-east,
               north-west, north-east) */
            static const int face[4][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}};
            for(int bay = 0; bay < BAYS && n == 4; bay++) {
                uint32_t col = bay & 1 ? white : red;
                float t0 = (float)bay / BAYS, t1 = (float)(bay + 1) / BAYS;
                for(int k = 0; k < 4; k++) {
                    leg(vadd(foot[k], vscale(vsub(top[k], foot[k]), t0)), vadd(foot[k], vscale(vsub(top[k], foot[k]), t1)), 0.09f, col);
                    /* A brace across each face, leaning the other way in
                       the next bay up. */
                    int a = face[k][bay & 1], b = face[k][!(bay & 1)];
                    leg(vadd(foot[a], vscale(vsub(top[a], foot[a]), t0)), vadd(foot[b], vscale(vsub(top[b], foot[b]), t1)), 0.04f, col);
                }
            }
            float ty = c.y + 17.0f;
            leg(v3(c.x, ty - 0.3f, c.z), v3(c.x, ty + 3.4f, c.z), 0.05f, white);
            box(h_flat, v3(c.x, ty, c.z), 0.5f, 0.06f, 0.5f, 0, 0xff2c2c30, 1.0f);
            box(h_flat, v3(c.x + 0.75f, ty - 2.4f, c.z), 0.14f, 0.62f, 0.62f, 0.3f, white, 1.0f);
            box(h_flat, v3(c.x - 0.2f, ty - 4.6f, c.z - 0.8f), 0.5f, 0.5f, 0.12f, -0.4f, white, 1.0f);
            /* The hut (the level's box). */
            float hx = c.x + 6.5f, hz = c.z - 4.0f, hy = terrain_height(hx, hz);
            box_seg(h_flat, v3(hx, hy + 1.1f, hz), 1.7f, 1.3f, 1.3f, 0, 0xff8e8880, 1.0f, 1.4f);
            box(h_flat, v3(hx, hy + 2.46f, hz), 1.9f, 0.07f, 1.5f, 0, 0xff2c2c30, 1.0f);
            box(h_flat, v3(hx - 0.5f, hy + 0.95f, hz - 1.31f), 0.42f, 0.95f, 0.03f, 0, white, 1.0f);
            break;
        }
        case PROP_BOAT: {
            /* A rowing boat pulled up above the water: clinker sides to a
               point at the bow, a transom, two thwarts. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            const uint32_t hull = 0xff3e5e48, inside = 0xffa07850;
            static const float st[5][2] = {{-2.0f, 0.62f}, {-1.0f, 0.82f}, {0.2f, 0.8f}, {1.3f, 0.52f}, {2.1f, 0.0f}};
#define BP(along, wide, up) v3(c.x + f.x * (along) + r.x * (wide), c.y + (up), c.z + f.z * (along) + r.z * (wide))
            for(int side = -1; side <= 1; side += 2) {
                sc_strip_begin(&scenery, h_flat);
                for(int k = 0; k < 5; k++) {
                    int i = side > 0 ? k : 4 - k;
                    V3 n = vnorm(vadd(vscale(r, (float)side), v3(0, -0.5f, 0)));
                    lv(BP(st[i][0], side * st[i][1], 0.74f + 0.05f * i), 0, 0, hull, n, 1.0f);
                    lv(BP(st[i][0] * 0.9f, side * st[i][1] * 0.35f, 0.12f), 0, 0, hull, n, 0.7f);
                }
                sc_strip_end(&scenery);
                /* The same side from within. */
                sc_strip_begin(&scenery, h_flat);
                for(int k = 0; k < 5; k++) {
                    int i = side > 0 ? 4 - k : k;
                    V3 n = vnorm(vadd(vscale(r, -(float)side), v3(0, 0.6f, 0)));
                    lv(BP(st[i][0], side * st[i][1] * 0.94f, 0.72f + 0.05f * i), 0, 0, inside, n, 0.9f);
                    lv(BP(st[i][0] * 0.9f, side * st[i][1] * 0.35f, 0.16f), 0, 0, inside, n, 0.6f);
                }
                sc_strip_end(&scenery);
            }
            sc_strip_begin(&scenery, h_flat);
            lv(BP(-2.0f, -0.62f, 0.74f), 0, 0, hull, vscale(f, -1.0f), 0.95f);
            lv(BP(-2.0f, 0.62f, 0.74f), 0, 0, hull, vscale(f, -1.0f), 0.95f);
            lv(BP(-1.8f, -0.22f, 0.12f), 0, 0, hull, vscale(f, -1.0f), 0.75f);
            lv(BP(-1.8f, 0.22f, 0.12f), 0, 0, hull, vscale(f, -1.0f), 0.75f);
            sc_strip_end(&scenery);
#undef BP
            for(int k = 0; k < 2; k++) {
                V3 q = vadd(c, vscale(f, k ? 0.5f : -0.9f));
                box(h_bark, v3(q.x, c.y + 0.6f, q.z), 0.74f, 0.03f, 0.13f, p->yaw, 0xffa07850, 1.0f);
            }
            break;
        }
        case PROP_POLE:
            /* A pole of the line over the pass, its arm across the road. */
            box_seg(h_bark, v3(c.x, c.y + 3.1f, c.z), 0.11f, 3.4f, 0.11f, p->yaw, 0xff6a4a30, 1.0f, 1.0f);
            box(h_bark, v3(c.x, c.y + 6.0f, c.z), 0.8f, 0.06f, 0.06f, p->yaw, 0xff6a4a30, 1.0f);
            for(int side = -1; side <= 1; side += 2)
                box(h_flat, v3(c.x + cy * side * 0.62f, c.y + 6.15f, c.z - sy * side * 0.62f), 0.05f, 0.1f, 0.05f, p->yaw,
                    0xffe6e2d6, 1.0f);
            break;
        case PROP_TRIG:
            /* A survey pillar: concrete, a metre and a bit, on the very top. */
            sc_strip_begin(&scenery, h_flat);
            cone_us = cone_vs = 0;
            cone(c, 0.34f, 0.2f, -0.2f, 1.22f, 4, ENG_PI * 0.25f, 0xffe6e2d6, 0.8f, 1.0f, false);
            sc_strip_end(&scenery);
            box(h_flat, v3(c.x, c.y + 1.23f, c.z), 0.15f, 0.02f, 0.15f, 0, 0xffa8a49c, 1.0f);
            break;
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
            break;
        }
        case PROP_PANEL: {
            /* A slab stood against the canyon wall, black with varnish
               (the figures pecked into it are a mark: marks_build), and
               what broke off it at its foot. */
            V3 r = v3(cy, 0, -sy);
            uint32_t seed = (uint32_t)(p->x * 19.0f) ^ (uint32_t)(p->z * 23.0f);
            box_seg(h_rock, v3(c.x, c.y + 1.5f, c.z), 1.9f, 1.5f, 0.5f, p->yaw, PANEL_VARNISH, 0.35f, 1.3f);
            boulder(v3(c.x + r.x * 2.3f, c.y + 0.7f, c.z + r.z * 2.3f), 0.9f, seed, 0);
            boulder(v3(c.x - r.x * 2.5f + sy * 0.5f, c.y + 0.6f, c.z - r.z * 2.5f + cy * 0.5f), 0.65f, seed * 5u, 0);
            /* (and the rest of the fall behind and over it, so it is a
               rock among rocks and not a board) */
            boulder(v3(c.x - r.x * 1.3f - sy * 0.5f, c.y + 2.9f, c.z - r.z * 1.3f - cy * 0.5f), 1.0f, seed * 7u, 0);
            boulder(v3(c.x + r.x * 1.1f - sy * 0.7f, c.y + 3.1f, c.z + r.z * 1.1f - cy * 0.7f), 1.2f, seed * 11u, 0);
            boulder(v3(c.x + r.x * 2.6f - sy * 0.9f, c.y + 2.0f, c.z + r.z * 2.6f - cy * 0.9f), 1.1f, seed * 13u, 0);
            boulder(v3(c.x - r.x * 2.8f - sy * 0.8f, c.y + 1.9f, c.z - r.z * 2.8f - cy * 0.8f), 0.95f, seed * 17u, 0);
            break;
        }
        case PROP_ADIT: {
            /* A mine's portal: the dark of the drift, a set of timbers
               round it, rails out across the dump and an ore cart left
               at the end of them. */
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
            const uint32_t timber = 0xff6a4a30, rust = 0xff8e5232;
            box(h_flat, vadd(c, v3(-f.x * 0.75f, 1.0f, -f.z * 0.75f)), 0.98f, 1.1f, 1.0f, p->yaw, MARK_BLACK, 1.0f);
            for(int side = -1; side <= 1; side += 2)
                box(h_bark, vadd(c, v3(r.x * side * 1.12f + f.x * 0.36f, 1.1f, r.z * side * 1.12f + f.z * 0.36f)), 0.14f, 1.25f, 0.14f,
                    p->yaw + side * 0.06f, timber, 1.0f);
            box(h_bark, vadd(c, v3(f.x * 0.36f, 2.42f, f.z * 0.36f)), 1.5f, 0.14f, 0.16f, p->yaw, timber, 1.0f);
            box(h_bark, vadd(c, v3(f.x * 0.2f, 2.2f, f.z * 0.2f)), 1.0f, 0.05f, 0.45f, p->yaw, 0xff4a3426, 1.0f);
            for(int side = -1; side <= 1; side += 2)
                box(h_flat, vadd(c, v3(r.x * side * 0.38f + f.x * 3.3f, 0.07f, r.z * side * 0.38f + f.z * 3.3f)), 0.035f, 0.05f, 3.7f,
                    p->yaw, rust, 1.0f);
            for(float t = 0.4f; t < 7.0f; t += 1.1f)
                box(h_bark, vadd(c, v3(f.x * t, 0.03f, f.z * t)), 0.62f, 0.04f, 0.09f, p->yaw, 0xff4a3426, 1.0f);
            V3 k = vadd(c, vscale(f, 6.4f));
            box(h_flat, vadd(k, v3(0, 0.8f, 0)), 0.5f, 0.36f, 0.72f, p->yaw, rust, 1.0f);
            box(h_flat, vadd(k, v3(0, 1.13f, 0)), 0.42f, 0.04f, 0.64f, p->yaw, 0xff3a2a20, 1.0f);
            boulder(vadd(k, v3(0.05f, 1.2f, -0.1f)), 0.3f, 0x5eedu, 1);
            for(int w = 0; w < 4; w++) {
                V3 q = vadd(k, vadd(vscale(f, w & 1 ? 0.45f : -0.45f), vscale(r, w & 2 ? 0.38f : -0.38f)));
                V3 a = vsub(q, vscale(r, 0.05f)), b = vadd(q, vscale(r, 0.05f));
                lvl_capsule_t wh = {a.x, c.y + 0.22f, a.z, b.x, c.y + 0.22f, b.z, 0.2f};
                log_mesh_in(&wh, 0xff3a2a20, 0xff4a3a30);
            }
            break;
        }
        case PROP_PLATE: {
            /* A trail's rating, as a ski run's: a shape on a pale board,
               both sides. Var 3 is the extreme trail's: two black
               diamonds side by side on a wider board. */
            static const uint32_t shade[4] = {0xff2f9a48, 0xff2a5cc4, MARK_BLACK, MARK_BLACK};
            static const int sides[4] = {10, 4, 4, 4};
            static const float rad[4] = {0.23f, 0.3f, 0.27f, 0.25f}, rot[4] = {0, ENG_PI * 0.25f, 0, 0};
            int var = p->var < 4 ? p->var : 2, shapes = var == 3 ? 2 : 1;
            V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy), m = v3(c.x, c.y + 1.72f, c.z);
            box(h_bark, v3(c.x, c.y + 0.75f, c.z), 0.06f, 0.75f, 0.06f, p->yaw, 0xff6a4a30, 1.0f);
            box(h_flat, m, var == 3 ? 0.56f : 0.36f, 0.36f, 0.035f, p->yaw, 0xffeadfc0, 1.0f);
            for(int face = -1; face <= 1; face += 2)
                for(int sh = 0; sh < shapes; sh++) {
                    V3 o = vadd(m, vscale(f, face * 0.045f)), rr = vscale(r, (float)-face), n = vscale(f, (float)face);
                    if(shapes == 2) o = vadd(o, vscale(r, sh ? 0.27f : -0.27f));
                    int ns = sides[var];
                    sc_strip_begin(&scenery, h_flat);
                    for(int k = 0; k < ns; k++) {
                        /* Round the shape from both ends at once: a strip. */
                        int idx = k & 1 ? ns - (k + 1) / 2 : k / 2;
                        float a = rot[var] + ENG_TAU * idx / ns;
                        lv(vadd(o, vadd(vscale(rr, sinf(a) * rad[var]), v3(0, cosf(a) * rad[var], 0))), 0, 0, shade[var], n,
                           1.0f);
                    }
                    sc_strip_end(&scenery);
                }
            break;
        }
        case PROP_TRACKS: break;   /* marks on the rock, no more (marks_build) */
    }
}

/* ---- Marks on the ground ----

   What lies on the rock and takes its shape: the pair of black lines
   tyres leave up the slickrock wherever a route crosses it, the dashes
   of white paint that blaze the way over bare rock, a dinosaur's tracks,
   and the figures on the petroglyph panel. Thin translucent strips a hand
   above the ground, built once and drawn within MARK_FAR_M: that is as
   far as a trail's ground stays on the grid they were laid over (further
   off, the darker band in the ground's own colours is the tyre marks). */
#define MARK_MAX 128
#define MARK_FAR_M 90.0f
#define MARK_LIFT 0.05f
#define MARK_STEP 1.25f            /* metres between a line's vertices */
#define MARK_RUN 20                /* steps to a chunk */
#define RUBBER_HALF 0.17f
#define RUBBER_TRACK 0.84f         /* each line's distance from the middle */
#define RUBBER_RGB 0x241a18u
static uint16_t mark_chunks[MARK_MAX];
static int mark_n;

static V3 mark_at(float x, float z, V3 *n) {
    V3 nn;
    float y = terrain_height_n(x, z, &nn);
    if(n) *n = nn;
    return v3(x + nn.x * MARK_LIFT, y + nn.y * MARK_LIFT, z + nn.z * MARK_LIFT);
}

/* Smooth noise along a line, 0..1. */
static float mark_noise(float s, uint32_t seed) {
    int i = (int)floorf(s);
    float f = s - i;
    f = f * f * (3 - 2 * f);
    float a = (float)(hash32((uint32_t)i * 2246822519u + seed) & 0xffff) / 65535.0f;
    float b = (float)(hash32((uint32_t)(i + 1) * 2246822519u + seed) & 0xffff) / 65535.0f;
    return a + (b - a) * f;
}

/* A flat quad on the ground about (x, z): half_w across, half_l along
   dir, its texture's v running along it. */
static void mark_quad(float x, float z, V3 dir, float half_w, float half_l, float u0, float v0, float u1, float v1,
                      uint32_t col, bool lit_it) {
    V3 side = v3(dir.z, 0, -dir.x);
    for(int k = 0; k < 4; k++) {
        float a = k & 2 ? half_l : -half_l, b = k & 1 ? half_w : -half_w;
        V3 n, q = mark_at(x + dir.x * a + side.x * b, z + dir.z * a + side.z * b, &n);
        float u = k & 1 ? u1 : u0, v = k & 2 ? v0 : v1;
        if(lit_it) lv(q, u, v, col, n, 1.0f);
        else lc(q, u, v, col);
    }
}

static void mark_chunk_begin(V3 lp, float far) {
    sc_chunk_begin(&scenery, HF_TR, 0, far, TAG_PROPS);
    sc_chunk_lod_point(&scenery, lp);
}

static void mark_chunk_end(uint32_t at) {
    sc_chunk_end(&scenery);
    if(scenery.chunk_n > at && mark_n < MARK_MAX) mark_chunks[mark_n++] = (uint16_t)at;
}

static bool mark_rock(float x, float z) {
    int s = surface_at(x, z);
    return s == SURF_SLICKROCK || s == SURF_ROCK;
}

/* The tyre marks and the blazes along route r. */
static void marks_route(int r) {
    const lvl_route_t *rt = &lvl_routes[r];
    typedef struct { V3 p, side; float a[2], s; bool on, paint; } Step;
    Step st[MARK_RUN + 1];
    int n = 0, count = 0;
    float s = 0, carry = 0;
    /* A route's own stretch: the extreme trail shares the rest with
       routes that have marked it already. */
    for(int k = rt->own0; k < rt->own1; k++) {
        V3 a = v3(rt->pts[k].x, rt->pts[k].y, rt->pts[k].z), b = v3(rt->pts[k + 1].x, rt->pts[k + 1].y, rt->pts[k + 1].z);
        float dx = b.x - a.x, dz = b.z - a.z, len = sqrtf(dx * dx + dz * dz);
        if(len < 0.01f) continue;
        V3 dir = v3(dx / len, 0, dz / len);
        float grade = fabsf(b.y - a.y) / len;
        bool last_seg = k + 1 >= rt->own1;
        float t = carry;
        for(;; t += MARK_STEP) {
            bool flush = t >= len;      /* past this stretch: at the route's end, what is left */
            if(flush && !(last_seg && n > 1)) break;
            if(!flush) {
                Step *q = &st[n++];
                /* The pair wanders a little across the trail, together. */
                float w = 0.24f * sinf(s * 0.11f + r * 1.7f) + 0.1f * sinf(s * 0.37f + r);
                q->side = v3(dir.z, 0, -dir.x);
                q->p = v3(a.x + dir.x * t + q->side.x * w, 0, a.z + dir.z * t + q->side.z * w);
                q->s = s;
                q->on = mark_rock(q->p.x, q->p.z);
                /* Blacker the steeper the climb, where tyres work, and
                   coming and going: each wheel finds its own grip. */
                float base = clampf(0.4f + 1.3f * grade, 0, 0.85f);
                for(int e = 0; e < 2; e++) {
                    float come = clampf((mark_noise(s * 0.085f, 77u + r * 31u + e * 977u) - 0.2f) * 3.4f, 0, 1);
                    q->a[e] = q->on ? base * come : 0;
                }
                q->paint = q->on && surface_at(q->p.x, q->p.z) == SURF_SLICKROCK && count % 8 == 3;
                count++;
                s += MARK_STEP;
            }
            if(n == MARK_RUN + 1 || flush) {
                uint32_t at = scenery.chunk_n;
                V3 lp = st[n / 2].p;
                lp.y = terrain_height(lp.x, lp.z);
                mark_chunk_begin(lp, MARK_FAR_M);
                /* The paint first, in a run of its own: it is lit, and a
                   headlight finds it (lamps.c); the rubber is neither. */
                uint32_t f0 = scenery.pool_n;
                for(int i = 0; i + 1 < n; i++) {
                    if(!st[i].paint) continue;
                    V3 d2 = vnorm(v3(st[i + 1].p.x - st[i].p.x, 0, st[i + 1].p.z - st[i].p.z));
                    sc_strip_begin(&scenery, h_marks);
                    mark_quad(st[i].p.x, st[i].p.z, d2, 0.075f, 0.5f, 0.53f, 0.53f, 0.97f, 0.97f, MARK_PALE, true);
                    sc_strip_end(&scenery);
                }
                if(scenery.pool_n > f0) {
                    lamps_lod(lp, MARK_FAR_M);
                    lamps_add(f0, scenery.pool_n);
                }
                for(int e = 0; e < 2; e++) {
                    bool open = false;
                    for(int i = 0; i < n; i++) {
                        bool draw = st[i].on && (st[i].a[e] > 0.02f || (i > 0 && st[i - 1].a[e] > 0.02f) ||
                                                 (i + 1 < n && st[i + 1].a[e] > 0.02f));
                        if(!draw) {
                            if(open) sc_strip_end(&scenery);
                            open = false;
                            continue;
                        }
                        if(!open) sc_strip_begin(&scenery, h_marks);
                        open = true;
                        float off = (e ? RUBBER_TRACK : -RUBBER_TRACK);
                        /* (the sheet's rubber does not tile: back and forth along it) */
                        float ph = st[i].s * (1.0f / 3.4f), v = ph - floorf(ph);
                        v = 0.03f + 0.94f * (v < 0.5f ? v * 2 : 2 - v * 2);
                        /* (its strength in three steps: each is one of the scenery's colours) */
                        uint32_t col = RUBBER_RGB | (uint32_t)((int)(st[i].a[e] * 2.4f + 0.5f) * 0x54) << 24;
                        for(int h = 0; h < 2; h++) {
                            float o = off + (h ? RUBBER_HALF : -RUBBER_HALF);
                            lc(mark_at(st[i].p.x + st[i].side.x * o, st[i].p.z + st[i].side.z * o, NULL), h ? 0.47f : 0.03f, v, col);
                        }
                    }
                    if(open) sc_strip_end(&scenery);
                }
                mark_chunk_end(at);
                /* The next run starts on this one's last step. */
                st[0] = st[n - 1];
                st[0].paint = false;
                n = flush ? 0 : 1;
                if(flush) break;
            }
        }
        carry = t - len;
    }
}

static void marks_build(void) {
    mark_n = 0;
    if(lvl_area != AREA_MOAB) return;
    for(int r = 0; r < LVL_ROUTES; r++) marks_route(r);
    for(int i = 0; i < LVL_PROPS; i++) {
        const lvl_prop_t *p = &lvl_props[i];
        float cy = cosf(p->yaw), sy = sinf(p->yaw);
        V3 f = v3(sy, 0, cy), r = v3(cy, 0, -sy);
        if(p->kind == PROP_TRACKS) {
            /* Nine prints, left and right, a stride apart, the toes a
               little out. */
            uint32_t at = scenery.chunk_n;
            mark_chunk_begin(v3(p->x + f.x * 5.0f, p->y, p->z + f.z * 5.0f), 60.0f);
            for(int k = 0; k < 9; k++) {
                float side = k & 1 ? 0.3f : -0.3f, out = k & 1 ? 0.16f : -0.16f;
                V3 d = vnorm(v3(f.x + r.x * out, 0, f.z + r.z * out));
                sc_strip_begin(&scenery, h_marks);
                mark_quad(p->x + f.x * k * 1.2f + r.x * side, p->z + f.z * k * 1.2f + r.z * side, d, 0.27f, 0.3f,
                          0.53f, 0.03f, 0.97f, 0.47f, RUBBER_RGB | 0xa8000000u, false);
                sc_strip_end(&scenery);
            }
            mark_chunk_end(at);
        }
        else if(p->kind == PROP_PANEL) {
            /* The figures, a finger's width proud of the slab's face. */
            uint32_t at = scenery.chunk_n;
            V3 m = v3(p->x + f.x * 0.53f, p->y + 1.75f, p->z + f.z * 0.53f);
            mark_chunk_begin(m, 70.0f);
            uint32_t f0 = scenery.pool_n;
            sc_strip_begin(&scenery, h_glyphs);
            for(int k = 0; k < 4; k++) {
                float a = k & 2 ? 1.6f : -1.6f, b = k & 1 ? -1.0f : 1.0f;
                lv(v3(m.x + r.x * a, m.y + b, m.z + r.z * a), k & 2 ? 0.0f : 1.0f, k & 1 ? 1.0f : 0.0f, MARK_PALE, f, 1.0f);
            }
            sc_strip_end(&scenery);
            lamps_lod(m, 70.0f);
            lamps_add(f0, scenery.pool_n);
            mark_chunk_end(at);
        }
    }
    lamps_lod(v3(0, 0, 0), 0);
}

static void marks_draw(void) {
    for(int i = 0; i < mark_n; i++) sc_draw_chunk(&scenery, mark_chunks[i]);
}

/* ---- Rock formations (level.h): spires, arches, balanced rocks ---- */

/* The area's rock by height, as the level tool colours its cliffs
   (tools/level_moab.py BEDS), in steps so a formation uses few albedos:
   dark red at the river, paler with every tier. */
static uint32_t rock_bed(float y, bool dark) {
    /* The Highlands' stones are one grey, lichen on them. */
    if(lvl_area == AREA_LOWLAND) return dark ? 0xff6c6660 : y > 100.0f ? 0xffa8a49c : 0xff8e8880;
    static const uint32_t beds[2][10] = {
        {0xffa24c34, 0xffb04e2c, 0xffb4502e, 0xffc05e38, 0xffd06e40, 0xffd27042, 0xffd87a48, 0xffdc8450, 0xffe08c56,
         0xffe2925c},
        {0xff803c2a, 0xff8a3e26, 0xff8e4026, 0xff984a2e, 0xffa45834, 0xffa65a36, 0xffac623c, 0xffb06a42, 0xffb47046,
         0xffb8764a},
    };
    int k = (int)((y + 8.0f) * (1.0f / 16.0f));
    return beds[dark][k < 0 ? 0 : k > 9 ? 9 : k];
}

/* A rock's skin over its rings: each an ellipse across the spine,
   roughened by a few waves round it that drift from ring to ring. The
   waves are of the angle, not the vertex, so the far version with fewer
   sides is the same rock. */
static void rock_loft(const lvl_rock_t *rk, int lod) {
    const lvl_ring_t *rg = &lvl->rings[rk->first];
    int n = rk->n < LVL_ROCK_RINGS ? rk->n : LVL_ROCK_RINGS;
    if(n < 2) return;
    int k = lod ? (rk->sides * 5 + 4) / 8 : rk->sides;
    if(k < 4) k = 4;
    if(k > 14) k = 14;
    V3 c[LVL_ROCK_RINGS], t[LVL_ROCK_RINGS], B[LVL_ROCK_RINGS], Nn[LVL_ROCK_RINGS];
    float s[LVL_ROCK_RINGS];
    for(int i = 0; i < n; i++) c[i] = v3(rg[i].x, rg[i].y, rg[i].z);
    s[0] = 0;
    for(int i = 0; i < n; i++) {
        t[i] = vnorm(vsub(c[i + 1 < n ? i + 1 : i], c[i ? i - 1 : 0]));
        if(i) s[i] = s[i - 1] + vlen(vsub(c[i], c[i - 1]));
    }
    /* Across the spine: square to the plane it bends in (an arch's), or
       level for a straight one (north-south for one standing upright). */
    V3 bn = v3(0, 0, 0);
    for(int i = 0; i + 1 < n; i++) bn = vadd(bn, vcross(t[i], t[i + 1]));
    if(vlen(bn) < 0.05f) bn = fabsf(t[0].y) > 0.9f ? v3(1, 0, 0) : vcross(v3(0, 1, 0), t[0]);
    for(int i = 0; i < n; i++) {
        B[i] = vnorm(vsub(bn, vscale(t[i], vdot(bn, t[i]))));
        Nn[i] = vcross(B[i], t[i]);
    }
    uint32_t seed = rk->seed * 2654435761u + 77u;
    float p1 = hrand(&seed) * ENG_TAU, p2 = hrand(&seed) * ENG_TAU, p3 = hrand(&seed) * ENG_TAU;
    float rough = rk->flags & ROCK_SMOOTH ? 0.035f : 0.1f;
    bool dark = rk->flags & ROCK_DARK;
    float top_y = c[0].y;
    for(int i = 1; i < n; i++) top_y = fmaxf(top_y, c[i].y);
    /* The far version takes every other ring, the ends always. */
    int idx[LVL_ROCK_RINGS], m = 0;
    for(int i = 0; i < n; i++)
        if(!lod || n <= 5 || !(i & 1) || i == n - 1) idx[m++] = i;
    float girth = ENG_TAU * 0.5f * (rg[n / 2].ra + rg[n / 2].rb);
    float us = floorf(girth * (1.0f / 5.0f) + 0.5f);
    if(us < 1) us = 1;
    sc_strip_begin(&scenery, h_stone);
    for(int b = 0; b + 1 < m; b++)
        for(int q = 0; q <= k; q++) {
            float a = ENG_TAU * (float)(q % k) / k, ca = cosf(a), sa = sinf(a);
            for(int e = 1; e >= 0; e--) {
                int i = idx[b + e];
                float w = 1.0f + rough * (0.55f * sinf(2 * a + p1 + 0.9f * i) + 0.3f * sinf(3 * a + p2 - 1.7f * i) +
                                          0.2f * sinf(5 * a + p3 + 0.6f * i));
                float ra = rg[i].ra * w, rb = rg[i].rb * w;
                V3 p = vadd(c[i], vadd(vscale(B[i], ca * ra), vscale(Nn[i], sa * rb)));
                /* An ellipse's normal, leaning back where the rock tapers. */
                int j0 = i ? i - 1 : 0, j1 = i + 1 < n ? i + 1 : i;
                float taper = ((rg[j1].ra + rg[j1].rb) - (rg[j0].ra + rg[j0].rb)) * 0.5f / fmaxf(s[j1] - s[j0], 0.1f);
                V3 nr = vnorm(vsub(vadd(vscale(B[i], ca * rg[i].rb), vscale(Nn[i], sa * rg[i].ra)),
                                   vscale(t[i], taper * fmaxf(rg[i].ra, rg[i].rb))));
                uint32_t alb = rock_bed(p.y, dark);
                if((rk->flags & ROCK_CAP) && p.y > top_y - 0.3f * (top_y - c[0].y)) alb = rock_bed(p.y + 48.0f, false);
                float ao = 0.84f + 0.2f * nr.y;
                float u = us * (float)q / k, v = s[i] * (1.0f / 5.0f);
                if(b && q == 0 && e == 1) lj(p, u, v, alb, nr, ao);
                else lv(p, u, v, alb, nr, ao);
            }
        }
    sc_strip_end(&scenery);
}

/* A lantern (glow.c lights it and draws its glow): a brass cap, amber
   glass in a colour used as is, so it shows lit by day too; what it
   hangs by, and its post if it has one. */
static void lantern(const Glow *g) {
    V3 q = g->p;
    float s = g->size;
    box(h_flat, v3(q.x, q.y + s * 1.7f, q.z), s * 1.25f, s * 0.25f, s * 1.25f, 0, 0xff5a4a30, 1.0f);
    if(g->hang > 0) leg(v3(q.x, q.y + s * 1.9f, q.z), v3(q.x, q.y + s * 1.9f + g->hang, q.z), 0.012f, 0xff3a3028);
    sc_strip_begin(&scenery, h_flat);
    for(int e = 0; e <= 4; e++) {
        float ex = ((e + 1) & 2) ? -s : s, ez = (e & 2) ? -s : s;
        lc(v3(q.x + ex, q.y + s * 1.45f, q.z + ez), 0, 0, 0xffe6a84c);
        lc(v3(q.x + ex * 0.8f, q.y - s * 1.3f, q.z + ez * 0.8f), 0, 0, 0xffc8802c);
    }
    sc_strip_end(&scenery);
    if(g->post) {
        /* The post stands half a metre east of it, an arm across the top. */
        float top = q.y + s * 1.9f + g->hang, px = q.x + 0.5f;
        box_seg(h_bark, v3(px, (terrain_height(px, q.z) - 0.2f + top + 0.12f) * 0.5f, q.z), 0.07f,
                (top + 0.12f - terrain_height(px, q.z) + 0.2f) * 0.5f, 0.07f, 0, 0xff6a4a30, 1.0f, 1.0f);
        box(h_bark, v3(q.x + 0.2f, top + 0.03f, q.z), 0.36f, 0.035f, 0.035f, 0, 0xff6a4a30, 1.0f);
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
#define CAP_BASE (look->cap_base)
#define CAP_TOP (look->cap_top)
#define CAP_SOFT 6.0f         /* the cloud thins over this either side of the base and top */
#define CAP_R (look->cap_r)   /* and out to here from the summit (horizontally) */
#define CAP_R_SOFT 35.0f
#define CAP_FOG_FAR 55.0f     /* the fog's far distance deep in it */
/* The collar: rings round the peak, each a height and how far out from
   the mountain's side it sits there. Clear at the top and the bottom
   rings, thick between; the mountain narrows as it
   climbs, so a straight run between two rings stays off the slope. */
#define CAP_SEG 32
#define CAP_RINGS 4
static float cap_ring_y[CAP_RINGS];
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
    /* Cascade Ridge's alone: the desert's sky is clear to the top. */
    cap_built = look->cloud_cap;
    if(!cap_built || !look->cap_collar) return;
    cap_ring_y[0] = CAP_TOP + 5.0f;
    cap_ring_y[1] = CAP_TOP - 3.0f;
    cap_ring_y[2] = CAP_BASE + 5.0f;
    cap_ring_y[3] = CAP_BASE - 4.0f;
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
    cap_k = cap_built ? cap_amount(tod.hour) : 0;
    cap_in = cap_built ? cap_depth(cam.pos) * smooth01(cap_k * 1.6f) : 0;
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

static void dsky_colours(uint32_t warm);
static void dsky_build(void);
/* The deserts have tablelands for a skyline (below); the mountain and the
   Highlands these ranges. */
static inline bool dsky_area(void) {
    return lvl_area == AREA_MOAB || lvl_area == AREA_DUNES;
}

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
    /* (in a shower they are all but gone into it) */
    float murk = 0.9f * rain_overcast();
    for(int r = 0; r < RANGES; r++) {
        float k = 0.35f + 0.22f * r;
        k += (1.0f - k) * murk;
        range_top[r][0] = color_lerp(base, pal->haze, k);
        range_bot[r][0] = color_lerp(range_top[r][0], pal->haze, 0.55f);
        range_top[r][1] = color_lerp(base, warm, k);
        range_bot[r][1] = color_lerp(range_top[r][1], warm, 0.55f);
        for(int s = 0; s < 2 && cap_in > 0; s++) {
            range_top[r][s] = color_lerp(range_top[r][s], cap_col, cap_in);
            range_bot[r][s] = color_lerp(range_bot[r][s], cap_col, cap_in);
        }
    }
    if(dsky_area()) dsky_colours(warm);
}

/* ---- The desert's skyline ----

   Round the desert the far country is tablelands, not hills: a ring of
   mesas and buttes with cliffs for edges, a longer, lower, hazier ring of
   them behind, and behind that to the east-south-east one range of real
   mountains with snow on it. Each ring is a run of vertices by bearing,
   as close together as a cliff needs. */
#define DSKY_MAX 160
#define DSKY_RUN 8             /* vertices a strip */
static float dsky_snowline;    /* the snow line on the mountains */
#define DSKY_SNOW dsky_snowline
static struct { float sa, ca, h; } dsky[RANGES][DSKY_MAX];
static int dsky_n[RANGES];
static uint32_t dsky_top[RANGES][2], dsky_bot[RANGES][2], dsky_snow[2];

static void dsky_put(int l, float az, float h) {
    if(dsky_n[l] >= DSKY_MAX) return;
    float a = az * (ENG_PI / 180.0f);
    dsky[l][dsky_n[l]].sa = sinf(a);
    dsky[l][dsky_n[l]].ca = cosf(a);
    dsky[l][dsky_n[l]++].h = h;
}

/* A flat-topped block from az, w degrees wide: talus, a cliff, the top
   (tilted by tilt), a cliff, talus. */
static void dsky_block(int l, float az, float w, float base, float top, float tilt, float cliff) {
    float skirt = (top - base) * 0.035f * cliff;   /* degrees of talus: about its angle of rest out there */
    dsky_put(l, az, base);
    dsky_put(l, az + skirt, base + (top - base) * 0.42f);
    dsky_put(l, az + skirt + 0.3f, top);
    dsky_put(l, az + w - skirt - 0.3f, top + tilt);
    dsky_put(l, az + w - skirt, base + (top + tilt - base) * 0.42f);
    dsky_put(l, az + w, base);
}

/* The dunes' skyline: the Columbia Basin, which is mostly sky. Low sage
   hills all round, never more than a couple of degrees high; the long
   whalebacks of the Frenchman Hills and the Saddle Mountains to the
   south, with the notch the river cut through the second; and far to
   the west the Cascades, a blue saw-edge with one white volcano standing
   over it. */
static float basin_hump(float az, float at, float half) {
    float d = (az - at) / half;
    return d > -1.0f && d < 1.0f ? (1.0f - d * d) * (1.0f - d * d) : 0;
}

static void basin_build(void) {
    uint32_t seed = 1949;
    dsky_snowline = 138.0f;
    for(int l = 0; l < RANGES; l++) dsky_n[l] = 0;
    float n0 = 0, n1 = 0;
    for(int i = 0; i <= 120; i++) {
        float az = i * 3.0f;
        n0 = n0 * 0.6f + 0.4f * hrand(&seed);
        n1 = n1 * 0.7f + 0.3f * hrand(&seed);
        /* Near: swells of sage, the Frenchman Hills south-west, the
           Beezley Hills north. */
        float h0 = 43.0f + 9.0f * n0 + 34.0f * basin_hump(az, 232.0f, 52.0f) + 22.0f * basin_hump(az, 8.0f, 46.0f) +
                   22.0f * basin_hump(az, 368.0f, 46.0f);
        /* Behind: the Saddle Mountains, cut clean through at the gap. */
        float gap = 1.0f - 0.85f * basin_hump(az, 204.0f, 5.0f);
        float h1 = 50.0f + 8.0f * n1 + 78.0f * basin_hump(az, 184.0f, 58.0f) * gap + 30.0f * basin_hump(az, 60.0f, 70.0f);
        /* Far off: the Cascades from the south-west round to the north,
           the Stuart Range's teeth, the volcano. */
        float casc = basin_hump(az, 286.0f, 74.0f);
        float h2 = 46.0f + casc * (64.0f + 46.0f * hrand(&seed)) + 120.0f * basin_hump(az, 306.0f, 15.0f) * (0.55f + 0.45f * hrand(&seed));
        float cone = (az - 262.0f) * (1.0f / 7.5f);
        if(cone > -1.0f && cone < 1.0f) h2 = fmaxf(h2, 58.0f + 132.0f * powf(1.0f - fabsf(cone), 1.3f) - 8.0f * basin_hump(az, 262.0f, 1.6f));
        dsky_put(0, az, h0);
        dsky_put(1, az, h1);
        dsky_put(2, az, h2);
    }
    for(int l = 0; l < RANGES; l++) dsky[l][dsky_n[l] - 1].h = dsky[l][0].h;
}

static void dsky_build(void) {
    if(lvl_area == AREA_DUNES) {
        basin_build();
        return;
    }
    dsky_snowline = 208.0f;
    uint32_t seed = 9091;
    for(int l = 0; l < 2; l++) {
        dsky_n[l] = 0;
        float az = 0, base0 = l ? 52.0f : 36.0f;
        dsky_put(l, 0, base0);
        while(az < 340.0f) {
            float base = base0 + 10.0f * hrand(&seed), pick = hrand(&seed);
            float w;
            if(l) {
                /* Far off: long tablelands. */
                w = 18.0f + 22.0f * hrand(&seed);
                dsky_block(l, az, w, base, 78.0f + 34.0f * hrand(&seed), (hrand(&seed) - 0.5f) * 8.0f, 0.8f);
            }
            else if(pick < 0.7f) {
                w = 16.0f + 18.0f * hrand(&seed);
                dsky_block(l, az, w, base, 84.0f + 44.0f * hrand(&seed), (hrand(&seed) - 0.5f) * 10.0f, 1.0f);
            }
            else if(pick < 0.9f) {
                /* A butte standing alone. */
                w = 9.0f + 3.0f * hrand(&seed);
                dsky_block(l, az, w, base, 104.0f + 40.0f * hrand(&seed), -3.0f, 0.9f);
            }
            else {
                /* Two spires on one footing. */
                w = 5.5f;
                float top = 130.0f + 50.0f * hrand(&seed);
                dsky_put(l, az, base);
                dsky_put(l, az + 1.2f, base + 28.0f);
                dsky_put(l, az + 1.7f, top);
                dsky_put(l, az + 2.2f, top - 6.0f);
                dsky_put(l, az + 2.6f, base + 34.0f);
                dsky_put(l, az + 3.3f, top * 0.8f);
                dsky_put(l, az + 3.8f, base + 26.0f);
                dsky_put(l, az + w, base);
            }
            az += w + 2.5f + (l ? 10.0f : 8.0f) * hrand(&seed);
        }
        dsky_put(l, 360.0f, base0);
    }
    /* The mountains: a cluster of peaks about bearing 118, low country
       either side. */
    dsky_n[2] = 0;
    for(int i = 0; i <= 120; i++) {
        float az = i * 3.0f, d = (az - 118.0f) * (1.0f / 38.0f);
        float m = d > -1.0f && d < 1.0f ? powf(1.0f - d * d, 1.3f) : 0;
        float jag = 0.6f + 0.4f * hrand(&seed) + 0.25f * sinf(az * 0.35f);
        dsky_put(2, az, i == 120 ? dsky[2][0].h : 58.0f + 16.0f * hrand(&seed) + 330.0f * m * jag);
    }
}

/* Each ring's colours away from the sun and toward it: red rock near,
   dustier behind, the mountains blue with distance; all into the haze by
   how far, all dark by night. */
static void dsky_colours(uint32_t warm) {
    static const uint32_t rocks[2][RANGES] = {{0xffa0502e, 0xff9a5e4c, 0xff4e6a96},
                                              /* Sage hills, tawny then dun; the Cascades blue. */
                                              {0xff8c8262, 0xff7e7a6a, 0xff526c98}};
    const uint32_t *rock = rocks[lvl_area == AREA_DUNES];
    for(int l = 0; l < RANGES; l++) {
        uint32_t base = color_lerp(rock[l], 0xff0a1424, pal->dark);
        /* Far enough off to be mostly haze: no more of the rock's colour
           than the land in front has left where the fog takes it. */
        float k = WORLD_SKYLINE_HAZE + 0.08f * l;
        dsky_top[l][0] = color_lerp(base, pal->haze, k);
        dsky_bot[l][0] = color_lerp(dsky_top[l][0], pal->haze, 0.3f);
        dsky_top[l][1] = color_lerp(base, warm, k);
        dsky_bot[l][1] = color_lerp(dsky_top[l][1], warm, 0.3f);
    }
    uint32_t snow = color_lerp(0xfff4f6fa, 0xff1a2438, pal->dark);
    dsky_snow[0] = color_lerp(snow, pal->haze, 0.55f);
    dsky_snow[1] = color_lerp(snow, warm, 0.55f);
}

static void dsky_draw(V3 c, float fx, float fz, bool turn) {
    for(int l = RANGES - 1; l >= 0; l--) {
        float rad = 1050.0f + 260.0f * l, lift = c.y * 0.15f - 40.0f;
        for(int i0 = 0; i0 + 1 < dsky_n[l]; i0 += DSKY_RUN) {
            int i1 = i0 + DSKY_RUN < dsky_n[l] - 1 ? i0 + DSKY_RUN : dsky_n[l] - 1, mid = (i0 + i1) / 2;
            /* Off the screen: the run's middle well off the view's heading
               (runs are short in bearing where the vertices crowd). */
            if(turn && dsky[l][mid].sa * fx + dsky[l][mid].ca * fz < 0.2f) continue;
            RV v[2 * (DSKY_RUN + 1)], sn[2 * (DSKY_RUN + 1)];
            int n = 0, ns = 0;
            bool any_snow = false;
            for(int i = i0; i <= i1; i++) {
                float sa = dsky[l][i].sa, ca = dsky[l][i].ca, h = dsky[l][i].h;
                int g = (int)(glow_at(sa, ca) * 256.0f);
                v[n++] = (RV){c.x + sa * rad, h + lift, c.z + ca * rad, 0, 0, mix256(dsky_top[l][0], dsky_top[l][1], g) | 0xff000000u};
                v[n++] = (RV){c.x + sa * rad, -60.0f, c.z + ca * rad, 0, 0, mix256(dsky_bot[l][0], dsky_bot[l][1], g) | 0xff000000u};
                if(l == 2) {
                    /* The snow, a little nearer so it lies over the rock. */
                    uint32_t sc = mix256(dsky_snow[0], dsky_snow[1], g) | 0xff000000u;
                    float line = h < DSKY_SNOW ? h : DSKY_SNOW + 0.25f * (h - DSKY_SNOW) * (float)(i & 1);
                    any_snow |= h > DSKY_SNOW;
                    sn[ns++] = (RV){c.x + sa * (rad - 8.0f), h + lift, c.z + ca * (rad - 8.0f), 0, 0, sc};
                    sn[ns++] = (RV){c.x + sa * (rad - 8.0f), line + lift, c.z + ca * (rad - 8.0f), 0, 0, sc};
                }
            }
            r_strip(v, n);
            if(any_snow) r_strip(sn, ns);
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
    if(dsky_area()) {
        dsky_draw(c, fx, fz, fl > 0.01f);
        return;
    }
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
   That is for grass, which is a texture of the meadow. Brush is bushes,
   each one a thing standing on bare ground, and a bush seen to swell or
   shrink 20 m ahead of the truck is plainly wrong: so every bush is
   drawn at its own size out to the area's reach (LvlLook.tuft_reach,
   56 m) and only the last few metres of that, where it is a dozen pixels
   across, bring it in. They stand far enough apart that this is fewer of
   them than a meadow has blades. The Highlands' moor is drawn the same
   way out to 44 m: its heather and its clumps of hill grass stand apart
   on dark ground too, and were seen to swell and shrink as plainly. It
   grows a fifth fewer of them for it, which keeps the count where it was.
   Working out a tuft (surface, water, height) is a handful of cache
   misses into the level, so tufts are worked out once per 8 m block and
   kept while the block stays near: a frame only reads them back. Each
   tuft's lit colour is read from the ground as it is drawn, so they
   follow the time of day. */
#define TUFT_CELL 2.0f
#define TUFT_NEAR 24.0f
#define TUFT_FAR 44.0f
#define BRUSH_IN 7.0f                       /* a bush comes up to size over this much of it */
#define TUFT_MAX 560
#define TB_CELLS 4                          /* cells a block side */
#define TB_SIZE (TUFT_CELL * TB_CELLS)
#define TB_RAD (TB_SIZE * 0.7072f + 1.2f)   /* a block's tufts lie within this of its centre */
#define TB_SLOTS 16                         /* cached blocks a side; > 2 * (the longest reach, 56 m) / TB_SIZE + 2 */
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
static const float tuft_aspects[AREA_COUNT][4] = {{1.25f, 1.6f, 1.0f, 1.3f},
                                                 /* The desert's brush: sage, blackbrush, rabbitbrush, ricegrass. */
                                                 {0.9f, 0.74f, 1.0f, 1.1f},
                                                 /* The dunes': sage, cattails, rabbitbrush, ricegrass. */
                                                 {0.9f, 2.0f, 1.0f, 1.1f},
                                                 /* The moor's: heather, bracken, grass, bog cotton. */
                                                 {0.72f, 1.05f, 1.25f, 1.25f}};
#define tuft_aspect (tuft_aspects[lvl_area])
#define TUFT_REED 1   /* the dunes' cattails: only where it is wet */

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
            bool dunes = lvl_area == AREA_DUNES, desert = dunes || lvl_area == AREA_MOAB, moor = lvl_area == AREA_LOWLAND;
            float wat = water_at(px, pz), y = terrain_height(px, pz);
            /* Cattails stand in the dunes' shallows and on the mud beside
               them, in beds. */
            bool reed = false;
            if(dunes && (sf == SURF_MUD || sf == SURF_HARDPACK || wat > -999.0f)) {
                bool wet = wat > -999.0f ? wat - y < 0.45f
                                         : water_at(px + 3.0f, pz) > -999.0f || water_at(px - 3.0f, pz) > -999.0f ||
                                               water_at(px, pz + 3.0f) > -999.0f || water_at(px, pz - 3.0f) > -999.0f;
                reed = wet && tuft_patch(px * 1.7f + 40.0f, pz * 1.7f) > 0.42f && (s >> 20 & 7) < 6;
            }
            /* The mountain's grass, thin in the forest; the desert's brush
               on its scrub flats, the odd bush out on the sand; the
               Highlands' turf and the heather moor (its FOREST) alike. */
            if(!reed && sf != SURF_GRASS && !(sf == SURF_FOREST && moor) &&
               !(sf == (desert ? SURF_SAND : SURF_FOREST) && (s >> 20 & 3) == 0))
                continue;
            if(!reed && wat > -999.0f) continue;
            uint32_t s2 = hash32(s ^ 0x68e31da4u);
            /* Up the mountain the grass thins and stays short: from the
               meadow's edge (30 m) to near the scree (160 m) a tuft
               grows a third as often and a third smaller. */
            float alp = desert ? 0 : moor ? clampf((y - 95.0f) * (1.0f / 50.0f), 0, 1) : clampf((y - 30.0f) * (1.0f / 130.0f), 0, 1);
            alp = alp * alp * (3 - 2 * alp);
            float dens = tuft_patch(px, pz) * (1.0f - 0.35f * alp);
            /* Brush stands apart, each bush with ground to itself. */
            float keep = desert ? 0.08f + 0.5f * dens : moor ? 0.78f * (0.4f + 0.8f * dens) * (1.0f - 0.5f * alp)
                                                              : (0.4f + dens) * (1.0f - 0.55f * alp);
            if(!reed && (s2 & 255) * (1.0f / 255) > keep) continue;
            Tuft *t = &b->t[b->n++];
            t->x = px;
            t->y = y - 0.05f;
            t->z = pz;
            int gi = lvl_clampi((int)((px - LEVEL_ORG) / LEVEL_D + 0.5f)), gj = lvl_clampi((int)((pz - LEVEL_ORG) / LEVEL_D + 0.5f));
            t->g = (uint32_t)(gj * LEVEL_N + gi);
            static const uint8_t var_pick[AREA_COUNT][8] = {{0, 0, 0, 1, 1, 2, 2, 3}, {0, 0, 0, 1, 1, 2, 3, 3},
                                                            {0, 0, 0, 0, 2, 2, 3, 3}, {0, 0, 0, 1, 1, 2, 2, 3}};
            t->var = var_pick[lvl_area][s2 >> 8 & 7];
            /* (out on the dunes' open sand, ricegrass alone) */
            if(dunes && sf == SURF_SAND) t->var = 3;
            if(reed) t->var = TUFT_REED;
            if(moor) {
                /* Heather in drifts on the moor, bracken where the
                   patches thin, hill grass through both; on the turf
                   grass, and bog cotton in its wettest hollows. */
                int roll = s2 >> 8 & 7;
                float patch = tuft_patch(px + 40.0f, pz - 17.0f);
                if(sf == SURF_GRASS) t->var = patch > 0.78f && roll < 2 ? 3 : 2;
                else t->var = roll == 7 ? 2 : patch > 0.44f ? (roll < 6 ? 0 : 2) : (roll < 4 ? 1 : 2);
            }
            t->flags = (uint8_t)((s >> 24 & 1) | (s2 >> 10 & 2));
            float w = (0.55f + ((s >> 16) & 15) / 30.0f) * (0.8f + 0.35f * dens) * (1.0f - 0.3f * alp);
            if(reed) w = 0.7f + ((s >> 16) & 15) / 40.0f;
            t->size = (uint8_t)fminf(w * 100.0f, 255.0f);
            t->tint = (uint8_t)(s2 >> 16);
        }
}

typedef struct {
    float d2;
    int16_t bi, bj;
} TuftDraw;

static const pvr_poly_hdr_t *h_tufts[4];

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
    /* A tuft is a quad across the view, standing up and leaning with the
       wind: those three ways through the view-projection once, and each
       tuft is then one FTRV of its foot (r_quad_edges). */
    RClipVec d_rt = r_clip_vec(rt), d_up = r_clip_vec(v3(0, 1, 0)), d_wind = r_clip_vec(v3(WIND_X, 0, WIND_Z));

    /* The blocks in reach and in view, nearest first, so a full buffer
       drops the furthest tufts. */
    static TuftDraw vis[TB_SLOTS * TB_SLOTS];
    int nvis = 0, fills = 0;
    bool desert = lvl_area == AREA_MOAB || lvl_area == AREA_DUNES;   /* brush: woody */
    /* Drawn at their own size as far as the area's reach, or the meadow's
       way (above). */
    bool own_size = look->tuft_reach > 0;
    const float reach = own_size ? look->tuft_reach : TUFT_FAR;
    int bi0 = (int)floorf((c.x - reach) / TB_SIZE), bi1 = (int)floorf((c.x + reach) / TB_SIZE);
    int bj0 = (int)floorf((c.z - reach) / TB_SIZE), bj1 = (int)floorf((c.z + reach) / TB_SIZE);
    for(int bj = bj0; bj <= bj1; bj++)
        for(int bi = bi0; bi <= bi1; bi++) {
            float dx = (bi + 0.5f) * TB_SIZE - c.x, dz = (bj + 0.5f) * TB_SIZE - c.z;
            float d2 = dx * dx + dz * dz;
            if(d2 > (reach + TB_RAD) * (reach + TB_RAD)) continue;
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
        /* By the fire or a lantern the grass takes their light as the
           ground under it does (glow.c), or it would stand black on it. */
        bool warm = glow_near(v3(bx, 0, bz), TB_RAD);
        /* And in a headlight beam, as the beam's pool on the ground. */
        int beams = lamps_ground_mask(bx, bz, TB_RAD);
        for(int k = 0; k < b->n && count < TUFT_MAX; k++) {
            const Tuft *t = &b->t[k];
            float dx = t->x - c.x, dz = t->z - c.z, d2 = dx * dx + dz * dz;
            bool far = t->flags & 2;
            if(d2 > (own_size ? reach * reach : far ? TUFT_FAR * TUFT_FAR : TUFT_NEAR * TUFT_NEAR)) continue;
            /* In view? (by the vertical frustum slope only roughly) */
            float fz = dx * r_cam.fwd.x + dz * r_cam.fwd.z;
            float fx = dx * r_cam.right.x + dz * r_cam.right.z;
            if(fz < -1.0f || fabsf(fx) > fz * tx + 2.0f) continue;
            float d = sqrtf(d2), w = t->size * 0.01f;
            if(own_size) w *= fminf(1.0f, (reach - d) * (1.0f / BRUSH_IN));
            else if(far) {
                w *= (1.0f + 0.45f * clampf((d - (TUFT_NEAR - 8.0f)) * (1.0f / 8.0f), 0, 1)) *
                     fminf(1.0f, (TUFT_FAR - d) * (1.0f / 8.0f));
            }
            else w *= fminf(1.0f, (TUFT_NEAR - d) * (1.0f / 8.0f));
            if(near_truck && under_truck(t->x, t->z, w * 0.6f)) continue;
            float h = w * tuft_aspect[t->var];
            uint32_t gc = lvl_ground[t->g].c, col;
            bool own = look->tuft_own >> t->var & 1;
            if(own) {
                /* Brush has its own colours: only the ground's light. */
                int l = (int)((((gc >> 16) & 255) * 5 + ((gc >> 8) & 255) * 9 + (gc & 255) * 4) * (0.8f + t->tint * (1.0f / 850))) / 10;
                if(l > 255) l = 255;
                col = 0xff000000u | (uint32_t)l << 16 | (uint32_t)l << 8 | (uint32_t)l;
            }
            else col = color_scale(gc, 1.35f * (0.8f + t->tint * (1.0f / 850)));
            if(warm) col = glow_tint(col, v3(t->x, t->y + 0.25f, t->z));
            if(beams) {
                float k = lamps_light(v3(t->x, t->y + h * 0.5f, t->z), beams, -1, NULL);
                if(k > 0.02f) {
                    /* Warm white, a little of the grass's own colour in it. */
                    int cr = (int)((col >> 16) & 255) + (int)((((col >> 16) & 255) * 0.6f + 120.0f) * k);
                    int cg = (int)((col >> 8) & 255) + (int)((((col >> 8) & 255) * 0.6f + 112.0f) * k);
                    int cb = (int)(col & 255) + (int)(((col & 255) * 0.6f + 84.0f) * k);
                    col = 0xff000000u | (uint32_t)(cr > 255 ? 255 : cr) << 16 | (uint32_t)(cg > 255 ? 255 : cg) << 8 |
                          (uint32_t)(cb > 255 ? 255 : cb);
                }
            }
            uint32_t root = color_scale(col, 0.7f);
            /* The tips lean downwind and back, a slow gust rolling across
               the meadow with a quicker flutter of each tuft's own. */
            float along = t->x * WIND_X + t->z * WIND_Z;
            float lean = h * (0.07f + 0.06f * sinf(sway_t * 1.3f - along * 0.11f) +
                              0.035f * sinf(sway_t * 3.1f + (float)(t->tint & 63)));
            if(desert ? t->var != 3 : own && t->var < 2) lean *= 0.3f;   /* woody: it barely stirs */
            float u0 = (t->flags & 1) ? 0.0f : 1.0f, u1 = 1.0f - u0;
            RQuadE q = {v3(t->x, t->y, t->z),
                        {d_rt.x * w, d_rt.y * w, d_rt.w * w},
                        {d_up.x * h + d_wind.x * lean, d_up.y * h + d_wind.y * lean, d_up.w * h + d_wind.w * lean},
                        u0, 0, u1, 1, col, root};
            r_hdr(h_tufts[t->var]);
            r_quad_edges(&q);
            count++;
        }
    }
    world_tufts = count;
}

/* ---- Sun, clouds and flare ---- */

static float flare_vis;   /* 0..1: how clearly the camera sees the sun */
/* And how much of the sun's and the moon's glow shows. The glow is drawn
   over everything (it blooms round a cloud or a ridge's edge), so with
   the disc behind a hill it has to go, or it is a patch of light on the
   hillside: plain on a crag across a corrie. */
static float sun_glow_vis = 1.0f, moon_glow_vis = 1.0f;

/* Whether the land hides what is in the sky that way: a march over the
   map, then over the land past its edge. */
static bool sky_occluded(V3 dir) {
    V3 p = r_cam.pos;
    for(int k = 1; k <= 30; k++) {
        float t = k * k * 0.9f + 4.0f;
        V3 q = vmad(p, dir, t);
        if(fabsf(q.x) > 383 || fabsf(q.z) > 383) {
            /* Out there the ground is drawn on a 24 m grid at the finest. */
            for(float s = t; s < 1500.0f; s += 60.0f) {
                q = vmad(p, dir, s);
                if(outland_height(q.x, q.z) > q.y) return true;
            }
            break;
        }
        if(terrain_height(q.x, q.z) > q.y) return true;
    }
    /* Then the ranges painted round the sky (sky_draw). */
    float hl = sqrtf(dir.x * dir.x + dir.z * dir.z);
    if(!dsky_area() && hl > 1e-3f) {
        float az = atan2f(dir.x, dir.z);
        for(int r = 0; r < RANGES; r++) {
            float u = (az - r * 0.37f) * (RANGE_SEG / ENG_TAU);
            u -= floorf(u * (1.0f / RANGE_SEG)) * RANGE_SEG;
            int i = (int)u;
            if(i >= RANGE_SEG) i = RANGE_SEG - 1;
            float top = range_h[r][i] + (range_h[r][i + 1] - range_h[r][i]) * (u - i) - 40.0f + p.y * 0.15f;
            if((top - p.y) * hl > dir.y * (1050.0f + 260.0f * r)) return true;
        }
    }
    return false;
}

float world_mist(void) {
    return cap_in;
}

float world_fog_far(void) {
    return fog_far_m;
}

void world_fog(float *near_m, float *far_m, uint32_t *col) {
    *near_m = fog_near_m;
    *far_m = fog_far_m;
    *col = fog_col;
}

uint32_t world_lit(uint32_t albedo, V3 n) {
    return lit(albedo, n, 1.0f);
}

void world_update(float dt) {
    haze_update();
    /* Whether the hills hide the sun: a march over the terrain, every
       fourth frame (the flare eases in and out over a quarter second). */
    static float flare_target;
    static bool sun_hid, moon_hid;
    if((atmo_tick & 3) == 1) {
        sun_hid = tod.sun_el > -0.06f && sky_occluded(tod.sun);
        flare_target = tod.sun_el > 0 && cap_in < 0.3f && !sun_hid && rain_overcast() < 0.2f ? 1.0f : 0.0f;
    }
    if((atmo_tick & 3) == 3) moon_hid = tod.moon_el > -0.06f && pal->dark > 0.05f && sky_occluded(tod.moon);
    flare_vis = approach(flare_vis, flare_target, dt * 4.0f);
    sun_glow_vis = approach(sun_glow_vis, sun_hid ? 0.0f : 1.0f, dt * 2.0f);
    moon_glow_vis = approach(moon_glow_vis, moon_hid ? 0.0f : 1.0f, dt * 2.0f);
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

/* The cap on a top that is a ridge, not a peak (a collar would have to
   pass through it): a few banks of cloud lying against the summit at the
   cap's height, drifting slowly round it, each an upright quad of the
   sky's cloud turned to the camera and gone as the camera comes into it
   (inside, the fog is the cloud). */
#define BANKS 7
static void cap_banks(uint32_t shade) {
    float out = (1.0f - cap_in) * cap_k;
    if(out < 0.02f || !r_sphere_visible(v3(LEVEL_SUMMIT_X, (CAP_BASE + CAP_TOP) * 0.5f, LEVEL_SUMMIT_Z), CAP_R + 75.0f)) return;
    V3 c = r_cam.pos;
    float fog_k = fog_far_m > fog_near_m ? 128.0f / (fog_far_m - fog_near_m) : 0;
    r_hdr(h_cloud);
    for(int i = 0; i < BANKS; i++) {
        uint32_t hs = hash32((uint32_t)i * 2654435761u + 99u);
        float a = ENG_TAU * i / BANKS + cloud_t * 0.006f + (hs & 255) * (0.5f / 255);
        float rad = (30.0f + 62.0f * ((hs >> 8) & 255) * (1.0f / 255)) * (CAP_R * (1.0f / 115.0f)), w = 52.0f + 34.0f * ((hs >> 16) & 255) * (1.0f / 255);
        V3 p = v3(LEVEL_SUMMIT_X + sinf(a) * rad, CAP_BASE + (CAP_TOP - CAP_BASE) * (0.35f + 0.4f * ((hs >> 24) & 255) * (1.0f / 255)),
                  LEVEL_SUMMIT_Z + cosf(a) * rad);
        float dx = p.x - c.x, dz = p.z - c.z, d = sqrtf(dx * dx + dz * dz + (p.y - c.y) * (p.y - c.y));
        float al = out * clampf((d - 70.0f) * (1.0f / 90.0f), 0, 1) * 0.9f;
        if(al < 0.02f) continue;
        float inv = 1.0f / sqrtf(dx * dx + dz * dz + 0.01f);
        V3 rt = v3(dz * inv, 0, -dx * inv);
        /* (in a shower the bank is more of the same grey, not a white
           cloud standing in it) */
        int fog = (int)clampf((d - fog_near_m) * fog_k, 0, 128);
        fog += (int)((256 - fog) * 0.85f * rain_overcast());
        uint32_t top = color_alpha(mix256(pal->cloud_col, fog_col, fog), al), base = color_alpha(mix256(shade, fog_col, fog), al);
        float h = w * 0.34f, u0 = (hs & 3) * 0.25f, u1 = u0 + 0.25f;
        V3 l = vmad(p, rt, -w), r = vmad(p, rt, w);
        RV q[4] = {{l.x, l.y + h, l.z, u0, 0, top}, {r.x, r.y + h, r.z, u1, 0, top},
                   {l.x, l.y - h, l.z, u0, 1, base}, {r.x, r.y - h, r.z, u1, 1, base}};
        r_strip(q, 4);
    }
}

static void cap_draw(uint32_t shade) {
    static bool fresh;
    if(cap_built && !look->cap_collar) {
        cap_banks(shade);
        return;
    }
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
#define MIST_MAX 12
static Mist mists[MIST_MAX];   /* the area's (lvl->mists), each with its height */
static int mist_n;
#define MISTS mist_n
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
    /* On a Highland loch some of it never lifts. */
    return fmaxf(k[i][1] + (k[i + 1][1] - k[i][1]) * t, look->mist_day);
}

/* Each sheet lies a little over the lowest ground near its middle. */
static void mist_build(void) {
    mist_n = lvl->nmists < MIST_MAX ? lvl->nmists : MIST_MAX;
    for(int m = 0; m < MISTS; m++) {
        Mist *s = &mists[m];
        *s = (Mist){lvl->mists[m].x, lvl->mists[m].z, lvl->mists[m].r, 0};
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
    /* (a loch's sheet is looked down on from the hills: at the small
       sheets' 30 m its texture is a pattern of tiles from up there) */
    float cell = 2.0f * s->r / n, x0 = s->x - s->r, z0 = s->z - s->r, uk = cell * (s->r > 100.0f ? 1.0f / 110.0f : 1.0f / 30.0f);
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
        /* (a sheet as big as a loch's shows from as far as the fog lets it) */
        if(sd > (s->r > 100.0f ? fog_far_m + s->r : 450.0f) || !r_sphere_visible(sc, s->r)) continue;
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
    marks_draw();
    outland_draw_tr();
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
    /* (under an overcast neither shows, nor its glow) */
    float clear_sky = clampf(1.0f - 1.6f * rain_overcast(), 0, 1);
    clear_sky *= clear_sky;
    float ma = horizon_alpha(tod.moon_el) * (1.0f - 0.85f * cap_in) * clear_sky;
    if(ma > 0) {
        V3 mp = vmad(c, tod.moon, 1600.0f);
        if(pal->dark > 0.05f) {
            r_hdr(h_glow_add);
            r_billboard(mp, 220.0f, 220.0f, 0, 0, 0, 1, 1, color_alpha(0xff8090b8, 0.3f * pal->dark * ma * moon_glow_vis));
        }
        r_hdr(h_moon);
        r_billboard(mp, 44.0f, 44.0f, 0, 0, 0, 1, 1, color_alpha(0xffffffff, ma * (0.35f + 0.65f * pal->dark)));
    }
    /* The sun: a big flat disc with a soft glow. */
    float sa = horizon_alpha(tod.sun_el) * (1.0f - 0.8f * cap_in) * clear_sky;
    if(sa > 0) {
        V3 sp = vmad(c, tod.sun, 1600.0f);
        r_hdr(h_glow_add);
        r_billboard(sp, 320.0f, 320.0f, 0, 0, 0, 1, 1, color_alpha(pal->sun_col, 0.375f * sa * sun_glow_vis));
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
    h_marks = r_header(TEX_MARKS, HF_TR);
    h_glyphs = r_header(TEX_GLYPHS, HF_TR);
    h_flat_nofog = r_header(TEX_NONE, HF_OP | HF_NOFOG);
    h_rock = r_header(TEX_ROCK, HF_OP);
    h_bark = r_header(TEX_BARK, HF_OP);
    h_crown = r_header(TEX_CROWN, HF_OP);
    h_sun = r_header(TEX_SUN, HF_TR | HF_NOFOG | HF_ADD);
    h_moon = r_header(TEX_MOON, HF_TR | HF_NOFOG);
    h_stars = r_header(TEX_STARS, HF_TR | HF_NOFOG | HF_ADD);
    h_glow_add = r_header(TEX_GLOW, HF_TR | HF_NOFOG | HF_ADD | HF_NOZTEST);
    h_ring_add = r_header(TEX_RING, HF_TR | HF_NOFOG | HF_ADD | HF_NOZTEST);
    h_cloud = r_header(TEX_CLOUD, HF_TR | HF_NOFOG);
    h_cap = r_header(TEX_NONE, HF_TR | HF_NOFOG);
    h_mist = r_header(TEX_MIST, HF_TR | HF_NOFOG);
    sky_setup();
    if(!sc_init(&scenery, 250000, 7000, 2400)) rl_no_memory("the scenery");
    else {
        /* Its strip and chunk tables (~280 KiB) move to the room the
           textures were linked into: with four areas linked the heap has
           under half a megabyte to spare. */
        ScStrip *st = spent_alloc(sizeof(ScStrip) * scenery.strip_max);
        ScChunk *ch = spent_alloc(sizeof(ScChunk) * scenery.chunk_max);
        if(st) {
            free(scenery.strips);
            scenery.strips = st;
        }
        if(ch) {
            free(scenery.chunks);
            scenery.chunks = ch;
        }
    }
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
    static uint16_t order[LVL_MAX_TREES];
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

/* A chunk (trees, rocks, a prop) if it is in range and in view; headers
   only on a change. */
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

/* Rocks and props, from their own lists as the trees are: a cell's rocks
   near or far by its middle's distance, every prop (and lantern) to
   600 m. The scenery's own walk would read every chunk's record to find
   these few. */
#define ROCK_NEAR_M 90.0f
#define ROCK_FAR_M 360.0f
typedef struct {
    V3 lp;
    uint16_t chunk[2];   /* near, far */
} RockCell;
static RockCell rock_cells[TCELLS * TCELLS];
static uint16_t prop_chunks[2 * LVL_MAX_PROPS + GLOW_MAX];   /* some have a far version too */
/* Rock formations: near and far, by each one's own distance. */
#define FORM_NEAR_M 170.0f
#define FORM_FAR_M 900.0f
static uint16_t form_chunks[LVL_MAX_ROCKS][2];
static int form_n;
static int rock_cell_n, prop_chunk_n;

static void rocks_draw(void) {
    const pvr_poly_hdr_t *last = NULL;
    V3 cam = r_cam.pos;
    for(int i = 0; i < rock_cell_n; i++) {
        const RockCell *rc = &rock_cells[i];
        V3 d = vsub(rc->lp, cam);
        float d2 = vdot(d, d);
        if(d2 < ROCK_NEAR_M * ROCK_NEAR_M) tree_draw_chunk(rc->chunk[0], &last);
        else if(d2 < ROCK_FAR_M * ROCK_FAR_M) tree_draw_chunk(rc->chunk[1], &last);
    }
    for(int i = 0; i < prop_chunk_n; i++) tree_draw_chunk(prop_chunks[i], &last);
    for(int i = 0; i < form_n; i++) {
        tree_draw_chunk(form_chunks[i][0], &last);
        tree_draw_chunk(form_chunks[i][1], &last);
    }
}

void world_build(void) {
    for(int i = 0; i < TB_SLOTS * TB_SLOTS; i++) tblocks[i].bi = TB_EMPTY;
    for(int k = 0; k < 4; k++) h_tufts[k] = r_header(look->tuft_tex[k], HF_PT);
    dsky_build();
    h_stone = r_header(look->ground_tex, HF_OP);
    sc_clear(&scenery);
    rock_cell_n = prop_chunk_n = 0;
    lamps_clear();
    glow_place();
    terrain_build(&scenery);
    int terrain_v = scenery.pool_n;
    world_lrec_first = lrec_marked = scenery.pool_n;
    alb_n = alb_over_n = 0;
    /* Room for every vertex after the terrain's, whose count moves with
       an area's water: any area's far ground alone is over 20,000. */
    lrec_max = scenery.pool_max - 20000;
    if(!world_lrec) world_lrec = spent_alloc(sizeof(LightRec) * lrec_max);
    if(!world_lrec) rl_no_memory("the scenery's relighting");
    cap_build();
    mist_build();
    /* Boulders, logs and ledges: near and far versions in one chunk per cell. */
    for(int c = 0; c < TCELLS * TCELLS; c++) {
        float x0 = LEVEL_ORG + (c % TCELLS) * TCELL, z0 = LEVEL_ORG + (c / TCELLS) * TCELL;
        V3 lp = v3(x0 + TCELL / 2, 0, z0 + TCELL / 2);
        lp.y = terrain_height(lp.x, lp.z);
        RockCell *rc = &rock_cells[rock_cell_n];
        rc->lp = lp;
        for(int lod = 0; lod < 2; lod++) {
            uint32_t at = scenery.chunk_n;
            sc_chunk_begin(&scenery, HF_OP, lod ? ROCK_NEAR_M : 0.0f, lod ? ROCK_FAR_M : ROCK_NEAR_M, TAG_ROCKS);
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
                /* The desert's ledges are the bed they stand in, in its
                   stone; the mountain's grey rock. */
                if(lvl_area == AREA_MOAB)
                    box(h_stone, v3(b->x, b->y, b->z), b->hx, b->hy, b->hz, b->yaw, rock_bed(b->y + b->hy, false), 0.2f);
                else box(h_rock, v3(b->x, b->y, b->z), b->hx, b->hy, b->hz, b->yaw, look->ledge, 0.35f);
                if(lod == 0) lamps_add(f0, scenery.pool_n);
            }
            sc_chunk_end(&scenery);
            rc->chunk[lod] = scenery.chunk_n > at ? (uint16_t)at : TREE_NONE;
        }
        if(rc->chunk[0] != TREE_NONE || rc->chunk[1] != TREE_NONE) rock_cell_n++;
    }
    form_n = 0;
    for(int i = 0; i < lvl->nrocks && i < LVL_MAX_ROCKS; i++) {
        const lvl_rock_t *rk = &lvl->rocks[i];
        const lvl_ring_t *mid = &lvl->rings[rk->first + rk->n / 2];
        V3 lp = v3(mid->x, mid->y, mid->z);
        for(int lod = 0; lod < 2; lod++) {
            uint32_t at = scenery.chunk_n;
            sc_chunk_begin(&scenery, HF_OP, lod ? FORM_NEAR_M : 0.0f, lod ? FORM_FAR_M : FORM_NEAR_M, TAG_ROCKS);
            sc_chunk_lod_point(&scenery, lp);
            lamps_lod(lp, 90.0f);
            uint32_t f0 = scenery.pool_n;
            rock_loft(rk, lod);
            if(lod == 0) lamps_add(f0, scenery.pool_n);
            sc_chunk_end(&scenery);
            form_chunks[form_n][lod] = scenery.chunk_n > at ? (uint16_t)at : TREE_NONE;
        }
        form_n++;
    }
    lamps_lod(v3(0, 0, 0), 0);
    for(int i = 0; i < LVL_PROPS; i++) {
        /* From the top of the king dune the whole camp and the whole
           powerline are in view half a kilometre off: four thousand
           vertices of awning poles and insulators nobody can see. What
           is small goes by PROP_NEAR_M, and a trailer and an H-frame
           are their outlines from there on. */
        const lvl_prop_t *p = &lvl_props[i];
        bool small = p->kind == PROP_QUAD || p->kind == PROP_RAIL || p->kind == PROP_FIREPIT || p->kind == PROP_FLAG;
        bool two = p->kind == PROP_CAMPER || p->kind == PROP_PYLON;
        V3 lp = v3(p->x, p->y, p->z);
        uint32_t at = scenery.chunk_n;
        sc_chunk_begin(&scenery, HF_OP, 0, small || two ? PROP_NEAR_M : 600.0f, TAG_PROPS);
        sc_chunk_lod_point(&scenery, lp);
        uint32_t f0 = scenery.pool_n;
        prop(p);
        lamps_add(f0, scenery.pool_n);
        sc_chunk_end(&scenery);
        if(scenery.chunk_n > at) prop_chunks[prop_chunk_n++] = (uint16_t)at;
        if(!two) continue;
        at = scenery.chunk_n;
        sc_chunk_begin(&scenery, HF_OP, PROP_NEAR_M, 700.0f, TAG_PROPS);
        sc_chunk_lod_point(&scenery, lp);
        prop_far(p);
        sc_chunk_end(&scenery);
        if(scenery.chunk_n > at) prop_chunks[prop_chunk_n++] = (uint16_t)at;
    }
    marks_build();
    uint32_t tree_c = scenery.chunk_n, tree_p = scenery.pool_n;
    trees_build();
    for(int i = 0; i < glow_n; i++) {
        if(!glows[i].lantern) continue;
        uint32_t at = scenery.chunk_n;
        sc_chunk_begin(&scenery, HF_OP, 0, 600.0f, TAG_PROPS);
        uint32_t f0 = scenery.pool_n;
        lantern(&glows[i]);
        lamps_add(f0, scenery.pool_n);
        sc_chunk_end(&scenery);
        if(scenery.chunk_n > at) prop_chunks[prop_chunk_n++] = (uint16_t)at;
    }
    lamps_index();
    printf("Ridgeline world: %lu vertices (terrain %d, trees %lu), %lu strips, %lu chunks%s\n",
           (unsigned long)scenery.pool_n, terrain_v, (unsigned long)(scenery.pool_n - tree_p),
           (unsigned long)scenery.strip_n, (unsigned long)scenery.chunk_n,
           scenery.full || scenery.pool_n - world_lrec_first > lrec_max || alb_over_n ? " - FULL, some scenery missing" : "");
    printf("Ridgeline marks: %d chunks, %d colours in the scenery\n", mark_n, alb_n + alb_over_n);
    scenery.chunk_n = tree_c;
}

uint64_t world_us[3];

void world_draw_op(void) {
    PROF_BEGIN(pw);
    uint64_t t0 = timer_us_gettime64();
    terrain_draw_near();
    int skirts, far = terrain_draw_far(&scenery, &skirts);
    far += outland_draw();
#ifdef RL_AUTOTEST
    extern int ter_count[4];
    ter_count[3] += far;
    ter_count[2] += skirts;
#else
    (void)far;
#endif
    PROF_LAP(PS_TERRAIN, pw);
    uint64_t t1 = timer_us_gettime64();
    trees_draw();
    rocks_draw();
    PROF_LAP(PS_TREES, pw);
    uint64_t t2 = timer_us_gettime64();
    world_us[0] += t1 - t0;
    world_us[1] += t2 - t1;
}
