/* Emerald Beach scenery.

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
    uint8_t far;       /* distant terrain: skipped in a split screen */
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
static V3 sky_dir[7][13];
static V3 ring_dir[25];

/* Headers. */
static const pvr_poly_hdr_t *h_road, *h_board, *h_curb, *h_sand, *h_grass, *h_barrier, *h_wood,
    *h_rock, *h_bark, *h_checker, *h_thatch, *h_crowd, *h_banner, *h_col, *h_water,
    *h_frond, *h_palm_far, *h_dash, *h_sky, *h_cloud, *h_mount, *h_foam, *h_glow_add,
    *h_flare, *h_dash_glow;

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

static void add_grandstand(void) {
    chunk_begin(HF_OP, 900.0f);
    float x0 = -52.0f, x1 = 26.0f;
    for(int tier = 0; tier < 5; tier++) {
        float z = -82.0f - tier * 2.6f, y = 0.9f + tier * 1.2f;
        /* Seats with spectators facing the track (north). */
        V3 a = v3(x0, y, z), b = v3(x1, y, z), c = v3(x0, y - 1.2f, z + 0.1f), d = v3(x1, y - 1.2f, z + 0.1f);
        quad(h_crowd, a, b, c, d, 0, 0, (x1 - x0) / 3.0f, 0.6f, argb(255, 255, 255, 255));
        V3 e = v3(x0, y, z - 2.6f), f = v3(x1, y, z - 2.6f);
        quad(h_col, e, f, a, b, 0, 0, 1, 1, argb(255, 190, 196, 210));
    }
    box(h_col, v3((x0 + x1) * 0.5f, 3.6f, -97.5f), v3((x1 - x0) * 0.5f, 3.6f, 0.4f), 0, argb(255, 150, 160, 185), 1);
    /* Striped canopy on posts. */
    for(int i = 0; i <= 6; i++) {
        float x = lerpf(x0, x1, (float)i / 6.0f);
        box(h_col, v3(x, 4.5f, -82.5f), v3(0.15f, 4.5f, 0.15f), 0, argb(255, 235, 235, 240), 1);
    }
    for(int i = 0; i < 12; i++) {
        float xa = lerpf(x0, x1, (float)i / 12.0f), xb = lerpf(x0, x1, (float)(i + 1) / 12.0f);
        uint32_t col = (i & 1) ? argb(255, 250, 250, 250) : argb(255, 40, 140, 230);
        quad(h_col, v3(xa, 10.5f, -98.0f), v3(xb, 10.5f, -98.0f), v3(xa, 9.0f, -81.5f), v3(xb, 9.0f, -81.5f),
             0, 0, 1, 1, col);
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

static void add_pier(float x) {
    chunk_begin(HF_OP, 900.0f);
    float z0 = 66.0f, z1 = 124.0f, w = 2.2f, y = 0.45f;
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

static void add_pond(void) {
    chunk_begin(HF_OP, 600.0f);
    const int n = 16;
    /* Sand rim then water, as rings from the centre. */
    for(int ring = 0; ring < 2; ring++) {
        strip_begin(ring ? h_water : h_sand);
        float ro = ring ? 0.82f : 1.0f, ri = ring ? 0.0f : 0.82f, y = ring ? 0.08f : 0.04f;
        for(int i = 0; i <= n; i++) {
            float a = (float)i / n * DK_TAU;
            float rx = 34.0f, rz = 13.0f;
            V3 o = v3(cosf(a) * rx * ro, y, sinf(a) * rz * ro);
            V3 in = v3(cosf(a) * rx * ri, y, sinf(a) * rz * ri);
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
    add_pond();
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
    add_grandstand();
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
    add_pier(32.0f);
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
    place_props();
    printf("DreamKarts world: %d chunks, %d strips, %d vertices (%lu KiB), %d palms%s\n",
           chunk_n, strip_n, pool_n, (unsigned long)(pool_n * sizeof(RV) / 1024), palm_n,
           built_ok ? "" : " [POOL OVERFLOW]");
}

void world_update(float dt) {
    world_time += dt;
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
        if(world_split) lod = lod < 1000.0f ? lod * 0.55f : fminf(lod, 400.0f);
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

/* Sky dome around the camera: vertex-coloured gradient, no fog. */
static void draw_sky(void) {
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

/* Animated sea: a camera-centred grid out to ~360 m, then wide bands to
   the horizon. Only the northern half-plane is ever visible. */
static void draw_sea(void) {
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
    for(int i = 0; i < DASH_PAD_COUNT; i++) {
        const DashPad *p = &track.pads[i];
        RV v[4];
        int n = 0;
        for(int k = 0; k < 2; k++) {
            float s = k ? p->s1 : p->s0;
            V3 a = track_point(s, p->d0), b = track_point(s, p->d1);
            a.y = b.y = 0.035f;
            float vv = (k ? 0.0f : 1.0f) + scroll;
            v[n++] = (RV){a.x, a.y, a.z, 0, vv, 0xffffffff};
            v[n++] = (RV){b.x, b.y, b.z, 1, vv, 0xffffffff};
        }
        r_strip(v, 4);
    }
}

void world_draw_op(void) {
    /* The fog table is global: pull it in for the shorter split-screen view. */
    static int fog_mode = -1;
    if(fog_mode != (int)world_split) {
        fog_mode = (int)world_split;
        if(world_split) pvr_fog_table_linear(110.0f, 400.0f);
        else pvr_fog_table_linear(160.0f, 820.0f);
    }
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
    /* Foam follows the waterline beyond the outer wall on the sea side. */
    r_hdr(h_foam);
    RV v[2 * 40];
    int n = 0;
    float pulse = 0.5f + 0.5f * sinf(world_time * 1.3f);
    for(int i = 0; i <= TRACK_SAMPLES; i += 2) {
        const TrackSample *t = &track.smp[i % TRACK_SAMPLES];
        bool sea = t->p.z > 8.0f;
        if(sea && n < 2 * 40) {
            float db = shore_db[i % TRACK_SAMPLES];
            float wob = 0.8f * sinf(world_time * 0.9f + t->s * 0.07f);
            V3 a = vmad(t->p, t->r, t->wall_out + db - 1.4f + wob * 0.5f);
            V3 b = vmad(t->p, t->r, t->wall_out + db + 3.0f + wob);
            a.y = b.y = WATER_Y + 0.05f;
            uint32_t ca = color_alpha(0xffffffff, 0.75f + 0.25f * pulse), cb = color_alpha(0xffffffff, 0.0f);
            v[n++] = (RV){a.x, a.y, a.z, t->s / 6.0f, world_time * 0.05f, ca};
            v[n++] = (RV){b.x, b.y, b.z, t->s / 6.0f, world_time * 0.05f + 1.0f, cb};
        }
        else if(n >= 4) {
            r_strip(v, n);
            n = 0;
        }
        else {
            n = 0;
        }
    }
    if(n >= 4) r_strip(v, n);
}

static void draw_clouds_and_mountains(void) {
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
        if(!(dk_probe_off & PROBE_DISTANT)) draw_lens_flare();
        return;
    }
    draw_shore_foam();
    /* Dash panels pulse with a glow. */
    r_hdr(h_dash_glow);
    for(int i = 0; i < DASH_PAD_COUNT; i++) {
        const DashPad *p = &track.pads[i];
        V3 c = track_point((p->s0 + p->s1) * 0.5f, (p->d0 + p->d1) * 0.5f);
        c.y = 0.4f;
        float a = 0.35f + 0.25f * sinf(world_time * 8.0f + i);
        r_billboard(c, 3.0f, 1.2f, 0, 0, 0, 1, 1, color_alpha(0xff60c0ff, a));
    }
    if(!(dk_probe_off & PROBE_DISTANT)) draw_lens_flare();
}
