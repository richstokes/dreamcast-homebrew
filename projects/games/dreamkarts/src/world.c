/* Track scenery: Emerald Beach, Coral Crossing, Palm Harbour, Temple Hill
   and Neon Skyway.

   Static geometry (track surfaces, terrain, props, palm trees) is generated
   once into a pool of world-space triangle strips grouped into chunks with
   bounding spheres; each frame only visible chunks are transformed. The sea,
   sky, clouds, shore foam and dash panels animate, so they are emitted per
   frame. */
#include "world.h"
#include "render.h"
#include "track.h"
#include "texture_assets.h"
#include <malloc.h>

V3 world_sun_dir;
float world_time;
bool world_split;
bool world_night;
uint16_t world_light[3] = {256, 256, 256};

typedef struct {
    const pvr_poly_hdr_t *hdr;
    uint16_t first, count;
} Strip;

typedef struct {
    V3 c;
    float r;
    float lod_far;     /* not drawn beyond this distance */
    uint16_t first_strip, nstrips;
    uint8_t list;
    uint8_t far;       /* distant terrain or fine detail: skipped in a split screen */
} Chunk;

typedef struct {
    V3 base;
    float height;
} Palm;

#define MAX_POOL   26000
#define MAX_STRIPS 4600
#define MAX_CHUNKS 420
#define MAX_PALMS  48
#define MAX_SPRITES 64

static RV *pool;
static int pool_n;
static Strip strips[MAX_STRIPS];
static int strip_n;
static Chunk chunks[MAX_CHUNKS];
static int chunk_n;
static Chunk *cur_chunk;
static int cur_strip_first;
static Palm palms[MAX_PALMS];
static int palm_n;
static V3 far_sprites[MAX_SPRITES];
static float far_sprite_h[MAX_SPRITES];
static int far_sprite_n;
static bool built_ok = true;
static float shore_db[TRACK_SAMPLES];
/* Shore foam: waterline points with a seaward normal; off = break. */
#define MAX_FOAM (TRACK_SAMPLES / 2 + 2)
static V3 foam_p[MAX_FOAM], foam_n[MAX_FOAM];
static float foam_u[MAX_FOAM];
static bool foam_on[MAX_FOAM];
static int foam_count;
static V3 sky_dir[7][13];
static V3 ring_dir[25];

/* Headers. */
static const pvr_poly_hdr_t *h_road, *h_board, *h_curb, *h_sand, *h_grass, *h_barrier, *h_wood,
    *h_rock, *h_bark, *h_checker, *h_thatch, *h_crowd, *h_banner, *h_col, *h_water,
    *h_frond, *h_palm_far, *h_dash, *h_sky, *h_cloud, *h_mount, *h_foam, *h_glow_add,
    *h_flare, *h_dash_glow, *h_stone, *h_flame, *h_nightsky, *h_skyline, *h_moon, *h_cloudsea, *h_star_add,
    *h_tower, *h_signs;
/* Neon Skyway's road: the strips point at one header whose texture is
   swapped each frame through the phases, so the lights run along it. */
#define SKYROAD_PHASES 16
static const pvr_poly_hdr_t *h_skyroad[SKYROAD_PHASES];
static pvr_poly_hdr_t sk_road_hdr __attribute__((aligned(32)));

/* ------------------------------------------------------------------ */
/* Builder                                                             */

static void chunk_begin(int list, float lod_far) {
    if(chunk_n >= MAX_CHUNKS) {
        built_ok = false;
        cur_chunk = NULL;
        return;
    }
    cur_chunk = &chunks[chunk_n];
    cur_chunk->list = (uint8_t)list;
    cur_chunk->lod_far = lod_far;
    cur_chunk->first_strip = (uint16_t)strip_n;
    cur_chunk->nstrips = 0;
    cur_chunk->far = 0;
    cur_strip_first = pool_n;
}

static void chunk_end(void) {
    if(!cur_chunk) return;
    if(strip_n == cur_chunk->first_strip) {
        cur_chunk = NULL;
        return;
    }
    cur_chunk->nstrips = (uint16_t)(strip_n - cur_chunk->first_strip);
    int v0 = strips[cur_chunk->first_strip].first;
    V3 lo = v3(1e9f, 1e9f, 1e9f), hi = v3(-1e9f, -1e9f, -1e9f);
    for(int i = v0; i < pool_n; i++) {
        lo.x = fminf(lo.x, pool[i].x); hi.x = fmaxf(hi.x, pool[i].x);
        lo.y = fminf(lo.y, pool[i].y); hi.y = fmaxf(hi.y, pool[i].y);
        lo.z = fminf(lo.z, pool[i].z); hi.z = fmaxf(hi.z, pool[i].z);
    }
    cur_chunk->c = vscale(vadd(lo, hi), 0.5f);
    cur_chunk->r = vlen(vsub(hi, lo)) * 0.5f;
    chunk_n++;
    cur_chunk = NULL;
}

static const pvr_poly_hdr_t *strip_hdr;

static void strip_begin(const pvr_poly_hdr_t *h) {
    strip_hdr = h;
    cur_strip_first = pool_n;
}

static void sv(float x, float y, float z, float u, float v, uint32_t c) {
    if(pool_n >= MAX_POOL) {
        built_ok = false;
        return;
    }
    pool[pool_n++] = (RV){x, y, z, u, v, c};
}

static void svp(V3 p, float u, float v, uint32_t c) {
    sv(p.x, p.y, p.z, u, v, c);
}

static void strip_end(void) {
    int n = pool_n - cur_strip_first;
    if(!cur_chunk || n < 3 || strip_n >= MAX_STRIPS) {
        if(strip_n >= MAX_STRIPS) built_ok = false;
        pool_n = cur_strip_first;
        return;
    }
    /* Long strips are split so r_strip's buffer always fits. */
    int first = cur_strip_first;
    while(n >= 3) {
        int take = n > 240 ? 240 : n;
        strips[strip_n++] = (Strip){strip_hdr, (uint16_t)first, (uint16_t)take};
        if(take == n) break;
        first += take - 2;
        n -= take - 2;
        if(strip_n >= MAX_STRIPS) {
            built_ok = false;
            break;
        }
    }
}

static uint32_t shade(uint32_t c, V3 n) {
    float l = 0.66f + 0.42f * fmaxf(0.0f, vdot(vnorm(n), world_sun_dir));
    return color_scale(c, l);
}

/* A 4-vertex strip (a b c d: a-b top edge, c-d bottom edge). */
static void quad(const pvr_poly_hdr_t *h, V3 a, V3 b, V3 c, V3 d, float u0, float v0, float u1, float v1,
                 uint32_t col) {
    V3 n = vcross(vsub(b, a), vsub(c, a));
    uint32_t s = shade(col, vscale(n, -1.0f));
    if(vdot(n, n) > 0 && vnorm(n).y > 0.0f) s = shade(col, n);
    strip_begin(h);
    svp(a, u0, v0, s);
    svp(b, u1, v0, s);
    svp(c, u0, v1, s);
    svp(d, u1, v1, s);
    strip_end();
}

/* Upright box without a bottom; yaw about Y. */
static void box(const pvr_poly_hdr_t *h, V3 c, V3 half, float yaw, uint32_t col, float uv_scale) {
    V3 ax = vscale(heading_right(yaw), half.x), az = vscale(heading_fwd(yaw), half.z), ay = v3(0, half.y, 0);
    V3 p[8];
    for(int i = 0; i < 8; i++)
        p[i] = vadd(c, vadd(vscale(ax, (i & 1) ? 1 : -1), vadd(vscale(ay, (i & 2) ? 1 : -1), vscale(az, (i & 4) ? 1 : -1))));
    float ux = half.x * 2 * uv_scale, uy = half.y * 2 * uv_scale, uz = half.z * 2 * uv_scale;
    /* top */
    strip_begin(h);
    uint32_t top = shade(col, v3(0, 1, 0));
    svp(p[6], 0, 0, top); svp(p[7], ux, 0, top); svp(p[2], 0, uz, top); svp(p[3], ux, uz, top);
    strip_end();
    const int faces[4][4] = {{2, 3, 0, 1}, {7, 6, 5, 4}, {6, 2, 4, 0}, {3, 7, 1, 5}};
    const V3 normals[4] = {vscale(az, -1), az, vscale(ax, -1), ax};
    for(int f = 0; f < 4; f++) {
        float uw = f < 2 ? ux : uz;
        uint32_t s = shade(col, normals[f]);
        strip_begin(h);
        svp(p[faces[f][0]], 0, 0, s); svp(p[faces[f][1]], uw, 0, s);
        svp(p[faces[f][2]], 0, uy, s); svp(p[faces[f][3]], uw, uy, s);
        strip_end();
    }
}

/* Tapered cylinder as one strip; colour may vary per ring via cb. */
static void cylinder(const pvr_poly_hdr_t *h, V3 base, float r0, float r1, float height, int sides,
                     uint32_t col0, uint32_t col1, float u_rep, float v_rep) {
    strip_begin(h);
    for(int i = 0; i <= sides; i++) {
        float a = (float)i / sides * DK_TAU;
        V3 n = v3(sinf(a), 0, cosf(a));
        V3 b = vmad(base, n, r0), t = vadd(vmad(base, n, r1), v3(0, height, 0));
        float u = (float)i / sides * u_rep;
        svp(t, u, 0, shade(col1, n));
        svp(b, u, v_rep, shade(col0, n));
    }
    strip_end();
}

static void cone(const pvr_poly_hdr_t *h, V3 apex, float radius, float drop, int sides, uint32_t ca, uint32_t cb) {
    for(int i = 0; i < sides; i++) {
        float a0 = (float)i / sides * DK_TAU, a1 = (float)(i + 1) / sides * DK_TAU;
        V3 p0 = vadd(apex, v3(sinf(a0) * radius, -drop, cosf(a0) * radius));
        V3 p1 = vadd(apex, v3(sinf(a1) * radius, -drop, cosf(a1) * radius));
        uint32_t c = (i & 1) ? cb : ca;
        V3 n = vcross(vsub(p1, apex), vsub(p0, apex));
        if(n.y < 0) n = vscale(n, -1);
        c = shade(c, n);
        strip_begin(h);
        svp(apex, 0.5f, 0, c);
        svp(p0, 0, 1, c);
        svp(p1, 1, 1, c);
        strip_end();
    }
}

/* ------------------------------------------------------------------ */
/* Terrain                                                             */

static float smooth01(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

/* Ground height beyond the outer wall: beach into the sea to the north,
   rising jungle hills to the south. db = distance beyond the wall. */
static float terrain_height(V3 p, float db, float shore) {
    float north = smooth01(-30.0f, 30.0f, p.z);
    float beach = lerpf(0.0f, -0.75f, shore) - 0.02f * db - 0.0016f * db * db;
    float hills = fmaxf(0.0f, db - 16.0f) * 0.13f;
    float bumps = (sinf(p.x * 0.021f) + sinf(p.z * 0.017f + 1.3f) + sinf((p.x + p.z) * 0.009f)) *
                  clampf((db - 24.0f) / 60.0f, 0, 1) * 4.0f;
    return lerpf(hills + bumps, beach, north);
}

#define WATER_Y (-0.35f)

static uint32_t ground_color(float h, float db, bool grassy) {
    if(h < WATER_Y) return argb(255, 200, 196, 170);           /* wet sand under water */
    if(grassy) return argb(255, 255, 255, 255);
    float wet = clampf((h - WATER_Y) / 0.25f, 0, 1);
    return color_lerp(argb(255, 196, 184, 150), argb(255, 255, 255, 255), wet);
}

static void build_track_chunk(int c) {
    const int per = 8;
    const float ds = track.ds;
    int mid = (c * per + per / 2) % TRACK_SAMPLES;
    bool board = track.smp[mid].section == SEC_BOARDWALK;
    chunk_begin(HF_OP, 2000.0f);

    /* Road surface (asphalt with curbs, or the wider boardwalk). */
    strip_begin(board ? h_board : h_road);
    for(int j = 0; j <= per; j++) {
        const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
        float s = (float)(c * per + j) * ds, half = board ? 9.6f : 8.0f;
        uint32_t col = board ? argb(255, 255, 255, 255) : argb(255, 250, 250, 250);
        svp(vmad(t->p, t->r, -half), 0, s / 8.0f, col);
        svp(vmad(t->p, t->r, half), 1, s / 8.0f, col);
    }
    strip_end();

    if(!board) {
        for(int side = -1; side <= 1; side += 2) {
            strip_begin(h_curb);
            for(int j = 0; j <= per; j++) {
                const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
                float s = (float)(c * per + j) * ds;
                V3 a = vmad(t->p, t->r, side * 8.0f), b = vmad(t->p, t->r, side * 9.6f);
                a.y = b.y = 0.01f;
                if(side < 0) {
                    svp(b, 0, s / 2.4f, 0xffffffff);
                    svp(a, 1, s / 2.4f, 0xffffffff);
                }
                else {
                    svp(a, 1, s / 2.4f, 0xffffffff);
                    svp(b, 0, s / 2.4f, 0xffffffff);
                }
            }
            strip_end();
        }
    }

    /* Sand shoulders to the barriers. */
    for(int side = -1; side <= 1; side += 2) {
        strip_begin(board && side > 0 ? h_wood : h_sand);
        bool any = false;
        for(int j = 0; j <= per; j++) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            float inner = board ? 9.6f : 9.6f;
            float outer = side < 0 ? t->wall_in : t->wall_out;
            if(outer <= inner + 0.05f) outer = inner + 0.05f;
            V3 a = vmad(t->p, t->r, side * inner), b = vmad(t->p, t->r, side * outer);
            uint32_t ca = argb(255, 238, 238, 238), cb = argb(255, 214, 214, 214);
            svp(a, a.x / 4.0f, a.z / 4.0f, ca);
            svp(b, b.x / 4.0f, b.z / 4.0f, cb);
            any = true;
        }
        if(any) strip_end();
    }

    /* Barriers: padded walls, or wooden rails along the boardwalk. */
    for(int side = -1; side <= 1; side += 2) {
        bool rail = board;
        strip_begin(rail ? h_wood : h_barrier);
        for(int j = 0; j <= per; j++) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            float s = (float)(c * per + j) * ds;
            float w = side < 0 ? t->wall_in : t->wall_out;
            V3 b = vmad(t->p, t->r, side * w);
            float hgt = rail ? 0.85f : 1.0f;
            uint32_t col = shade(argb(255, 255, 255, 255), vscale(t->r, (float)-side));
            svp(vadd(b, v3(0, hgt, 0)), s / 4.0f, rail ? 0.0f : 0.0f, col);
            svp(b, s / 4.0f, rail ? 0.25f : 1.0f, color_scale(col, 0.8f));
        }
        strip_end();
        /* Barrier top cap so the walls read as solid from above. */
        strip_begin(h_col);
        for(int j = 0; j <= per; j++) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            float w = side < 0 ? t->wall_in : t->wall_out;
            float hgt = rail ? 0.85f : 1.0f;
            V3 a = vmad(t->p, t->r, side * (w - 0.25f)), b = vmad(t->p, t->r, side * (w + 0.25f));
            a.y = b.y = hgt;
            uint32_t cap = rail ? argb(255, 190, 140, 90) : argb(255, 255, 212, 50);
            svp(a, 0, 0, cap);
            svp(b, 0, 0, cap);
        }
        strip_end();
    }
    chunk_end();

    /* Infield: from the inner wall to the oval's spine. */
    chunk_begin(HF_OP, 2000.0f);
    strip_begin(h_grass);
    for(int j = 0; j <= per; j++) {
        const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
        V3 a = vmad(t->p, t->r, -t->wall_in);
        V3 spine = v3(clampf(t->p.x, -85.0f, 85.0f), 0, 0);
        float dist = vlen(vsub(a, spine));
        V3 a2 = vlerp(a, spine, clampf(4.0f / fmaxf(dist, 1.0f), 0, 1));
        svp(a, a.x / 6.0f, a.z / 6.0f, argb(255, 196, 206, 196));
        svp(a2, a2.x / 6.0f, a2.z / 6.0f, argb(255, 240, 248, 240));
    }
    strip_end();
    strip_begin(h_grass);
    for(int j = 0; j <= per; j++) {
        const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
        V3 a = vmad(t->p, t->r, -t->wall_in);
        V3 spine = v3(clampf(t->p.x, -85.0f, 85.0f), 0, 0);
        float dist = vlen(vsub(a, spine));
        V3 a2 = vlerp(a, spine, clampf(4.0f / fmaxf(dist, 1.0f), 0, 1));
        svp(a2, a2.x / 6.0f, a2.z / 6.0f, argb(255, 240, 248, 240));
        svp(spine, spine.x / 6.0f, spine.z / 6.0f, argb(255, 255, 255, 255));
    }
    strip_end();
    chunk_end();

    /* Outside terrain bands. */
    static const float bands[] = {0.0f, 5.0f, 12.0f, 22.0f, 40.0f, 75.0f, 130.0f, 230.0f, 420.0f};
    const int nb = (int)(sizeof(bands) / sizeof(bands[0]));
    chunk_begin(HF_OP, 2000.0f);
    for(int b = 0; b + 1 < nb; b++) {
        int step = b >= 4 ? 2 : 1;
        if(b == 6) {
            /* The outermost bands form their own chunk. */
            chunk_end();
            chunk_begin(HF_OP, 2000.0f);
            if(cur_chunk) cur_chunk->far = 1;
        }
        /* Pick the band's material from its middle: grass on dry southern
           ground, sand on beaches and the sea floor. */
        const TrackSample *tm = &track.smp[mid];
        V3 pm = vmad(tm->p, tm->r, tm->wall_out + bands[b + 1]);
        pm.y = terrain_height(pm, bands[b + 1], tm->shore);
        bool grassy = b >= 2 && pm.z < 10.0f && pm.y > 0.4f;
        float uvs = grassy ? 6.0f : 4.0f;
        strip_begin(grassy ? h_grass : h_sand);
        for(int j = 0; j <= per; j += step) {
            const TrackSample *t = &track.smp[(c * per + j) % TRACK_SAMPLES];
            for(int k = 0; k < 2; k++) {
                float db = bands[b + k];
                V3 p = vmad(t->p, t->r, t->wall_out + db);
                p.y = terrain_height(p, db, t->shore);
                svp(p, p.x / uvs, p.z / uvs, ground_color(p.y, db, grassy));
            }
        }
        strip_end();
    }
    chunk_end();
}

/* ------------------------------------------------------------------ */
/* Props                                                               */

static void add_palm(V3 base, float height, float lean_yaw, float lean, uint32_t seed) {
    if(palm_n >= MAX_PALMS) return;
    palms[palm_n++] = (Palm){base, height};
    chunk_begin(HF_OP, 135.0f);
    V3 lean_dir = heading_fwd(lean_yaw);
    const int segs = 5;
    V3 prev = base;
    V3 ring_c[6];
    for(int i = 0; i <= segs; i++) {
        float t = (float)i / segs;
        V3 c = vadd(base, v3(0, height * t, 0));
        c = vmad(c, lean_dir, lean * height * t * t);
        ring_c[i] = c;
    }
    (void)prev;
    for(int side = 0; side < 5; side++) {
        float a0 = side / 5.0f * DK_TAU, a1 = (side + 1) / 5.0f * DK_TAU;
        V3 n0 = v3(sinf(a0), 0, cosf(a0)), n1 = v3(sinf(a1), 0, cosf(a1));
        strip_begin(h_bark);
        for(int i = 0; i <= segs; i++) {
            float r = lerpf(0.34f, 0.2f, (float)i / segs);
            float v = (float)i * 0.9f;
            svp(vmad(ring_c[i], n0, r), 0, v, shade(argb(255, 255, 255, 255), n0));
            svp(vmad(ring_c[i], n1, r), 1, v, shade(argb(255, 255, 255, 255), n1));
        }
        strip_end();
    }
    chunk_end();

    /* Fronds: alpha-tested bent leaves in the punch-through list. */
    chunk_begin(HF_PT, 135.0f);
    V3 top = ring_c[segs];
    int fronds = 8;
    for(int f = 0; f < fronds; f++) {
        float a = (float)f / fronds * DK_TAU + (float)(seed % 7) * 0.3f;
        V3 dir = v3(sinf(a), 0, cosf(a)), side = v3(cosf(a), 0, -sinf(a));
        float len = height * (0.50f + 0.08f * (float)((seed + f * 3) % 4));
        float rise = 0.55f, w = len * 0.30f;
        strip_begin(h_frond);
        for(int k = 0; k <= 3; k++) {
            float t = (float)k / 3.0f;
            V3 c = vadd(top, vadd(vscale(dir, len * t), v3(0, len * (rise * t - 1.15f * t * t), 0)));
            float ww = w * (k == 0 ? 0.25f : 1.0f);
            uint32_t col = color_scale(argb(255, 255, 255, 255), 0.85f + 0.25f * t);
            svp(vmad(c, side, -ww), 0, 1.0f - t, col);
            svp(vmad(c, side, ww), 1, 1.0f - t, col);
        }
        strip_end();
    }
    chunk_end();
}

static void add_umbrella(V3 base, uint32_t ca, uint32_t cb) {
    chunk_begin(HF_OP, 260.0f);
    box(h_col, vadd(base, v3(0, 1.1f, 0)), v3(0.05f, 1.1f, 0.05f), 0, argb(255, 240, 240, 240), 1);
    cone(h_col, vadd(base, v3(0, 2.6f, 0)), 1.9f, 0.7f, 8, ca, cb);
    /* Towel and cool box. */
    float yaw = base.x * 0.37f;
    box(h_col, vadd(base, v3(1.2f, 0.03f, 0.4f)), v3(0.55f, 0.03f, 1.0f), yaw, cb == 0xffffffff ? ca : cb, 1);
    box(h_col, vadd(base, v3(-0.9f, 0.22f, -0.8f)), v3(0.3f, 0.22f, 0.22f), yaw, argb(255, 60, 140, 230), 1);
    chunk_end();
}

static void add_hut(V3 c, float yaw) {
    chunk_begin(HF_OP, 600.0f);
    box(h_wood, vadd(c, v3(0, 1.4f, 0)), v3(2.6f, 1.4f, 2.2f), yaw, argb(255, 255, 240, 220), 0.5f);
    /* Thatch roof: four sloped faces meeting at a ridge point. */
    V3 ax = vscale(heading_right(yaw), 3.2f), az = vscale(heading_fwd(yaw), 2.8f);
    V3 roof_base = vadd(c, v3(0, 2.7f, 0)), apex = vadd(c, v3(0, 4.6f, 0));
    V3 corners[4] = {vsub(vsub(roof_base, ax), az), vsub(vadd(roof_base, ax), az),
                     vadd(vadd(roof_base, ax), az), vadd(vsub(roof_base, ax), az)};
    for(int i = 0; i < 4; i++) {
        V3 a = corners[i], b = corners[(i + 1) % 4];
        V3 n = vcross(vsub(b, a), vsub(apex, a));
        if(n.y < 0) n = vscale(n, -1);
        uint32_t col = shade(argb(255, 255, 255, 255), n);
        strip_begin(h_thatch);
        svp(apex, 0.5f, 0, col);
        svp(a, 0, 2, col);
        svp(b, 2, 2, col);
        strip_end();
    }
    chunk_end();
}

static void add_rock(V3 c, float r, float hgt, uint32_t seed, bool grass_top) {
    chunk_begin(HF_OP, 2000.0f);
    const int sides = 7;
    float rad[3][7];
    for(int k = 0; k < 3; k++)
        for(int i = 0; i < sides; i++) {
            seed = seed * 1103515245u + 12345u;
            rad[k][i] = r * (0.75f + 0.5f * (float)((seed >> 16) & 255) / 255.0f) * (k == 0 ? 1.0f : k == 1 ? 0.92f : 0.7f);
        }
    float ys[3] = {-3.0f, hgt * 0.55f, hgt};
    for(int k = 0; k < 2; k++) {
        strip_begin(h_rock);
        for(int i = 0; i <= sides; i++) {
            int ii = i % sides;
            float a = (float)i / sides * DK_TAU;
            V3 n = v3(sinf(a), 0.25f, cosf(a));
            V3 top = vadd(c, v3(sinf(a) * rad[k + 1][ii], ys[k + 1], cosf(a) * rad[k + 1][ii]));
            V3 bot = vadd(c, v3(sinf(a) * rad[k][ii], ys[k], cosf(a) * rad[k][ii]));
            float u = (float)i / sides * 3.0f;
            svp(top, u, ys[k + 1] / 8.0f, shade(0xffffffff, n));
            svp(bot, u, ys[k] / 8.0f, shade(0xffffffff, n));
        }
        strip_end();
    }
    /* Cap. */
    strip_begin(grass_top ? h_grass : h_rock);
    V3 capc = vadd(c, v3(0, hgt + r * 0.15f, 0));
    for(int i = 0; i <= sides; i++) {
        int ii = i % sides;
        float a = (float)i / sides * DK_TAU;
        V3 p = vadd(c, v3(sinf(a) * rad[2][ii], hgt, cosf(a) * rad[2][ii]));
        svp(p, p.x / 5.0f, p.z / 5.0f, shade(0xffffffff, v3(sinf(a), 1.5f, cosf(a))));
        svp(capc, capc.x / 5.0f, capc.z / 5.0f, 0xffffffff);
    }
    strip_end();
    chunk_end();
}

/* Emerald Coast's signature: a natural rock arch standing in the sea. */
static void add_arch(V3 c, float yaw) {
    chunk_begin(HF_OP, 2000.0f);
    const int segs = 9, sides = 6;
    V3 ax = heading_right(yaw);
    float span = 26.0f, rise = 24.0f;
    for(int s = 0; s < segs; s++) {
        float t0 = (float)s / segs, t1 = (float)(s + 1) / segs;
        float a0 = DK_PI * t0, a1 = DK_PI * t1;
        V3 c0 = vadd(c, vadd(vscale(ax, -cosf(a0) * span), v3(0, sinf(a0) * rise - 4.0f, 0)));
        V3 c1 = vadd(c, vadd(vscale(ax, -cosf(a1) * span), v3(0, sinf(a1) * rise - 4.0f, 0)));
        float th0 = 6.5f - 3.0f * sinf(a0), th1 = 6.5f - 3.0f * sinf(a1);
        V3 tan0 = vnorm(vsub(c1, c0));
        V3 up0 = vnorm(vcross(heading_fwd(yaw), tan0));
        V3 fw = heading_fwd(yaw);
        strip_begin(h_rock);
        for(int i = 0; i <= sides; i++) {
            float a = (float)i / sides * DK_TAU;
            V3 off0 = vadd(vscale(up0, cosf(a) * th0), vscale(fw, sinf(a) * th0 * 0.8f));
            V3 off1 = vadd(vscale(up0, cosf(a) * th1), vscale(fw, sinf(a) * th1 * 0.8f));
            V3 n = vnorm(off0);
            svp(vadd(c1, off1), (float)i / sides * 2.0f, t1 * 6.0f, shade(0xffffffff, n));
            svp(vadd(c0, off0), (float)i / sides * 2.0f, t0 * 6.0f, shade(0xffffffff, n));
        }
        strip_end();
    }
    chunk_end();
    /* Greenery on top. */
    add_rock(vadd(c, v3(0, rise - 6.0f, 0)), 5.0f, 5.5f, 77, true);
}

static void add_lighthouse(V3 c) {
    add_rock(c, 22.0f, 7.0f, 991, true);
    chunk_begin(HF_OP, 2000.0f);
    V3 b = vadd(c, v3(0, 7.0f, 0));
    for(int band = 0; band < 5; band++) {
        float y0 = band * 4.2f, y1 = y0 + 4.2f;
        float r0 = lerpf(3.0f, 1.9f, y0 / 21.0f), r1 = lerpf(3.0f, 1.9f, y1 / 21.0f);
        uint32_t col = (band & 1) ? argb(255, 220, 40, 40) : argb(255, 250, 250, 245);
        cylinder(h_col, vadd(b, v3(0, y0, 0)), r0, r1, 4.2f, 8, col, col, 1, 1);
    }
    cylinder(h_col, vadd(b, v3(0, 21.0f, 0)), 2.3f, 2.3f, 2.6f, 8, argb(255, 255, 240, 160), argb(255, 255, 250, 200), 1, 1);
    cone(h_col, vadd(b, v3(0, 25.6f, 0)), 2.8f, 2.0f, 8, argb(255, 200, 40, 40), argb(255, 170, 30, 30));
    chunk_end();
}

static void add_boat(V3 c, float yaw, uint32_t hull) {
    chunk_begin(HF_OP, 2000.0f);
    box(h_col, vadd(c, v3(0, 0.6f, 0)), v3(1.6f, 0.7f, 5.0f), yaw, hull, 1);
    box(h_col, vadd(c, v3(0, 5.0f, 0)), v3(0.08f, 4.0f, 0.08f), yaw, argb(255, 230, 230, 230), 1);
    V3 fw = heading_fwd(yaw);
    V3 mast_top = vadd(c, v3(0, 9.0f, 0)), mast_bot = vadd(c, v3(0, 1.5f, 0));
    V3 clew = vadd(vmad(c, fw, -4.2f), v3(0, 1.6f, 0));
    strip_begin(h_col);
    svp(mast_top, 0, 0, argb(255, 255, 255, 255));
    svp(mast_bot, 0, 0, argb(255, 235, 235, 240));
    svp(clew, 0, 0, argb(255, 225, 228, 236));
    strip_end();
    V3 bow = vadd(vmad(c, fw, 5.5f), v3(0, 1.4f, 0));
    strip_begin(h_col);
    svp(mast_top, 0, 0, argb(255, 255, 200, 60));
    svp(mast_bot, 0, 0, argb(255, 255, 170, 40));
    svp(bow, 0, 0, argb(255, 255, 150, 30));
    strip_end();
    chunk_end();
}

static void add_gantry(void) {
    const TrackSample *t = &track.smp[0];
    chunk_begin(HF_OP, 900.0f);
    for(int side = -1; side <= 1; side += 2) {
        V3 p = vmad(t->p, t->r, side * 10.8f);
        box(h_col, vadd(p, v3(0, 3.4f, 0)), v3(0.45f, 3.4f, 0.45f), 0, argb(255, 240, 240, 245), 1);
        box(h_col, vadd(p, v3(0, 0.3f, 0)), v3(0.7f, 0.3f, 0.7f), 0, argb(255, 40, 90, 200), 1);
    }
    /* Banner panel, both faces. */
    V3 l = vmad(t->p, t->r, -10.4f), r = vmad(t->p, t->r, 10.4f);
    float y0 = 5.0f, y1 = 6.8f;
    V3 off = vscale(t->t, 0.3f);
    for(int f = -1; f <= 1; f += 2) {
        V3 o = vscale(off, (float)f);
        /* Text must read left-to-right from whichever side it faces. */
        V3 a = vadd(vadd(f < 0 ? l : r, o), v3(0, y1, 0)), b = vadd(vadd(f < 0 ? r : l, o), v3(0, y1, 0));
        V3 c = vadd(vadd(f < 0 ? l : r, o), v3(0, y0, 0)), d = vadd(vadd(f < 0 ? r : l, o), v3(0, y0, 0));
        strip_begin(h_banner);
        svp(a, 0, 0, 0xffffffff); svp(b, 1, 0, 0xffffffff);
        svp(c, 0, 0.5f, 0xffffffff); svp(d, 1, 0.5f, 0xffffffff);
        strip_end();
    }
    V3 top0 = vadd(l, v3(0, y1, 0)), top1 = vadd(r, v3(0, y1, 0));
    quad(h_col, vadd(top0, vscale(off, -1)), vadd(top1, vscale(off, -1)), vadd(top0, off), vadd(top1, off),
         0, 0, 1, 1, argb(255, 30, 70, 190));
    chunk_end();

    /* Chequered start line, slightly above the asphalt. */
    chunk_begin(HF_OP, 600.0f);
    strip_begin(h_checker);
    for(int k = 0; k <= 1; k++) {
        float s = (k ? 1.2f : -1.2f);
        V3 a = track_point(s, -8.0f), b = track_point(s, 8.0f);
        a.y = b.y = 0.025f;
        svp(a, 0, s / 1.2f, 0xffffffff);
        svp(b, 8, s / 1.2f, 0xffffffff);
    }
    strip_end();
    chunk_end();
}

/* Tiered seating along z = zf, spectators facing north. */
static void add_grandstand(float x0, float x1, float zf) {
    chunk_begin(HF_OP, 900.0f);
    float dz = zf + 82.0f;
    for(int tier = 0; tier < 5; tier++) {
        float z = -82.0f + dz - tier * 2.6f, y = 0.9f + tier * 1.2f;
        /* Seats with spectators facing the track (north). */
        V3 a = v3(x0, y, z), b = v3(x1, y, z), c = v3(x0, y - 1.2f, z + 0.1f), d = v3(x1, y - 1.2f, z + 0.1f);
        quad(h_crowd, a, b, c, d, 0, 0, (x1 - x0) / 3.0f, 0.6f, argb(255, 255, 255, 255));
        V3 e = v3(x0, y, z - 2.6f), f = v3(x1, y, z - 2.6f);
        quad(h_col, e, f, a, b, 0, 0, 1, 1, argb(255, 190, 196, 210));
    }
    box(h_col, v3((x0 + x1) * 0.5f, 3.6f, -97.5f + dz), v3((x1 - x0) * 0.5f, 3.6f, 0.4f), 0, argb(255, 150, 160, 185), 1);
    /* Striped canopy on posts. */
    for(int i = 0; i <= 6; i++) {
        float x = lerpf(x0, x1, (float)i / 6.0f);
        box(h_col, v3(x, 4.5f, -82.5f + dz), v3(0.15f, 4.5f, 0.15f), 0, argb(255, 235, 235, 240), 1);
    }
    for(int i = 0; i < 12; i++) {
        float xa = lerpf(x0, x1, (float)i / 12.0f), xb = lerpf(x0, x1, (float)(i + 1) / 12.0f);
        uint32_t col = (i & 1) ? argb(255, 250, 250, 250) : argb(255, 40, 140, 230);
        quad(h_col, v3(xa, 10.5f, -98.0f + dz), v3(xb, 10.5f, -98.0f + dz), v3(xa, 9.0f, -81.5f + dz),
             v3(xb, 9.0f, -81.5f + dz), 0, 0, 1, 1, col);
    }
    chunk_end();
}

static void add_sign(V3 c, float yaw) {
    chunk_begin(HF_OP, 700.0f);
    V3 ax = heading_right(yaw), fw = heading_fwd(yaw);
    for(int side = -1; side <= 1; side += 2)
        box(h_col, vadd(vmad(c, ax, side * 7.0f), v3(0, 3.0f, 0)), v3(0.3f, 3.0f, 0.3f), yaw, argb(255, 230, 230, 235), 1);
    for(int f = -1; f <= 1; f += 2) {
        V3 o = vscale(fw, 0.25f * f);
        V3 l = vadd(vmad(c, ax, (float)f * 8.0f), o), r = vadd(vmad(c, ax, (float)f * -8.0f), o);
        strip_begin(h_banner);
        svp(vadd(l, v3(0, 9.0f, 0)), 0, 0, 0xffffffff);
        svp(vadd(r, v3(0, 9.0f, 0)), 1, 0, 0xffffffff);
        svp(vadd(l, v3(0, 5.0f, 0)), 0, 0.5f, 0xffffffff);
        svp(vadd(r, v3(0, 5.0f, 0)), 1, 0.5f, 0xffffffff);
        strip_end();
    }
    chunk_end();
}

static void add_pier(float x, float z0) {
    chunk_begin(HF_OP, 900.0f);
    float z1 = z0 + 58.0f, w = 2.2f, y = 0.45f;
    strip_begin(h_board);
    svp(v3(x - w, y, z0), 0, 0, 0xffffffff);
    svp(v3(x + w, y, z0), 1, 0, 0xffffffff);
    svp(v3(x - w, y, z1), 0, (z1 - z0) / 8.0f, 0xffffffff);
    svp(v3(x + w, y, z1), 1, (z1 - z0) / 8.0f, 0xffffffff);
    strip_end();
    for(float z = z0 + 4.0f; z < z1; z += 8.0f)
        for(int side = -1; side <= 1; side += 2)
            box(h_wood, v3(x + side * (w - 0.2f), -0.6f, z), v3(0.18f, 1.1f, 0.18f), 0, 0xffffffff, 1);
    add_hut(v3(x, y, z1 - 3.0f), 0.0f);
    chunk_end();
}

static void add_pond(V3 c, float rx, float rz) {
    chunk_begin(HF_OP, 600.0f);
    const int n = 16;
    /* Sand rim then water, as rings from the centre. */
    for(int ring = 0; ring < 2; ring++) {
        strip_begin(ring ? h_water : h_sand);
        float ro = ring ? 0.82f : 1.0f, ri = ring ? 0.0f : 0.82f, y = ring ? 0.08f : 0.04f;
        for(int i = 0; i <= n; i++) {
            float a = (float)i / n * DK_TAU;
            V3 o = v3(c.x + cosf(a) * rx * ro, c.y + y, c.z + sinf(a) * rz * ro);
            V3 in = v3(c.x + cosf(a) * rx * ri, c.y + y, c.z + sinf(a) * rz * ri);
            uint32_t co = ring ? argb(255, 200, 250, 250) : argb(255, 255, 255, 255);
            uint32_t ci = ring ? argb(255, 120, 200, 235) : argb(255, 230, 225, 210);
            svp(o, o.x / 6.0f, o.z / 6.0f, co);
            svp(in, in.x / 6.0f, in.z / 6.0f, ci);
        }
        strip_end();
    }
    chunk_end();
}

static void place_props(void) {
    /* Infield. */
    add_pond(v3(0, 0, 0), 34.0f, 13.0f);
    add_sign(v3(-58.0f, 0, 0), 0.0f);
    add_sign(v3(58.0f, 0, 0), 0.0f);
    const V3 infield_palms[] = {
        {-30, 0, 15}, {-12, 0, 17}, {10, 0, 16}, {30, 0, -15}, {12, 0, -17}, {-14, 0, -16},
        {-40, 0, -6}, {40, 0, 6}, {-95, 0, 12}, {95, 0, -12}, {-110, 0, -6}, {110, 0, 8}
    };
    for(unsigned i = 0; i < sizeof(infield_palms) / sizeof(infield_palms[0]); i++)
        add_palm(infield_palms[i], 8.5f + (float)(i % 3) * 1.5f, (float)i * 1.7f, 0.18f, i * 13 + 5);
    add_umbrella(v3(-22, 0, -8), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(0, 0, 9), argb(255, 40, 120, 230), 0xffffffff);
    add_umbrella(v3(22, 0, -6), argb(255, 255, 210, 40), argb(255, 60, 190, 90));
    add_umbrella(v3(-70, 0, -18), argb(255, 255, 120, 180), 0xffffffff);
    add_umbrella(v3(70, 0, 18), argb(255, 255, 150, 30), 0xffffffff);

    /* South side: grandstand, huts, palms. */
    add_grandstand(-52.0f, 26.0f, -82.0f);
    add_gantry();
    add_hut(v3(-120, 0, -98), 0.3f);
    add_hut(v3(-100, 0, -104), -0.2f);
    add_hut(v3(118, 0, -96), 0.6f);
    const V3 south_palms[] = {
        {-70, 0, -86}, {44, 0, -84}, {60, 0, -92}, {-140, 0, -80}, {150, 0, -78}, {-160, 0, -60},
        {170, 0, -50}, {-90, 0, -92}, {90, 0, -100}, {0, 0, -104}
    };
    for(unsigned i = 0; i < sizeof(south_palms) / sizeof(south_palms[0]); i++)
        add_palm(south_palms[i], 10.0f + (float)(i % 4) * 1.2f, (float)i * 2.3f, 0.22f, i * 7 + 3);

    /* North beaches at the turn ends. */
    const V3 beach_palms[] = {
        {150, 0, 50}, {165, 0, 30}, {-150, 0, 48}, {-166, 0, 26}, {120, 0, 72}, {-120, 0, 72}
    };
    for(unsigned i = 0; i < sizeof(beach_palms) / sizeof(beach_palms[0]); i++) {
        V3 p = beach_palms[i];
        TrackPos tp;
        track_locate(p, -1, &tp);
        float wall = track_wall_out_at(&tp);
        p = vmad(tp.center, tp.right, wall + 4.0f);
        p.y = terrain_height(p, 4.0f, track.smp[tp.idx].shore);
        if(p.y > WATER_Y + 0.1f)
            add_palm(p, 9.5f + (float)i, (float)i * 1.1f + 2.0f, 0.35f, i * 5 + 11);
    }
    add_umbrella(v3(160, -0.05f, 40), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(-158, -0.05f, 40), argb(255, 40, 200, 160), 0xffffffff);

    /* Sea features. */
    add_arch(v3(-40, 0, 165), 0.15f);
    add_rock(v3(110, 0, 230), 14.0f, 18.0f, 5, true);
    add_rock(v3(-210, 0, 150), 10.0f, 12.0f, 9, true);
    add_rock(v3(290, 0, 60), 16.0f, 22.0f, 17, true);
    add_rock(v3(60, 0, 140), 5.0f, 4.0f, 21, false);
    add_lighthouse(v3(230, 0, 210));
    add_boat(v3(-170, WATER_Y, 330), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 480), -0.4f, argb(255, 200, 40, 40));
    add_boat(v3(420, WATER_Y, 260), 1.9f, argb(255, 250, 250, 250));
    add_pier(32.0f, 66.0f);
    chunk_begin(HF_OP, 500.0f);
    for(int i = 0; i < 9; i++) {
        V3 b = v3(-80.0f + i * 20.0f, WATER_Y - 0.2f, 104.0f);
        cylinder(h_col, b, 0.45f, 0.35f, 0.9f, 6, (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40),
                 (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40), 1, 1);
    }
    chunk_end();

    /* Distant palm silhouettes on the hills. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t r = (uint32_t)i * 2654435761u;
        float t = (float)(r & 1023) / 1023.0f;
        float db = 30.0f + (float)((r >> 10) & 255) / 255.0f * 150.0f;
        float s = track.length * (0.55f + 0.9f * t);
        V3 p = track_point(s, 21.0f + db);
        if(p.z > 5.0f) continue;
        p.y = terrain_height(p, db, 0.0f);
        far_sprites[far_sprite_n] = p;
        far_sprite_h[far_sprite_n] = 9.0f + (float)((r >> 18) & 7);
        far_sprite_n++;
    }
}

/* ------------------------------------------------------------------ */
/* Coral Crossing                                                      */

/* Ground: a flat headland around the track, jungle hills rising to the
   south and a beach falling into the sea to the north. Palm Harbour shares
   it with its own plateau extent. */
static float gnd_xw = 200.0f, gnd_zn = 84.0f, gnd_zs = 96.0f;

static float f8_ground(float x, float z) {
    float dx = fmaxf(0.0f, fabsf(x) - gnd_xw);
    float dzn = fmaxf(0.0f, z - gnd_zn), dzs = fmaxf(0.0f, -z - gnd_zs);
    float dn = sqrtf(dx * dx + dzn * dzn), dsth = sqrtf(dx * dx + dzs * dzs);
    float beach = -0.012f * dn - 0.0005f * dn * dn;
    float bumps = (sinf(x * 0.021f) + sinf(z * 0.017f + 1.3f) + sinf((x + z) * 0.009f)) *
                  clampf((dsth - 20.0f) / 60.0f, 0, 1) * 4.0f;
    float hills = fmaxf(0.0f, dsth - 8.0f) * 0.14f + bumps;
    return lerpf(hills, beach, smooth01(-40.0f, 40.0f, z));
}

#define F8_GRASS_Y (-0.06f)

static float f8_ground_y(float x, float z) {
    float h = f8_ground(x, z);
    return h > -0.01f && h < 0.01f ? F8_GRASS_Y : h + F8_GRASS_Y;
}

static void f8_build_ground(void) {
    static const float zs[] = {-400, -340, -280, -230, -190, -150, -120, -96, -60, -20, 20, 60, 84, 92, 100,
                               108, 120, 140, 170, 210, 260};
    const int nz = (int)(sizeof(zs) / sizeof(zs[0]));
    const int nx = 24;          /* 40 m columns from x = -480 */
    for(int cz = 0; cz + 1 < nz; cz += 4) {
        for(int cx = 0; cx < nx; cx += 4) {
            chunk_begin(HF_OP, 2000.0f);
            bool far = true;
            for(int j = cz; j < cz + 4 && j + 1 < nz; j++) {
                float za = zs[j], zb = zs[j + 1];
                float xm = -480.0f + (cx + 2) * 40.0f, zm = (za + zb) * 0.5f;
                float hm = f8_ground(xm, zm);
                bool grassy = zm < 70.0f && hm > -0.1f;
                float uvs = grassy ? 6.0f : 4.0f;
                if(fabsf(xm) < 300.0f && zm > -200.0f && zm < 200.0f) far = false;
                strip_begin(grassy ? h_grass : h_sand);
                for(int i = cx; i <= cx + 4 && i <= nx; i++) {
                    float x = -480.0f + i * 40.0f;
                    for(int k = 1; k >= 0; k--) {
                        float z = k ? zb : za;
                        float y = f8_ground_y(x, z);
                        float db = fmaxf(0.0f, fmaxf(fabsf(x) - gnd_xw, -z - gnd_zs));
                        svp(v3(x, y, z), x / uvs, z / uvs, ground_color(y, db, grassy));
                    }
                }
                strip_end();
            }
            if(cur_chunk) cur_chunk->far = far;
            chunk_end();
        }
    }
}

/* Lap distances for chunk c's road: the regular samples plus the jump's
   profile breakpoints (so the lip and deck edges are exact) or a viaduct's
   ends, with extra samples where the road's height bends sharply. */
#define MAX_SVALS 40
static int f8_chunk_svals(int c, float *out) {
    const int per = 8;
    float s0 = (float)(c * per) * track.ds, s1 = (float)((c + 1) * per) * track.ds;
    int n = 0;
    for(int j = 0; j <= per; j++) out[n++] = (float)(c * per + j) * track.ds;
    if(track.has_jump || track.has_bridge) {
        const Jump *jp = &track.jump;
        float marks[6], at;
        int nm = 0;
        if(track.has_jump) {
            at = jp->cross;
            marks[nm++] = jp->ramp; marks[nm++] = jp->lip - 0.01f; marks[nm++] = jp->lip;
            marks[nm++] = jp->land; marks[nm++] = jp->land + 0.01f; marks[nm++] = jp->foot;
        }
        else {
            at = track.bridge_s;
            marks[nm++] = -track.bridge_half;
            marks[nm++] = track.bridge_half;
        }
        for(int m = 0; m < nm; m++) {
            float s = at + marks[m];
            while(s < s0) s += track.length;
            while(s >= s0 + track.length) s -= track.length;
            if(s <= s0 || s >= s1) continue;
            int k = n++;
            while(k > 0 && out[k - 1] > s) {
                out[k] = out[k - 1];
                k--;
            }
            out[k] = s;
        }
    }
    /* Halve intervals whose midpoint height strays from a straight line. */
    for(int pass = 0; pass < 2 && track.id != TRACK_FIGURE8; pass++) {
        for(int k = 0; k + 1 < n && n < MAX_SVALS; k++) {
            float a = out[k], b = out[k + 1], m = (a + b) * 0.5f;
            if(b - a < 0.8f) continue;
            if(fabsf(track_height(m) - 0.5f * (track_height(a) + track_height(b))) < 0.04f) continue;
            for(int q = n; q > k + 1; q--) out[q] = out[q - 1];
            out[k + 1] = m;
            n++;
            k++;
        }
    }
    return n;
}

/* Road-surface point at lap distance s, lateral d, raised by dy. */
static V3 f8_pt(float s, float d, float dy) {
    V3 p = track_point(s, d);
    p.y = track_road_y(s, d) + dy;
    return p;
}

static float f8_wall(float s) {
    int i = (int)(track_wrap(s) / track.ds) % TRACK_SAMPLES;
    int j = (i + 1) % TRACK_SAMPLES;
    float f = clampf((track_wrap(s) - track.smp[i].s) / track.ds, 0, 1);
    return lerpf(track.smp[i].wall_out, track.smp[j].wall_out, f);
}

/* One ribbon along the chunk's lap distances between lateral offsets d0
   and d1 (scaled by the barrier distance when flagged), broken over the
   jump gap. UVs either follow the track or the world plane. */
enum { RB_WALL0 = 1, RB_WALL1 = 2 };
static void f8_ribbon(const float *sv, int n, const pvr_poly_hdr_t *h, float d0, float d1, float dy0, float dy1,
                      int flags, float uscale, float vscale, bool world_uv, uint32_t c0, uint32_t c1) {
    bool open = false;
    for(int k = 0; k < n; k++) {
        float s = sv[k];
        if(track_in_gap(s)) {
            if(open) strip_end();
            open = false;
            continue;
        }
        if(!open) {
            strip_begin(h);
            open = true;
        }
        float w = f8_wall(s);
        V3 pa = f8_pt(s, (flags & RB_WALL0) ? d0 * w : d0, dy0);
        V3 pb = f8_pt(s, (flags & RB_WALL1) ? d1 * w : d1, dy1);
        if(world_uv) {
            svp(pa, pa.x / uscale, pa.z / uscale, c0);
            svp(pb, pb.x / uscale, pb.z / uscale, c1);
        }
        else {
            svp(pa, 0, s / vscale, c0);
            svp(pb, uscale, s / vscale, c1);
        }
    }
    if(open) strip_end();
}

static bool in_bridge(float s) {
    return track.has_bridge && fabsf(track_delta(track.bridge_s, s)) < track.bridge_half - 0.01f;
}

static void th_banks(const float *sv, int n);

static void f8_build_track_chunk(int c) {
    float sv[MAX_SVALS];
    int n = f8_chunk_svals(c, sv);
    chunk_begin(HF_OP, 2000.0f);
    /* Asphalt and curbs. */
    f8_ribbon(sv, n, h_road, -8.0f, 8.0f, 0.0f, 0.0f, 0, 1.0f, 8.0f, false, argb(255, 250, 250, 250),
              argb(255, 250, 250, 250));
    f8_ribbon(sv, n, h_curb, -9.6f, -8.0f, 0.01f, 0.01f, 0, 0.0f, 2.4f, false, 0xffffffff, 0xffffffff);
    f8_ribbon(sv, n, h_curb, 8.0f, 9.6f, 0.01f, 0.01f, 0, 1.0f, 2.4f, false, 0xffffffff, 0xffffffff);
    /* Sand shoulders to the barriers. */
    f8_ribbon(sv, n, h_sand, -9.6f, -1.0f, 0.0f, 0.0f, RB_WALL1, 4.0f, 0, true, argb(255, 238, 238, 238),
              argb(255, 214, 214, 214));
    f8_ribbon(sv, n, h_sand, 1.0f, 9.6f, 0.0f, 0.0f, RB_WALL0, 4.0f, 0, true, argb(255, 214, 214, 214),
              argb(255, 238, 238, 238));
    /* Barriers with a top cap (stone parapets across a viaduct), then the
       ramp's wooden side skirts or a hill's embankments. */
    for(int side = -1; side <= 1; side += 2) {
        bool open = false;
        int run_bridge = -1;
        for(int k = 0; k < n; k++) {
            float s = sv[k];
            if(track_in_gap(s)) {
                if(open) strip_end();
                open = false;
                continue;
            }
            /* The interval after this sample decides the material; a run
               ends on the shared sample so the wall stays continuous. */
            int br = k + 1 < n ? in_bridge((s + sv[k + 1]) * 0.5f) : run_bridge;
            if(open && br != run_bridge) {
                V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
                uint32_t col = shade(argb(255, 255, 255, 255), vscale(r, (float)-side));
                V3 b = f8_pt(s, side * f8_wall(s), 0.0f);
                if(side < 0) {
                    svp(vadd(b, v3(0, 1.0f, 0)), s / 4.0f, 0.0f, col);
                    svp(b, s / 4.0f, 1.0f, color_scale(col, 0.8f));
                }
                else {
                    svp(b, s / 4.0f, 1.0f, color_scale(col, 0.8f));
                    svp(vadd(b, v3(0, 1.0f, 0)), s / 4.0f, 0.0f, col);
                }
                strip_end();
                open = false;
            }
            if(!open) {
                strip_begin(br > 0 ? h_stone : h_barrier);
                run_bridge = br;
                open = true;
            }
            V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
            uint32_t col = shade(argb(255, 255, 255, 255), vscale(r, (float)-side));
            V3 b = f8_pt(s, side * f8_wall(s), 0.0f);
            /* Wind the faces toward the road on both sides. */
            if(side < 0) {
                svp(vadd(b, v3(0, 1.0f, 0)), s / 4.0f, 0.0f, col);
                svp(b, s / 4.0f, 1.0f, color_scale(col, 0.8f));
            }
            else {
                svp(b, s / 4.0f, 1.0f, color_scale(col, 0.8f));
                svp(vadd(b, v3(0, 1.0f, 0)), s / 4.0f, 0.0f, col);
            }
        }
        if(open) strip_end();
        /* Top cap, +-0.25 m about the barrier line (2% of a 12.5 m wall). */
        uint32_t cap = track.has_bridge ? argb(255, 214, 190, 140) : argb(255, 255, 212, 50);
        f8_ribbon(sv, n, h_col, side * 0.98f, side * 1.02f, 1.0f, 1.0f, RB_WALL0 | RB_WALL1, 1, 1, true, cap, cap);
    }
    if(track.id == TRACK_TEMPLE) {
        th_banks(sv, n);
        chunk_end();
        return;
    }
    /* Skirts: from the barrier foot on the raised deck down to the grass. */
    for(int side = -1; side <= 1; side += 2) {
        bool open = false;
        for(int k = 0; k < n; k++) {
            float s = sv[k];
            bool raised = track_height(s) > 0.001f ||
                          (k + 1 < n && track_height(sv[k + 1]) > 0.001f) ||
                          (k > 0 && track_height(sv[k - 1]) > 0.001f);
            if(track_in_gap(s) || !raised) {
                if(open) strip_end();
                open = false;
                continue;
            }
            if(!open) {
                strip_begin(h_wood);
                open = true;
            }
            float h = track_height(s);
            V3 top = f8_pt(s, side * (f8_wall(s) + 0.25f), 0.0f), bot = top;
            bot.y = F8_GRASS_Y;
            V3 r = vsub(track_point(s, 1.0f), track_point(s, 0.0f));
            uint32_t col = shade(argb(255, 255, 240, 220), vscale(r, (float)side));
            if(side > 0) {
                svp(top, s / 3.0f, 0, col);
                svp(bot, s / 3.0f, h / 3.0f, color_scale(col, 0.75f));
            }
            else {
                svp(bot, s / 3.0f, h / 3.0f, color_scale(col, 0.75f));
                svp(top, s / 3.0f, 0, col);
            }
        }
        if(open) strip_end();
    }
    chunk_end();
}

/* The lip and the landing deck's front: solid faces over the gap with a
   hazard-striped top band. */
static void f8_jump_face(float s, float dir) {
    float h = track_height(s - dir * 0.02f);
    float w = f8_wall(s) + 0.25f;
    V3 t = track_tangent(s);
    uint32_t col = shade(argb(255, 255, 240, 220), vscale(t, dir));
    V3 l = f8_pt(s, -w * dir, 0), r = f8_pt(s, w * dir, 0);
    l.y = r.y = h;
    V3 lb = l, rb = r;
    lb.y = rb.y = F8_GRASS_Y;
    float band = 0.7f;
    quad(h_wood, vsub(l, v3(0, band, 0)), vsub(r, v3(0, band, 0)), lb, rb, 0, 0, 2.0f * w / 3.0f, h / 3.0f, col);
    const int stripes = 10;
    for(int i = 0; i < stripes; i++) {
        V3 a = vlerp(l, r, (float)i / stripes), b = vlerp(l, r, (float)(i + 1) / stripes);
        uint32_t sc = (i & 1) ? argb(255, 40, 40, 44) : argb(255, 255, 206, 30);
        quad(h_col, a, b, vsub(a, v3(0, band, 0)), vsub(b, v3(0, band, 0)), 0, 0, 1, 1, sc);
    }
}

/* Foam along the beach's waterline. */
static void f8_foam(void) {
    for(float x = -460.0f; x <= 460.0f && foam_count < MAX_FOAM; x += 20.0f) {
        float z = 60.0f;
        while(z < 300.0f && f8_ground(x, z) >= WATER_Y) z += 1.0f;
        foam_p[foam_count] = v3(x, 0, z);
        foam_n[foam_count] = v3(0, 0, 1);
        foam_u[foam_count] = x;
        foam_on[foam_count] = z < 300.0f;
        foam_count++;
    }
}

static void f8_place_props(void) {
    const float cx = 89.34f;   /* loop centres at (+-cx, 0) */
    chunk_begin(HF_OP, 900.0f);
    f8_jump_face(track_wrap(track.jump.cross + track.jump.lip), 1.0f);
    f8_jump_face(track_wrap(track.jump.cross + track.jump.land), -1.0f);
    chunk_end();
    add_gantry();
    add_grandstand(-15.0f, 45.0f, -62.0f);

    /* Lagoons in both loops. */
    for(int side = -1; side <= 1; side += 2) {
        V3 c = v3(side * cx, F8_GRASS_Y, 0);
        add_pond(c, 21.0f, 15.0f);
        add_umbrella(v3(side * cx - 14.0f, 0, 19.0f), argb(255, 230, 40, 40), 0xffffffff);
        add_umbrella(v3(side * cx + 16.0f, 0, -18.0f), argb(255, 40, 120, 230), 0xffffffff);
        add_umbrella(v3(side * cx + 4.0f, 0, 22.0f), argb(255, 255, 210, 40), argb(255, 60, 190, 90));
        for(int i = 0; i < 6; i++) {
            float a = (float)i / 6.0f * DK_TAU + 0.4f * side;
            V3 p = v3(c.x + cosf(a) * 30.0f, 0, sinf(a) * 25.0f);
            add_palm(p, 8.5f + (float)(i % 3) * 1.5f, (float)i * 1.7f, 0.18f, (uint32_t)(i * 13 + 5 + side));
        }
    }
    add_sign(v3(0, 0, 48.0f), 0.0f);

    /* Between the legs and around the outside. */
    const V3 palms_in[] = {
        {0, 0, 40}, {-14, 0, 62}, {16, 0, 66}, {0, 0, -38}, {-18, 0, -48},
        {-70, 0, -86}, {60, 0, -92}, {-140, 0, -80}, {150, 0, -78}, {-160, 0, -60},
        {170, 0, -50}, {-90, 0, -92}, {90, 0, -100}, {0, 0, -100}, {-180, 0, 10}, {182, 0, -6}
    };
    for(unsigned i = 0; i < sizeof(palms_in) / sizeof(palms_in[0]); i++) {
        V3 p = palms_in[i];
        p.y = f8_ground_y(p.x, p.z);
        add_palm(p, 9.5f + (float)(i % 4) * 1.2f, (float)i * 2.3f, 0.22f, i * 7 + 3);
    }
    add_hut(v3(-120, 0, -98), 0.3f);
    add_hut(v3(-100, 0, -104), -0.2f);
    add_hut(v3(118, 0, -96), 0.6f);

    /* North beach and the sea. */
    const float beach_x[] = {-160, -110, -60, 70, 120, 170};
    for(int i = 0; i < 6; i++) {
        V3 p = v3(beach_x[i], 0, 90.0f + (float)(i % 2) * 4.0f);
        p.y = f8_ground_y(p.x, p.z);
        add_palm(p, 9.5f + (float)i, (float)i * 1.1f + 2.0f, 0.35f, i * 5 + 11);
    }
    add_umbrella(v3(-130, f8_ground_y(-130, 94), 94), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(140, f8_ground_y(140, 94), 94), argb(255, 40, 200, 160), 0xffffffff);
    add_arch(v3(-40, 0, 185), 0.15f);
    add_rock(v3(110, 0, 250), 14.0f, 18.0f, 5, true);
    add_rock(v3(-210, 0, 170), 10.0f, 12.0f, 9, true);
    add_rock(v3(290, 0, 120), 16.0f, 22.0f, 17, true);
    add_lighthouse(v3(230, 0, 230));
    add_boat(v3(-170, WATER_Y, 350), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 500), -0.4f, argb(255, 200, 40, 40));
    add_pier(30.0f, 94.0f);
    chunk_begin(HF_OP, 500.0f);
    for(int i = 0; i < 9; i++) {
        V3 b = v3(-80.0f + i * 20.0f, WATER_Y - 0.2f, 130.0f);
        uint32_t col = (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40);
        cylinder(h_col, b, 0.45f, 0.35f, 0.9f, 6, col, col, 1, 1);
    }
    chunk_end();

    /* Distant palm silhouettes on the southern hills. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t r = (uint32_t)i * 2654435761u;
        float x = -420.0f + (float)(r & 1023) / 1023.0f * 840.0f;
        float z = -130.0f - (float)((r >> 10) & 255) / 255.0f * 230.0f;
        V3 p = v3(x, 0, z);
        p.y = f8_ground_y(x, z);
        far_sprites[far_sprite_n] = p;
        far_sprite_h[far_sprite_n] = 9.0f + (float)((r >> 18) & 7);
        far_sprite_n++;
    }

    f8_foam();
}

/* ------------------------------------------------------------------ */
/* Palm Harbour                                                        */

/* True when p (on the ground plane) is at least margin behind the nearest
   barrier. */
static bool hb_clear(V3 p, float margin) {
    TrackPos tp;
    track_locate(v3(p.x, 0, p.z), -1, &tp);
    V3 d = vsub(v3(p.x, 0, p.z), tp.center);
    return vlen(d) > track_wall_out_at(&tp) + margin;
}

static V3 hb_ground(float x, float z) {
    return v3(x, f8_ground_y(x, z), z);
}

/* A stack of shipping crates on the quay. */
static void hb_crates(V3 c, float yaw, uint32_t seed) {
    static const uint32_t cols[] = {0xffd83c30, 0xff2f6fd8, 0xfff0b020, 0xff38a050, 0xffe8e8e0};
    chunk_begin(HF_OP, 500.0f);
    V3 ax = heading_right(yaw);
    for(int i = 0; i < 4; i++) {
        seed = seed * 1103515245u + 12345u;
        uint32_t col = cols[(seed >> 16) % 5];
        V3 p = i < 3 ? vmad(c, ax, (float)(i - 1) * 2.7f) : vadd(c, v3(0, 2.5f, 0));
        float h = 1.25f;
        box(h_wood, vadd(p, v3(0, h, 0)), v3(1.25f, h, 2.6f), yaw + 0.04f * (float)(i - 1), col, 0.4f);
    }
    chunk_end();
}

/* The harbour wall: a curve of boulders out to the lighthouse. */
static void hb_breakwater(void) {
    for(int i = 0; i < 9; i++) {
        float t = (float)i / 8.0f;
        V3 p = v3(lerpf(118.0f, 205.0f, t), 0, 96.0f + 125.0f * t * t);
        add_rock(p, 6.0f + (float)(i % 3), 2.4f + (float)((i * 5) % 3) * 0.8f, (uint32_t)(i * 31 + 7), false);
    }
}

static void hb_place_props(void) {
    add_gantry();
    add_grandstand(-78.0f, -8.0f, -113.0f);

    /* Infield: the big field inside turn 1, the pocket between the hairpins
       and the seafront lawn. */
    add_pond(v3(-12.0f, F8_GRASS_Y, -62.0f), 26.0f, 12.0f);
    add_sign(v3(22.0f, 0, -64.0f), 0.0f);
    add_sign(v3(-40.0f, 0, 40.0f), 0.0f);
    add_umbrella(v3(-32.0f, 0, -54.0f), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(v3(8.0f, 0, -72.0f), argb(255, 40, 120, 230), 0xffffffff);
    add_umbrella(v3(-58.0f, 0, 36.0f), argb(255, 255, 210, 40), argb(255, 60, 190, 90));
    add_umbrella(v3(-14.0f, 0, 44.0f), argb(255, 255, 120, 180), 0xffffffff);
    add_hut(v3(30.0f, 0, -14.0f), 0.4f);
    add_hut(v3(28.0f, 0, 10.0f), -0.3f);
    add_hut(v3(-120, 0, -118), 0.3f);
    add_hut(v3(-100, 0, -124), -0.2f);
    add_hut(v3(112, 0, -112), 0.6f);

    /* Palms scattered over the headland, kept behind the barriers. */
    uint32_t r = 0x50a1u;
    for(int tries = 0; tries < 400 && palm_n < 40; tries++) {
        r = r * 1664525u + 1013904223u;
        float x = -150.0f + (float)((r >> 8) & 1023) / 1023.0f * 300.0f;
        float z = -135.0f + (float)((r >> 18) & 1023) / 1023.0f * 228.0f;
        V3 p = hb_ground(x, z);
        if(p.y < WATER_Y + 0.15f || !hb_clear(p, 3.0f)) continue;
        /* Keep the grandstand, the pier and the props above clear. */
        if(z < -108.0f && x > -84.0f && x < -2.0f) continue;
        if(fabsf(x - 70.0f) < 8.0f && z > 84.0f) continue;
        bool crowded = false;
        for(int i = 0; i < palm_n; i++) {
            V3 d = vsub(palms[i].base, p);
            if(d.x * d.x + d.z * d.z < 12.0f * 12.0f) crowded = true;
        }
        if(crowded) continue;
        add_palm(p, 9.0f + (float)((r >> 4) % 4) * 1.2f, (float)(r % 628) * 0.01f, 0.18f + (z > 70.0f ? 0.15f : 0.0f),
                 r >> 12);
    }

    /* The quay: the pier, moored boats, crates and the harbour wall. */
    add_pier(70.0f, 92.0f);
    hb_crates(hb_ground(48.0f, 88.0f), 0.1f, 3);
    hb_crates(hb_ground(-104.0f, 90.0f), -0.2f, 11);
    add_boat(v3(80.0f, WATER_Y, 122.0f), 0.0f, argb(255, 200, 40, 40));
    add_boat(v3(60.0f, WATER_Y, 128.0f), 0.1f, argb(255, 250, 250, 250));
    add_boat(v3(92.0f, WATER_Y, 146.0f), -0.15f, argb(255, 30, 60, 140));
    add_boat(v3(-170, WATER_Y, 350), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 500), -0.4f, argb(255, 250, 200, 40));
    hb_breakwater();
    add_lighthouse(v3(214.0f, 0, 236.0f));
    add_umbrella(hb_ground(-60.0f, 92.0f), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(hb_ground(-20.0f, 94.0f), argb(255, 40, 200, 160), 0xffffffff);
    add_umbrella(hb_ground(20.0f, 91.0f), argb(255, 255, 150, 30), 0xffffffff);
    add_arch(v3(-90, 0, 190), 0.25f);
    add_rock(v3(-210, 0, 170), 10.0f, 12.0f, 9, true);
    add_rock(v3(10, 0, 260), 14.0f, 18.0f, 5, true);
    chunk_begin(HF_OP, 500.0f);
    for(int i = 0; i < 9; i++) {
        V3 b = v3(-110.0f + i * 20.0f, WATER_Y - 0.2f, 134.0f);
        uint32_t col = (i & 1) ? argb(255, 250, 250, 250) : argb(255, 230, 50, 40);
        cylinder(h_col, b, 0.45f, 0.35f, 0.9f, 6, col, col, 1, 1);
    }
    chunk_end();

    /* Distant palm silhouettes on the southern hills. */
    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        float x = -420.0f + (float)(h & 1023) / 1023.0f * 840.0f;
        float z = -165.0f - (float)((h >> 10) & 255) / 255.0f * 200.0f;
        far_sprites[far_sprite_n] = hb_ground(x, z);
        far_sprite_h[far_sprite_n] = 9.0f + (float)((h >> 18) & 7);
        far_sprite_n++;
    }
    f8_foam();
}

static void build_harbour(void) {
    gnd_xw = 160.0f;
    gnd_zn = 84.0f;
    gnd_zs = 135.0f;
    f8_build_ground();
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        f8_build_track_chunk(c);
    hb_place_props();
}

/* ------------------------------------------------------------------ */
/* Temple Hill                                                         */

/* The ground: the headland with a jungle hill rising to the south and a
   sea inlet cut into it, then carved around the road so the road always
   sits on top: within a bank's width of a barrier the ground drops just
   below the road, and beyond it may rise again at a 0.6 slope (cuttings
   between the switchbacks) or fall away under an embankment ribbon. */
#define TH_BANK     8.0f       /* embankment width beyond the barrier */
#define TH_SINK     0.4f       /* ground below the road at the bank's foot */
#define TH_FLAMES   12
static float th_road_y[TRACK_SAMPLES];
static V3 th_inlet[5];
static V3 th_flame_p[TH_FLAMES];
static int th_flame_n;

static float seg_dist(V3 p, V3 a, V3 b) {
    float sx = b.x - a.x, sz = b.z - a.z;
    float t = clampf(((p.x - a.x) * sx + (p.z - a.z) * sz) / (sx * sx + sz * sz), 0.0f, 1.0f);
    float dx = p.x - a.x - sx * t, dz = p.z - a.z - sz * t;
    return sqrtf(dx * dx + dz * dz);
}

static float th_terrain(float x, float z) {
    float t = f8_ground(x, z) + 20.0f * smooth01(15.0f, 110.0f, -z);
    /* The inlet: a channel 5 m deep, walls easing up over 10 to 26 m. */
    V3 p = v3(x, 0, z);
    float di = 1e9f;
    for(int i = 0; i + 1 < 5; i++) di = fminf(di, seg_dist(p, th_inlet[i], th_inlet[i + 1]));
    t = fminf(t, -5.0f + 5.4f * smooth01(10.0f, 26.0f, di));
    /* Carve around the road. */
    if(x > -190.0f && x < 190.0f && z > -180.0f && z < 130.0f) {
        for(int i = 0; i < TRACK_SAMPLES; i++) {
            const TrackSample *a = &track.smp[i];
            float dx = x - a->p.x, dz = z - a->p.z;
            float lim = th_road_y[i] - TH_SINK;
            if(lim >= t) continue;
            float e = sqrtf(dx * dx + dz * dz) - (a->wall_out + TH_BANK);
            t = fminf(t, lim + 0.6f * fmaxf(0.0f, e));
        }
    }
    return t;
}

static void th_build_ground(void) {
    float xs[64], zs[48];
    int nx = 0, nz = 0;
    for(float x = -480.0f; x < -150.0f; x += 40.0f) xs[nx++] = x;
    for(float x = -150.0f; x < 150.0f; x += 15.0f) xs[nx++] = x;
    for(float x = 150.0f; x <= 480.0f; x += 40.0f) xs[nx++] = x;
    for(float z = -400.0f; z < -170.0f; z += 46.0f) zs[nz++] = z;
    for(float z = -170.0f; z < 100.0f; z += 15.0f) zs[nz++] = z;
    const float zn[] = {100.0f, 108.0f, 120.0f, 140.0f, 170.0f, 210.0f, 260.0f};
    for(unsigned i = 0; i < sizeof(zn) / sizeof(zn[0]); i++) zs[nz++] = zn[i];
    static float hy[48][64];
    for(int j = 0; j < nz; j++)
        for(int i = 0; i < nx; i++) hy[j][i] = th_terrain(xs[i], zs[j]) + F8_GRASS_Y;
    for(int cz = 0; cz + 1 < nz; cz += 4) {
        /* Chunks 8 cells wide: long strips cost the console less per cell. */
        for(int cx = 0; cx + 1 < nx; cx += 8) {
            chunk_begin(HF_OP, 2000.0f);
            bool far = true;
            for(int j = cz; j < cz + 4 && j + 1 < nz; j++) {
                float zm = (zs[j] + zs[j + 1]) * 0.5f;
                float xm = (xs[cx] + xs[cx + 8 < nx ? cx + 8 : nx - 1]) * 0.5f;
                float hm = 0.25f * (hy[j][cx] + hy[j + 1][cx] + hy[j][cx + 1] + hy[j + 1][cx + 1]);
                bool grassy = zm < 70.0f && hm > -0.1f;
                float uvs = grassy ? 6.0f : 4.0f;
                if(fabsf(xm) < 300.0f && zm > -220.0f && zm < 200.0f) far = false;
                strip_begin(grassy ? h_grass : h_sand);
                for(int i = cx; i <= cx + 8 && i < nx; i++) {
                    for(int k = 1; k >= 0; k--) {
                        float x = xs[i], z = zs[j + k], y = hy[j + k][i];
                        uint32_t col = ground_color(y, 0.0f, grassy);
                        /* Shade slopes a touch so the hill reads. */
                        if(grassy && i + 1 < nx) {
                            float gx = (hy[j + k][i + 1] - y) / (xs[i + 1] - x);
                            col = color_scale(col, clampf(1.0f + gx * 0.5f, 0.8f, 1.08f));
                        }
                        svp(v3(x, y, z), x / uvs, z / uvs, col);
                    }
                }
                strip_end();
            }
            if(cur_chunk) cur_chunk->far = far;
            chunk_end();
        }
    }
}

/* Embankments: grass ribbons from each barrier's foot down to the carved
   ground, left open across the viaduct. */
static void th_banks(const float *sv, int n) {
    for(int side = -1; side <= 1; side += 2) {
        bool open = false;
        for(int k = 0; k < n; k++) {
            float s = sv[k];
            bool skip = in_bridge(s) || (k + 1 < n && in_bridge((s + sv[k + 1]) * 0.5f) &&
                                         in_bridge(sv[k + 1]));
            if(skip) {
                if(open) strip_end();
                open = false;
                continue;
            }
            if(!open) {
                strip_begin(h_grass);
                open = true;
            }
            float w = f8_wall(s);
            V3 a = f8_pt(s, side * (w + 0.25f), 0.0f);
            V3 b = track_point(s, side * (w + TH_BANK));
            b.y = th_terrain(b.x, b.z) + F8_GRASS_Y - 0.15f;
            uint32_t ca = argb(255, 226, 232, 222), cb = argb(255, 246, 252, 240);
            if(side > 0) {
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
            }
            else {
                svp(b, b.x / 6.0f, b.z / 6.0f, cb);
                svp(a, a.x / 6.0f, a.z / 6.0f, ca);
            }
        }
        if(open) strip_end();
    }
}

/* The viaduct: three stone arches on two piers between abutments, with
   spandrel walls under the parapets and a vaulted underside. */
static void th_viaduct(void) {
    const float bs = track.bridge_s, half = track.bridge_half, base = -1.6f;
    const float edges[] = {-half, -10.0f, -7.0f, 7.0f, 10.0f, half};   /* opening, pier, opening, ... */
    chunk_begin(HF_OP, 2000.0f);
    float w = f8_wall(bs) + 0.25f;
    for(int o = 0; o < 3; o++) {
        float a = edges[o * 2], b = edges[o * 2 + 1];
        const int segs = 9;
        float sl[segs + 1], yl[segs + 1];
        for(int k = 0; k <= segs; k++) {
            float t = (float)k / segs;
            sl[k] = track_wrap(bs + lerpf(a, b, t));
            float crown = track_height(sl[k]) - 1.6f;
            yl[k] = base + (crown - base) * sqrtf(sinf(DK_PI * t));
        }
        /* Vault. */
        strip_begin(h_stone);
        for(int k = 0; k <= segs; k++) {
            V3 l = track_point(sl[k], -w), r = track_point(sl[k], w);
            l.y = r.y = yl[k];
            uint32_t c = color_scale(0xffffffff, 0.62f);
            svp(l, 0, sl[k] / 4.0f, c);
            svp(r, 2.0f * w / 4.0f, sl[k] / 4.0f, c);
        }
        strip_end();
        /* Spandrels, both faces of the bridge. */
        for(int side = -1; side <= 1; side += 2) {
            strip_begin(h_stone);
            for(int k = 0; k <= segs; k++) {
                V3 top = track_point(sl[k], side * w), bot = top;
                top.y = track_height(sl[k]);
                bot.y = yl[k];
                V3 r = vsub(track_point(sl[k], 1.0f), track_point(sl[k], 0.0f));
                uint32_t c = shade(argb(255, 255, 246, 230), vscale(r, (float)side));
                svp(top, sl[k] / 4.0f, top.y / 4.0f, c);
                svp(bot, sl[k] / 4.0f, bot.y / 4.0f, color_scale(c, 0.85f));
            }
            strip_end();
        }
    }
    /* Pier sides between the arches. */
    for(int pi = 0; pi < 2; pi++) {
        float a = track_wrap(bs + edges[1 + pi * 2]), b = track_wrap(bs + edges[2 + pi * 2]);
        for(int side = -1; side <= 1; side += 2) {
            V3 ta = track_point(a, side * w), tb = track_point(b, side * w);
            ta.y = track_height(a);
            tb.y = track_height(b);
            V3 ba = ta, bb = tb;
            ba.y = bb.y = base;
            quad(h_stone, ta, tb, ba, bb, 0, ta.y / 4.0f, 0.75f, base / 4.0f, argb(255, 255, 246, 230));
        }
        /* Cutwater: a pointed buttress on each face. */
        float m = track_wrap(bs + (edges[1 + pi * 2] + edges[2 + pi * 2]) * 0.5f);
        for(int side = -1; side <= 1; side += 2) {
            V3 c = track_point(m, side * (w + 1.2f));
            box(h_stone, v3(c.x, (base + track_height(m) - 1.0f) * 0.5f, c.z),
                v3(1.0f, (track_height(m) - 1.0f - base) * 0.5f, 1.2f), atan2f(track_tangent(m).x, track_tangent(m).z),
                argb(255, 236, 226, 206), 0.25f);
        }
    }
    /* Abutment faces closing the embankments at each end. */
    for(int e = -1; e <= 1; e += 2) {
        float se = track_wrap(bs + e * half);
        float h = track_height(se);
        strip_begin(h_stone);
        for(int k = -3; k <= 3; k++) {
            float d = k == 0 ? 0.0f : (float)(k < 0 ? -1 : 1) * (k * k == 1 ? w : k * k == 4 ? w + TH_BANK * 0.5f : w + TH_BANK);
            V3 top = track_point(se, (float)e * d);
            float y = fabsf(d) <= w + 0.01f ? h : lerpf(h, th_terrain(top.x, top.z) + F8_GRASS_Y, (fabsf(d) - w) / TH_BANK);
            top.y = y;
            V3 bot = top;
            bot.y = base;
            svp(top, d / 4.0f, y / 4.0f, argb(255, 236, 226, 206));
            svp(bot, d / 4.0f, base / 4.0f, argb(255, 200, 190, 170));
        }
        strip_end();
    }
    chunk_end();
}

/* A stepped temple with a shrine on top and a stair up its north face. */
static void th_temple(V3 c) {
    chunk_begin(HF_OP, 2000.0f);
    const int tiers = 5;
    float y = c.y - 2.0f, half = 18.0f;
    for(int t = 0; t < tiers; t++) {
        float th = t == 0 ? 5.0f : 3.0f;
        box(h_stone, v3(c.x, y + th * 0.5f, c.z), v3(half, th * 0.5f, half), 0, argb(255, 255, 248, 232), 0.22f);
        y += th;
        half -= 3.0f;
    }
    /* Shrine: walls, a dark doorway and a stone roof slab. */
    box(h_stone, v3(c.x, y + 2.2f, c.z), v3(4.2f, 2.2f, 4.2f), 0, argb(255, 255, 240, 214), 0.3f);
    quad(h_col, v3(c.x - 1.4f, y + 3.2f, c.z + 4.25f), v3(c.x + 1.4f, y + 3.2f, c.z + 4.25f), v3(c.x - 1.4f, y, c.z + 4.25f),
         v3(c.x + 1.4f, y, c.z + 4.25f), 0, 0, 1, 1, argb(255, 40, 30, 24));
    box(h_stone, v3(c.x, y + 4.8f, c.z), v3(5.4f, 0.4f, 5.4f), 0, argb(255, 230, 214, 180), 0.3f);
    th_flame_p[th_flame_n++] = v3(c.x - 4.6f, y + 1.5f, c.z + 5.4f);
    th_flame_p[th_flame_n++] = v3(c.x + 4.6f, y + 1.5f, c.z + 5.4f);
    /* The great stair, a ramp of steps on the north face. */
    float top_y = y;
    strip_begin(h_stone);
    for(int k = 0; k <= 1; k++) {
        float yy = k ? top_y : c.y - 2.0f;
        float zz = k ? c.z + 3.0f + 4.25f : c.z + 18.0f + 6.0f;
        svp(v3(c.x - 3.5f, yy, zz), 0, yy * 1.5f, argb(255, 220, 206, 176));
        svp(v3(c.x + 3.5f, yy, zz), 1.5f, yy * 1.5f, argb(255, 220, 206, 176));
    }
    strip_end();
    chunk_end();
}

/* A carved stone head on a plinth, facing yaw. */
static void th_head(V3 base, float yaw, float size) {
    chunk_begin(HF_OP, 260.0f);
    V3 f = heading_fwd(yaw);
    uint32_t stone = argb(255, 214, 204, 186);
    box(h_stone, vadd(base, v3(0, 0.6f * size, 0)), vscale(v3(1.4f, 0.6f, 1.4f), size), yaw, stone, 0.4f);
    box(h_rock, vadd(base, v3(0, 3.0f * size, 0)), vscale(v3(1.0f, 1.8f, 0.9f), size), yaw, stone, 0.35f);
    /* Brow, nose and mouth stand proud of the face. */
    V3 face = vmad(base, f, 0.95f * size);
    box(h_rock, vadd(face, v3(0, 3.9f * size, 0)), vscale(v3(1.05f, 0.22f, 0.25f), size), yaw, color_scale(stone, 0.9f), 0.5f);
    box(h_rock, vadd(vmad(face, f, 0.2f * size), v3(0, 3.1f * size, 0)), vscale(v3(0.22f, 0.65f, 0.3f), size), yaw, stone, 0.5f);
    box(h_col, vadd(face, v3(0, 2.1f * size, 0)), vscale(v3(0.55f, 0.1f, 0.12f), size), yaw, argb(255, 60, 48, 40), 1);
    for(int e = -1; e <= 1; e += 2)
        box(h_col, vadd(vmad(face, heading_right(yaw), 0.42f * size * e), v3(0, 3.55f * size, 0)),
            vscale(v3(0.2f, 0.14f, 0.1f), size), yaw, argb(255, 40, 34, 30), 1);
    chunk_end();
}

/* An ancient column: plinth, fluted shaft (maybe broken off) and capital. */
static void th_column(V3 base, float height, bool broken) {
    chunk_begin(HF_OP, 220.0f);
    uint32_t col = argb(255, 246, 236, 214);
    box(h_stone, vadd(base, v3(0, 0.35f, 0)), v3(1.2f, 0.35f, 1.2f), 0, col, 0.4f);
    cylinder(h_stone, vadd(base, v3(0, 0.7f, 0)), 0.85f, 0.75f, height, 8, color_scale(col, 0.85f), col, 2.0f, height / 3.0f);
    if(!broken)
        box(h_stone, vadd(base, v3(0, 0.7f + height + 0.3f, 0)), v3(1.15f, 0.3f, 1.15f), 0, col, 0.4f);
    chunk_end();
}

/* A brazier: a stone pedestal with a fire bowl; the flame is drawn live. */
static void th_brazier(V3 base) {
    chunk_begin(HF_OP, 180.0f);
    box(h_stone, vadd(base, v3(0, 0.8f, 0)), v3(0.45f, 0.8f, 0.45f), 0, argb(255, 236, 226, 206), 0.5f);
    cylinder(h_col, vadd(base, v3(0, 1.6f, 0)), 0.35f, 0.8f, 0.5f, 8, argb(255, 90, 70, 50), argb(255, 130, 100, 60), 1, 1);
    chunk_end();
    if(th_flame_n < TH_FLAMES) th_flame_p[th_flame_n++] = vadd(base, v3(0, 2.4f, 0));
}

/* A tumbled ruin wall: a few offset courses of blocks. */
static void th_ruin(V3 c, float yaw, uint32_t seed) {
    chunk_begin(HF_OP, 170.0f);
    V3 ax = heading_right(yaw);
    for(int row = 0; row < 3; row++) {
        for(int i = 0; i < 4 - row; i++) {
            seed = seed * 1103515245u + 12345u;
            if(row > 0 && ((seed >> 16) & 3) == 0) continue;
            V3 p = vmad(c, ax, ((float)i - (3 - row) * 0.5f) * 2.1f + (row & 1) * 0.5f);
            p.y = c.y + 0.5f + row * 1.0f;
            box(h_stone, p, v3(1.0f, 0.5f, 0.7f), yaw + 0.05f * (float)((seed >> 20) % 5 - 2), argb(255, 230, 220, 200), 0.4f);
        }
    }
    chunk_end();
}

static V3 th_ground(float x, float z) {
    return v3(x, th_terrain(x, z) + F8_GRASS_Y, z);
}

static void th_place_props(void) {
    add_gantry();
    add_grandstand(-30.0f, 40.0f, 46.5f);
    th_viaduct();
    th_temple(th_ground(-4.0f, -152.0f));

    /* Braziers at the start line and both ends of the viaduct. */
    const TrackSample *t0 = &track.smp[0];
    for(int side = -1; side <= 1; side += 2) {
        V3 p = vmad(vmad(t0->p, t0->r, side * 14.5f), t0->t, 6.0f);
        th_brazier(th_ground(p.x, p.z));
        for(int e = -1; e <= 1; e += 2) {
            float se = track.bridge_s + e * (track.bridge_half + 4.0f);
            V3 q = track_point(se, side * 14.0f);
            q.y = th_terrain(q.x, q.z) + F8_GRASS_Y;
            q.y = fmaxf(q.y, track_height(se) - 0.2f);
            th_brazier(q);
        }
    }

    /* A colonnade along the summit ridge, some columns fallen short. */
    float ridge = track_seg_s(8);
    for(int i = 0; i < 9; i++) {
        float s = ridge + 6.0f + i * 14.0f;
        V3 p = track_point(s, 17.5f);
        p.y = track_height(s) - 0.2f;
        bool broken = (i % 3) == 1;
        th_column(p, broken ? 2.0f + (float)(i % 2) * 1.5f : 6.5f, broken);
    }
    /* Stone heads watch the hairpins and the summit. */
    for(int hp = 0; hp < 2; hp++) {
        float s = track_seg_s(5 + hp * 2) + 26.7f;          /* hairpin apex */
        float d = hp == 0 ? -20.0f : 20.0f;                  /* outside of the bend */
        V3 p = track_point(s, d);
        V3 toward = vsub(track_point(s, 0), p);
        th_head(th_ground(p.x, p.z), atan2f(toward.x, toward.z), 1.6f);
        th_brazier(th_ground(track_point(s + 9.0f, d).x, track_point(s + 9.0f, d).z));
    }
    {
        float s = ridge + 95.0f;
        V3 p = track_point(s, -19.0f);
        V3 toward = vsub(track_point(s, 0), p);
        V3 g = th_ground(p.x, p.z);
        g.y = fmaxf(g.y, track_height(s) - 0.5f);
        th_head(g, atan2f(toward.x, toward.z), 2.0f);
    }
    /* Tumbled walls on the terraces and in the infield. */
    const float ruins[][3] = {{-40, -2, 0.3f}, {20, 12, -0.5f}, {-100, -40, 1.2f}, {70, -60, 0.2f}, {-60, -120, 0.9f}};
    for(int i = 0; i < 5; i++) {
        V3 p = th_ground(ruins[i][0], ruins[i][1]);
        if(hb_clear(p, 2.0f)) th_ruin(p, ruins[i][2], (uint32_t)(i * 17 + 3));
    }
    for(int i = 0; i < 6; i++) {
        float a = (float)i / 6.0f * DK_TAU;
        V3 p = th_ground(-20.0f + cosf(a) * 26.0f, 8.0f + sinf(a) * 16.0f);
        if(hb_clear(p, 2.0f)) th_column(p, (i & 1) ? 3.0f : 5.5f, i & 1);
    }

    /* Jungle palms, kept behind the barriers and off the inlet. */
    uint32_t r = 0x7e3au;
    for(int tries = 0; tries < 500 && palm_n < 42; tries++) {
        r = r * 1664525u + 1013904223u;
        float x = -160.0f + (float)((r >> 8) & 1023) / 1023.0f * 320.0f;
        float z = -175.0f + (float)((r >> 18) & 1023) / 1023.0f * 268.0f;
        V3 p = th_ground(x, z);
        if(p.y < WATER_Y + 0.2f || !hb_clear(p, 3.0f)) continue;
        if(z > 28.0f && z < 50.0f && x > -36.0f && x < 46.0f) continue;     /* grandstand */
        if(fabsf(x + 4.0f) < 28.0f && z < -128.0f) continue;                /* temple */
        bool crowded = false;
        for(int i = 0; i < palm_n; i++) {
            V3 d = vsub(palms[i].base, p);
            if(d.x * d.x + d.z * d.z < 11.0f * 11.0f) crowded = true;
        }
        if(crowded) continue;
        add_palm(p, 9.0f + (float)((r >> 4) % 4) * 1.3f, (float)(r % 628) * 0.01f, 0.2f, r >> 12);
    }

    /* The beach and the sea. */
    add_umbrella(th_ground(-40.0f, 90.0f), argb(255, 230, 40, 40), 0xffffffff);
    add_umbrella(th_ground(10.0f, 92.0f), argb(255, 40, 200, 160), 0xffffffff);
    add_umbrella(th_ground(60.0f, 89.0f), argb(255, 255, 150, 30), 0xffffffff);
    add_rock(v3(-140, 0, 200), 12.0f, 14.0f, 9, true);
    add_rock(v3(60, 0, 250), 15.0f, 20.0f, 5, true);
    add_rock(v3(250, 0, 140), 10.0f, 10.0f, 17, true);
    for(int i = 0; i < 4; i++) {
        V3 p = vlerp(th_inlet[1], th_inlet[3], (float)i / 3.0f);
        add_rock(vadd(p, v3((i & 1) ? 24.0f : -24.0f, 0, (i & 1) ? -6.0f : 6.0f)), 4.0f, 2.5f, (uint32_t)(i * 7 + 1), false);
    }
    add_boat(v3(-60, WATER_Y, 300), 0.8f, argb(255, 30, 60, 140));
    add_boat(v3(160, WATER_Y, 420), -0.4f, argb(255, 200, 40, 40));

    for(int i = 0; i < MAX_SPRITES; i++) {
        uint32_t h = (uint32_t)i * 2654435761u;
        float x = -420.0f + (float)(h & 1023) / 1023.0f * 840.0f;
        float z = -190.0f - (float)((h >> 10) & 255) / 255.0f * 180.0f;
        far_sprites[far_sprite_n] = th_ground(x, z);
        far_sprite_h[far_sprite_n] = 9.0f + (float)((h >> 18) & 7);
        far_sprite_n++;
    }
    f8_foam();
}

static void build_temple(void) {
    gnd_xw = 170.0f;
    gnd_zn = 84.0f;
    gnd_zs = 125.0f;
    th_flame_n = 0;
    for(int i = 0; i < TRACK_SAMPLES; i++) th_road_y[i] = track_height(track.smp[i].s);
    /* The inlet runs east under the viaduct, then north out to the sea. */
    V3 b = track_point(track.bridge_s, 0);
    th_inlet[0] = v3(b.x - 34.0f, 0, b.z);
    th_inlet[1] = v3(b.x + 10.0f, 0, b.z);
    th_inlet[2] = v3(b.x + 44.0f, 0, b.z + 6.0f);
    th_inlet[3] = v3(b.x + 86.0f, 0, b.z + 58.0f);
    th_inlet[4] = v3(b.x + 92.0f, 0, b.z + 190.0f);
    th_build_ground();
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        f8_build_track_chunk(c);
    th_place_props();
}

static void th_draw_flames(void) {
    if(track.id != TRACK_TEMPLE || !th_flame_n) return;
    r_hdr(h_flame);
    for(int i = 0; i < th_flame_n; i++) {
        V3 p = th_flame_p[i];
        V3 d = vsub(p, r_cam.pos);
        if(vdot(d, d) > 260.0f * 260.0f || !r_sphere_visible(p, 2.0f)) continue;
        float f = sinf(world_time * 13.0f + i * 2.1f) * 0.5f + sinf(world_time * 7.3f + i) * 0.5f;
        float h = 1.9f + 0.3f * f, w = 1.25f + 0.12f * f;
        r_billboard_y(vsub(p, v3(0, 0.75f, 0)), w, h, 0, 0, 1, 1, argb(255, 255, 200, 120));
    }
    r_hdr(h_glow_add);
    for(int i = 0; i < th_flame_n; i++) {
        V3 p = th_flame_p[i];
        V3 d = vsub(p, r_cam.pos);
        if(vdot(d, d) > 260.0f * 260.0f || !r_sphere_visible(p, 3.0f)) continue;
        float a = 0.28f + 0.06f * sinf(world_time * 11.0f + i * 1.7f);
        r_billboard(p, 4.5f, 4.5f, 0, 0, 0, 1, 1, color_alpha(argb(255, 255, 150, 60), a));
    }
}

static void build_figure8(void) {
    gnd_xw = 200.0f;
    gnd_zn = 84.0f;
    gnd_zs = 96.0f;
    f8_build_ground();
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        f8_build_track_chunk(c);
    f8_place_props();
}

static void build_beach(void) {
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        build_track_chunk(c);
    /* Waterline distance beyond the outer wall, for the foam strip. */
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        const TrackSample *t = &track.smp[i];
        float db = 0;
        for(int k = 0; k < 80; k++) {
            V3 p = vmad(t->p, t->r, t->wall_out + db);
            if(terrain_height(p, db, t->shore) < WATER_Y) break;
            db += 0.25f;
        }
        shore_db[i] = db;
    }
    for(int i = 0; i <= TRACK_SAMPLES && foam_count < MAX_FOAM; i += 2) {
        const TrackSample *t = &track.smp[i % TRACK_SAMPLES];
        foam_p[foam_count] = vmad(t->p, t->r, t->wall_out + shore_db[i % TRACK_SAMPLES]);
        foam_n[foam_count] = t->r;
        foam_u[foam_count] = t->s;
        foam_on[foam_count] = t->p.z > 8.0f;
        foam_count++;
    }
    place_props();
}

/* ------------------------------------------------------------------ */
/* Neon Skyway                                                      */

/* A neon road through a cyberpunk city at night, high above a sea of
   cloud: a dark deck under a glowing grid with running lights down its
   edges (cyan on the left, magenta on the right), glass rails carrying neon
   tubes round the turns, and no rails at all on the straights. Neon gates
   frame the line, the waves, the plunge and the infield, and dark towers
   rise out of the clouds all round, lit windows, neon crowns and signs. */
#define SK_RAIL     10.2f      /* glass rails on the closed sides */
#define SK_EDGE     9.6f       /* road edge (deck to 8 m, then the light strip) */
#define SK_DEPTH    0.7f       /* deck thickness */
#define SK_RAIL_H   0.75f      /* neon tube height */
#define SK_SPARKLES 40
#define SK_TOWERS   30
#define SK_BEAMS    5
#define CLOUD_Y     (-60.0f)
static float sk_vscale;
static V3 sk_sparkle[SK_SPARKLES];
static V3 sk_beam[SK_BEAMS];     /* searchlights on the tallest towers' roofs */
static int sk_beam_n;

static const uint32_t neon_cyan = 0xff3cdcff, neon_violet = 0xff9646ff, neon_magenta = 0xffff32c8,
                      neon_pink = 0xffff78e6, neon_blue = 0xff4678ff;

/* Across the road: cyan on the left, through violet, to magenta on the
   right (the karts' underglow picks it up). */
uint32_t world_lane_color(float d) {
    float t = clampf((d + 9.6f) / 19.2f, 0.0f, 1.0f);
    return t < 0.5f ? color_lerp(neon_cyan, neon_violet, t * 2.0f) : color_lerp(neon_violet, neon_magenta, t * 2.0f - 1.0f);
}

/* Rail tubes: cyan shimmering to blue on the left, magenta to pink on the
   right, slowly along the lap. */
static uint32_t rail_color(float s, int side) {
    float w = 0.5f + 0.5f * sinf(s * (DK_TAU / 90.0f));
    return side < 0 ? color_lerp(neon_cyan, neon_blue, w * 0.6f) : color_lerp(neon_magenta, neon_pink, w * 0.7f);
}

static bool sk_closed(float s, int side) {
    int i = (int)(track_wrap(s) / track.ds) % TRACK_SAMPLES;
    return !(track.smp[i].open & (side < 0 ? OPEN_LEFT : OPEN_RIGHT));
}

/* One rail run's piece: a ribbon at lateral side * d between heights y0
   and y1 above the road, or flat at height y0 between d and d1 when flat.
   Neon pieces take the rail colour with c0/c1's alpha. */
static void sk_rail_ribbon(const float *sv, int k0, int k1, int side, float d, float d1, float y0, float y1,
                           const pvr_poly_hdr_t *h, uint32_t c0, uint32_t c1, bool neon, bool flat) {
    strip_begin(h);
    for(int k = k0; k <= k1; k++) {
        float s = sv[k];
        uint32_t a = c0, b = c1;
        if(neon) {
            uint32_t n = rail_color(s, side);
            a = color_alpha(n, (float)(c0 >> 24) / 255.0f);
            b = color_alpha(n, (float)(c1 >> 24) / 255.0f);
        }
        V3 p = f8_pt(s, side * d, y0), q = flat ? f8_pt(s, side * d1, y0) : f8_pt(s, side * d, y1);
        /* Wind toward the road on both sides. */
        if(side < 0) {
            svp(q, 0.5f, flat ? 0.0f : 1.0f, b);
            svp(p, 0.5f, 0.0f, a);
        }
        else {
            svp(p, 0.5f, 0.0f, a);
            svp(q, 0.5f, flat ? 0.0f : 1.0f, b);
        }
    }
    strip_end();
}

static void sk_build_track_chunk(int c) {
    float sv[MAX_SVALS];
    int n = f8_chunk_svals(c, sv);
    chunk_begin(HF_OP, 2000.0f);
    /* The deck: grid and edge lights in one texture, the glowing side
       faces and the dark underside. */
    f8_ribbon(sv, n, &sk_road_hdr, -SK_EDGE, SK_EDGE, 0.0f, 0.0f, 0, 1.0f, sk_vscale, false, 0xffffffff, 0xffffffff);
    f8_ribbon(sv, n, h_col, -SK_EDGE, -SK_EDGE, -SK_DEPTH, 0.0f, 0, 1, 1, true, argb(255, 10, 30, 70),
              argb(255, 60, 200, 255));
    f8_ribbon(sv, n, h_col, SK_EDGE, SK_EDGE, 0.0f, -SK_DEPTH, 0, 1, 1, true, argb(255, 255, 50, 200),
              argb(255, 70, 10, 70));
    /* Glass rails on the closed sides: a dark panel, a ledge out to it from
       the road edge and a neon tube along the top. */
    int runs[MAX_SVALS][3], nruns = 0;
    for(int side = -1; side <= 1; side += 2) {
        int k = 0;
        while(k < n) {
            if(!sk_closed(sv[k], side) || track_in_gap(sv[k])) {
                k++;
                continue;
            }
            int k0 = k;
            while(k + 1 < n && sk_closed(sv[k + 1], side) && !track_in_gap(sv[k + 1])) k++;
            if(k > k0) {
                sk_rail_ribbon(sv, k0, k, side, SK_EDGE, SK_RAIL, 0.0f, 0.0f, h_col, argb(255, 16, 16, 40),
                               argb(255, 16, 16, 40), false, true);
                sk_rail_ribbon(sv, k0, k, side, SK_RAIL, 0, -SK_DEPTH, SK_RAIL_H, h_col, argb(255, 14, 14, 40),
                               argb(255, 40, 50, 110), false, false);
                sk_rail_ribbon(sv, k0, k, side, SK_RAIL - 0.12f, SK_RAIL + 0.12f, SK_RAIL_H, 0.0f, h_col, 0xffffffff,
                               0xffffffff, true, true);
                sk_rail_ribbon(sv, k0, k, side, SK_RAIL - 0.12f, 0, SK_RAIL_H - 0.16f, SK_RAIL_H, h_col, 0xffffffff,
                               0xffffffff, true, false);
                runs[nruns][0] = k0;
                runs[nruns][1] = k;
                runs[nruns][2] = side;
                nruns++;
            }
            k++;
        }
    }
    chunk_end();
    /* The deck's dark underside, only seen from below (left out of a split
       screen, like the glows). */
    chunk_begin(HF_OP, 2000.0f);
    if(cur_chunk) cur_chunk->far = 1;
    f8_ribbon(sv, n, h_col, SK_EDGE, -SK_EDGE, -SK_DEPTH, -SK_DEPTH, 0, 1, 1, true, argb(255, 14, 12, 34),
              argb(255, 14, 12, 34));
    chunk_end();
    /* The tubes' glow (additive). */
    if(nruns) {
        chunk_begin(HF_TR, 700.0f);
        if(cur_chunk) cur_chunk->far = 1;
        for(int r = 0; r < nruns; r++)
            sk_rail_ribbon(sv, runs[r][0], runs[r][1], runs[r][2], SK_RAIL, 0, SK_RAIL_H - 0.55f, SK_RAIL_H + 0.65f,
                           h_dash_glow, argb(150, 255, 255, 255), argb(150, 255, 255, 255), true, false);
        chunk_end();
    }
}

/* An octagonal neon gate the road passes through: a magenta frame round
   a cyan one, with a soft additive halo. */
static void sk_gate(float s) {
    V3 c = track_point(s, 0.0f), r = vsub(track_point(s, 1.0f), c);
    c.y = track_road_y(s, 0.0f) + 2.0f;
    const int segs = 8;
    const float r0 = 12.6f;
    /* Corners on the diagonals, so the gate has flat sides and top. */
    const float off = DK_PI / 8.0f;
    static const struct { float ri, ro; uint32_t col; } bands[2] = {
        {12.6f, 13.1f, 0xff3cdcff}, {13.5f, 14.1f, 0xffff32c8}};
    chunk_begin(HF_OP, 1100.0f);
    for(int b = 0; b < 2; b++) {
        strip_begin(h_col);
        for(int i = 0; i <= segs; i++) {
            float a = (float)i / segs * DK_TAU + off;
            V3 dir = vadd(vscale(r, cosf(a)), v3(0, sinf(a), 0));
            svp(vmad(c, dir, bands[b].ro), 0, 0, bands[b].col);
            svp(vmad(c, dir, bands[b].ri), 0, 0, color_scale(bands[b].col, 0.8f));
        }
        strip_end();
    }
    chunk_end();
    chunk_begin(HF_TR, 1100.0f);
    if(cur_chunk) cur_chunk->far = 1;
    strip_begin(h_dash_glow);
    for(int i = 0; i <= segs; i++) {
        float a = (float)i / segs * DK_TAU + off;
        V3 dir = vadd(vscale(r, cosf(a)), v3(0, sinf(a), 0));
        uint32_t col = argb(120, 170, 110, 255);
        svp(vmad(c, dir, 14.1f + 2.6f), 0.5f, 0.0f, col);
        svp(vmad(c, dir, r0 - 2.6f), 0.5f, 1.0f, col);
    }
    strip_end();
    chunk_end();
}

/* The jump's lip and the deck's front edge: glowing faces across the road,
   cyan to magenta. */
static void sk_jump_face(float s, float dir) {
    float h = track_height(s - dir * 0.02f);
    const int stripes = 8;
    for(int i = 0; i < stripes; i++) {
        float d0 = -SK_EDGE + 2.0f * SK_EDGE * (float)i / stripes, d1 = -SK_EDGE + 2.0f * SK_EDGE * (float)(i + 1) / stripes;
        V3 a = track_point(s, d0 * dir), b = track_point(s, d1 * dir);
        a.y = b.y = h;
        V3 ab = a, bb = b;
        ab.y = bb.y = h - SK_DEPTH - 0.4f;
        uint32_t col = (i & 1) ? argb(255, 14, 12, 34) : world_lane_color((d0 + d1) * 0.5f * dir);
        strip_begin(h_col);
        svp(a, 0, 0, col); svp(b, 0, 0, col); svp(ab, 0, 0, color_scale(col, 0.6f)); svp(bb, 0, 0, color_scale(col, 0.6f));
        strip_end();
    }
}

/* Horizontal distance from p to the nearest stretch of road. */
static float sk_road_dist(V3 p) {
    float best = 1e9f;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        float dx = p.x - track.smp[i].p.x, dz = p.z - track.smp[i].p.z;
        best = fminf(best, dx * dx + dz * dz);
    }
    return sqrtf(best);
}

/* A city tower rising out of the clouds: dark glass with lit windows, a
   neon crown round the roof, a neon strip down one corner and a sign on
   the face toward the road. */
static void sk_tower(V3 base, float hx, float hz, float top, float yaw, uint32_t neon, int sign, V3 face_to) {
    chunk_begin(HF_OP, 2000.0f);
    float bottom = CLOUD_Y - 8.0f, hh = (top - bottom) * 0.5f;
    box(h_tower, v3(base.x, bottom + hh, base.z), v3(hx, hh, hz), yaw, 0xffffffff, 1.0f / 24.0f);
    V3 ax = heading_right(yaw), az = heading_fwd(yaw);
    /* Crown: a band of neon just proud of each face, under the roofline. */
    V3 corner[4] = {vadd(vscale(ax, -hx - 0.2f), vscale(az, -hz - 0.2f)), vadd(vscale(ax, hx + 0.2f), vscale(az, -hz - 0.2f)),
                    vadd(vscale(ax, hx + 0.2f), vscale(az, hz + 0.2f)), vadd(vscale(ax, -hx - 0.2f), vscale(az, hz + 0.2f))};
    strip_begin(h_col);
    for(int i = 0; i <= 4; i++) {
        V3 p = vadd(base, corner[i % 4]);
        svp(v3(p.x, top - 0.6f, p.z), 0, 0, neon);
        svp(v3(p.x, top - 1.6f, p.z), 0, 0, color_scale(neon, 0.7f));
    }
    strip_end();
    /* A neon strip down one corner. */
    V3 k = vadd(base, corner[(int)(fabsf(yaw) * 7.0f) & 3]);
    V3 kn = vnorm(v3(k.x - base.x, 0, k.z - base.z));
    V3 kt = v3(kn.z, 0, -kn.x);
    strip_begin(h_col);
    svp(vadd(vmad(k, kt, -0.5f), v3(0, top - 2.0f, 0)), 0, 0, neon);
    svp(vadd(vmad(k, kt, 0.5f), v3(0, top - 2.0f, 0)), 0, 0, neon);
    svp(vadd(vmad(k, kt, -0.5f), v3(0, top - 45.0f, 0)), 0, 0, color_scale(neon, 0.5f));
    svp(vadd(vmad(k, kt, 0.5f), v3(0, top - 45.0f, 0)), 0, 0, color_scale(neon, 0.5f));
    strip_end();
    /* The sign, on whichever face looks most toward the road. */
    if(sign >= 0) {
        V3 to = vnorm(v3(face_to.x - base.x, 0, face_to.z - base.z));
        V3 n = az;
        float hw = hx;
        if(fabsf(vdot(to, ax)) > fabsf(vdot(to, az))) {
            n = vdot(to, ax) > 0 ? ax : vscale(ax, -1);
            hw = hz;
        }
        else if(vdot(to, az) < 0) n = vscale(az, -1);
        float depth = (n.x == ax.x && n.z == ax.z) || (n.x == -ax.x && n.z == -ax.z) ? hx : hz;
        V3 c = vmad(base, n, depth + 0.35f);
        V3 right = v3(-n.z, 0, n.x);   /* viewer's right, looking at the face */
        float w = fminf(hw * 0.85f, 14.0f), h = w * 0.25f, y = top - 6.0f - h;
        float v0 = (float)sign * 0.25f, v1 = v0 + 0.25f;
        strip_begin(h_signs);
        svp(vadd(vmad(c, right, -w), v3(0, y + h, 0)), 0, v0, 0xffffffff);
        svp(vadd(vmad(c, right, w), v3(0, y + h, 0)), 1, v0, 0xffffffff);
        svp(vadd(vmad(c, right, -w), v3(0, y - h, 0)), 0, v1, 0xffffffff);
        svp(vadd(vmad(c, right, w), v3(0, y - h, 0)), 1, v1, 0xffffffff);
        strip_end();
    }
    chunk_end();
}

static void sk_place_city(void) {
    static const uint32_t crowns[4] = {0xff3cdcff, 0xffff32c8, 0xff9646ff, 0xffff78e6};
    V3 placed[SK_TOWERS];
    float size[SK_TOWERS], tops[SK_TOWERS];
    int n = 0;
    uint32_t r = 0xc17eu;
    for(int tries = 0; tries < 600 && n < SK_TOWERS; tries++) {
        r = r * 1664525u + 1013904223u;
        float x = -250.0f + (float)((r >> 8) & 1023) / 1023.0f * 500.0f;
        float z = -220.0f + (float)((r >> 18) & 1023) / 1023.0f * 440.0f;
        float hs = 8.0f + (float)((r >> 4) & 15) * 0.6f;
        V3 p = v3(x, 0, z);
        float road = sk_road_dist(p);
        if(road < hs * 1.45f + 22.0f) continue;
        bool crowded = false;
        for(int i = 0; i < n; i++) {
            float dx = placed[i].x - x, dz = placed[i].z - z;
            if(dx * dx + dz * dz < (hs + size[i] + 14.0f) * (hs + size[i] + 14.0f)) crowded = true;
        }
        if(crowded) continue;
        /* Towers near the road stay low enough to see past; further out the
           skyline climbs. */
        float top = 10.0f + clampf((road - 30.0f) / 90.0f, 0.0f, 1.0f) * 70.0f + (float)((r >> 12) & 31);
        placed[n] = p;
        size[n] = hs;
        tops[n] = top;
        r = r * 1664525u + 1013904223u;
        float yaw = (float)((r >> 8) & 255) / 255.0f * 1.5f;
        TrackPos tp;
        track_locate(p, -1, &tp);
        sk_tower(p, hs, hs * (0.7f + 0.3f * (float)((r >> 16) & 3) / 3.0f), top, yaw, crowns[n & 3],
                 road < 110.0f ? (n % 4) : -1, tp.center);
        n++;
    }
    /* Searchlights from the tallest. */
    sk_beam_n = 0;
    for(int k = 0; k < SK_BEAMS; k++) {
        int best = -1;
        for(int i = 0; i < n; i++) {
            bool used = false;
            for(int j = 0; j < sk_beam_n; j++)
                if(sk_beam[j].x == placed[i].x && sk_beam[j].z == placed[i].z) used = true;
            if(!used && (best < 0 || tops[i] > tops[best])) best = i;
        }
        if(best < 0) break;
        sk_beam[sk_beam_n++] = v3(placed[best].x, tops[best], placed[best].z);
    }
}

static void sk_place_props(void) {
    /* Start line. */
    chunk_begin(HF_OP, 600.0f);
    strip_begin(h_checker);
    for(int k = 0; k <= 1; k++) {
        float s = (k ? 1.2f : -1.2f);
        V3 a = f8_pt(s, -8.0f, 0.03f), b = f8_pt(s, 8.0f, 0.03f);
        svp(a, 0, s / 1.2f, 0xffffffff);
        svp(b, 8, s / 1.2f, 0xffffffff);
    }
    strip_end();
    chunk_end();
    chunk_begin(HF_OP, 900.0f);
    sk_jump_face(track_wrap(track.jump.cross + track.jump.lip), 1.0f);
    sk_jump_face(track_wrap(track.jump.cross + track.jump.land), -1.0f);
    chunk_end();

    /* Gates: the line, the main straight, the waves' troughs, over the
       crest into the plunge and the bottom of the infield dip. */
    float waves = track_seg_s(2) + 18.0f;
    float gates[] = {0.0f, 70.0f, waves + 26.0f, waves + 52.0f, track_seg_s(4) + 47.0f, track_seg_s(6) + 45.0f};
    for(unsigned i = 0; i < sizeof(gates) / sizeof(gates[0]); i++) sk_gate(track_wrap(gates[i]));

    sk_place_city();

    /* Sparkles hanging in the sky around the course, clear of the road. */
    uint32_t r = 0x5a17u;
    for(int i = 0; i < SK_SPARKLES; i++) {
        V3 p;
        for(int tries = 0; tries < 20; tries++) {
            r = r * 1664525u + 1013904223u;
            float s = (float)((r >> 8) & 1023) / 1023.0f * track.length;
            float side = (r & 1) ? 1.0f : -1.0f;
            float d = side * (24.0f + (float)((r >> 18) & 255) / 255.0f * 70.0f);
            p = track_point(s, d);
            p.y = track_height(s) - 12.0f + (float)((r >> 4) & 63) * 0.6f;
            if(sk_road_dist(p) > 22.0f) break;
        }
        sk_sparkle[i] = p;
    }
}

static void build_skyway(void) {
    sk_vscale = track.length / roundf(track.length / 8.0f);
    for(int c = 0; c < TRACK_SAMPLES / 8; c++)
        sk_build_track_chunk(c);
    sk_place_props();
}

/* ------------------------------------------------------------------ */

void world_init(void) {
    world_sun_dir = vnorm(v3(0.30f, 0.55f, 0.78f));
    pool = memalign(32, sizeof(RV) * MAX_POOL);
    if(!pool) {
        printf("DreamKarts world: out of memory for scenery\n");
        built_ok = false;
        return;
    }
    h_road = r_header(TEX_ROAD, HF_OP);
    h_board = r_header(TEX_BOARDWALK, HF_OP);
    h_curb = r_header(TEX_CURB, HF_OP);
    h_sand = r_header(TEX_SAND, HF_OP);
    h_grass = r_header(TEX_GRASS, HF_OP);
    h_barrier = r_header(TEX_BARRIER, HF_OP);
    h_wood = r_header(TEX_WOOD, HF_OP);
    h_rock = r_header(TEX_ROCK, HF_OP);
    h_bark = r_header(TEX_BARK, HF_OP);
    h_checker = r_header(TEX_CHECKER, HF_OP);
    h_thatch = r_header(TEX_THATCH, HF_OP);
    h_crowd = r_header(TEX_CROWD, HF_OP);
    h_banner = r_header(TEX_BANNER, HF_OP);
    h_col = r_header(TEX_NONE, HF_OP);
    h_water = r_header(TEX_WATER, HF_OP);
    h_dash = r_header(TEX_DASH, HF_OP);
    h_frond = r_header(TEX_FROND, HF_PT);
    h_palm_far = r_header(TEX_PALM_FAR, HF_PT);
    h_sky = r_header(TEX_NONE, HF_OP | HF_NOFOG);
    h_cloud = r_header(TEX_CLOUD, HF_TR | HF_NOFOG);
    h_mount = r_header(TEX_MOUNTAINS, HF_TR | HF_NOFOG);
    h_foam = r_header(TEX_FOAM, HF_TR);
    h_glow_add = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    h_flare = r_header(TEX_FLARE, HF_TR | HF_ADD | HF_NOFOG | HF_NOZTEST);
    h_dash_glow = r_header(TEX_GLOW, HF_TR | HF_ADD);
    h_stone = r_header(TEX_STONE, HF_OP);
    h_flame = r_header(TEX_FLAME, HF_TR | HF_ADD);
    for(int i = 0; i < SKYROAD_PHASES; i++) h_skyroad[i] = r_header(TEX_SKYROAD_0 + i, HF_OP);
    sk_road_hdr = *h_skyroad[0];
    h_nightsky = r_header(TEX_NIGHTSKY, HF_OP | HF_NOFOG);
    h_skyline = r_header(TEX_SKYLINE, HF_TR | HF_NOFOG);
    h_moon = r_header(TEX_MOON, HF_TR | HF_NOFOG | HF_CLAMP);
    h_cloudsea = r_header(TEX_CLOUDSEA, HF_OP);
    h_tower = r_header(TEX_TOWER, HF_OP);
    h_signs = r_header(TEX_SIGNS, HF_OP);
    h_star_add = r_header(TEX_STAR, HF_TR | HF_ADD);

    static const float elev[] = {-0.25f, 0.0f, 0.10f, 0.28f, 0.62f, 1.05f, 1.5708f};
    for(int e = 0; e < 7; e++)
        for(int i = 0; i <= 12; i++) {
            float a = (float)i / 12 * DK_TAU;
            sky_dir[e][i] = v3(sinf(a) * cosf(elev[e]), sinf(elev[e]), cosf(a) * cosf(elev[e]));
        }
    for(int i = 0; i <= 24; i++) {
        float a = (float)i / 24 * DK_TAU;
        ring_dir[i] = v3(sinf(a), -cosf(a), cosf(a));
    }
}

void world_build(void) {
    if(!pool) return;
    pool_n = strip_n = chunk_n = palm_n = far_sprite_n = foam_count = 0;
    cur_chunk = NULL;
    built_ok = true;
    world_night = track.id == TRACK_SKYWAY;
    /* The sun, or at night the moon (north-west, low over the waves). */
    world_sun_dir = world_night ? vnorm(v3(-0.50f, 0.33f, 0.80f)) : vnorm(v3(0.30f, 0.55f, 0.78f));
    /* Moonlight on the karts: dimmer and bluer (applied as they are painted). */
    world_light[0] = world_night ? 176 : 256;
    world_light[1] = world_night ? 168 : 256;
    world_light[2] = world_night ? 240 : 256;
    if(track.id == TRACK_FIGURE8) build_figure8();
    else if(track.id == TRACK_HARBOUR) build_harbour();
    else if(track.id == TRACK_TEMPLE) build_temple();
    else if(track.id == TRACK_SKYWAY) build_skyway();
    else build_beach();
    /* Release builds log only the first build: under dcload-ip each printf
       stalls the console with IRQs off. */
#ifdef DK_AUTOTEST
    bool log = true;
#else
    static bool logged;
    bool log = !logged;
    logged = true;
#endif
    if(log)
        printf("DreamKarts world: %s, %d chunks, %d strips, %d vertices (%lu KiB), %d palms%s\n", track.name,
           chunk_n, strip_n, pool_n, (unsigned long)(pool_n * sizeof(RV) / 1024), palm_n,
           built_ok ? "" : " [POOL OVERFLOW]");
}

void world_update(float dt) {
    world_time += dt;
    /* The road's lights run forward a repeat (about 8 m) every 0.55 s. */
    if(world_night) {
        int phase = (int)(world_time * (SKYROAD_PHASES / 0.55f)) % SKYROAD_PHASES;
        sk_road_hdr = *h_skyroad[phase];
    }
}

static void draw_chunks(int list) {
    V3 cam = r_cam.pos;
    for(int i = 0; i < chunk_n; i++) {
        const Chunk *c = &chunks[i];
        if(c->list != list) continue;
        if(world_split && c->far) continue;
        V3 d = vsub(c->c, cam);
        /* Split screen: props switch to sprites/vanish sooner (palms at
           ~75 m instead of 135 m) and nothing is drawn past the fog. */
        float lod = c->lod_far;
        if(world_split) lod = lod < 250.0f ? lod * 0.45f : lod < 1000.0f ? lod * 0.55f : fminf(lod, 400.0f);
        float lim = lod + c->r;
        if(vdot(d, d) > lim * lim) continue;
        if(!r_sphere_visible(c->c, c->r)) continue;
        for(int s = 0; s < c->nstrips; s++) {
            const Strip *st = &strips[c->first_strip + s];
            r_hdr(st->hdr);
            r_strip(pool + st->first, st->count);
        }
    }
}

/* Night sky: the dome textured with stars, six repeats round, from the
   zenith (v = 0) to the horizon glow and below (v = 1, the fog colour).
   The texture gives the lowest 45 degrees 70% of its rows (see
   tools/build_textures.py), so v at each ring's elevation follows that. */
static void draw_night_sky(void) {
    static const float v_at[7] = {0.995f, 0.995f, 0.911f, 0.750f, 0.448f, 0.199f, 0.005f};
    const float R = 1500.0f;
    V3 c = r_cam.pos;
    r_hdr(h_nightsky);
    for(int e = 0; e < 6; e++) {
        RV v[2 * 13];
        int n = 0;
        for(int i = 0; i <= 12; i++) {
            for(int k = 1; k >= 0; k--) {
                V3 d = sky_dir[e + k][i];
                v[n++] = (RV){c.x + d.x * R, c.y + d.y * R, c.z + d.z * R, (float)i * 0.5f, v_at[e + k], 0xffffffff};
            }
        }
        r_strip(v, n);
    }
}

/* Sky dome around the camera: vertex-coloured gradient, no fog. */
static void draw_sky(void) {
    if(world_night) {
        draw_night_sky();
        return;
    }
    static const uint32_t cols[] = {
        0xffbde6f8, 0xffbde6f8, 0xff98d2f6, 0xff6cb6f0, 0xff3f90e4, 0xff2a74d8, 0xff2266d0
    };
    const int az = 12;
    const float R = 1500.0f;
    V3 c = r_cam.pos;
    r_hdr(h_sky);
    for(int e = 0; e < 6; e++) {
        RV v[2 * 13];
        int n = 0;
        for(int i = 0; i <= az; i++) {
            for(int k = 1; k >= 0; k--) {
                V3 d = sky_dir[e + k][i];
                v[n++] = (RV){c.x + d.x * R, c.y + d.y * R, c.z + d.z * R, 0, 0, cols[e + k]};
            }
        }
        r_strip(v, n);
    }
}

static uint32_t sea_color(float x, float z) {
    float dist = sqrtf(x * x * 0.36f + z * z);
    float t = clampf((dist - 70.0f) / 260.0f, 0, 1);
    return color_lerp(argb(255, 236, 255, 252), argb(255, 96, 150, 222), t);
}

/* The cloud sea far below the sky road: a camera-centred grid drifting
   slowly, lit purple with patches of city glow coming up through it, then
   wide bands out to the horizon. */
static uint32_t cloud_color(float x, float z) {
    float glow = sinf(x * 0.0041f + 0.7f) * sinf(z * 0.0053f - 0.4f);
    glow = clampf(glow * 1.6f - 0.3f, 0.0f, 1.0f);
    return color_lerp(argb(255, 64, 70, 170), argb(255, 220, 80, 210), glow * 0.55f);
}

static void draw_clouds_below(void) {
    const float cell = world_split ? 90.0f : 64.0f;
    const int n = world_split ? 10 : 14;
    float cx = floorf(r_cam.pos.x / cell) * cell, cz = floorf(r_cam.pos.z / cell) * cell;
    float x0 = cx - n * cell * 0.5f, z0 = cz - n * cell * 0.5f;
    float su = world_time * 0.006f, sv2 = world_time * 0.004f;
    const float uvs = 1.0f / 110.0f;
    r_hdr(h_cloudsea);
    RV row[2 * (14 + 1)];
    for(int j = 0; j < n; j++) {
        float za = z0 + j * cell, zb = za + cell;
        V3 mid = v3(cx, CLOUD_Y, (za + zb) * 0.5f);
        if(!r_sphere_visible(mid, n * cell * 0.75f)) continue;
        int k = 0;
        for(int i = 0; i <= n; i++) {
            float x = x0 + i * cell;
            row[k++] = (RV){x, CLOUD_Y, zb, x * uvs + su, zb * uvs + sv2, cloud_color(x, zb)};
            row[k++] = (RV){x, CLOUD_Y, za, x * uvs + su, za * uvs + sv2, cloud_color(x, za)};
        }
        r_strip(row, k);
    }
    /* Out to the horizon on all four sides of the grid. */
    float gx0 = x0, gx1 = x0 + n * cell, gz0 = z0, gz1 = z0 + n * cell;
    const float E = 4000.0f;
    uint32_t far = argb(255, 64, 70, 170);
    const float xs[4][4] = {{-E, E, -E, E}, {-E, E, -E, E}, {-E, gx0, -E, gx0}, {gx1, E, gx1, E}};
    const float zs[4][4] = {{E, E, gz1, gz1}, {gz0, gz0, -E, -E}, {gz1, gz1, gz0, gz0}, {gz1, gz1, gz0, gz0}};
    for(int b = 0; b < 4; b++) {
        RV q[4];
        for(int i = 0; i < 4; i++)
            q[i] = (RV){xs[b][i], CLOUD_Y, zs[b][i], xs[b][i] * uvs + su, zs[b][i] * uvs + sv2, far};
        r_strip(q, 4);
    }
}

/* Animated sea: a camera-centred grid out to ~360 m, then wide bands to
   the horizon. Only the northern half-plane is ever visible. */
static void draw_sea(void) {
    if(world_night) {
        draw_clouds_below();
        return;
    }
    const float cell = world_split ? 60.0f : 45.0f;
    const int n = world_split ? 10 : 16;
    float cx = floorf(r_cam.pos.x / cell) * cell, cz = floorf(r_cam.pos.z / cell) * cell;
    float x0 = cx - n * cell * 0.5f, z0 = cz - n * cell * 0.5f;
    float su = world_time * 0.018f, sv2 = world_time * 0.011f;
    r_hdr(h_water);
    RV row[2 * (16 + 1)];
    for(int j = 0; j < n; j++) {
        float za = z0 + j * cell, zb = za + cell;
        if(zb < -60.0f) continue;
        V3 mid = v3(cx, WATER_Y, (za + zb) * 0.5f);
        if(!r_sphere_visible(mid, n * cell * 0.75f)) continue;
        int k = 0;
        for(int i = 0; i <= n; i++) {
            float x = x0 + i * cell;
            row[k++] = (RV){x, WATER_Y, zb, x / 26.0f + su, zb / 26.0f + sv2, sea_color(x, zb)};
            row[k++] = (RV){x, WATER_Y, za, x / 26.0f + su, za / 26.0f + sv2, sea_color(x, za)};
        }
        r_strip(row, k);
    }
    /* Horizon bands around the grid. */
    float gx0 = x0, gx1 = x0 + n * cell, gz0 = fmaxf(z0, -60.0f), gz1 = z0 + n * cell;
    const float E = 4000.0f;
    uint32_t deep = argb(255, 96, 150, 222);
    float uvs = 1.0f / 26.0f;
    RV q[4];
    /* north */
    q[0] = (RV){-E, WATER_Y, E, -E * uvs, E * uvs, deep};
    q[1] = (RV){E, WATER_Y, E, E * uvs, E * uvs, deep};
    q[2] = (RV){-E, WATER_Y, gz1, -E * uvs, gz1 * uvs, deep};
    q[3] = (RV){E, WATER_Y, gz1, E * uvs, gz1 * uvs, deep};
    r_strip(q, 4);
    /* west and east of the grid */
    if(gz1 > gz0) {
        q[0] = (RV){-E, WATER_Y, gz1, -E * uvs, gz1 * uvs, deep};
        q[1] = (RV){gx0, WATER_Y, gz1, gx0 * uvs, gz1 * uvs, sea_color(gx0, gz1)};
        q[2] = (RV){-E, WATER_Y, -60.0f, -E * uvs, -60.0f * uvs, deep};
        q[3] = (RV){gx0, WATER_Y, gz0, gx0 * uvs, gz0 * uvs, sea_color(gx0, gz0)};
        r_strip(q, 4);
        q[0] = (RV){gx1, WATER_Y, gz1, gx1 * uvs, gz1 * uvs, sea_color(gx1, gz1)};
        q[1] = (RV){E, WATER_Y, gz1, E * uvs, gz1 * uvs, deep};
        q[2] = (RV){gx1, WATER_Y, gz0, gx1 * uvs, gz0 * uvs, sea_color(gx1, gz0)};
        q[3] = (RV){E, WATER_Y, -60.0f, E * uvs, -60.0f * uvs, deep};
        r_strip(q, 4);
    }
}

static void draw_dash_pads(void) {
    float scroll = -world_time * 2.2f;
    r_hdr(h_dash);
    for(int i = 0; i < track.npads; i++) {
        const DashPad *p = &track.pads[i];
        RV v[4];
        int n = 0;
        for(int k = 0; k < 2; k++) {
            float s = k ? p->s1 : p->s0;
            V3 a = track_point(s, p->d0), b = track_point(s, p->d1);
            a.y = track_road_y(s, p->d0) + 0.035f;
            b.y = track_road_y(s, p->d1) + 0.035f;
            float vv = (k ? 0.0f : 1.0f) + scroll;
            v[n++] = (RV){a.x, a.y, a.z, 0, vv, 0xffffffff};
            v[n++] = (RV){b.x, b.y, b.z, 1, vv, 0xffffffff};
        }
        r_strip(v, 4);
    }
}

void world_set_fog(bool split) {
    /* The fog table is global: pull it in for the shorter split-screen view.
       The PVR reads the table while it renders, so the table only changes
       once the previous frame's render has finished, before a new scene.
       At night it fades to the purple of the sky's horizon. */
    static int fog_mode = -1;
    int mode = (int)split | (world_night ? 2 : 0);
    if(fog_mode == mode) return;
    fog_mode = mode;
    pvr_wait_ready();
    if(world_night) {
        pvr_fog_table_color(1.0f, 20 / 255.0f, 16 / 255.0f, 56 / 255.0f);
        pvr_set_bg_color(20 / 255.0f, 16 / 255.0f, 56 / 255.0f);
    }
    else {
        pvr_fog_table_color(1.0f, 0.74f, 0.89f, 0.98f);
        pvr_set_bg_color(0.55f, 0.82f, 0.98f);
    }
    if(split) pvr_fog_table_linear(world_night ? 140.0f : 110.0f, world_night ? 520.0f : 400.0f);
    else pvr_fog_table_linear(world_night ? 240.0f : 160.0f, world_night ? 1300.0f : 820.0f);
}

void world_draw_op(void) {
    r_load_vp();
    if(!(dk_probe_off & PROBE_SKY)) draw_sky();
    if(!(dk_probe_off & PROBE_SEA)) draw_sea();
    if(!(dk_probe_off & PROBE_WORLD)) {
        draw_chunks(HF_OP);
        draw_dash_pads();
    }
}

void world_draw_pt(void) {
    r_load_vp();
    draw_chunks(HF_PT);
    /* Far palms as upright sprites. */
    r_hdr(h_palm_far);
    for(int i = 0; i < palm_n; i++) {
        V3 d = vsub(palms[i].base, r_cam.pos);
        float d2 = vdot(d, d);
        float far = world_split ? 360.0f : 800.0f;
        float near = world_split ? 135.0f * 0.55f : 135.0f;
        if(d2 < near * near || d2 > far * far) continue;
        V3 mid = vadd(palms[i].base, v3(0, palms[i].height * 0.6f, 0));
        if(!r_sphere_visible(mid, palms[i].height)) continue;
        r_billboard_y(palms[i].base, palms[i].height * 0.62f, palms[i].height * 1.25f, 0, 0, 1, 1, 0xffffffff);
    }
    for(int i = 0; i < far_sprite_n; i += world_split ? 2 : 1) {
        V3 mid = vadd(far_sprites[i], v3(0, far_sprite_h[i] * 0.6f, 0));
        if(!r_sphere_visible(mid, far_sprite_h[i])) continue;
        r_billboard_y(far_sprites[i], far_sprite_h[i] * 0.62f, far_sprite_h[i] * 1.25f, 0, 0, 1, 1, 0xffd8e8d8);
    }
}

static void draw_shore_foam(void) {
    /* Foam rides the waterline, washing in and out along its normal. */
    r_hdr(h_foam);
    RV v[2 * 40];
    int n = 0;
    float pulse = 0.5f + 0.5f * sinf(world_time * 1.3f);
    uint32_t ca = color_alpha(0xffffffff, 0.75f + 0.25f * pulse), cb = color_alpha(0xffffffff, 0.0f);
    for(int i = 0; i < foam_count; i++) {
        if(!foam_on[i]) {
            if(n >= 4) r_strip(v, n);
            n = 0;
            continue;
        }
        float wob = 0.8f * sinf(world_time * 0.9f + foam_u[i] * 0.07f);
        V3 a = vmad(foam_p[i], foam_n[i], -1.4f + wob * 0.5f);
        V3 b = vmad(foam_p[i], foam_n[i], 3.0f + wob);
        a.y = b.y = WATER_Y + 0.05f;
        v[n++] = (RV){a.x, a.y, a.z, foam_u[i] / 6.0f, world_time * 0.05f, ca};
        v[n++] = (RV){b.x, b.y, b.z, foam_u[i] / 6.0f, world_time * 0.05f + 1.0f, cb};
        if(n == 2 * 40) {
            r_strip(v, n);
            v[0] = v[n - 2];
            v[1] = v[n - 1];
            n = 2;
        }
    }
    if(n >= 4) r_strip(v, n);
}

/* Night: the far city on the horizon, rising through the cloud sea, a
   crescent moon, now and then a shooting star, and searchlights sweeping
   up from the tallest towers. */
static void draw_night_distant(void) {
    V3 c = r_cam.pos;
    r_hdr(h_skyline);
    const int segs = 24;
    const float R = 1350.0f;
    RV v[2 * 25];
    int n = 0;
    for(int i = 0; i <= segs; i++) {
        float x = c.x + ring_dir[i].x * R, z = c.z + ring_dir[i].z * R;
        v[n++] = (RV){x, 70.0f, z, (float)i / segs * 6.0f, 0.0f, 0xffffffff};
        v[n++] = (RV){x, CLOUD_Y - 40.0f, z, (float)i / segs * 6.0f, 1.0f, 0xffffffff};
    }
    r_strip(v, n);

    V3 mp = vmad(c, world_sun_dir, 1300.0f);
    if(r_sphere_visible(mp, 220.0f)) {
        r_hdr(h_glow_add);
        r_billboard(mp, 460.0f, 460.0f, 0, 0, 0, 1, 1, argb(60, 255, 230, 170));
        r_hdr(h_moon);
        r_billboard(mp, 150.0f, 150.0f, 0, 0, 0, 1, 1, 0xffffffff);
    }
    /* A shooting star every few seconds, somewhere new each time. */
    const float every = 3.7f;
    int idx = (int)(world_time / every);
    float t = (world_time - (float)idx * every) / 0.7f;
    if(t < 1.0f) {
        uint32_t h = (uint32_t)idx * 2654435761u;
        float az = (float)(h & 1023) / 1023.0f * DK_TAU, el = 0.35f + (float)((h >> 10) & 255) / 255.0f * 0.5f;
        V3 d0 = v3(sinf(az) * cosf(el), sinf(el), cosf(az) * cosf(el));
        V3 dir = vnorm(v3(cosf(az) * ((h & 1024) ? 1.0f : -1.0f), -0.45f, -sinf(az) * ((h & 1024) ? 1.0f : -1.0f)));
        V3 head = vadd(c, vscale(vmad(d0, dir, t * 0.28f), 1250.0f));
        V3 tail = vadd(c, vscale(vmad(d0, dir, fmaxf(t - 0.25f, 0.0f) * 0.28f), 1250.0f));
        float a = sinf(t * DK_PI);
        r_hdr(h_glow_add);
        r_streak(head, tail, 7.0f, argb((int)(255 * a), 255, 255, 255), argb(0, 160, 200, 255));
    }
}

/* Sparkles hanging round the sky road, twinkling. */
static void draw_sparkles(void) {
    r_hdr(h_star_add);
    float range = world_split ? 220.0f : 340.0f;
    for(int i = 0; i < SK_SPARKLES; i += world_split ? 2 : 1) {
        V3 p = sk_sparkle[i];
        V3 d = vsub(p, r_cam.pos);
        if(vdot(d, d) > range * range || !r_sphere_visible(p, 5.0f)) continue;
        float tw = 0.55f + 0.45f * sinf(world_time * (2.0f + (float)(i % 5) * 0.6f) + (float)i * 1.9f);
        float size = 4.0f + (float)(i % 4) * 1.5f;
        r_billboard(p, size * tw, size * tw, world_time * 0.3f + (float)i, 0, 0, 1, 1,
                    color_alpha(world_lane_color((float)(i % 7) * 3.2f - 9.6f), 0.8f * tw));
    }
    /* Searchlights (not in a split screen): long soft beams turning slowly over the city, each
       facing the camera about its own axis. */
    r_hdr(h_glow_add);
    for(int i = 0; i < (world_split ? 0 : sk_beam_n); i++) {
        float a = world_time * (0.25f + 0.04f * (float)i) + (float)i * 2.1f;
        V3 base = sk_beam[i];
        V3 dir = vnorm(v3(sinf(a) * 0.45f, 1.0f, cosf(a) * 0.45f));
        V3 tip = vmad(base, dir, 420.0f);
        V3 side0 = vnorm(vcross(dir, vsub(r_cam.pos, base))), side1 = vnorm(vcross(dir, vsub(r_cam.pos, tip)));
        uint32_t c0 = (i & 1) ? argb(70, 120, 200, 255) : argb(70, 210, 120, 255), c1 = color_alpha(c0, 0.0f);
        RV q[4] = {
            {base.x - side0.x * 2.0f, base.y - side0.y * 2.0f, base.z - side0.z * 2.0f, 0, 0.5f, c0},
            {base.x + side0.x * 2.0f, base.y + side0.y * 2.0f, base.z + side0.z * 2.0f, 1, 0.5f, c0},
            {tip.x - side1.x * 26.0f, tip.y - side1.y * 26.0f, tip.z - side1.z * 26.0f, 0, 0.5f, c1},
            {tip.x + side1.x * 26.0f, tip.y + side1.y * 26.0f, tip.z + side1.z * 26.0f, 1, 0.5f, c1},
        };
        r_strip(q, 4);
    }
}

static void draw_clouds_and_mountains(void) {
    if(world_night) {
        draw_night_distant();
        return;
    }
    V3 c = r_cam.pos;
    /* Distant hills ring. */
    r_hdr(h_mount);
    const int segs = 24;
    const float R = 1350.0f;
    RV v[2 * 25];
    int n = 0;
    for(int i = 0; i <= segs; i++) {
        float x = c.x + ring_dir[i].x * R, z = c.z + ring_dir[i].z * R;
        float south = clampf(ring_dir[i].y * 1.2f + 0.35f, 0.25f, 1.0f);
        float top = 26.0f + 70.0f * south;
        uint32_t col = color_alpha(0xffffffff, 0.55f + 0.45f * south);
        v[n++] = (RV){x, top, z, (float)i / segs * 4.0f, 0, col};
        v[n++] = (RV){x, -6.0f, z, (float)i / segs * 4.0f, 1, col};
    }
    r_strip(v, n);

    /* Clouds drift slowly around the dome. */
    r_hdr(h_cloud);
    for(int i = 0; i < 11; i += world_split ? 2 : 1) {
        float a = (float)i * 0.63f * DK_PI * 0.9f + world_time * 0.004f;
        float el = 0.07f + 0.05f * (float)(i % 4) / 3.0f + (i & 1) * 0.04f;
        float d = 1250.0f;
        V3 p = v3(c.x + sinf(a) * cosf(el) * d, sinf(el) * d + 20.0f, c.z + cosf(a) * cosf(el) * d);
        if(!r_sphere_visible(p, 260.0f)) continue;
        float w = 150.0f + (float)(i % 3) * 50.0f;
        float u0 = (i & 1) ? 0.5f : 0.0f;
        r_billboard(p, w, w * 0.8f, 0, u0, 0, u0 + 0.5f, 1, color_alpha(0xffffffff, 0.92f));
    }

    /* The sun. */
    V3 sp = vmad(c, world_sun_dir, 1300.0f);
    if(r_sphere_visible(sp, 200.0f)) {
        r_hdr(h_glow_add);
        r_billboard(sp, 210.0f, 210.0f, 0, 0, 0, 1, 1, argb(200, 255, 250, 210));
        r_billboard(sp, 70.0f, 70.0f, 0, 0, 0, 1, 1, argb(255, 255, 255, 240));
    }
}

/* Lens flare ghosts along the line from the sun through the screen centre. */
static void draw_lens_flare(void) {
    V3 sp = vmad(r_cam.pos, world_sun_dir, 1300.0f);
    float sx, sy, w;
    if(!r_project(sp, &sx, &sy, &w)) return;
    if(sx < -40 || sx > 680 || sy < -40 || sy > 520) return;
    float edge = clampf(1.0f - fmaxf(fabsf(sx - 320) / 380.0f, fabsf(sy - 240) / 300.0f), 0, 1);
    static const float pos[] = {0.35f, 0.62f, 1.0f, 1.35f, 1.7f};
    static const float size[] = {44, 22, 64, 30, 90};
    static const uint32_t col[] = {0xffffd090, 0xff90ffb0, 0xff80c0ff, 0xffffa0e0, 0xffa0d0ff};
    r_hdr(h_flare);
    for(int i = 0; i < 5; i++) {
        float x = sx + (320 - sx) * pos[i] * 2.0f, y = sy + (240 - sy) * pos[i] * 2.0f;
        float s = size[i];
        r_quad2d(x - s, y - s, x + s, y + s, 0, 0, 1, 1, color_alpha(col[i], 0.35f * edge), 9.0f);
    }
}

void world_draw_tr(void) {
    r_load_vp();
    if(!(dk_probe_off & PROBE_DISTANT)) draw_clouds_and_mountains();
    if(dk_probe_off & PROBE_FOAM) {
        if(!(dk_probe_off & PROBE_DISTANT) && !world_night) draw_lens_flare();
        return;
    }
    if(world_night) {
        /* Neon glows are additive, so they need no sorting. */
        if(!(dk_probe_off & PROBE_WORLD)) draw_chunks(HF_TR);
        draw_sparkles();
    }
    draw_shore_foam();
    th_draw_flames();
    /* Dash panels pulse with a glow. */
    r_hdr(h_dash_glow);
    for(int i = 0; i < track.npads; i++) {
        const DashPad *p = &track.pads[i];
        float sm = track_wrap(p->s0 + track_delta(p->s0, p->s1) * 0.5f);
        V3 c = track_point(sm, (p->d0 + p->d1) * 0.5f);
        c.y = track_height(sm) + 0.4f;
        float a = 0.35f + 0.25f * sinf(world_time * 8.0f + i);
        r_billboard(c, 3.0f, 1.2f, 0, 0, 0, 1, 1, color_alpha(0xff60c0ff, a));
    }
    if(!(dk_probe_off & PROBE_DISTANT) && !world_night) draw_lens_flare();
}
