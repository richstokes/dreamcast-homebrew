/* The city (city.h): generation, drawing and demolition. */
#include "city.h"
#include "cel.h"
#include "fx.h"
#include "score.h"
#include "audio.h"
#include "life.h"
#include "engine/scenery.h"
#include "engine/sky.h"

enum { SK_WALL, SK_SHOP, SK_ROOF, SK_FLAT, SK_AD, SK_INK, SK_SHADOW, SK_RUBBLE, SK_PROP, SK_FAR /* and three more */ };
#define SK_BOX 0x80   /* the strip is a box's four walls, a face every four vertices */
enum { ST_OFFICE, ST_APT, ST_BRICK, ST_RIBBON, ST_GLASS, ST_TANK, ST_HOUSE, ST_WARE, ST_DECO, ST_COUNT };
enum { M_PLAIN, M_SETBACK, M_CROWN, M_SPIRE, M_HOUSE, M_TANK, M_TV, M_WARE, M_SAW, M_STACK, M_CONT, M_CRANE, M_STADIUM, M_PAGODA, M_TORII };

/* Room for the city at its size: a block averages seven buildings, and a
   building about 105 vertices with its far strips. Twelve seeds took
   315,000 to 340,000; a city that runs out is missing its last few
   buildings, and says so in the log. */
#define MAX_B (CITY_N * CITY_N * 8)
#define POOL_VERTS (CITY_N * CITY_N * 820)
#define MAX_TREES 1800
#define INK_W 0.42f
#define INK_RANGE 330.0f
#define DRAW_RANGE 1100.0f
#define DETAIL_RANGE 330.0f   /* rooftop clutter */
#define SMALL_RANGE 480.0f    /* the low buildings: beyond it only the skyline */
#define SHADOW_K 0.5f       /* shadows drawn this much of their true length */
#define WALL_TILE 8.0f      /* a wall texture: two bays by two floors */

Building *city_b;
int city_nb;
static RV *pool;
static uint32_t pool_n;
static int standing;
/* reach: how far from the block's middle its buildings stand. */
static struct { uint16_t first, count; float max_h, reach; uint8_t kind; } blocks[CITY_N * CITY_N];
enum { BK_NORMAL, BK_PARK, BK_GREEN };
/* The buildings with something going on (shaking, burning, a fuse lit,
   falling): all city_update looks at. */
static uint16_t awake[MAX_B];
static int nawake;
static inline void wake(int i) {
    if(!(city_b[i].flags & BF_AWAKE)) {
        city_b[i].flags |= BF_AWAKE;
        awake[nawake++] = (uint16_t)i;
    }
}
typedef struct { float x, z, s; bool alive; } Tree;
/* Trees pulled up: in the jaws, or in the air. */
#define UPROOTED 4
static struct { bool on, held; V3 p, v; float s, rot; } uprooted[UPROOTED];
static Tree trees[MAX_TREES];
static int ntrees;
static Scenery ground;
static SkyDome sky;
static const pvr_poly_hdr_t *h_wall[ST_COUNT], *h_shop, *h_roof, *h_flat, *h_ad, *h_rubble, *h_shadow, *h_tree,
    *h_cloud, *h_flat_nofog, *h_sun;
static int blast_depth;     /* the depth the next buildings to fall take */
/* Chunks knocked out of buildings, in flight. */
#define BODIES 40
typedef struct {
    bool on;
    uint8_t style;
    int16_t owner;
    V3 p, v;
    float ax, ay, wx, wy, hx, hy, hz;
    uint32_t tint;
    float dmg;      /* what it does to a building it lands on */
    bool held;      /* in the kaiju's jaws */
} Body;
static int rip_body = -2;   /* -2: not ripping; else the chunk city_rip is taking (-1: none yet) */
static Body bodies[BODIES];
static int body_next;

/* ---- The generator's own random numbers: a seed is a city ---- */
static uint32_t cr;
static uint32_t crand(void) {
    cr = cr * 1664525u + 1013904223u;
    return cr >> 8;
}
static float cf(void) { return (float)(crand() & 0xffff) / 65535.0f; }
static float cfr(float a, float b) { return a + (b - a) * cf(); }
static int ci(int n) { return (int)(crand() % (uint32_t)n); }

/* ---- Building geometry ---- */
static Building *cb;
static bool overflow;

static void s_begin(int kind) {
    if(cb->nstrips >= B_STRIPS) {
        overflow = true;
        return;
    }
    cb->strip[cb->nstrips].off = cb->nv;
    cb->strip[cb->nstrips].n = 0;
    cb->strip[cb->nstrips].kind = (uint8_t)kind;
}
static void s_v(float x, float y, float z, float u, float v, uint32_t c) {
    if(cb->nstrips >= B_STRIPS || cb->nv >= cb->vcap || cb->strip[cb->nstrips].n == 255) {
        overflow = true;
        return;
    }
    pool[cb->v0 + cb->nv++] = (RV){x, y, z, u, v, c};
    cb->strip[cb->nstrips].n++;
}
static void s_end(void) {
    if(cb->nstrips < B_STRIPS && cb->strip[cb->nstrips].n >= 3) cb->nstrips++;
}

static const float corner[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
static const float face_n[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

/* Four walls, each its own flat tone, as one strip (the corners repeat, so
   the joins are zero-area), and the first wall again at the end: any two
   walls that meet are then next to each other in the strip, so the pair
   the camera sees goes out in one piece. top_k narrows the top (rubble
   mounds). */
static void walls(int kind, float cx, float cz, float hw, float hd, float y0, float y1, float top_k, uint32_t tint,
                  float tile) {
    s_begin(kind | (top_k == 1.0f ? SK_BOX : 0));
    float vb = tile > 0 ? floorf((y1 - y0) / tile * 2.0f + 0.5f) * 0.5f : 1.0f;
    if(vb < 0.5f) vb = 0.5f;
    for(int f = 0; f < 5; f++) {
        int e = f & 3;
        const float *a = corner[e], *b = corner[(e + 1) & 3];
        float len = (e & 1) ? 2 * hd : 2 * hw;
        float nu = tile > 0 ? floorf(len / tile + 0.5f) : 1.0f;
        if(nu < 1) nu = 1;
        uint32_t c = cel_band(tint, face_n[e][0] * cel_sun.x + face_n[e][1] * cel_sun.z);
        s_v(cx + a[0] * hw, y0, cz + a[1] * hd, 0, vb, c);
        s_v(cx + a[0] * hw * top_k, y1, cz + a[1] * hd * top_k, 0, 0, c);
        s_v(cx + b[0] * hw, y0, cz + b[1] * hd, nu, vb, c);
        s_v(cx + b[0] * hw * top_k, y1, cz + b[1] * hd * top_k, nu, 0, c);
    }
    s_end();
}

static void roof(int kind, float cx, float cz, float hw, float hd, float y, uint32_t tint, float uv) {
    s_begin(kind);
    for(int k = 0; k < 4; k++) {
        float sx = (k & 1) ? 1.0f : -1.0f, sz = (k & 2) ? 1.0f : -1.0f;
        s_v(cx + sx * hw, y, cz + sz * hd, sx > 0 ? uv : 0, sz > 0 ? uv : 0, tint);
    }
    s_end();
}

/* The ink shell of a box: its walls pushed out, back faces only. */
static void ink(float cx, float cz, float hw, float hd, float y0, float y1) {
    s_begin(SK_INK);
    for(int k = 0; k <= 5; k++) {
        const float *a = corner[k & 3];
        s_v(cx + a[0] * (hw + INK_W), y0, cz + a[1] * (hd + INK_W), 0, 0, INK_COL);
        s_v(cx + a[0] * (hw + INK_W), y1 + INK_W, cz + a[1] * (hd + INK_W), 0, 0, INK_COL);
    }
    s_end();
}

static void box(int kind, float cx, float cz, float hw, float hd, float y0, float y1, uint32_t tint) {
    walls(kind, cx, cz, hw, hd, y0, y1, 1.0f, tint, 0);
    roof(kind, cx, cz, hw, hd, y1, cel_band(tint, cel_sun.y), 1.0f);
}

/* The shadow on the ground: the footprint swept away from the sun, a
   hexagon, with the halftone laid out in world space so that shadows
   that overlap print as one. */
static void shadow(float cx, float cz, float hw, float hd, float h) {
    float ax = cel_sun.x / cel_sun.y * h * SHADOW_K, bz = -cel_sun.z / cel_sun.y * h * SHADOW_K;
    float x0 = cx - hw, x1 = cx + hw, z0 = cz - hd, z1 = cz + hd;
    const float p[6][2] = {{x0, z0}, {x1, z0}, {x0 - ax, z0 + bz}, {x1, z1}, {x0 - ax, z1 + bz}, {x1 - ax, z1 + bz}};
    float bu = floorf(x0 * 0.125f), bv = floorf(z0 * 0.125f);
    s_begin(SK_SHADOW);
    for(int k = 0; k < 6; k++) s_v(p[k][0], 0.07f, p[k][1], p[k][0] * 0.125f - bu, p[k][1] * 0.125f - bv, 0xffffffffu);
    s_end();
}

static const uint32_t palette[ST_COUNT][5] = {
    {0xffeef0f6, 0xffd6e6f8, 0xfff6ead6, 0xffdcf2e2, 0xfff0dcec},
    {0xfffbe3c0, 0xffffd2c2, 0xfff4f0d0, 0xffd8e8f8, 0xffe8d4f0},
    {0xffee9c86, 0xfff2b890, 0xffdca888, 0xfff4d8a8, 0xffc8a0b0},
    {0xfff6f6f6, 0xffece2d0, 0xffd0e6ea, 0xffffe6b0, 0xffe0e0f4},
    {0xffffffff, 0xffd8f8ff, 0xffe8e0ff, 0xffc8ffe8, 0xffffe8d8},
    {0xfff4f4f4, 0xfff4f4f4, 0xfff4f4f4, 0xfff4f4f4, 0xfff4f4f4},
    {0xfffaf0dc, 0xfff8dcc0, 0xffe0ecf4, 0xfff4e0e8, 0xffe8f0d0},
    {0xffe4eef2, 0xfff2e8d4, 0xffd4e2ee, 0xffe2ecd4, 0xfff4ecec},
    {0xfffcf2dc, 0xfff2e2c4, 0xffece2da, 0xfffce6cc, 0xffe6e8ea},
};
/* Shipping containers, and the cranes that stack them. */
static const uint32_t box_cols[8] = {0xffd8503c, 0xff3c7cc8, 0xfff0b43c, 0xff48a068, 0xffe87c34, 0xff8c5cb4, 0xffd8d8dc, 0xff40a8b8};
static const uint32_t crane_cols[3] = {0xff3c78c8, 0xffd84840, 0xfff0c040};

/* Another piece on the end of the strip under way, joined by repeating a
   vertex either side (zero-area triangles, which the PowerVR drops). */
static void s_join(float x, float y, float z) {
    if(cb->nstrips < B_STRIPS && cb->strip[cb->nstrips].n) {
        RV last = pool[cb->v0 + cb->nv - 1];
        s_v(last.x, last.y, last.z, last.u, last.v, last.c);
        s_v(x, y, z, 0, 0, last.c);
    }
}
/* A flat quad on the strip under way: a and b along one edge, c and d the other. */
static void jquad(V3 a, V3 b, V3 c, V3 d, uint32_t col) {
    s_join(a.x, a.y, a.z);
    s_v(a.x, a.y, a.z, 0, 0, col);
    s_v(b.x, b.y, b.z, 0, 0, col);
    s_v(c.x, c.y, c.z, 0, 0, col);
    s_v(d.x, d.y, d.z, 0, 0, col);
}
static void jtri(V3 a, V3 b, V3 c, uint32_t col) {
    s_join(a.x, a.y, a.z);
    s_v(a.x, a.y, a.z, 0, 0, col);
    s_v(b.x, b.y, b.z, 0, 0, col);
    s_v(c.x, c.y, c.z, 0, 0, col);
}

/* A pitched roof over (x, z), ridge along the longer side, rh high, its
   eaves o past the walls; the gable ends under it in the wall's tint. */
static void gable(float x, float z, float hw, float hd, float h, float rh, float o, uint32_t rc, uint32_t tint) {
    bool along_x = hw >= hd;
    float inv = 1.0f / sqrtf(rh * rh + (along_x ? hd * hd : hw * hw));
    float ny = (along_x ? hd : hw) * inv, ns = rh * inv;
    for(int side = -1; side <= 1; side += 2) {
        float d = along_x ? ny * cel_sun.y + side * ns * cel_sun.z : ny * cel_sun.y + side * ns * cel_sun.x;
        uint32_t c = cel_band(rc, d);
        s_begin(SK_FLAT);
        for(int k = 0; k < 4; k++) {
            float along = (k & 1) ? 1.0f : -1.0f, up = (k & 2) ? 1.0f : 0.0f;
            float ax = along_x ? along * (hw + o) : side * (hw + o) * (1 - up);
            float az = along_x ? side * (hd + o) * (1 - up) : along * (hd + o);
            s_v(x + ax, h + up * rh - (1 - up) * 0.5f, z + az, 0, 0, c);
        }
        s_end();
        c = cel_band(tint, along_x ? side * cel_sun.x : side * cel_sun.z);
        s_begin(SK_FLAT);
        if(along_x) {
            s_v(x + side * hw, h, z - hd, 0, 0, c);
            s_v(x + side * hw, h, z + hd, 0, 0, c);
            s_v(x + side * hw, h + rh, z, 0, 0, c);
        }
        else {
            s_v(x - hw, h, z + side * hd, 0, 0, c);
            s_v(x + hw, h, z + side * hd, 0, 0, c);
            s_v(x, h + rh, z + side * hd, 0, 0, c);
        }
        s_end();
    }
}

/* The way the next crane's jib points: a face_n index, out to sea. */
static int gen_face;

static Building *add_building(int block, float x, float z, float hw, float hd, float h, int style, int massing, int flags) {
    if(city_nb >= MAX_B || pool_n + 700 > POOL_VERTS) {
        overflow = true;
        return NULL;
    }
    Building *b = cb = &city_b[city_nb];
    memset(b, 0, sizeof(*b));
    b->x = x; b->z = z; b->hw = hw; b->hd = hd; b->h = b->h0 = h;
    b->style = (uint8_t)style;
    b->block = (uint16_t)block;
    b->tint = palette[style][ci(5)];
    b->v0 = pool_n;
    b->vcap = 700;
    uint32_t tint = b->tint, rooftint = color_lerp(tint, 0xffe6e6ee, 0.6f);
    float top = h;      /* where the roof props stand */
    float thw = hw, thd = hd;
    float shw = hw, shd = hd, sh_h = h;   /* what casts the shadow */
    switch(massing) {
        case M_HOUSE: {
            flags |= BF_ONE;
            walls(SK_WALL, x, z, hw, hd, 0, h, 1, tint, WALL_TILE);
            ink(x, z, hw, hd, 0, h);
            static const uint32_t tiles[4] = {0xffd8644c, 0xffa85c50, 0xff7c8cac, 0xff6c9c74};
            gable(x, z, hw, hd, h, fminf(hw, hd) * 0.75f, 0.8f, tiles[ci(4)], tint);
            top = -1;
            break;
        }
        case M_WARE: {
            flags |= BF_ONE;
            /* A shed: corrugated walls, a shallow pitch. */
            static const uint32_t tins[4] = {0xff8c9cb0, 0xff98b098, 0xffc4c8d0, 0xffb87c64};
            walls(SK_WALL, x, z, hw, hd, 0, h, 1, tint, WALL_TILE);
            ink(x, z, hw, hd, 0, h);
            gable(x, z, hw, hd, h, fminf(hw, hd) * 0.24f, 0.6f, tins[ci(4)], tint);
            top = -1;
            break;
        }
        case M_SAW: {
            flags |= BF_ONE;
            /* A factory: a saw-tooth roof, its glass facing north. */
            walls(SK_WALL, x, z, hw, hd, 0, h, 1, tint, WALL_TILE);
            ink(x, z, hw, hd, 0, h);
            int n = (int)(hd * 2.0f / 8.0f);
            n = n < 2 ? 2 : n > 8 ? 8 : n;
            float tw = hd * 2.0f / (float)n, t = 4.2f, l = sqrtf(t * t + tw * tw);
            uint32_t slope = cel_band(0xffb4b8c4, (tw * cel_sun.y - t * cel_sun.z) / l);
            uint32_t glass = cel_band(0xff5c88c4, cel_sun.z), end_e = cel_band(tint, cel_sun.x), end_w = cel_band(tint, -cel_sun.x);
            s_begin(SK_FLAT);
            for(int k = 0; k < n; k++) {
                float z0 = z - hd + tw * (float)k, z1 = z0 + tw;
                jquad(v3(x - hw, h, z0), v3(x + hw, h, z0), v3(x - hw, h + t, z1), v3(x + hw, h + t, z1), slope);
            }
            s_end();
            s_begin(SK_FLAT);
            for(int k = 0; k < n; k++) {
                float z1 = z - hd + tw * (float)(k + 1);
                jquad(v3(x - hw, h + t, z1), v3(x + hw, h + t, z1), v3(x - hw, h, z1), v3(x + hw, h, z1), glass);
            }
            s_end();
            s_begin(SK_FLAT);
            for(int k = 0; k < n; k++) {
                float z0 = z - hd + tw * (float)k, z1 = z0 + tw;
                jtri(v3(x - hw, h, z0), v3(x - hw, h, z1), v3(x - hw, h + t, z1), end_w);
                jtri(v3(x + hw, h, z0), v3(x + hw, h, z1), v3(x + hw, h + t, z1), end_e);
            }
            s_end();
            top = -1;
            break;
        }
        case M_STACK: {
            /* A chimney on a brick plinth, in red and white bands. */
            walls(SK_WALL, x, z, hw, hd, 0, 7, 1, tint, WALL_TILE);
            roof(SK_ROOF, x, z, hw, hd, 7, rooftint, 1);
            ink(x, z, hw, hd, 0, 7);
            float s = fminf(hw, hd) * 0.42f;
            for(int k = 0; k < 4; k++) {
                float y0 = 7 + (h - 7) * (float)k / 4.0f, y1 = 7 + (h - 7) * (float)(k + 1) / 4.0f;
                walls(SK_FLAT, x, z, s, s, y0, y1, 1, (k & 1) ? 0xffe85048 : 0xfff2eeea, 0);
            }
            roof(SK_FLAT, x, z, s, s, h, 0xff3c3848, 1);
            ink(x, z, s, s, 7, h);
            shw = shd = s;
            top = -1;
            break;
        }
        case M_CONT: {
            flags |= BF_ONE;
            /* A stack of shipping containers, each its own colour. */
            int layers = (int)(h / 2.6f + 0.5f);
            for(int k = 0; k < layers; k++)
                walls(SK_WALL, x, z, hw, hd, 2.6f * (float)k, 2.6f * (float)(k + 1), 1, box_cols[ci(8)], 6.0f);
            roof(SK_FLAT, x, z, hw, hd, h, cel_band(0xffc8ccd4, cel_sun.y), 1);
            ink(x, z, hw, hd, 0, h);
            top = -1;
            break;
        }
        case M_CRANE: {
            flags |= BF_ONE;
            /* A container crane on the quay: four legs, a deck, the cab
               house and a jib out over the water. */
            uint32_t col = crane_cols[ci(3)];
            float fx = face_n[gen_face][0], fz = face_n[gen_face][1];
            float ext = fabsf(fx) * hw + fabsf(fz) * hd;      /* the footprint along the jib */
            for(int k = 0; k < 4; k++)
                walls(SK_FLAT, x + corner[k][0] * (hw - 1.1f), z + corner[k][1] * (hd - 1.1f), 1.1f, 1.1f, 0, h - 6, 1, col, 0);
            box(SK_FLAT, x, z, hw, hd, h - 6, h - 3, col);
            ink(x, z, hw, hd, h - 6, h - 3);
            box(SK_FLAT, x - fx * ext * 0.4f, z - fz * ext * 0.4f, hw * 0.5f, hd * 0.5f, h - 3, h + 2, 0xfff2f0ee);
            float front = ext + 24.0f, back = ext + 9.0f, along = (front + back) * 0.5f, off = (front - back) * 0.5f;
            float jw = fabsf(fx) * along + fabsf(fz) * 1.4f, jd = fabsf(fz) * along + fabsf(fx) * 1.4f;
            box(SK_FLAT, x + fx * off, z + fz * off, jw, jd, h - 1, h + 1.2f, col);
            ink(x + fx * off, z + fz * off, jw, jd, h - 1, h + 1.2f);
            for(int k = 0; k < 4; k++) shadow(x + corner[k][0] * (hw - 1.1f), z + corner[k][1] * (hd - 1.1f), 1.1f, 1.1f, h - 6);
            flags |= BF_NOSHADOW | BF_FAR1;
            top = -1;
            break;
        }
        case M_STADIUM: {
            flags |= BF_ONE;
            /* A bowl: the outside, terraces down to the pitch, floodlights. */
            static const uint32_t team[4] = {0xffe85048, 0xff3c7cc8, 0xfff0b43c, 0xff48a068};
            uint32_t tc = team[ci(4)];
            walls(SK_WALL, x, z, hw, hd, 0, h - 3, 1, 0xfff0eef4, WALL_TILE);
            walls(SK_FLAT, x, z, hw, hd, h - 3, h, 1, tc, 0);
            ink(x, z, hw, hd, 0, h);
            #define SP(e, in, y) v3(x + corner[e][0] * (hw - (in)), (y), z + corner[e][1] * (hd - (in)))
            /* The terraces: rows of seats in two shades of the team's colour. */
            uint32_t dark = color_lerp(tc, 0xff303048, 0.35f), pale = color_lerp(tc, 0xffffffff, 0.45f);
            s_begin(SK_FLAT);
            for(int f = 0; f < 4; f++) {
                int a = f, c = (f + 1) & 3;
                float lit = -face_n[f][0] * cel_sun.x - face_n[f][1] * cel_sun.z;
                jquad(SP(a, 0, h), SP(c, 0, h), SP(a, 1.4f, h), SP(c, 1.4f, h), cel_band(0xffe0e0e8, cel_sun.y));
                for(int row = 0; row < 5; row++) {
                    float i0 = 1.4f + 1.5f * (float)row, i1 = i0 + 1.5f, y0 = h - (h - 2.5f) * (float)row / 5.0f, y1 = h - (h - 2.5f) * (float)(row + 1) / 5.0f;
                    jquad(SP(a, i0, y0), SP(c, i0, y0), SP(a, i1, y1), SP(c, i1, y1), cel_band((row & 1) ? dark : pale, 0.55f * cel_sun.y + 0.8f * lit));
                }
            }
            s_end();
            /* The pitch, mown in stripes, a line across the middle. */
            s_begin(SK_FLAT);
            float px = hw - 8.9f, pz = hd - 8.9f;
            for(int k = 0; k < 6; k++) {
                float x0 = x - px + 2 * px * (float)k / 6.0f, x1 = x - px + 2 * px * (float)(k + 1) / 6.0f;
                jquad(v3(x0, 2.5f, z - pz), v3(x1, 2.5f, z - pz), v3(x0, 2.5f, z + pz), v3(x1, 2.5f, z + pz), (k & 1) ? 0xff84d070 : 0xff64b058);
            }
            jquad(v3(x - 0.35f, 2.56f, z - pz), v3(x + 0.35f, 2.56f, z - pz), v3(x - 0.35f, 2.56f, z + pz), v3(x + 0.35f, 2.56f, z + pz), 0xfff8f8f8);
            s_end();
            #undef SP
            for(int k = 0; k < 4; k++) {
                float lx = x + corner[k][0] * (hw - 1.0f), lz = z + corner[k][1] * (hd - 1.0f);
                /* (props: close up only, and not in the far strip) */
                walls(SK_PROP, lx, lz, 0.6f, 0.6f, h, h + 15, 1, 0xff9ca0b0, 0);
                walls(SK_PROP, lx, lz, 2.6f, 2.6f, h + 15, h + 18, 1, 0xfffff4c0, 0);
            }
            flags |= BF_FAR1;
            top = -1;
            break;
        }
        case M_PAGODA: {
            flags |= BF_ONE;
            /* Four storeys under swept roofs, a gilt spire on top. */
            box(SK_FLAT, x, z, hw, hd, 0, 2.5f, 0xffd8d0c4);
            ink(x, z, hw, hd, 0, 2.5f);
            float y = 2.5f, w = fminf(hw, hd) * 0.6f;
            for(int k = 0; k < 4; k++) {
                float bh = 5.4f - 0.4f * (float)k, nw = w * 0.84f, eave = w + 3.2f;
                walls(SK_FLAT, x, z, w, w, y, y + bh, 1, k ? 0xffd84838 : 0xfff4ece0, 0);
                ink(x, z, w, w, y, y + bh);
                walls(SK_FLAT, x, z, eave, eave, y + bh, y + bh + 2.6f, k == 3 ? 0.12f : (nw + 0.2f) / eave, 0xff3c6c78, 0);
                y += bh + 2.6f;
                w = nw;
            }
            walls(SK_FLAT, x, z, 0.45f, 0.45f, y - 1, y + 9, 1, 0xffe8c050, 0);
            h = b->h = b->h0 = y + 9;
            shw = shd = fminf(hw, hd) * 0.7f;
            sh_h = h * 0.8f;
            flags |= BF_FAR1;
            top = -1;
            break;
        }
        case M_TORII: {
            flags |= BF_ONE;
            /* The gate: two posts, a tie beam, the lintel over. */
            float px = hw - 1.2f;
            walls(SK_FLAT, x - px, z, 0.8f, 0.8f, 0, h - 1.4f, 1, 0xffe0482c, 0);
            walls(SK_FLAT, x + px, z, 0.8f, 0.8f, 0, h - 1.4f, 1, 0xffe0482c, 0);
            box(SK_FLAT, x, z, hw - 0.6f, 0.6f, h * 0.72f, h * 0.72f + 1, 0xffe0482c);
            box(SK_FLAT, x, z, hw + 1.0f, 0.9f, h - 1.4f, h, 0xff302830);
            flags |= BF_NOSHADOW;
            top = -1;
            break;
        }
        case M_TANK:
            flags |= BF_ONE;
            walls(SK_FLAT, x, z, hw, hd, 0, h * 0.3f, 1, 0xffe85048, 0);
            walls(SK_FLAT, x, z, hw, hd, h * 0.3f, h, 1, tint, 0);
            roof(SK_FLAT, x, z, hw, hd, h, 0xffe0e0e8, 1);
            ink(x, z, hw, hd, 0, h);
            box(SK_FLAT, x, z, hw * 0.25f, hd * 0.25f, h, h + 1.5f, 0xffe85048);
            top = -1;
            break;
        case M_TV: {
            /* A podium, a shaft, the pod and the mast. */
            box(SK_FLAT, x, z, hw * 2.6f, hd * 2.6f, 0, 7, 0xffe8e4ec);
            ink(x, z, hw * 2.6f, hd * 2.6f, 0, 7);
            walls(SK_FLAT, x, z, hw, hd, 7, h * 0.66f, 0.7f, 0xfff4f0f0, 0);
            ink(x, z, hw, hd, 7, h * 0.66f);
            box(SK_WALL, x, z, hw * 2.4f, hd * 2.4f, h * 0.66f, h * 0.76f, 0xffffffff);
            ink(x, z, hw * 2.4f, hd * 2.4f, h * 0.66f, h * 0.76f);
            box(SK_FLAT, x, z, hw * 0.25f, hd * 0.25f, h * 0.76f, h, 0xffe85048);
            sh_h = h * 0.7f;
            top = -1;
            break;
        }
        default: {
            int tiers = massing == M_SETBACK || massing == M_SPIRE ? 3 : massing == M_CROWN ? 2 : 1;
            static const float set_y[3] = {0.62f, 0.86f, 1.0f}, set_k[3] = {1.0f, 0.78f, 0.56f};
            static const float spire_y[3] = {0.7f, 0.9f, 1.0f}, spire_k[3] = {1.0f, 0.84f, 0.68f};
            static const float crown_y[2] = {0.9f, 1.0f}, crown_k[2] = {1.0f, 0.62f};
            const float *ty = massing == M_SETBACK ? set_y : massing == M_SPIRE ? spire_y : crown_y;
            const float *tk = massing == M_SETBACK ? set_k : massing == M_SPIRE ? spire_k : crown_k;
            float y0 = 0;
            for(int t = 0; t < tiers; t++) {
                float k = tiers > 1 ? tk[t] : 1.0f;
                float y1 = h * (tiers > 1 ? ty[t] : 1.0f);
                y1 = floorf(y1 * 0.25f + 0.5f) * 4.0f;   /* whole floors */
                if(t == tiers - 1) y1 = h;
                float wy0 = y0;
                if(t == 0 && h > 11 && style != ST_GLASS) {
                    /* Shops along the street. */
                    walls(SK_SHOP, x, z, hw, hd, 0, 5, 1, tint, 16.0f);
                    /* (its v runs one tile, whatever the height) */
                    for(int q = 0; q < cb->strip[cb->nstrips - 1].n; q++) {
                        RV *v = &pool[cb->v0 + cb->strip[cb->nstrips - 1].off + q];
                        if(v->v > 0) v->v = 1.0f;
                    }
                    wy0 = 5;
                }
                walls(SK_WALL, x, z, hw * k, hd * k, wy0, y1, 1, tint, WALL_TILE);
                roof(SK_ROOF, x, z, hw * k, hd * k, y1, rooftint, 1);
                ink(x, z, hw * k, hd * k, y0, y1);
                thw = hw * k;
                thd = hd * k;
                y0 = y1;
                b->tier_y[t] = y1;
                b->tier_k[t] = k;
            }
            b->ntiers = (uint8_t)tiers;
            if(h >= 32 && !(flags & BF_DONUT)) {
                /* Big enough to come apart: bands of about three floors. */
                int bands = (int)(h / 12.0f + 0.5f);
                b->bands = (uint8_t)(bands > CITY_BANDS ? CITY_BANDS : bands);
                b->band_h = h / (float)b->bands;
                b->cols = fminf(hw, hd) >= 9.5f ? 4 : 1;
                memset(b->cell, b->cols == 4 ? 0xf : 1, b->bands);
                b->cells = b->cells_left = (uint8_t)(b->bands * b->cols);
            }
            if(massing == M_CROWN) {
                /* A mast, red and white. */
                box(SK_FLAT, x, z, 0.9f, 0.9f, h, h * 1.16f, 0xffe85048);
                top = -1;
            }
            else if(massing == M_SPIRE) {
                /* A pointed cap in copper, gold or steel, and a needle. */
                static const uint32_t caps[3] = {0xff78b0a0, 0xffe8c060, 0xffd0d8e8};
                float ch = fminf(thw, thd) * 1.6f;
                walls(SK_FLAT, x, z, thw, thd, h, h + ch, 0.06f, caps[ci(3)], 0);
                walls(SK_FLAT, x, z, 0.5f, 0.5f, h + ch * 0.9f, h + ch + h * 0.08f, 1, 0xffd0d8e8, 0);
                top = -1;
            }
            break;
        }
    }
    b->flags = (uint8_t)flags;
    /* On the roof. */
    if(top > 0 && thw > 5 && thd > 5) {
        int prop = ci(6);
        if(flags & BF_DONUT) prop = 6;
        switch(prop) {
            case 0: case 1:
                box(SK_PROP, x + cfr(-0.4f, 0.4f) * thw, z + cfr(-0.4f, 0.4f) * thd, 2.0f, 1.6f, top, top + 2.2f, 0xffc8ccd8);
                break;
            case 2: {
                /* A billboard facing the sun, another behind it. */
                float w = fminf(thw * 0.8f, 9.0f), y1 = top + w * 1.0f, u0 = 0.5f * (float)ci(2), v0 = 0.5f * (float)ci(2);
                float pz = z + cfr(-0.3f, 0.3f) * thd;
                for(int side = 0; side < 2; side++) {
                    float zz = pz + (side ? 0.2f : -0.2f);
                    uint32_t c = side ? 0xffb0b0c8 : 0xffffffff;
                    s_begin(SK_AD);
                    s_v(x + (side ? w : -w), top + 1, zz, u0, v0 + 0.5f, c);
                    s_v(x + (side ? w : -w), y1 + 1, zz, u0, v0, c);
                    s_v(x + (side ? -w : w), top + 1, zz, u0 + 0.5f, v0 + 0.5f, c);
                    s_v(x + (side ? -w : w), y1 + 1, zz, u0 + 0.5f, v0, c);
                    s_end();
                }
                box(SK_PROP, x, pz, w + 0.3f, 0.35f, top, top + 1, INK_COL);
                break;
            }
            case 3:
                b->prop = PROP_WTANK;
                b->prop_x = x + cfr(-0.35f, 0.35f) * thw;
                b->prop_y = top;
                b->prop_z = z + cfr(-0.35f, 0.35f) * thd;
                break;
            case 4:
                walls(SK_PROP, x + cfr(-0.3f, 0.3f) * thw, z + cfr(-0.3f, 0.3f) * thd, 0.35f, 0.35f, top, top + cfr(8, 16), 1, 0xff505068, 0);
                break;
            case 6:
                b->prop = PROP_DONUT;
                b->prop_x = x;
                b->prop_y = top;
                b->prop_z = z;
                break;
            default: break;
        }
    }
    if(!(flags & BF_NOSHADOW)) shadow(x, z, shw, shd, sh_h);
    /* From far off: the walls and roofs again as one strip under the wall's
       texture. A tall building has one for each quarter the camera can be
       in, with only the two walls of each box that face that way; a low one
       (drawn far off only as far as SMALL_RANGE) has one of all its walls,
       which costs a few vertices more to draw and a lot less to keep.
       Whatever is not wall takes a texel of plain wall, which at that
       distance is the texture's average anyway. */
    if(h < 36.0f || (flags & BF_ONE)) flags |= BF_FAR1;
    b->flags = (uint8_t)flags;
    int parts = b->nstrips, quarters = (flags & BF_FAR1) ? 1 : 4;
    bool shops = false;
    for(int quarter = 0; quarter < quarters; quarter++) {
        /* The pair of walls: east or west (bit 0), north or south (bit 1). */
        static const uint8_t pair_at[4] = {12, 0, 8, 4};
        s_begin(SK_FAR + quarter);
        for(int s = 0; s < parts; s++) {
            int kind = b->strip[s].kind & ~SK_BOX, n = b->strip[s].n;
            if(kind != SK_WALL && kind != SK_ROOF && kind != SK_FLAT && kind != SK_SHOP) continue;
            if(kind == SK_SHOP) {
                /* The shops' band goes in with the wall over it. */
                shops = true;
                continue;
            }
            const RV *v = &pool[b->v0 + b->strip[s].off];
            int first = 0;
            if(quarters == 4 && (b->strip[s].kind & SK_BOX) && n == 20) {
                first = pair_at[quarter];
                n = 8;
            }
            bool plain = kind != SK_WALL;
            bool down = shops && kind == SK_WALL;   /* the wall over the shops: down to the street */
            for(int q = -1; q < n; q++) {
                RV w = v[first + (q < 0 ? 0 : q)];
                if(plain) w.u = w.v = 0.03f;
                if(down && !((first + (q < 0 ? 0 : q)) & 1)) {
                    w.v += w.y * (1.0f / WALL_TILE);
                    w.y = 0;
                }
                if(q < 0) {
                    /* Joined to the last piece. */
                    if(cb->nstrips < B_STRIPS && cb->strip[cb->nstrips].n) {
                        RV last = pool[b->v0 + b->nv - 1];
                        s_v(last.x, last.y, last.z, last.u, last.v, last.c);
                        s_v(w.x, w.y, w.z, w.u, w.v, w.c);
                    }
                    continue;
                }
                s_v(w.x, w.y, w.z, w.u, w.v, w.c);
            }
            if(down) shops = false;
        }
        s_end();
        shops = false;
    }
    if(flags & BF_ONE) {
        /* Close up too it is the far strip, with its ink, shadow and
           props: the walls and roofs on their own go. */
        int keep = 0;
        uint16_t at = 0;
        for(int s = 0; s < b->nstrips; s++) {
            int kind = b->strip[s].kind & ~SK_BOX, n = b->strip[s].n;
            if(kind == SK_WALL || kind == SK_ROOF || kind == SK_FLAT || kind == SK_SHOP) continue;
            memmove(&pool[b->v0 + at], &pool[b->v0 + b->strip[s].off], (size_t)n * sizeof(RV));
            b->strip[keep] = b->strip[s];
            b->strip[keep].off = at;
            keep++;
            at = (uint16_t)(at + n);
        }
        b->nstrips = (uint8_t)keep;
        b->nv = at;
    }
    /* (room for the rubble it turns into: a mound, and a slab if it falls) */
    b->vcap = b->nv < 44 ? 44 : b->nv;
    pool_n += b->vcap;
    b->hp = b->hp_max = (30.0f + h * 3.0f + (hw + hd) * 1.5f) * (b->bands ? 1.5f : 1.0f);
    b->value = (uint32_t)(hw * hd * 4.0f * h * 1.2f) * ((flags & BF_LANDMARK) ? 3 : 1);
    b->len = h;
    city_nb++;
    blocks[block].count++;
    if(h > blocks[block].max_h) blocks[block].max_h = h;
    float bx = city_road(block % CITY_N) + CITY_PITCH * 0.5f, bz = city_road(block / CITY_N) + CITY_PITCH * 0.5f;
    float reach = sqrtf((x - bx) * (x - bx) + (z - bz) * (z - bz)) + fmaxf(hw, hd) + (massing == M_CRANE ? 26.0f : 0.0f);
    if(reach > blocks[block].reach) blocks[block].reach = reach;
    return b;
}

static void plant(float x, float z, float s) {
    if(ntrees < MAX_TREES) trees[ntrees++] = (Tree){x, z, s, true};
}

/* An nx by nz grid of lots in a block. */
static void lots(int block, float cx, float cz, int nx, int nz, float h0, float h1, const uint8_t *styles, int nstyles,
                 float setback_p, float crown_p, float gap_p) {
    float cw = 56.0f / (float)nx, cd = 56.0f / (float)nz;
    for(int j = 0; j < nz; j++) {
        for(int i = 0; i < nx; i++) {
            float x = cx - 28.0f + cw * ((float)i + 0.5f), z = cz - 28.0f + cd * ((float)j + 0.5f);
            if(cf() < gap_p) {
                /* An empty lot: a couple of trees. */
                for(int t = 0; t < 2; t++) plant(x + cfr(-0.3f, 0.3f) * cw, z + cfr(-0.3f, 0.3f) * cd, cfr(11, 16));
                continue;
            }
            float hw = cw * 0.5f - cfr(1.4f, 3.0f), hd = cd * 0.5f - cfr(1.4f, 3.0f);
            float h = floorf(cfr(h0, h1) * 0.25f + 0.5f) * 4.0f;
            int style = styles[ci(nstyles)];
            int massing = M_PLAIN;
            if(style == ST_HOUSE) {
                massing = M_HOUSE;
                h = cfr(6.5f, 10.0f);
            }
            else if(h > 40) {
                float r = cf();
                massing = r < setback_p ? M_SETBACK : r < setback_p + crown_p ? M_CROWN : r < setback_p + crown_p + 0.12f ? M_SPIRE : M_PLAIN;
            }
            add_building(block, x, z, hw, hd, h, style, massing, 0);
        }
    }
}

/* Houses on their own plots, a tree in most gardens. */
static void houses(int block, float cx, float cz, int n) {
    float cw = 56.0f / (float)n;
    static const uint8_t st[] = {ST_HOUSE, ST_HOUSE, ST_HOUSE, ST_BRICK};
    for(int j = 0; j < n; j++) {
        for(int i = 0; i < n; i++) {
            float x = cx - 28.0f + cw * ((float)i + 0.5f), z = cz - 28.0f + cw * ((float)j + 0.5f);
            if(cf() < 0.14f) {
                plant(x + cfr(-0.2f, 0.2f) * cw, z, cfr(11, 16));
                continue;
            }
            float hw = cw * cfr(0.26f, 0.34f), hd = cw * cfr(0.24f, 0.32f);
            float ox = cfr(-0.1f, 0.1f) * cw, oz = cfr(-0.1f, 0.1f) * cw;
            add_building(block, x + ox, z + oz, hw, hd, cfr(6.5f, 9.5f), st[ci(4)] == ST_BRICK ? ST_BRICK : ST_HOUSE,
                         M_HOUSE, 0);
            if(cf() < 0.55f) {
                float tx = x + (ox > 0 ? -1.0f : 1.0f) * cw * 0.38f, tz = z + (oz > 0 ? -1.0f : 1.0f) * cw * 0.38f;
                plant(tx, tz, cfr(10, 14));
            }
        }
    }
}

/* Joined rows round the block's edge, shops under them, a yard of trees
   in the middle. */
static void terraces(int block, float cx, float cz, float h0, float h1, const uint8_t *styles, int nstyles) {
    const float half = 28.0f;
    float depth = cfr(9.5f, 12.5f);
    for(int side = 0; side < 4; side++) {
        float len = (side & 1) ? 2 * (half - depth) : 2 * half;
        int n = 2 + ci(2);
        float seg = len / (float)n;
        for(int k = 0; k < n; k++) {
            float a = -len * 0.5f + seg * ((float)k + 0.5f), off = half - depth * 0.5f;
            float h = floorf(cfr(h0, h1) * 0.25f + 0.5f) * 4.0f;
            float along = seg * 0.5f - 0.25f, across = depth * 0.5f;
            float x = cx, z = cz, hw, hd;
            switch(side) {
                case 0: x += a; z -= off; hw = along; hd = across; break;
                case 1: x += off; z += a; hw = across; hd = along; break;
                case 2: x -= a; z += off; hw = along; hd = across; break;
                default: x -= off; z -= a; hw = across; hd = along; break;
            }
            add_building(block, x, z, hw, hd, h, styles[ci(nstyles)], h > 30 && cf() < 0.3f ? M_CROWN : M_PLAIN, 0);
        }
    }
    int t = 2 + ci(3);
    for(int k = 0; k < t; k++) plant(cx + cfr(-1, 1) * (half - depth - 4), cz + cfr(-1, 1) * (half - depth - 4), cfr(11, 15));
}

/* Shipping containers in rows, x0..x1 by z0..z1. */
static void containers(int block, float x0, float x1, float z0, float z1, bool along_x) {
    float lx = along_x ? 6.1f : 2.5f, lz = along_x ? 2.5f : 6.1f;
    float sx = lx * 2 + (along_x ? 3.0f : 1.6f), sz = lz * 2 + (along_x ? 1.6f : 3.0f);
    int nx = (int)((x1 - x0) / sx), nz = (int)((z1 - z0) / sz);
    float mx = (x0 + x1) * 0.5f - (float)(nx - 1) * sx * 0.5f, mz = (z0 + z1) * 0.5f - (float)(nz - 1) * sz * 0.5f;
    for(int j = 0; j < nz; j++)
        for(int i = 0; i < nx; i++)
            if(cf() > 0.3f) add_building(block, mx + sx * (float)i, mz + sz * (float)j, lx, lz, 2.6f * (float)(1 + ci(3)), ST_WARE, M_CONT, 0);
}

/* ---- Districts ---- */
enum { D_CORE, D_MID, D_UPTOWN, D_OLDTOWN, D_SUBURB, D_INDUSTRY, D_MIXED };
static uint8_t district[CITY_N * CITY_N];
/* The shore with the docks (a face_n index), and the blocks along it that have them. */
static int harbor_side;
static bool harbor[CITY_N * CITY_N];
#define C_MID (CITY_N / 2)

static inline int block_side(int i, int j) {
    return j == 0 ? 0 : i == CITY_N - 1 ? 1 : j == CITY_N - 1 ? 2 : i == 0 ? 3 : -1;
}

static void industry(int bi, float cx, float cz) {
    float r = cf();
    if(r < 0.38f) {
        /* Two sheds. */
        bool ax = ci(2);
        for(int k = -1; k <= 1; k += 2) {
            float hw = cfr(10.5f, 12.5f), hd = cfr(20, 25.5f);
            add_building(bi, cx + (ax ? k * 14.0f : 0), cz + (ax ? 0 : k * 14.0f), ax ? hw : hd, ax ? hd : hw, floorf(cfr(9, 14)),
                         ST_WARE, cf() < 0.5f ? M_SAW : M_WARE, 0);
        }
    }
    else if(r < 0.62f) {
        /* A works: one long shed, an office, a chimney or two. */
        add_building(bi, cx - 4, cz + 6, 22, 15.5f, floorf(cfr(10, 13)), ST_WARE, M_SAW, 0);
        add_building(bi, cx + 16, cz - 19, 10, 6.5f, 12, ci(2) ? ST_BRICK : ST_APT, M_PLAIN, 0);
        add_building(bi, cx - 19, cz - 19, 5.5f, 5.5f, floorf(cfr(46, 72)), ST_BRICK, M_STACK, 0);
        if(cf() < 0.4f) add_building(bi, cx - 7, cz - 21, 4.5f, 4.5f, floorf(cfr(36, 52)), ST_BRICK, M_STACK, 0);
    }
    else if(r < 0.84f) containers(bi, cx - 28, cx + 28, cz - 28, cz + 28, ci(2));
    else {
        /* Fuel: four tanks, and they go up. */
        for(int k = 0; k < 4; k++)
            add_building(bi, cx + ((k & 1) ? 14.0f : -14.0f), cz + ((k & 2) ? 14.0f : -14.0f), 9, 9, 15, ST_TANK, M_TANK, BF_EXPLOSIVE);
    }
}

/* The quay in front of an edge block: a crane at the water and boxes
   stacked behind it. Its buildings stand on the beach, outside the block,
   but belong to it. */
static void docks(int bi, int i, int j) {
    float b0 = city_road(harbor_side & 1 ? j : i) + CITY_ROAD * 0.5f + 2, b1 = b0 + CITY_BLOCK - 4;
    float in = CITY_HALF + 3, out = CITY_SHORE - 2;
    float fx = face_n[harbor_side][0], fz = face_n[harbor_side][1];
    /* Along the shore u, out to sea w: to x and z. */
    #define DX(u, w) (fx != 0 ? fx * (w) : (u))
    #define DZ(u, w) (fz != 0 ? fz * (w) : (u))
    gen_face = harbor_side;
    if(cf() < 0.7f) {
        float u = cfr(b0 + 10, b1 - 10), w = out - 7.5f;
        add_building(bi, DX(u, w), DZ(u, w), fx != 0 ? 7.0f : 9.0f, fx != 0 ? 9.0f : 7.0f, floorf(cfr(36, 44)), ST_WARE, M_CRANE, 0);
    }
    /* Two rows of stacks between the road and the crane's legs. */
    for(int row = 0; row < 2; row++) {
        float w = in + 3.0f + (float)row * 7.0f;
        for(float u = b0 + 7; u < b1 - 6; u += 15.0f)
            if(cf() > 0.25f) add_building(bi, DX(u, w), DZ(u, w), fx != 0 ? 2.5f : 6.1f, fx != 0 ? 6.1f : 2.5f, 2.6f * (float)(1 + ci(3)), ST_WARE, M_CONT, 0);
    }
    #undef DX
    #undef DZ
}

static void build_ground(void) {
    const pvr_poly_hdr_t *h_road = r_header(TEX_ROAD, HF_OP), *h_cross = r_header(TEX_CROSS, HF_OP),
        *h_pave = r_header(TEX_PAVE, HF_OP), *h_grass = r_header(TEX_GRASS, HF_OP), *h_sand = r_header(TEX_SAND, HF_OP),
        *h_water = r_header(TEX_WATER, HF_OP);
    const uint32_t w = 0xffffffffu;
    sc_clear(&ground);
    /* The streets in squares of GROUND_G crossings, each its own chunk with
       one joined strip a texture: a strip is worth a few vertices to send,
       and a chunk a crossing was two thousand of them. */
    #define GROUND_G 3
    for(int cj = 0; cj < CITY_ROADS; cj += GROUND_G) {
        for(int ci0 = 0; ci0 < CITY_ROADS; ci0 += GROUND_G) {
            int j1 = cj + GROUND_G < CITY_ROADS ? cj + GROUND_G : CITY_ROADS, i1 = ci0 + GROUND_G < CITY_ROADS ? ci0 + GROUND_G : CITY_ROADS;
            sc_chunk_begin(&ground, HF_OP, 0, 2200.0f, 0);
            for(int pass = 0; pass < 7; pass++) {
                const pvr_poly_hdr_t *hp = pass == 0 ? h_cross : pass == 1 ? h_road : pass == 2 ? h_pave : pass == 3 ? h_grass : pass == 4 ? h_water : pass == 5 ? h_pave : h_road;
                sc_strip_begin(&ground, hp);
                for(int j = cj; j < j1; j++) {
                    for(int i = ci0; i < i1; i++) {
                        float x = city_road(i), z = city_road(j), r = CITY_ROAD * 0.5f;
                        /* A quad: corners p0..p3 in strip order, with their uvs. */
                        #define GQ(x0, z0, u0, v0, x1, z1, u1, v1, x2, z2, u2, v2, x3, z3, u3, v3_, y, col) do { \
                            sc_join(&ground, v3(x0, y, z0), u0, v0, col);                                     \
                            sc_vert(&ground, v3(x1, y, z1), u1, v1, col);                                     \
                            sc_vert(&ground, v3(x2, y, z2), u2, v2, col);                                     \
                            sc_vert(&ground, v3(x3, y, z3), u3, v3_, col);                                    \
                        } while(0)
                        if(pass == 0) GQ(x - r, z - r, 0, 0, x + r, z - r, 1, 0, x - r, z + r, 0, 1, x + r, z + r, 1, 1, 0, w);
                        if(pass == 1 && i < CITY_N)
                            /* The road east of the crossing: u across it, v along. */
                            GQ(x + r, z - r, 0, 0, x + r + CITY_BLOCK, z - r, 0, 4, x + r, z + r, 1, 0, x + r + CITY_BLOCK, z + r, 1, 4, 0, w);
                        if(pass == 1 && j < CITY_N)
                            GQ(x - r, z + r, 0, 0, x + r, z + r, 1, 0, x - r, z + r + CITY_BLOCK, 0, 4, x + r, z + r + CITY_BLOCK, 1, 4, 0, w);
                        if(i >= CITY_N || j >= CITY_N) continue;
                        int bi = j * CITY_N + i, kind = blocks[bi].kind;
                        bool green = kind != BK_NORMAL;
                        if((pass == 2 && !green) || (pass == 3 && green)) {
                            float uv = green ? 4.0f : 8.0f;
                            GQ(x + r, z + r, 0, 0, x + r + CITY_BLOCK, z + r, uv, 0, x + r, z + r + CITY_BLOCK, 0, uv, x + r + CITY_BLOCK, z + r + CITY_BLOCK,
                               uv, uv, 0, w);
                        }
                        if(pass == 4 && kind == BK_PARK) {
                            /* A pond. */
                            float px = x + r + 32, pz = z + r + 30;
                            GQ(px - 13, pz - 9, 0, 0, px + 13, pz - 9, 1, 0, px - 13, pz + 9, 0, 0.7f, px + 13, pz + 9, 1, 0.7f, 0.08f, w);
                        }
                        if(pass >= 5 && harbor[bi]) {
                            /* The quay out in front: paving to the water, a dark edge. */
                            float b0 = (harbor_side & 1 ? z : x) + r - 1, b1 = b0 + CITY_BLOCK + 2;
                            float fx = face_n[harbor_side][0], fz = face_n[harbor_side][1];
                            int part = pass - 5;
                            float w0 = part ? CITY_SHORE - 1.4f : CITY_HALF - 0.5f, w1 = part ? CITY_SHORE + 1.0f : CITY_SHORE - 1.4f;
                            float y = 0.05f + 0.01f * (float)part;
                            RV q[4];
                            for(int k = 0; k < 4; k++) {
                                float u = (k & 1) ? b1 : b0, ww = (k & 2) ? w1 : w0;
                                q[k] = (RV){fx != 0 ? fx * ww : u, y, fz != 0 ? fz * ww : u, part ? 0.05f : ((k & 1) ? 8.0f : 0.0f),
                                            part ? ((k & 1) ? 4.0f : 0.0f) : ((k & 2) ? 4.5f : 0.0f), part ? 0xff8890a0u : w};
                            }
                            GQ(q[0].x, q[0].z, q[0].u, q[0].v, q[1].x, q[1].z, q[1].u, q[1].v, q[2].x, q[2].z, q[2].u, q[2].v, q[3].x, q[3].z, q[3].u,
                               q[3].v, y, q[0].c);
                        }
                        #undef GQ
                    }
                }
                sc_strip_end(&ground);
            }
            sc_chunk_end(&ground);
        }
    }
    /* The beach round the city, then the sea. Each side in pieces, so no
       strip's texture coordinates run far (large ones swim on the console). */
    for(int ring = 0; ring < 2; ring++) {
        float in = ring ? CITY_SHORE : CITY_HALF, out = ring ? 2600.0f : CITY_SHORE;
        float tile = ring ? 96.0f : 16.0f;
        int pieces = ring ? 6 : 12;
        for(int side = 0; side < 4; side++) {
            for(int p = 0; p < pieces; p++) {
                float t0 = (float)p / (float)pieces, t1 = (float)(p + 1) / (float)pieces;
                sc_chunk_begin(&ground, HF_OP, 0, 4000.0f, 0);
                sc_strip_begin(&ground, ring ? h_water : h_sand);
                for(int k = 0; k < 4; k++) {
                    float t = (k & 1) ? t1 : t0, e = (k & 2) ? out : in;
                    /* Along the side from corner to corner at distance e. */
                    float a = -e + 2 * e * t, bx, bz;
                    switch(side) {
                        case 0: bx = a; bz = -e; break;
                        case 1: bx = e; bz = a; break;
                        case 2: bx = -a; bz = e; break;
                        default: bx = -e; bz = -a; break;
                    }
                    sc_vert(&ground, v3(bx, ring ? -0.6f * (float)(k >> 1) : 0, bz), (a + e) / tile - floorf((-e + 2 * e * t0 + e) / tile),
                            (e - in) / tile, ring ? 0xffe8f4ffu : w);
                }
                sc_strip_end(&ground);
                sc_chunk_end(&ground);
            }
        }
    }
    if(ground.full) printf("KL city: the ground did not fit\n");
}

/* The districts round the middle: eight wedges, turned a different way
   every lunch, with ragged edges. */
static void plan_districts(void) {
    static const uint8_t wedge[8] = {D_UPTOWN, D_OLDTOWN, D_SUBURB, D_INDUSTRY, D_MIXED, D_SUBURB, D_OLDTOWN, D_INDUSTRY};
    float turn = cf() * ENG_TAU;
    for(int j = 0; j < CITY_N; j++) {
        for(int i = 0; i < CITY_N; i++) {
            int di = i - C_MID, dj = j - C_MID, d = abs(di) > abs(dj) ? abs(di) : abs(dj);
            uint8_t *k = &district[j * CITY_N + i];
            if(d <= 2) *k = D_CORE;
            else if(d <= 4) *k = D_MID;
            else {
                float a = atan2f((float)dj, (float)di) - turn + cfr(-0.22f, 0.22f);
                int s = (int)floorf(a / (ENG_TAU / 8.0f));
                *k = wedge[((s % 8) + 8) % 8];
            }
            harbor[j * CITY_N + i] = false;
        }
    }
    /* The docks: on the shore the first industrial wedge faces. */
    float a = turn + 3.5f * (ENG_TAU / 8.0f);
    float sx = cosf(a), sz = sinf(a);
    harbor_side = fabsf(sx) > fabsf(sz) ? (sx > 0 ? 1 : 3) : (sz > 0 ? 2 : 0);
    for(int j = 0; j < CITY_N; j++)
        for(int i = 0; i < CITY_N; i++)
            if(block_side(i, j) == harbor_side && district[j * CITY_N + i] == D_INDUSTRY) harbor[j * CITY_N + i] = true;
}

/* The block of kind k nearest the middle of its wedge at ring d. */
static int pick_block(int k, int d) {
    int best = -1;
    float bd = 1e9f;
    for(int j = 0; j < CITY_N; j++) {
        for(int i = 0; i < CITY_N; i++) {
            int di = i - C_MID, dj = j - C_MID;
            if(district[j * CITY_N + i] != k || (abs(di) > abs(dj) ? abs(di) : abs(dj)) != d) continue;
            /* (how many of its neighbours are the same: the wedge's middle) */
            int same = 0;
            for(int y = -1; y <= 1; y++)
                for(int x = -1; x <= 1; x++) {
                    int ni = i + x, nj = j + y;
                    if(ni >= 0 && nj >= 0 && ni < CITY_N && nj < CITY_N && district[nj * CITY_N + ni] == k) same++;
                }
            float score = -(float)same + cf() * 0.5f;
            if(score < bd) {
                bd = score;
                best = j * CITY_N + i;
            }
        }
    }
    return best;
}

void city_build(uint32_t seed) {
    cr = seed * 2654435761u + 12345u;
    city_nb = 0;
    pool_n = 0;
    ntrees = 0;
    overflow = false;
    blast_depth = 0;
    nawake = 0;
    static const uint8_t core[] = {ST_GLASS, ST_GLASS, ST_OFFICE, ST_RIBBON, ST_DECO};
    static const uint8_t mid[] = {ST_OFFICE, ST_RIBBON, ST_APT, ST_GLASS, ST_BRICK, ST_DECO};
    static const uint8_t low[] = {ST_APT, ST_BRICK, ST_APT, ST_OFFICE};
    static const uint8_t old[] = {ST_BRICK, ST_DECO, ST_APT, ST_BRICK, ST_DECO};
    static const uint8_t up[] = {ST_DECO, ST_GLASS, ST_RIBBON, ST_OFFICE, ST_DECO};
    static const uint8_t edge[] = {ST_HOUSE, ST_HOUSE, ST_BRICK, ST_APT};
    plan_districts();
    int uptown = pick_block(D_UPTOWN, 7), stadium = pick_block(D_SUBURB, 6), temple = pick_block(D_OLDTOWN, 6);
    int ui = uptown % CITY_N, uj = uptown / CITY_N;
    for(int j = 0; j < CITY_N; j++) {
        for(int i = 0; i < CITY_N; i++) {
            int bi = j * CITY_N + i;
            float cx = city_road(i) + CITY_PITCH * 0.5f, cz = city_road(j) + CITY_PITCH * 0.5f;
            const int C = C_MID;   /* the middle block */
            int dx = abs(i - C), dz = abs(j - C), d = dx > dz ? dx : dz;
            int dist = district[bi];
            blocks[bi].first = (uint16_t)city_nb;
            blocks[bi].count = 0;
            blocks[bi].max_h = 0;
            blocks[bi].reach = 48.0f;
            blocks[bi].kind = BK_NORMAL;
            if(i == C && j == C) {
                /* The tallest thing in town. */
                add_building(bi, cx, cz, 21, 21, 228, ST_GLASS, M_SETBACK, BF_LANDMARK);
            }
            else if(i == C - 2 && (j == C + 1 || j == C + 2)) {
                blocks[bi].kind = BK_PARK;
                for(int t = 0; t < 18; t++) {
                    float tx = cx + cfr(-29, 29), tz = cz + cfr(-29, 29);
                    if(fabsf(tx - cx) < 15 && fabsf(tz - cz + 2) < 11) continue;   /* the pond */
                    plant(tx, tz, cfr(12, 18));
                }
            }
            else if(i == C + 2 && j == C - 2) {
                add_building(bi, cx, cz, 4.5f, 4.5f, 176, ST_OFFICE, M_TV, BF_LANDMARK);
            }
            else if(i == C + 1 && j == C + 2) {
                Building *b = add_building(bi, cx - 12, cz, 13, 16, 12, ST_BRICK, M_PLAIN, BF_DONUT | BF_LANDMARK);
                if(b) b->tint = 0xffffc0d8;
                lots(bi, cx + 14, cz, 1, 2, 16, 28, low, 4, 0, 0, 0);
            }
            else if(i == C - 1 && j == C - 1) {
                /* The twins. */
                add_building(bi, cx - 15, cz - 12, 12, 12, 172, ST_RIBBON, M_CROWN, 0);
                add_building(bi, cx + 15, cz + 12, 12, 12, 160, ST_RIBBON, M_CROWN, 0);
            }
            else if((i - C <= -4 && j - C <= -4 && i - C + j - C <= -9 && d == 5) || (i == C + 5 && j == C + 5)) {
                /* The tank farm: one spark and the lot goes. */
                for(int tz = 0; tz < 3; tz++)
                    for(int tx = 0; tx < 3; tx++)
                        add_building(bi, cx - 19 + tx * 19, cz - 19 + tz * 19, 7, 7, 13, ST_TANK, M_TANK, BF_EXPLOSIVE);
            }
            else if(bi == stadium) {
                add_building(bi, cx, cz, 28, 24, 16, ST_DECO, M_STADIUM, BF_LANDMARK);
            }
            else if(bi == temple) {
                /* A pagoda in a garden, its gate on the street. */
                blocks[bi].kind = BK_GREEN;
                add_building(bi, cx, cz + 4, 13, 13, 40, ST_BRICK, M_PAGODA, BF_LANDMARK);
                add_building(bi, cx, cz - 22, 8, 1, 11, ST_BRICK, M_TORII, 0);
                for(int t = 0; t < 12; t++) {
                    float tx = cx + cfr(-28, 28), tz = cz + cfr(-28, 28);
                    if(fabsf(tx - cx) < 19 && tz > cz - 26) continue;
                    plant(tx, tz, cfr(10, 15));
                }
            }
            else if(d <= 1) {
                if(cf() < 0.4f) {
                    float hw = cfr(17, 22);
                    float r = cf();
                    add_building(bi, cx, cz, hw, hw, cfr(120, 178), core[ci(5)], r < 0.5f ? M_SETBACK : r < 0.8f ? M_CROWN : M_SPIRE, 0);
                }
                else lots(bi, cx, cz, 2, 2, 64, 140, core, 5, 0.4f, 0.3f, 0);
            }
            else if(d == 2) lots(bi, cx, cz, 2, ci(2) ? 2 : 3, 40, 96, mid, 6, 0.35f, 0.2f, 0);
            else if(cf() < 0.05f) {
                /* A green: some with a pond. */
                blocks[bi].kind = ci(2) ? BK_PARK : BK_GREEN;
                for(int t = 0; t < 14; t++) {
                    float tx = cx + cfr(-29, 29), tz = cz + cfr(-29, 29);
                    if(blocks[bi].kind == BK_PARK && fabsf(tx - cx) < 15 && fabsf(tz - cz + 2) < 11) continue;
                    plant(tx, tz, cfr(12, 18));
                }
            }
            else if(dist == D_MID) lots(bi, cx, cz, 3, ci(2) ? 2 : 3, 20, 52, mid, 6, 0.3f, 0.1f, 0.04f);
            else if(dist == D_UPTOWN) {
                int ud = abs(i - ui) > abs(j - uj) ? abs(i - ui) : abs(j - uj);
                if(ud == 0) {
                    float hw = cfr(16, 19);
                    add_building(bi, cx, cz, hw, hw, cfr(150, 185), up[ci(5)], M_SPIRE, 0);
                }
                else if(ud == 1) {
                    if(cf() < 0.35f) {
                        /* A slab. */
                        bool ax = ci(2);
                        add_building(bi, cx, cz, ax ? 22 : 9, ax ? 9 : 22, cfr(70, 110), up[ci(5)], M_PLAIN, 0);
                    }
                    else lots(bi, cx, cz, 2, 2, 48, 116, up, 5, 0.3f, 0.2f, 0);
                }
                else lots(bi, cx, cz, 3, 2, 22, 56, up, 5, 0.2f, 0.15f, 0.06f);
            }
            else if(dist == D_OLDTOWN) {
                if(cf() < 0.75f) terraces(bi, cx, cz, d >= 9 ? 10 : 14, d >= 9 ? 20 : 30, old, 5);
                else lots(bi, cx, cz, 3, 3, 10, 26, old, 5, 0, 0, 0.12f);
            }
            else if(dist == D_SUBURB) {
                float r = cf();
                blocks[bi].kind = BK_GREEN;
                if(r < 0.12f) {
                    /* A parade of shops and flats over them. */
                    blocks[bi].kind = BK_NORMAL;
                    lots(bi, cx, cz, 2, 2, 8, 16, low, 4, 0, 0, 0.1f);
                }
                else houses(bi, cx, cz, r < 0.85f ? 3 : 4);
            }
            else if(dist == D_INDUSTRY) industry(bi, cx, cz);
            else if((i + j) & 1) lots(bi, cx, cz, 3, 3, 9, 24, low, 4, 0, 0, 0.1f);
            else lots(bi, cx, cz, 3, 3, 7, 14, edge, 4, 0, 0, 0.2f);
            if(harbor[bi]) docks(bi, i, j);
        }
    }
    /* Palms along the beach, where there is no quay. */
    for(int t = 0; t < 44 * CITY_N / 11; t++) {
        float a = cfr(-CITY_HALF, CITY_HALF), e = CITY_HALF + cfr(10, 30);
        int side = ci(4);
        if(side == harbor_side) {
            int k = (int)floorf((a + CITY_HALF) / CITY_PITCH);
            k = k < 0 ? 0 : k >= CITY_N ? CITY_N - 1 : k;
            if(harbor[harbor_side & 1 ? (k * CITY_N + (harbor_side == 1 ? CITY_N - 1 : 0)) : ((harbor_side == 2 ? CITY_N - 1 : 0) * CITY_N + k)]) continue;
        }
        plant(side == 0 ? a : side == 1 ? e : side == 2 ? a : -e, side == 0 ? -e : side == 1 ? a : side == 2 ? e : a, cfr(12, 17));
    }
    standing = city_nb;
    memset(bodies, 0, sizeof(bodies));
    memset(uprooted, 0, sizeof(uprooted));
    build_ground();
    if(overflow) printf("KL city: out of room for buildings or their vertices\n");
    printf("KL city: %d buildings, %lu vertices, %d trees, %lu ground vertices\n", city_nb, (unsigned long)pool_n, ntrees,
           (unsigned long)ground.pool_n);
}

/* ---- Queries ---- */
static inline int block_of(float v) {
    int i = (int)floorf((v + CITY_HALF) / CITY_PITCH);
    return i < 0 ? 0 : i >= CITY_N ? CITY_N - 1 : i;
}

/* The blocks that can hold a building reaching into x0..x1 by z0..z1
   (one stands in the block its middle is in, and none reaches more than
   30 m out of it). Iterate with for(int j = r.j0; j <= r.j1; j++) and so on. */
typedef struct { int i0, i1, j0, j1; } BlockRange;
static inline BlockRange block_range(float x0, float x1, float z0, float z1) {
    return (BlockRange){block_of(x0 - 30), block_of(x1 + 30), block_of(z0 - 30), block_of(z1 + 30)};
}
#define FOR_BLOCK_RANGE(r, idx)                                    \
    for(int bj_ = (r).j0; bj_ <= (r).j1; bj_++)                    \
        for(int bi_ = (r).i0; bi_ <= (r).i1; bi_++)                \
            for(int bk_ = 0, idx; bk_ < blocks[bj_ * CITY_N + bi_].count && ((idx = blocks[bj_ * CITY_N + bi_].first + bk_), 1); bk_++)

/* Distance from (x, z) to b's footprint (0 inside), and the way out. */
static inline float rect_dist(const Building *b, float x, float z, float *nx, float *nz) {
    float dx = x - b->x, dz = z - b->z;
    float qx = fabsf(dx) - b->hw, qz = fabsf(dz) - b->hd;
    if(qx <= 0 && qz <= 0) {
        /* Inside: out by the nearer wall. */
        if(qx > qz) { *nx = signf(dx); *nz = 0; return qx; }
        *nx = 0; *nz = signf(dz);
        return qz;
    }
    float ox = qx > 0 ? qx : 0, oz = qz > 0 ? qz : 0;
    float d = sqrtf(ox * ox + oz * oz);
    *nx = ox * signf(dx) / d;
    *nz = oz * signf(dz) / d;
    return d;
}

int city_at(float x, float z) {
    int bi = block_of(z) * CITY_N + block_of(x);
    for(int k = 0; k < blocks[bi].count; k++) {
        const Building *b = &city_b[blocks[bi].first + k];
        if(b->state == B_INTACT && fabsf(x - b->x) <= b->hw && fabsf(z - b->z) <= b->hd) return blocks[bi].first + k;
    }
    return -1;
}

int city_standing(void) { return standing; }
int city_total(void) { return city_nb; }

int city_nearest(V3 p, float min_h) {
    int best = -1;
    float bd = 1e12f;
    for(int i = 0; i < city_nb; i++) {
        const Building *b = &city_b[i];
        if(b->state != B_INTACT || b->h < min_h) continue;
        float d = (b->x - p.x) * (b->x - p.x) + (b->z - p.z) * (b->z - p.z);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

int city_aim(V3 p, V3 fwd, float dmin, float dmax, float cone, int prev) {
    int best = -1;
    float bs = 1e12f;
    BlockRange r = block_range(p.x - dmax, p.x + dmax, p.z - dmax, p.z + dmax);
    FOR_BLOCK_RANGE(r, i) {
        const Building *b = &city_b[i];
        if(b->state != B_INTACT) continue;
        float dx = b->x - p.x, dz = b->z - p.z, d = sqrtf(dx * dx + dz * dz) + 1e-3f;
        if(d < dmin || d > dmax) continue;
        float c = (dx * fwd.x + dz * fwd.z) / d;
        if(c < cone) continue;
        /* (best about a block and a half out: nearer is hidden behind the kaiju) */
        float s = (1.0f - c) * 900.0f + fabsf(d - 120.0f) * 0.2f - fminf(b->h, 120.0f) * 0.3f - (i == prev ? 25.0f : 0.0f);
        if(s < bs) {
            bs = s;
            best = i;
        }
    }
    return best;
}

/* ---- Coming down ---- */
static void quad_flat(int kind, V3 a, V3 b, V3 c, V3 d, float u, float v, uint32_t col) {
    s_begin(kind);
    s_v(a.x, a.y, a.z, 0, 0, col);
    s_v(b.x, b.y, b.z, u, 0, col);
    s_v(c.x, c.y, c.z, 0, v, col);
    s_v(d.x, d.y, d.z, u, v, col);
    s_end();
}

static void make_rubble(Building *b, bool fallen) {
    cb = b;
    b->nv = 0;
    b->nstrips = 0;
    b->bands = b->carved = 0;
    uint32_t tint = color_lerp(b->tint, 0xffb0aab4, 0.5f);
    float hr = clampf(b->h0 * 0.06f, 1.6f, 7.0f);
    walls(SK_RUBBLE, b->x, b->z, b->hw, b->hd, 0, hr, 0.7f, tint, 8.0f);
    roof(SK_RUBBLE, b->x, b->z, b->hw * 0.7f, b->hd * 0.7f, hr, tint, fmaxf(1.0f, floorf(b->hw * 0.18f)));
    if(fallen) {
        /* Where the tower lies: a long low slab of it. */
        float L = b->len * 0.94f, hwid = fabsf(b->dz) * b->hw + fabsf(b->dx) * b->hd, y = fminf(3.2f, hr);
        V3 p = v3(b->px, 0, b->pz), d = v3(b->dx, 0, b->dz), n = v3(b->dz, 0, -b->dx);
        V3 a0 = vmad(p, n, -hwid), a1 = vmad(p, n, hwid), b0 = vmad(a0, d, L), b1 = vmad(a1, d, L);
        V3 up = v3(0, y, 0), in0 = vmad(vmad(vadd(a0, up), n, 1.2f), d, 1.0f), in1 = vmad(vmad(vadd(a1, up), n, -1.2f), d, 1.0f);
        V3 jn0 = vmad(vmad(vadd(b0, up), n, 1.2f), d, -1.5f), jn1 = vmad(vmad(vadd(b1, up), n, -1.2f), d, -1.5f);
        uint32_t lit = cel_band(b->tint, 0.9f), sh = cel_band(tint, 0.0f), md = cel_band(tint, 0.3f);
        float u = fmaxf(1, floorf(L / 10)), v = fmaxf(1, floorf(hwid / 5));
        quad_flat(SK_RUBBLE, in0, jn0, in1, jn1, u, v, lit);
        quad_flat(SK_RUBBLE, a0, b0, in0, jn0, u, 0.4f, n.x * cel_sun.x + n.z * cel_sun.z < 0 ? md : sh);
        quad_flat(SK_RUBBLE, a1, b1, in1, jn1, u, 0.4f, n.x * cel_sun.x + n.z * cel_sun.z > 0 ? md : sh);
        quad_flat(SK_RUBBLE, b0, b1, jn0, jn1, v, 0.4f, md);
    }
    b->state = B_RUBBLE;
    b->flags &= (uint8_t)~(BF_ONE | BF_FAR1);
    b->h = hr;
}

static bool carve(int bi, V3 at, int n, V3 dir);

static void bring_down(int i, V3 dir, int src, int depth) {
    Building *b = &city_b[i];
    if(b->state != B_INTACT) return;
    float dl = sqrtf(dir.x * dir.x + dir.z * dir.z);
    if(dl < 1e-3f) {
        float a = frand() * ENG_TAU;
        dir = v3(sinf(a), 0, cosf(a));
        dl = 1;
    }
    wake(i);
    b->dx = dir.x / dl;
    b->dz = dir.z / dl;
    b->src = (uint8_t)src;
    b->depth = (uint8_t)(depth > 250 ? 250 : depth);
    b->t = 0;
    b->flags &= (uint8_t)~BF_BURN;
    standing--;
    float w = 2.0f * fmaxf(b->hw, b->hd);
    bool topple = b->h >= 30 && b->h > 1.2f * w && src != SRC_FLOP && !(b->flags & BF_EXPLOSIVE);
    V3 c = v3(b->x, fminf(b->h * 0.4f, 24.0f), b->z);
    if(topple) {
        b->state = B_TOPPLE;
        float support = fabsf(b->dx) * b->hw + fabsf(b->dz) * b->hd;
        b->px = b->x + b->dx * support;
        b->pz = b->z + b->dz * support;
        b->ang = 0.03f;
        b->angv = 0.32f;
        b->len = b->h;
        audio_at(KL_SFX_CREAK, c, 230);
        audio_at(KL_SFX_CRASH_M, c, 200);
    }
    else {
        b->state = B_CRUMBLE;
        b->dur = clampf(0.6f + b->h * 0.022f, 0.7f, 2.6f);
        audio_at(b->h > 60 ? KL_SFX_CRASH_L : b->h > 20 ? KL_SFX_CRASH_M : KL_SFX_CRASH_S, c, 235);
    }
    float size = clampf(b->h * 0.12f + w * 0.2f, 5, 22);
    fx_puffs(v3(b->x, 0, b->z), w * 0.7f, 5 + (int)(w * 0.2f), size);
    fx_debris(c, 4 + (int)(b->h * 0.08f), 16 + b->h * 0.12f, b->tint, clampf(w * 0.09f, 1.2f, 4.0f));
    fx_shake(clampf(b->h * 0.008f + 0.15f, 0.2f, 1.4f));
    if(b->h >= 60) fx_hitstop(b->h >= 120 ? 0.1f : 0.06f);
    fx_callout(v3(b->x, b->h * 0.7f, b->z), b->h > 110 ? 3 : b->h > 50 ? 2 : b->h > 18 ? 1 : 0);
    /* The bill. */
    score.buildings++;
    /* (what was knocked out of it piece by piece has been paid for) */
    score_hit(src, b->bands ? (uint32_t)((float)b->value * fmaxf(0.4f, (float)b->cells_left / (float)b->cells)) : b->value,
              v3(b->x, b->h, b->z));
    if(b->flags & BF_LANDMARK) score_trick(TR_LANDMARK, 1);
    if(src == SRC_DOMINO) score_trick(TR_DOMINO, depth);
    else if(topple && b->h >= 100) score_trick(TR_TIMBER, 1);
    if(src == SRC_FLOP) score_trick(TR_BELLY_FLOP, 1);
    if(src == SRC_BLAST && (b->flags & BF_EXPLOSIVE) && depth >= 1) score_trick(TR_CHAIN_REACTION, depth);
    if(b->flags & BF_EXPLOSIVE) b->fuse = 0.22f + frand() * 0.12f;
    int left = 0;
    for(int k = 0; k < blocks[b->block].count; k++)
        if(city_b[blocks[b->block].first + k].state == B_INTACT) left++;
    if(!left && blocks[b->block].count >= 3) score_trick(TR_BLOCK_PARTY, 1);
}

/* ---- Buildings in chunks ---- */

/* The footprint's scale at height y: the tier that holds it. */
static inline float tier_scale(const Building *b, float y) {
    for(int t = 0; t < b->ntiers; t++)
        if(y < b->tier_y[t]) return b->tier_k[t];
    return b->tier_k[b->ntiers - 1];
}

/* Chunk q of band k: its centre and half sizes. */
static void cell_box(const Building *b, int k, int q, V3 *c, V3 *half) {
    float y0 = (float)k * b->band_h, s = tier_scale(b, y0 + b->band_h * 0.5f);
    float hw = b->hw * s, hd = b->hd * s;
    if(b->cols == 1) {
        *c = v3(b->x, y0 + b->band_h * 0.5f, b->z);
        *half = v3(hw, b->band_h * 0.5f, hd);
        return;
    }
    *c = v3(b->x + ((q & 1) ? 0.5f : -0.5f) * hw, y0 + b->band_h * 0.5f, b->z + ((q & 2) ? 0.5f : -0.5f) * hd);
    *half = v3(hw * 0.5f, b->band_h * 0.5f, hd * 0.5f);
}

/* A chunk leaves the building as a piece of its own. */
static void cell_out(int bi, int k, int q, V3 push) {
    Building *b = &city_b[bi];
    V3 c, half;
    cell_box(b, k, q, &c, &half);
    b->cell[k] &= (uint8_t)~(1u << q);
    b->cells_left--;
    b->carved = 1;
    score_chunk(b->value / (uint32_t)(b->cells * 2));
    for(int tries = 0; tries < BODIES; tries++) {
        Body *o = &bodies[body_next];
        body_next = (body_next + 1) % BODIES;
        if(o->on) continue;
        *o = (Body){true, b->style, (int16_t)bi, c, push, frand() * 0.3f, 0, frand_s() * 2.2f, frand_s() * 2.2f, half.x, half.y, half.z, b->tint};
        o->dmg = 50;
        if(rip_body == -1) {
            /* The one being torn out stays in the jaws. */
            rip_body = (int)(o - bodies);
            o->held = true;
        }
        return;
    }
    /* No room for another piece: it goes in a cloud. */
    fx_puffs(c, half.x, 3, half.x);
}

/* n chunks out of building bi, nearest at first, thrown clear; then
   whatever that leaves with nothing under it. True if the building can
   no longer stand. */
static bool carve(int bi, V3 at, int n, V3 dir) {
    Building *b = &city_b[bi];
    float dl = sqrtf(dir.x * dir.x + dir.z * dir.z) + 1e-3f;
    dir = v3(dir.x / dl, 0, dir.z / dl);
    at.y = clampf(at.y, b->band_h * 0.5f, b->h - b->band_h * 0.5f);
    V3 first = at;
    for(int out = 0; out < n; out++) {
        int bk = -1, bq = 0;
        float bd = 1e12f;
        for(int k = 0; k < b->bands; k++) {
            for(int q = 0; q < b->cols; q++) {
                if(!(b->cell[k] & (1u << q))) continue;
                V3 c, half;
                cell_box(b, k, q, &c, &half);
                float dy = c.y - at.y, d = dist2_xz(c, at) + dy * dy * 1.6f;
                if(d < bd) {
                    bd = d;
                    bk = k;
                    bq = q;
                }
            }
        }
        if(bk < 0) break;
        V3 c, half;
        cell_box(b, bk, bq, &c, &half);
        if(out == 0) first = c;
        /* Out and away from the building's middle, with the blow behind it. */
        V3 away = v3(c.x - b->x, 0, c.z - b->z);
        if(b->cols == 1) away = frand() < 0.5f ? v3(dir.z, 0, -dir.x) : v3(-dir.z, 0, dir.x);
        away = vnorm(vadd(vnorm(away), vscale(dir, 0.45f)));
        float sp = 26.0f + frand() * 20.0f;
        cell_out(bi, bk, bq, v3(away.x * sp, 10.0f + frand() * 16.0f, away.z * sp));
    }
    fx_boom(first, clampf(b->band_h * 0.9f, 8, 14));
    fx_debris(first, 4, 26, b->tint, 1.6f);
    fx_callout(first, 1);
    audio_at(KL_SFX_CRASH_S, first, 220);
    /* What still has something under it: the chunk below, or a neighbour
       in its band that has. The rest falls. */
    uint8_t under = b->cell[0];
    int top = -1;
    bool gap = false;
    for(int k = 0; k < b->bands; k++) {
        if(k > 0) {
            uint8_t base = b->cell[k] & under, held = base;
            for(int q = 0; q < b->cols; q++)
                if((b->cell[k] & (1u << q)) && (base & ((1u << (q ^ 1)) | (1u << (q ^ 2))))) held |= (uint8_t)(1u << q);
            for(int q = 0; q < b->cols; q++)
                if((b->cell[k] & ~held) & (1u << q)) cell_out(bi, k, q, v3(frand_s() * 3, -2, frand_s() * 3));
            under = held;
        }
        if(b->cell[k]) {
            if(top >= 0 && top < k - 1) gap = true;
            top = k;
        }
        else if(k == 0) gap = true;
    }
    return gap || top < 0 || b->cells_left * 2 < b->cells;
}

static void bodies_update(float dt) {
    for(int i = 0; i < BODIES; i++) {
        Body *o = &bodies[i];
        if(!o->on || o->held) continue;
        o->v.y -= 55.0f * dt;
        o->p = vmad(o->p, o->v, dt);
        o->ax += o->wx * dt;
        o->ay += o->wy * dt;
        int under = city_at(o->p.x, o->p.z);
        bool into = under >= 0 && under != o->owner && o->p.y - o->hy < city_b[under].h;
        if(!into && o->p.y > fminf(o->hx, o->hz)) continue;
        /* It lands: on a roof, a road, or somebody's car. */
        o->on = false;
        if(into) {
            city_damage_at(under, o->dmg, o->v, SRC_THROWN, o->p);
            if(o->dmg > 100) score_trick(TR_DELIVERY, 1);   /* thrown, not fallen */
        }
        V3 g = v3(o->p.x, fmaxf(o->p.y - o->hy, 0), o->p.z);
        fx_puffs(g, o->hx, 4, clampf(o->hx * 0.8f, 5, 12));
        fx_debris(vadd(g, v3(0, 2, 0)), 4, 22, o->tint, clampf(o->hx * 0.22f, 1.2f, 2.8f));
        fx_shake(0.25f);
        audio_at(KL_SFX_CRASH_S, g, 190);
        if(!into) life_stomp(g, fmaxf(o->hx, o->hz) + 2.0f);
    }
}

void city_damage_at(int i, float dmg, V3 dir, int src, V3 at) {
    Building *b = &city_b[i];
    if(b->state != B_INTACT) return;
    wake(i);
    b->hp -= dmg;
    b->shake = 0.3f;
    if(b->hp <= 0) {
        bring_down(i, dir, src, blast_depth);
        return;
    }
    if(b->bands) {
        /* A chunk for every 45 of damage, the odd points carried over. */
        b->carry += dmg;
        int n = (int)(b->carry / 45.0f);
        b->carry -= (float)n * 45.0f;
        if(n && carve(i, at, n > 6 ? 6 : n, dir)) {
            bring_down(i, dir, src, blast_depth);
            return;
        }
        if(b->hp < b->hp_max * 0.6f) b->flags |= BF_BURN;
        if(!n && dmg > 12) score_chip((uint32_t)((float)b->value * 0.01f));
        return;
    }
    if(b->hp < b->hp_max * 0.6f) b->flags |= BF_BURN;
    if(dmg > 12) {
        /* Chips off the side that was hit. */
        V3 c = v3(b->x - dir.x * b->hw * 0.8f, fminf(b->h * 0.6f, 22.0f), b->z - dir.z * b->hd * 0.8f);
        fx_debris(c, 3, 18, b->tint, 1.4f);
        fx_puff(c, v3(0, 4, 0), 6, 0.5f, 0xffffffff);
        fx_boom(c, 5);
        audio_at(KL_SFX_CRASH_S, c, 150);
        score_chip((uint32_t)((float)b->value * 0.04f));
    }
}

void city_damage(int i, float dmg, V3 dir, int src) {
    /* Nowhere in particular: the middle of the side it came from. */
    const Building *b = &city_b[i];
    float l = sqrtf(dir.x * dir.x + dir.z * dir.z) + 1e-3f;
    city_damage_at(i, dmg, dir, src, v3(b->x - dir.x / l * b->hw, fminf(b->h * 0.5f, 20.0f), b->z - dir.z / l * b->hd));
}

int city_damage_circle(V3 c, float r, float dmg, int src) {
    int i0 = block_of(c.x - r - 30), i1 = block_of(c.x + r + 30), j0 = block_of(c.z - r - 30), j1 = block_of(c.z + r + 30);
    int before = standing;
    for(int j = j0; j <= j1; j++) {
        for(int i = i0; i <= i1; i++) {
            int bi = j * CITY_N + i;
            for(int k = 0; k < blocks[bi].count; k++) {
                Building *b = &city_b[blocks[bi].first + k];
                if(b->state != B_INTACT) continue;
                float nx, nz, d = rect_dist(b, c.x, c.z, &nx, &nz);
                if(d > r) continue;
                float f = d <= 0 ? 1.0f : 1.0f - 0.5f * d / r;
                V3 dir = v3(b->x - c.x, 0, b->z - c.z);
                /* It lands on the nearest of the building to c, at c's height. */
                V3 at = v3(clampf(c.x, b->x - b->hw, b->x + b->hw), c.y, clampf(c.z, b->z - b->hd, b->z + b->hd));
                city_damage_at(blocks[bi].first + k, dmg * f, dir, src, at);
            }
        }
    }
    return before - standing;
}

int city_damage_beam(V3 o, V3 d, float len, float dmg, int src, float *first) {
    int n = 0;
    float nearest = len;
    float ix = fabsf(d.x) > 1e-5f ? 1.0f / d.x : 1e5f, iz = fabsf(d.z) > 1e-5f ? 1.0f / d.z : 1e5f;
    float ex = o.x + d.x * len, ez = o.z + d.z * len;
    BlockRange r = block_range(fminf(o.x, ex), fmaxf(o.x, ex), fminf(o.z, ez), fmaxf(o.z, ez));
    FOR_BLOCK_RANGE(r, i) {
        Building *b = &city_b[i];
        if(b->state != B_INTACT) continue;
        float t0 = (b->x - b->hw - o.x) * ix, t1 = (b->x + b->hw - o.x) * ix;
        float u0 = (b->z - b->hd - o.z) * iz, u1 = (b->z + b->hd - o.z) * iz;
        float tn = fmaxf(fminf(t0, t1), fminf(u0, u1)), tf = fminf(fmaxf(t0, t1), fmaxf(u0, u1));
        if(tn > tf || tf < 0 || tn > len) continue;
        if(tn < 0) tn = 0;
        if(o.y + d.y * tn > b->h + 1.0f) continue;     /* over the roof */
        n++;
        if(tn < nearest) nearest = tn;
        city_damage_at(i, dmg, d, src, vmad(o, d, tn + 2.0f));
        if((crand() & 7) == 0) fx_puff(v3(o.x + d.x * tn, o.y + d.y * tn, o.z + d.z * tn), v3(frand_s() * 6, 8, frand_s() * 6), 7, 0.5f, 0xffffe0a0);
    }
    if(first) *first = nearest;
    return n;
}

bool city_collide(V3 *pos, float radius, float y, V3 move, float ram, float plough) {
    bool pushed = false;
    int i0 = block_of(pos->x - radius - 30), i1 = block_of(pos->x + radius + 30);
    int j0 = block_of(pos->z - radius - 30), j1 = block_of(pos->z + radius + 30);
    for(int j = j0; j <= j1; j++) {
        for(int i = i0; i <= i1; i++) {
            int bi = j * CITY_N + i;
            for(int k = 0; k < blocks[bi].count; k++) {
                int idx = blocks[bi].first + k;
                Building *b = &city_b[idx];
                if(b->state != B_INTACT || b->h < y - 1.0f) continue;
                float nx, nz, d = rect_dist(b, pos->x, pos->z, &nx, &nz);
                if(d >= radius) continue;
                if(b->h < fmaxf(14.5f, plough)) {
                    /* Underfoot. */
                    city_damage(idx, 9999, v3(-nx, 0, -nz), SRC_RAM);
                    continue;
                }
                pos->x += nx * (radius - d);
                pos->z += nz * (radius - d);
                pushed = true;
                if(ram > 0 && move.x * nx + move.z * nz < -0.25f)
                    city_damage_at(idx, ram, v3(-nx, 0, -nz), SRC_RAM, v3(pos->x - nx * radius, 14.0f, pos->z - nz * radius));
            }
        }
    }
    return pushed;
}

int city_rip(V3 at, float reach) {
    int best = -1;
    float bd = reach;
    BlockRange r = block_range(at.x - reach, at.x + reach, at.z - reach, at.z + reach);
    FOR_BLOCK_RANGE(r, i) {
        const Building *b = &city_b[i];
        if(b->state != B_INTACT || !b->bands) continue;
        float nx, nz, d = rect_dist(b, at.x, at.z, &nx, &nz);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    if(best < 0) return -1;
    Building *b = &city_b[best];
    V3 dir = v3(b->x - at.x, 0, b->z - at.z);
    rip_body = -1;
    bool falls = carve(best, at, 1, dir);
    int got = rip_body;
    rip_body = -2;
    if(got >= 0) bodies[got].v = v3(0, 0, 0);
    if(falls) bring_down(best, dir, SRC_CLAW, 0);
    return got;
}

void city_chunk_hold(int handle, V3 p) {
    Body *o = &bodies[handle];
    if(!o->on || !o->held) return;
    /* It hangs under the jaws by its top. */
    o->p = v3(p.x, p.y - o->hy, p.z);
    o->ax *= 0.9f;
}

void city_chunk_throw(int handle, V3 v) {
    Body *o = &bodies[handle];
    if(!o->on || !o->held) return;
    o->held = false;
    o->v = v;
    o->owner = -1;
    o->dmg = 280;
    o->wx = 3.0f + frand() * 2.0f;
}

void city_damage_ring(V3 c, float r0, float r1, float dmg, int src) {
    BlockRange r = block_range(c.x - r1, c.x + r1, c.z - r1, c.z + r1);
    FOR_BLOCK_RANGE(r, i) {
        Building *b = &city_b[i];
        if(b->state != B_INTACT) continue;
        float nx, nz, d = rect_dist(b, c.x, c.z, &nx, &nz);
        if(d < r0 || d >= r1) continue;
        V3 at = v3(clampf(c.x, b->x - b->hw, b->x + b->hw), 5.0f, clampf(c.z, b->z - b->hd, b->z + b->hd));
        city_damage_at(i, dmg, v3(b->x - c.x, 0, b->z - c.z), src, at);
    }
}

bool city_eat(V3 p, float r) {
    BlockRange br = block_range(p.x - r, p.x + r, p.z - r, p.z + r);
    FOR_BLOCK_RANGE(br, i) {
        Building *b = &city_b[i];
        if(b->state != B_INTACT || b->prop != PROP_DONUT) continue;
        float dx = b->prop_x - p.x, dz = b->prop_z - p.z;
        if(dx * dx + dz * dz > r * r) continue;
        b->prop = PROP_NONE;
        score_hit(SRC_EAT, 60000, v3(b->x, b->h + 8, b->z));
        score_trick(TR_DESSERT, 1);
        return true;
    }
    return false;
}

int city_grab_tree(V3 p, float r) {
    int best = -1, slot = -1;
    float bd = r * r;
    for(int i = 0; i < UPROOTED; i++)
        if(!uprooted[i].on) slot = i;
    if(slot < 0) return -1;
    for(int i = 0; i < ntrees; i++) {
        const Tree *t = &trees[i];
        float d = (t->x - p.x) * (t->x - p.x) + (t->z - p.z) * (t->z - p.z);
        if(t->alive && d < bd) {
            bd = d;
            best = i;
        }
    }
    if(best < 0) return -1;
    trees[best].alive = false;
    uprooted[slot].on = uprooted[slot].held = true;
    uprooted[slot].s = trees[best].s;
    uprooted[slot].rot = 0;
    uprooted[slot].p = v3(trees[best].x, 0, trees[best].z);
    score_chip(4);
    return slot;
}

void city_tree_hold(int handle, V3 p) {
    if(uprooted[handle].held) uprooted[handle].p = v3(p.x, p.y - uprooted[handle].s * 0.4f, p.z);
}

void city_tree_throw(int handle, V3 v) {
    if(!uprooted[handle].held) return;
    uprooted[handle].held = false;
    uprooted[handle].v = v;
}

static void uprooted_update(float dt) {
    for(int i = 0; i < UPROOTED; i++) {
        typeof(uprooted[0]) *u = &uprooted[i];
        if(!u->on || u->held) continue;
        u->v.y -= 55.0f * dt;
        u->p = vmad(u->p, u->v, dt);
        u->rot += 7.0f * dt;
        int under = city_at(u->p.x, u->p.z);
        bool into = under >= 0 && u->p.y < city_b[under].h;
        if(!into && u->p.y > 0) continue;
        u->on = false;
        if(into) {
            city_damage_at(under, 45, u->v, SRC_THROWN, u->p);
            score_trick(TR_DELIVERY, 1);
        }
        fx_puff(u->p, v3(frand_s() * 4, 6, frand_s() * 4), u->s * 0.6f, 0.6f, 0xff90e098);
        fx_puff(vadd(u->p, v3(2, 2, 0)), v3(frand_s() * 6, 8, frand_s() * 6), u->s * 0.4f, 0.5f, 0xff90e098);
        audio_at(KL_SFX_CRASH_S, u->p, 120);
    }
}

void city_trample(V3 p, float r) {
    for(int i = 0; i < ntrees; i++) {
        Tree *t = &trees[i];
        if(!t->alive || (t->x - p.x) * (t->x - p.x) + (t->z - p.z) * (t->z - p.z) > r * r) continue;
        t->alive = false;
        fx_puff(v3(t->x, t->s * 0.5f, t->z), v3(frand_s() * 4, 6, frand_s() * 4), t->s * 0.5f, 0.5f, 0xff90e098);
        score_chip(4);
    }
}

static void topple_step(int i, float dt) {
    Building *b = &city_b[i];
    b->angv += (90.0f / b->len * sinf(b->ang) + 0.2f) * dt;
    b->ang += b->angv * dt;
    float sn = sinf(b->ang), cs = cosf(b->ang), reach = b->len * sn;
    float halfw = fabsf(b->dz) * b->hw + fabsf(b->dx) * b->hd;
    /* What it lands on goes with it. */
    float ex = b->px + b->dx * reach, ez = b->pz + b->dz * reach;
    BlockRange r = block_range(fminf(b->px, ex) - halfw, fmaxf(b->px, ex) + halfw, fminf(b->pz, ez) - halfw, fmaxf(b->pz, ez) + halfw);
    FOR_BLOCK_RANGE(r, j) {
        Building *o = &city_b[j];
        if(o->state != B_INTACT) continue;
        float rx = o->x - b->px, rz = o->z - b->pz;
        float s = rx * b->dx + rz * b->dz, ext = fmaxf(o->hw, o->hd);
        if(s <= 0 || s - ext > reach || fabsf(rx * b->dz - rz * b->dx) > halfw + ext * 0.8f) continue;
        float near = fmaxf(s - ext, 1.0f);
        if(o->h * sn > near * cs) {
            bring_down(j, v3(b->dx, 0, b->dz), SRC_DOMINO, b->depth + 1);
            b->angv *= 0.86f;
        }
    }
    if(b->ang < 1.5f) return;
    /* Down. */
    V3 p = v3(b->px, 0, b->pz), d = v3(b->dx, 0, b->dz);
    for(float s = 8; s < b->len; s += 13) {
        V3 q = vmad(p, d, s);
        fx_puffs(q, halfw + 4, 2, clampf(halfw * 0.9f, 7, 18));
        if(((int)s & 1) == 0) fx_debris(vadd(q, v3(0, 3, 0)), 2, 24, b->tint, clampf(halfw * 0.16f, 1.4f, 3.6f));
    }
    V3 mid = vmad(p, d, b->len * 0.5f);
    fx_shake(clampf(b->len * 0.012f, 0.5f, 2.4f));
    fx_hitstop(b->len > 110 ? 0.12f : 0.07f);
    fx_callout(vadd(mid, v3(0, 12, 0)), b->len > 110 ? 3 : 2);
    audio_at(KL_SFX_CRASH_L, mid, 255);
    life_crush_line(p, b->dx, b->dz, b->len, halfw);
    for(float s = 0; s < b->len; s += 12) city_trample(vmad(p, d, s), halfw + 6);
    make_rubble(b, true);
}

void city_update(float dt) {
    bodies_update(dt);
    uprooted_update(dt);
    /* (the list grows as this goes: what falls brings others down) */
    for(int n = 0; n < nawake; n++) {
        int i = awake[n];
        Building *b = &city_b[i];
        if(b->shake > 0) b->shake -= dt;
        if(b->fuse > 0 && (b->fuse -= dt) <= 0) {
            /* A tank goes up, and takes its neighbours' turn with it. */
            V3 c = v3(b->x, 6, b->z);
            fx_boom(vadd(c, v3(0, 8, 0)), 30);
            fx_puffs(c, 14, 8, 14);
            fx_debris(c, 10, 46, 0xffe85048, 2.4f);
            fx_shake(1.2f);
            fx_hitstop(0.05f);
            fx_callout(vadd(c, v3(0, 22, 0)), 3);
            audio_at(KL_SFX_EXPLODE, c, 255);
            blast_depth = b->depth + 1;
            city_damage_circle(c, 46, 420, SRC_BLAST);
            blast_depth = 0;
            life_launch(c, 52, 1.3f);
        }
        switch(b->state) {
            case B_INTACT:
                if((b->flags & BF_BURN) && (b->smoke -= dt) <= 0) {
                    b->smoke = 0.35f + frand() * 0.3f;
                    fx_puff(v3(b->x + frand_s() * b->hw * 0.6f, b->h + 4, b->z + frand_s() * b->hd * 0.6f),
                            v3(2 + frand_s() * 2, 14, frand_s() * 2), 5 + frand() * 4, 1.6f, 0xff787088);
                }
                break;
            case B_TOPPLE:
                topple_step(i, dt);
                break;
            case B_CRUMBLE:
                b->t += dt;
                if((b->smoke -= dt) <= 0) {
                    b->smoke = 0.07f;
                    float a = frand() * ENG_TAU;
                    fx_puff(v3(b->x + sinf(a) * b->hw * 1.2f, 2, b->z + cosf(a) * b->hd * 1.2f),
                            v3(sinf(a) * 12, 6 + frand() * 6, cosf(a) * 12), clampf(b->hw * 0.7f, 5, 16), 0.9f, 0xfff4f0f8);
                }
                if(b->t >= b->dur) {
                    fx_puffs(v3(b->x, 0, b->z), fmaxf(b->hw, b->hd) * 1.3f, 6, clampf(b->hw * 0.8f, 6, 18));
                    life_stomp(v3(b->x, 0, b->z), fmaxf(b->hw, b->hd) + 4);
                    make_rubble(b, false);
                }
                break;
            default: break;
        }
    }
    /* Back to sleep: whatever has settled. */
    int kept = 0;
    for(int n = 0; n < nawake; n++) {
        Building *b = &city_b[awake[n]];
        bool busy = b->shake > 0 || b->fuse > 0 || b->state == B_TOPPLE || b->state == B_CRUMBLE ||
                    (b->state == B_INTACT && (b->flags & BF_BURN));
        if(busy) awake[kept++] = awake[n];
        else b->flags &= (uint8_t)~BF_AWAKE;
    }
    nawake = kept;
}

/* ---- Drawing ---- */
/* A turn of ang about the horizontal axis through pivot that tips +Y
   toward (dx, dz). */
static Xf tip_xf(V3 pivot, float dx, float dz, float ang) {
    V3 a = v3(dz, 0, -dx);
    float s = sinf(ang), c = cosf(ang);
    Xf r;
    V3 e[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, *out[3] = {&r.x, &r.y, &r.z};
    for(int k = 0; k < 3; k++)
        *out[k] = vadd(vadd(vscale(e[k], c), vscale(vcross(a, e[k]), s)), vscale(a, vdot(a, e[k]) * (1 - c)));
    r.p = vsub(pivot, xf_dir(&r, pivot));
    return r;
}

static bool building_xf(const Building *b, Xf *x) {
    if(b->state == B_TOPPLE) {
        *x = tip_xf(v3(b->px, 0, b->pz), b->dx, b->dz, b->ang);
        return true;
    }
    if(b->state == B_CRUMBLE) {
        float k = b->t / b->dur;
        *x = tip_xf(v3(b->x, 0, b->z), b->dx, b->dz, 0.1f * k);
        x->p.x += sinf(b->t * 71.0f) * 0.5f;
        x->p.z += sinf(b->t * 57.0f) * 0.5f;
        x->p.y -= b->h0 * k * k;
        return true;
    }
    if(b->shake > 0 && b->state == B_INTACT) {
        *x = tip_xf(v3(b->x, 0, b->z), 1, 0, sinf(b->shake * 80.0f) * 0.02f * b->shake / 0.3f);
        return true;
    }
    return false;
}

static inline const pvr_poly_hdr_t *strip_hdr(const Building *b, int kind) {
    switch(kind & ~SK_BOX) {
        case SK_WALL: return h_wall[b->style];
        case SK_SHOP: return h_shop;
        case SK_ROOF: return h_roof;
        case SK_AD: return h_ad;
        case SK_INK: return cel_ink_hdr();
        case SK_RUBBLE: return h_rubble;
        case SK_PROP: return h_flat;
        default: return h_flat;
    }
}

/* ---- Carved buildings and their pieces: built as they are drawn ---- */
/* A quad: a and c along the foot, b and d over them. */
static void quad(const pvr_poly_hdr_t *h, V3 a, V3 b, V3 c, V3 d, float u0, float v0, float u1, float v1, uint32_t col) {
    RV v[4] = {{a.x, a.y, a.z, u0, v1, col}, {b.x, b.y, b.z, u0, v0, col}, {c.x, c.y, c.z, u1, v1, col}, {d.x, d.y, d.z, u1, v0, col}};
    r_hdr(h);
    r_strip(v, 4);
}

static void ink_ring(float x0, float x1, float z0, float z1, float y0, float y1) {
    RV v[10];
    const float xs[5] = {x0 - INK_W, x1 + INK_W, x1 + INK_W, x0 - INK_W, x0 - INK_W};
    const float zs[5] = {z0 - INK_W, z0 - INK_W, z1 + INK_W, z1 + INK_W, z0 - INK_W};
    for(int k = 0; k < 5; k++) {
        v[k * 2] = (RV){xs[k], y0, zs[k], 0, 0, INK_COL};
        v[k * 2 + 1] = (RV){xs[k], y1 + INK_W, zs[k], 0, 0, INK_COL};
    }
    r_hdr(cel_ink_hdr());
    r_strip(v, 10);
}

/* The walls of the box (x0..x1, z0..z1, y0..y1) that face the camera
   (all: every one). Bit e of broken: wall e is a break, not a face of the
   building: -Z, +X, +Z, -X. top is the tier's, where the windows count from. */
static void box_walls(const Building *b, float x0, float x1, float z0, float z1, float y0, float y1, float top, int skip,
                      int broken, V3 cp, bool all) {
    const bool see[4] = {all || cp.z < z0, all || cp.x > x1, all || cp.z > z1, all || cp.x < x0};
    const V3 a[4] = {{x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}};
    for(int e = 0; e < 4; e++) {
        if(!see[e] || (skip & (1 << e))) continue;
        V3 p = a[e], q = a[(e + 1) & 3];
        float len = (e & 1) ? z1 - z0 : x1 - x0;
        float d = face_n[e][0] * cel_sun.x + face_n[e][1] * cel_sun.z;
        if(broken & (1 << e))
            quad(h_rubble, p, v3(p.x, y1, p.z), q, v3(q.x, y1, q.z), 0, 0, len * 0.1f, (y1 - y0) * 0.1f, cel_band(0xff7c7688, d));
        else {
            /* The windows keep to the building's grid: from its corner. */
            float o = ((e & 1) ? p.z - (b->z - b->hd) : p.x - (b->x - b->hw)) * (1.0f / WALL_TILE);
            if(e >= 2) o = -o;
            o -= floorf(o);
            quad(h_wall[b->style], p, v3(p.x, y1, p.z), q, v3(q.x, y1, q.z), o, (top - y1) * (1.0f / WALL_TILE), o + len * (1.0f / WALL_TILE),
                 (top - y0) * (1.0f / WALL_TILE), cel_band(b->tint, d));
        }
    }
}

static void draw_carved(const Building *b, V3 cp, bool all, bool inked, bool detail) {
    const uint8_t full = b->cols == 4 ? 0xf : 1;
    const uint32_t rooftint = color_lerp(b->tint, 0xffe6e6ee, 0.6f), broke = cel_band(0xff8e8898, cel_sun.y);
    for(int k = 0; k < b->bands;) {
        uint8_t m = b->cell[k];
        if(!m) {
            k++;
            continue;
        }
        float y0 = (float)k * b->band_h, s = tier_scale(b, y0 + b->band_h * 0.5f), hw = b->hw * s, hd = b->hd * s;
        /* Whole bands of one tier go out as one ring of walls. */
        int k1 = k + 1;
        if(m == full)
            while(k1 < b->bands && b->cell[k1] == full && tier_scale(b, ((float)k1 + 0.5f) * b->band_h) == s) k1++;
        float y1 = (float)k1 * b->band_h, top = b->h;
        for(int t = b->ntiers - 1; t >= 0; t--)
            if(y0 < b->tier_y[t]) top = b->tier_y[t];
        uint8_t below = k > 0 ? b->cell[k - 1] : full, above = k1 < b->bands ? b->cell[k1] : 0;
        bool ledge = k1 >= b->bands || tier_scale(b, ((float)k1 + 0.5f) * b->band_h) < s;
        if(m == full) {
            box_walls(b, b->x - hw, b->x + hw, b->z - hd, b->z + hd, y0, y1, top, 0, 0, cp, all);
            if(inked) ink_ring(b->x - hw, b->x + hw, b->z - hd, b->z + hd, y0, y1);
        }
        for(int q = 0; q < b->cols; q++) {
            if(!(m & (1u << q))) continue;
            float x0 = b->x - hw, x1 = b->x + hw, z0 = b->z - hd, z1 = b->z + hd;
            int skip = 0, broken = 0;
            if(b->cols == 4) {
                /* A quarter: two walls are the building's, and the two
                   toward its middle show only where a neighbour has gone. */
                if(q & 1) { x0 = b->x; if(m & (1u << (q ^ 1))) skip |= 8; else broken |= 8; }
                else { x1 = b->x; if(m & (1u << (q ^ 1))) skip |= 2; else broken |= 2; }
                if(q & 2) { z0 = b->z; if(m & (1u << (q ^ 2))) skip |= 1; else broken |= 1; }
                else { z1 = b->z; if(m & (1u << (q ^ 2))) skip |= 4; else broken |= 4; }
            }
            if(m != full) {
                box_walls(b, x0, x1, z0, z1, y0, y1, top, skip, broken, cp, all);
                if(inked) ink_ring(x0, x1, z0, z1, y0, y1);
            }
            /* Its top, where nothing sits on it; its underside likewise. */
            if((ledge || !(above & (1u << q))) && (all || cp.y > y1)) {
                float u0 = (x0 - (b->x - hw)) / (2 * hw), u1 = (x1 - (b->x - hw)) / (2 * hw);
                float v0 = (z0 - (b->z - hd)) / (2 * hd), v1 = (z1 - (b->z - hd)) / (2 * hd);
                RV v[4] = {{x0, y1, z0, u0, v0, 0}, {x1, y1, z0, u1, v0, 0}, {x0, y1, z1, u0, v1, 0}, {x1, y1, z1, u1, v1, 0}};
                for(int i = 0; i < 4; i++) v[i].c = ledge ? rooftint : broke;
                r_hdr(ledge ? h_roof : h_rubble);
                r_strip(v, 4);
            }
            if(!(below & (1u << q)) && (all || cp.y < y0)) {
                RV v[4] = {{x0, y0, z0, 0, 0, 0xff5a5468}, {x1, y0, z0, 1, 0, 0xff5a5468}, {x0, y0, z1, 0, 1, 0xff5a5468},
                           {x1, y0, z1, 1, 1, 0xff5a5468}};
                r_hdr(h_rubble);
                r_strip(v, 4);
            }
        }
        k = k1;
    }
    /* The mast and the rooftop, while the top floor is all there. */
    if(b->cell[b->bands - 1] != full) return;
    for(int s = 0; s < b->nstrips; s++) {
        int kind = b->strip[s].kind & ~SK_BOX;
        if(kind != SK_FLAT && !((kind == SK_PROP || kind == SK_AD) && detail)) continue;
        r_hdr(strip_hdr(b, kind));
        r_strip(pool + b->v0 + b->strip[s].off, b->strip[s].n);
    }
}

static void bodies_draw(void) {
    bool any = false;
    for(int i = 0; i < BODIES; i++) {
        const Body *o = &bodies[i];
        if(!o->on || !r_sphere_visible(o->p, o->hx + o->hy + o->hz)) continue;
        Xf ry = xf_rot_y(o->ay), rx = xf_rot_x(o->ax), x = xf_mul(&ry, &rx);
        x.p = o->p;
        float m[16] __attribute__((aligned(32)));
        xf_m4(&x, m);
        r_load_model(m);
        any = true;
        V3 l = xf_local(&x, cel_sun);
        const float nd[4] = {-l.z, l.x, l.z, -l.x};
        RV v[16];
        float vt = o->hy * 2.0f / WALL_TILE;
        for(int e = 0; e < 4; e++) {
            const float *a = corner[e], *c = corner[(e + 1) & 3];
            float nu = fmaxf(1.0f, floorf(((e & 1) ? o->hz : o->hx) * 2.0f / WALL_TILE + 0.5f));
            uint32_t col = cel_band(o->tint, nd[e]);
            v[e * 4] = (RV){a[0] * o->hx, -o->hy, a[1] * o->hz, 0, vt, col};
            v[e * 4 + 1] = (RV){a[0] * o->hx, o->hy, a[1] * o->hz, 0, 0, col};
            v[e * 4 + 2] = (RV){c[0] * o->hx, -o->hy, c[1] * o->hz, nu, vt, col};
            v[e * 4 + 3] = (RV){c[0] * o->hx, o->hy, c[1] * o->hz, nu, 0, col};
        }
        r_hdr(h_wall[o->style]);
        r_strip(v, 16);
        /* The broken faces it was torn off by. */
        for(int side = -1; side <= 1; side += 2) {
            float y = (float)side * o->hy;
            uint32_t col = cel_band(0xff8e8898, (float)side * l.y);
            RV c[4] = {{-o->hx, y, -o->hz, 0, 0, col}, {o->hx, y, -o->hz, 1, 0, col}, {-o->hx, y, o->hz, 0, 1, col}, {o->hx, y, o->hz, 1, 1, col}};
            r_hdr(h_rubble);
            r_strip(c, 4);
        }
        ink_ring(-o->hx, o->hx, -o->hz, o->hz, -o->hy - INK_W, o->hy);
    }
    if(any) r_load_vp();
}

void city_draw_op(void) {
    sc_draw(&ground, HF_OP, ~0u);
    V3 cp = r_cam.pos;
    static uint16_t vis[MAX_B];
    int nvis = 0;
    for(int bi = 0; bi < CITY_N * CITY_N; bi++) {
        if(!blocks[bi].count) continue;
        float cx = city_road(bi % CITY_N) + CITY_PITCH * 0.5f, cz = city_road(bi / CITY_N) + CITY_PITCH * 0.5f;
        float mh = blocks[bi].max_h;
        /* Too far for anything in it to be drawn: the whole block goes
           before any of its buildings is looked at. (Its towers may lie
           well outside it, fallen.) */
        float br = blocks[bi].reach;
        float bd = sqrtf((cx - cp.x) * (cx - cp.x) + (cz - cp.z) * (cz - cp.z)) - br - mh;
        if(bd > DRAW_RANGE || (bd > SMALL_RANGE && mh < 36.0f)) continue;
        if(!r_sphere_visible(v3(cx, mh * 0.5f, cz), br + mh)) continue;
        bool only_tall = bd > SMALL_RANGE;
        for(int k = 0; k < blocks[bi].count; k++) {
            int i = blocks[bi].first + k;
            const Building *b = &city_b[i];
            if(only_tall && b->h < 36.0f && b->state != B_TOPPLE) continue;
            float rad = b->state == B_INTACT ? fmaxf(b->hw, b->hd) * 1.5f + b->h * 0.6f : b->len + 20.0f;
            if(r_sphere_visible(v3(b->x, b->state == B_INTACT ? b->h * 0.5f : 0, b->z), rad)) vis[nvis++] = (uint16_t)i;
        }
    }
    /* Shadows first (the ground's own list), then the buildings. */
    r_hdr(h_shadow);
    for(int n = 0; n < nvis; n++) {
        const Building *b = &city_b[vis[n]];
        if(b->state != B_INTACT || dist2_xz(v3(b->x, 0, b->z), cp) > SMALL_RANGE * SMALL_RANGE) continue;
        for(int s = 0; s < b->nstrips; s++)
            if(b->strip[s].kind == SK_SHADOW) r_strip(pool + b->v0 + b->strip[s].off, b->strip[s].n);
    }
    for(int n = 0; n < nvis; n++) {
        const Building *b = &city_b[vis[n]];
        float d2 = (b->x - cp.x) * (b->x - cp.x) + (b->z - cp.z) * (b->z - cp.z);
        if(d2 > DRAW_RANGE * DRAW_RANGE || (d2 > SMALL_RANGE * SMALL_RANGE && b->h < 36.0f && b->state != B_TOPPLE)) continue;
        bool inked = d2 < INK_RANGE * INK_RANGE, detail = d2 < DETAIL_RANGE * DETAIL_RANGE;
        bool lod = !inked && b->state != B_RUBBLE;
        int far_kind = SK_FAR + ((b->flags & BF_FAR1) ? 0 : (cp.x > b->x ? 1 : 0) + (cp.z > b->z ? 2 : 0));
        Xf x;
        float m[16] __attribute__((aligned(32)));
        bool moved = building_xf(b, &x);
        /* Everything of the building, mast and rooftop included, for the
           renderer to take whole strips on. */
        V3 bc = v3(b->x, b->h * 0.6f, b->z);
        float br = b->state == B_INTACT ? sqrtf(b->hw * b->hw * 7.0f + b->hd * b->hd * 7.0f + b->h * b->h * 0.42f) + 14.0f : b->len + 24.0f;
        if(moved) {
            xf_m4(&x, m);
            r_load_model(m);
        }
        bool carved = b->carved && b->state != B_RUBBLE;
        if(carved) draw_carved(b, cp, moved, inked, detail);
        for(int s = 0; s < b->nstrips && !carved; s++) {
            int kind = b->strip[s].kind & ~SK_BOX, n = b->strip[s].n;
            const RV *v = pool + b->v0 + b->strip[s].off;
            if(kind >= SK_FAR) {
                /* (a building in one piece draws it close up too) */
                if(kind != far_kind || !(lod || (b->flags & BF_ONE))) continue;
                r_hdr(h_wall[b->style]);
                if(moved) r_strip(v, n);
                else r_strip_bounded(v, n, bc, br);
                if(lod) break;
                continue;
            }
            if(lod || kind == SK_SHADOW || ((kind == SK_PROP || kind == SK_AD) && !detail)) continue;
            if(moved) {
                r_hdr(strip_hdr(b, kind));
                r_strip(v, kind == SK_INK ? 10 : (b->strip[s].kind & SK_BOX) ? 16 : n);
                continue;
            }
            /* A box standing still shows two walls at most: only those are
               sent, and of its ink shell only the walls facing away, which
               are the ones that show round the silhouette. */
            if(kind == SK_ROOF && cp.y < v[0].y) continue;
            bool box = (b->strip[s].kind & SK_BOX) && n == 20, shell = kind == SK_INK && n == 12;
            r_hdr(strip_hdr(b, kind));
            if(!box && !shell) {
                r_strip_bounded(v, n, bc, br);
                continue;
            }
            const RV *far = v + (box ? 8 : 4);
            bool face[5] = {cp.z < v[0].z, cp.x > far->x, cp.z > far->z, cp.x < v[0].x};
            face[4] = face[0];
            /* The first wall wanted that follows one that is not, and on
               round from there (the fifth is the first again). */
            int e = 0, run = 0, wanted = (face[0] != shell) + (face[1] != shell) + (face[2] != shell) + (face[3] != shell);
            if(wanted >= 3) run = 4;    /* (a shell seen from over the roof: all of it) */
            else {
                if((face[0] != shell) && (face[3] != shell)) e = 3;
                else while(e < 4 && face[e] == shell) e++;
                while(e + run < 5 && face[e + run] != shell) run++;
            }
            if(!run) continue;
            if(box) r_strip_bounded(v + e * 4, run * 4, bc, br);
            else r_strip_bounded(v + e * 2, run * 2 + 2, bc, br);
        }
        if(b->prop && b->state != B_RUBBLE && d2 < 260.0f * 260.0f && (!carved || b->cell[b->bands - 1] == (b->cols == 4 ? 0xf : 1))) {
            Xf t = xf_trans(v3(b->prop_x, b->prop_y, b->prop_z));
            if(moved) t = xf_mul(&x, &t);
            flat_mesh_draw(&cel_flat[b->prop == PROP_DONUT ? FM_DONUT : FM_WTANK], &t, 0xffffffff, d2 < INK_RANGE * INK_RANGE);
            moved = true;
        }
        if(moved) r_load_vp();
    }
    bodies_draw();
}

void city_draw_pt(void) {
    V3 cp = r_cam.pos;
    r_hdr(h_tree);
    for(int i = 0; i < UPROOTED; i++)
        if(uprooted[i].on)
            r_billboard(vadd(uprooted[i].p, v3(0, uprooted[i].s * 0.5f, 0)), uprooted[i].s * 0.5f, uprooted[i].s * 0.5f, uprooted[i].rot, 0, 0, 1, 1,
                        0xffffffff);
    for(int i = 0; i < ntrees; i++) {
        const Tree *t = &trees[i];
        if(!t->alive) continue;
        float d2 = (t->x - cp.x) * (t->x - cp.x) + (t->z - cp.z) * (t->z - cp.z);
        if(d2 > 700.0f * 700.0f || !r_sphere_visible(v3(t->x, t->s * 0.5f, t->z), t->s)) continue;
        r_billboard_y(v3(t->x, 0, t->z), t->s * 0.5f, t->s, 0, 0, 1, 1, 0xffffffff);
    }
    for(int n = 0; n < nawake; n++) {
        int i = awake[n];
        const Building *b = &city_b[i];
        if(b->state != B_INTACT || !(b->flags & BF_BURN)) continue;
        float d2 = (b->x - cp.x) * (b->x - cp.x) + (b->z - cp.z) * (b->z - cp.z);
        /* (not in the camera's face, when it is up among the rooftops) */
        if(d2 > 600.0f * 600.0f || (d2 < 55.0f * 55.0f && fabsf(b->h - cp.y) < 40.0f)) continue;
        float s = clampf(fminf(b->hw, b->hd) * 0.9f, 4, 12);
        fx_flame(v3(b->x - b->hw * 0.4f, b->h, b->z - b->hd * 0.3f), s, (float)i);
        fx_flame(v3(b->x + b->hw * 0.35f, b->h, b->z + b->hd * 0.4f), s * 0.8f, (float)i * 1.7f);
    }
}

/* ---- The sky: bands of blue, a comic sun, clouds, far hills ---- */
#define HILLS 40
#define CLOUDS 12
static float hill_h[2][HILLS + 1];
static struct { float az, el, w; } clouds[CLOUDS];

void city_draw_sky(void) {
    sky_draw(&sky);
    V3 c = r_cam.pos;
    r_hdr(h_flat_nofog);
    static const uint32_t hill_col[2] = {0xffa8cfe8, 0xff8cb8de};
    for(int l = 1; l >= 0; l--) {
        float rad = 1650.0f + 150.0f * (float)l;
        RV v[(HILLS + 1) * 2];
        for(int i = 0; i <= HILLS; i++) {
            float a = (float)i / HILLS * ENG_TAU + (float)l * 0.4f;
            float x = c.x + sinf(a) * rad, z = c.z + cosf(a) * rad;
            v[i * 2] = (RV){x, -30, z, 0, 0, hill_col[l]};
            v[i * 2 + 1] = (RV){x, hill_h[l][i], z, 0, 0, hill_col[l]};
        }
        /* In quarters: one strip of the lot would always cross behind the camera. */
        for(int q = 0; q < 4; q++) r_strip(v + q * (HILLS / 4) * 2, (HILLS / 4 + 1) * 2);
    }
}

static void draw_sky_sprites(void) {
    V3 c = r_cam.pos;
    r_hdr(h_sun);
    V3 s = vmad(c, cel_sun, 1500.0f);
    r_billboard(s, 150, 150, game.time * 0.1f, 0, 0, 1, 1, 0xffffe880);
    r_billboard(s, 120, 120, -game.time * 0.07f + 0.3f, 0, 0, 1, 1, 0xfffffbe0);
    r_hdr(h_cloud);
    for(int i = 0; i < CLOUDS; i++) {
        float a = clouds[i].az + game.time * 0.004f;
        V3 p = v3(c.x + sinf(a) * 1300.0f, 1300.0f * clouds[i].el, c.z + cosf(a) * 1300.0f);
        r_billboard(p, clouds[i].w, clouds[i].w * 0.5f, 0, 0, 0, 1, 1, 0xffffffff);
    }
}

void city_draw_sky_pt(void) { draw_sky_sprites(); }

bool city_init(void) {
    city_b = malloc(sizeof(Building) * MAX_B);
    pool = memalign(32, sizeof(RV) * POOL_VERTS);
    if(!city_b || !pool || !sc_init(&ground, CITY_ROADS * CITY_ROADS * 22 + 1200, CITY_ROADS * CITY_ROADS * 6 + 300, CITY_ROADS * CITY_ROADS + 64 * CITY_N)) return false;
    static const int wall_tex[ST_COUNT] = {TEX_WALL_OFFICE, TEX_WALL_APT, TEX_WALL_BRICK, TEX_WALL_RIBBON, TEX_WALL_GLASS,
                                           TEX_NONE, TEX_WALL_BRICK, TEX_WALL_WARE, TEX_WALL_DECO};
    for(int i = 0; i < ST_COUNT; i++) h_wall[i] = r_header(wall_tex[i], HF_OP);
    h_shop = r_header(TEX_SHOP, HF_OP);
    h_roof = r_header(TEX_ROOF, HF_OP | HF_CLAMP);
    h_flat = r_header(TEX_NONE, HF_OP);
    h_ad = r_header(TEX_ADS, HF_OP | HF_CLAMP);
    h_rubble = r_header(TEX_RUBBLE, HF_OP);
    h_shadow = r_header(TEX_HALFTONE, HF_OP);
    h_tree = r_header(TEX_TREE, HF_PT | HF_CLAMP);
    h_cloud = r_header(TEX_CLOUD, HF_PT | HF_CLAMP | HF_NOFOG);
    h_sun = r_header(TEX_POW, HF_PT | HF_CLAMP | HF_NOFOG);
    h_flat_nofog = r_header(TEX_NONE, HF_OP | HF_NOFOG);
    /* Hard bands: each colour change is two rings a hair apart. */
    static const float elev[8] = {-0.25f, 0.05f, 0.056f, 0.26f, 0.266f, 0.62f, 0.626f, 1.5f};
    static const uint32_t band[8] = {0xffcce8fa, 0xffcce8fa, 0xff9ed4f8, 0xff9ed4f8, 0xff72bcf4, 0xff72bcf4, 0xff4ea2ee, 0xff4ea2ee};
    if(!sky_init(&sky, elev, 8, 16, 1900.0f, h_flat_nofog)) return false;
    for(int r = 0; r < 8; r++) sky_ring_color(&sky, r, band[r]);
    uint32_t s = 77;
    for(int l = 0; l < 2; l++) {
        for(int i = 0; i < HILLS; i++) {
            s = s * 1664525u + 1013904223u;
            hill_h[l][i] = 50.0f + 60.0f * (float)l + (float)((s >> 10) & 255) * 0.5f;
        }
        hill_h[l][HILLS] = hill_h[l][0];
    }
    /* One volcano on the far range. */
    hill_h[1][8] = 200; hill_h[1][9] = 330; hill_h[1][10] = 340; hill_h[1][11] = 210;
    for(int i = 0; i < CLOUDS; i++) {
        s = s * 1664525u + 1013904223u;
        clouds[i].az = (float)i / CLOUDS * ENG_TAU + (float)((s >> 8) & 255) * 0.002f;
        clouds[i].el = 0.16f + (float)((s >> 16) & 255) * 0.0012f;
        clouds[i].w = 90.0f + (float)((s >> 12) & 127);
    }
    return true;
}
