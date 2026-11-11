/* Ridgeline: the land beyond the map (outland.h).

   The desert is seen from above, and from its overlook the map's edge
   would be the edge of the world. So the country carries on past it: a
   ring of ground 768 m deep on a 24 m grid (tools/outland.py, drawn by
   tools/level_moab.py), mesas, buttes and the rivers running on, to look
   at and never to drive on. It has no collision and no surface: a vertex
   is a height, an albedo and a normal, lit by the hour as the map's
   ground is, shadows from its own horizon map and all, under the same
   texture at the same scale, so the map's edge is not a line.

   It fogs itself, vertex by vertex, and never all the way: the PVR's fog
   is solid from where it ends, and a mesa out there would stand paler
   than the skyline painted behind it (world.c), which reads as a mistake.
   Held to the haze the skyline's nearest ring has, far ground becomes
   one more ring of it, and one that moves as the truck does.

   Its vertices along the map's edge are the map's own, every eighth one
   (the far ground's coarsest grid), with the map's colours. It is drawn
   in chunks of 8 x 8 cells, every vertex near, every other one further
   off and every fourth past the fog, where all that is left of it is an
   outline against the sky, and coarser than that wherever the coarser
   grid is no further from the ground than would show from where the
   camera is (open desert is flat: most of it is always every fourth).
   Where a chunk meets a coarser one its edge takes the coarser one's
   line, so there is no crack between them. The
   map's own edge has vertices this one's has not: a skirt hangs from this
   edge as far as the map's dips under it.

   Most of the drive is under the cliffs, where the map hides nearly all
   of this: each chunk is checked against the map's skyline from the
   camera (the steepest sight line over the map's ground, by bearing,
   reworked a few bearings a frame) and skipped where it stands under it. */
#include "outland.h"
#include "terrain.h"
#include "tod.h"
#include <string.h>

#define ON 97                   /* vertices a side */
#define OHZ ((ON - 1) / 2 + 1)
#define CH 8                    /* cells a chunk's side */
#define CHN ((ON - 1) / CH)
#define MAP0 32                 /* the map's edges, in vertices here */
#define MAP1 64
#define MAP_STEP 8              /* map vertices to one of these */
#define OUT_LOD2_M 520.0f       /* every other vertex from here */
#define OUT_LOD4_M 960.0f       /* every fourth */
#define OUT_LOD_ERR 0.005f      /* or sooner: metres a coarser grid may be out, per metre away */
#define RIB_RUN 8               /* pairs of a river to a strip */
#define SIDE_N (MAP1 - MAP0)    /* cells along a side of the map */
#define SKY_N 64                /* bearings the map's skyline is kept in */
#define SKY_PER_FRAME 8
#define SKY_STEP_M 18.0f
#define SKY_MARGIN 0.02f        /* of tangent: about a degree */
#define RIPPLE_ACROSS (1.0f / 4.0f)   /* water.c's */
#define RIPPLE_ALONG (1.0f / 7.0f)
#define RIVER_FLOW 0.8f         /* m/s the ripples run at */
#define TEX_TILE (1.0f / 9.0f)  /* terrain.c's */

typedef struct { uint16_t h; uint8_t r, g, b; int8_t nx, nz; uint8_t fade; } OutV;
typedef struct { float xl, zl, xr, zr, y; } OutRib;
typedef struct { uint16_t first, n; } OutRun;
_Static_assert(sizeof(OutV) == 8 && sizeof(OutRib) == 20, "tools/outland.py pack()");
_Static_assert(MAP0 + (LEVEL_N - 1) / MAP_STEP == MAP1, "the map is the middle of it");

extern const uint8_t rl_outland_moab[];

static const OutV *ov;          /* NULL: this area has none */
static const uint8_t *ohz;
static const OutRib *rib;
static const OutRun *runs;
static int nruns;
static float cell, org, hbase, hstep;
static uint32_t ocol[ON * ON];
static uint32_t rib_col;
static struct {
    float lo, hi, rad;
    float err[2];       /* how far the grids of every other and every fourth vertex stray from the ground */
} chy[CHN][CHN];
static float skirt[3][4][SIDE_N + 1];   /* by step (1, 2, 4), side (south, north, west, east) and vertex along it */
static const pvr_poly_hdr_t *h_out, *h_river;

/* What the map hides: the tangent of its skyline's elevation from
   sky_from, by bearing. */
static float sky_tan[SKY_N];
static V3 sky_from;
static int sky_next;

static struct {
    float sky[3], sun[3];
    V3 L;
    const uint8_t *hz0, *hz1;
    float hz_t, el2;
    uint32_t haze;
} rt;

static inline float out_y(const OutV *v) {
    return hbase + hstep * (float)v->h;
}

static inline bool in_map(int i, int j) {
    return i >= MAP0 && i <= MAP1 && j >= MAP0 && j <= MAP1;
}

/* Vertex k along a side of the map, here. */
static inline void side_ij(int side, int k, int *i, int *j) {
    *i = side < 2 ? MAP0 + k : side == 2 ? MAP0 : MAP1;
    *j = side < 2 ? (side == 0 ? MAP0 : MAP1) : MAP0 + k;
}

/* How far the map's edge dips under this one's between its vertices, for
   each step this edge is drawn at: the map's has MAP_STEP to each of
   these. */
static void skirt_build(void) {
    for(int lv = 0; lv < 3; lv++)
        for(int side = 0; side < 4; side++) {
            int step = 1 << lv;
            float dip[SIDE_N];
            for(int k = 0; k < SIDE_N; k += step) {
                int i0, j0, i1, j1;
                side_ij(side, k, &i0, &j0);
                side_ij(side, k + step, &i1, &j1);
                float y0 = out_y(&ov[j0 * ON + i0]), y1 = out_y(&ov[j1 * ON + i1]), worst = 0;
                int n = step * MAP_STEP;
                for(int t = 1; t < n; t++) {
                    int mi = (i0 - MAP0) * MAP_STEP + (i1 - i0) * MAP_STEP * t / n;
                    int mj = (j0 - MAP0) * MAP_STEP + (j1 - j0) * MAP_STEP * t / n;
                    float d = y0 + (y1 - y0) * (float)t / (float)n - lvl_h(mi, mj);
                    if(d > worst) worst = d;
                }
                dip[k] = worst;
            }
            for(int k = 0; k <= SIDE_N; k += step) {
                float a = k > 0 ? dip[k - step] : 0, b = k < SIDE_N ? dip[k] : 0;
                skirt[lv][side][k] = (a > b ? a : b) + 0.6f;
            }
        }
}

void outland_init(void) {
    h_out = r_header(look->ground_tex, HF_OP | HF_NOFOG);
    h_river = r_header(TEX_RIPPLE, HF_TR | HF_NOFOG);
    ov = NULL;
    const uint8_t *b = lvl_area == AREA_MOAB ? rl_outland_moab : NULL;
    if(!b) return;
    struct {
        char magic[4];
        uint32_t version;
        uint16_t n, hz_n;
        float cell, org, hbase, hstep;
        uint32_t pairs, runs, off_hz, off_rib, off_runs;
    } hd;
    memcpy(&hd, b, sizeof(hd));
    if(memcmp(hd.magic, "RLOU", 4) || hd.version != 1 || hd.n != ON || hd.hz_n != OHZ) {
        printf("Ridgeline outland: not a ring this build can draw\n");
        return;
    }
    cell = hd.cell;
    org = hd.org;
    hbase = hd.hbase;
    hstep = hd.hstep;
    ov = (const OutV *)(b + sizeof(hd));
    ohz = b + hd.off_hz;
    rib = (const OutRib *)(b + hd.off_rib);
    runs = (const OutRun *)(b + hd.off_runs);
    nruns = (int)hd.runs;
    for(int cj = 0; cj < CHN; cj++)
        for(int ci = 0; ci < CHN; ci++) {
            float lo = 1e9f, hi = -1e9f;
            for(int j = cj * CH; j <= cj * CH + CH; j++)
                for(int i = ci * CH; i <= ci * CH + CH; i++) {
                    float y = out_y(&ov[j * ON + i]);
                    if(y < lo) lo = y;
                    if(y > hi) hi = y;
                }
            chy[cj][ci].lo = lo;
            chy[cj][ci].hi = hi;
            float hw = CH * cell * 0.5f;
            chy[cj][ci].rad = sqrtf(2.0f * hw * hw + (hi - lo) * (hi - lo) * 0.25f);
            for(int lv = 0; lv < 2; lv++) {
                int st = 2 << lv;
                float worst = 0;
                for(int j = 0; j <= CH; j++)
                    for(int i = 0; i <= CH; i++) {
                        /* The coarse grid's height here: between its four
                           vertices round this one. */
                        int i0 = i & ~(st - 1), j0 = j & ~(st - 1), i1 = i0 < CH ? i0 + st : i0, j1 = j0 < CH ? j0 + st : j0;
                        float fx = (float)(i - i0) / st, fz = (float)(j - j0) / st;
                        const OutV *row0 = &ov[(cj * CH + j0) * ON + ci * CH], *row1 = &ov[(cj * CH + j1) * ON + ci * CH];
                        float a = out_y(&row0[i0]) + (out_y(&row0[i1]) - out_y(&row0[i0])) * fx;
                        float b = out_y(&row1[i0]) + (out_y(&row1[i1]) - out_y(&row1[i0])) * fx;
                        float d = fabsf(a + (b - a) * fz - out_y(&ov[(cj * CH + j) * ON + ci * CH + i]));
                        if(d > worst) worst = d;
                    }
                chy[cj][ci].err[lv] = worst;
            }
        }
    skirt_build();
    for(int b = 0; b < SKY_N; b++) sky_tan[b] = -1e9f;
    outland_relight_setup(pal, tod.moonlit ? tod.moon : tod.sun);
    outland_relight(0, ON * ON);
    printf("Ridgeline outland: %d x %d vertices, %d rivers\n", ON, ON, nruns);
}

void outland_relight_setup(const Palette *p, V3 light) {
    if(!ov) return;
    terrain_light(p, rt.sky, rt.sun);
    rt.L = light;
    float az = atan2f(light.x, light.z) * 180.0f / ENG_PI;
    if(az < 0) az += 360.0f;
    float u = clampf((az - LEVEL_HZ_AZ0) / LEVEL_HZ_STEP, 0, LEVEL_HZ_DIRS - 1.001f);
    int d = (int)u;
    rt.hz_t = u - d;
    rt.hz0 = ohz + d * OHZ * OHZ;
    rt.hz1 = rt.hz0 + OHZ * OHZ;
    rt.el2 = asinf(clampf(light.y, -1, 1)) * (360.0f / ENG_PI);
    rt.haze = p->haze;
    /* The rivers: the map's water at one depth (water.c). */
    rib_col = color_alpha(color_scale(color_lerp(look->water_shallow, look->water_deep, 0.75f), p->water_k), 0.72f);
}

int outland_relight_n(void) {
    return ov ? ON * ON : 0;
}

/* The skyline's elevation toward the sun, as terrain.c's horizon_at(). */
static inline float horizon_at(int i, int j) {
    int hi = i >> 1, hj = j >> 1;
    int hi1 = (i & 1) && hi + 1 < OHZ ? hi + 1 : hi, hj1 = (j & 1) && hj + 1 < OHZ ? hj + 1 : hj;
    int a = hj * OHZ + hi, b = hj * OHZ + hi1, c = hj1 * OHZ + hi, d = hj1 * OHZ + hi1;
    float h0 = 0.25f * (rt.hz0[a] + rt.hz0[b] + rt.hz0[c] + rt.hz0[d]);
    float h1 = 0.25f * (rt.hz1[a] + rt.hz1[b] + rt.hz1[c] + rt.hz1[d]);
    return h0 + (h1 - h0) * rt.hz_t;
}

void outland_relight(int from, int to) {
    if(!ov) return;
    int i = from % ON, j = from / ON;
    for(int idx = from; idx < to; idx++) {
        if(in_map(i, j)) {
            /* The map's own, where the two meet. */
            ocol[idx] = lvl_c((i - MAP0) * MAP_STEP, (j - MAP0) * MAP_STEP);
        }
        else {
            const OutV *v = &ov[idx];
            float nx = v->nx * (1.0f / 127.0f), nz = v->nz * (1.0f / 127.0f);
            float ny = sqrtf(fmaxf(1.0f - nx * nx - nz * nz, 0.0f));
            float d = nx * rt.L.x + ny * rt.L.y + nz * rt.L.z;
            if(d > 0) d *= clampf((rt.el2 - horizon_at(i, j)) * (1.0f / 6.0f) + 0.5f, 0, 1);
            else d = 0;
            float up = 0.55f + 0.45f * ny;
            int r = (int)(v->r * (rt.sky[0] * up + rt.sun[0] * d));
            int g = (int)(v->g * (rt.sky[1] * up + rt.sun[1] * d));
            int b = (int)(v->b * (rt.sky[2] * up + rt.sun[2] * d));
            if(r > 255) r = 255;
            if(g > 255) g = 255;
            if(b > 255) b = 255;
            uint32_t c = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
            /* Its outer edge goes a little further into the haze, to meet
               the foot of the skyline behind it. */
            if(v->fade) c = mix256(c, rt.haze, v->fade * 80 >> 8) | 0xff000000u;
            ocol[idx] = c;
        }
        if(++i == ON) {
            i = 0;
            j++;
        }
    }
}

/* The fog by hand: by depth, as the PVR's is on the map beside it. */
#define OUT_FOG_MAX ((int)(WORLD_SKYLINE_HAZE * 256.0f))
static struct {
    V3 cam, fwd;
    float near, k;      /* k: 256ths a metre */
    uint32_t col;
} fg;

static inline uint32_t out_fog(uint32_t c, float x, float y, float z) {
    float depth = (x - fg.cam.x) * fg.fwd.x + (y - fg.cam.y) * fg.fwd.y + (z - fg.cam.z) * fg.fwd.z;
    int f = (int)((depth - fg.near) * fg.k);
    if(f <= 0) return c;
    return mix256(c, fg.col, f < OUT_FOG_MAX ? f : OUT_FOG_MAX) | 0xff000000u;
}

static inline RV out_v(int i, int j) {
    int idx = j * ON + i;
    float x = org + i * cell, z = org + j * cell;
    return (RV){x, out_y(&ov[idx]), z, x * TEX_TILE, z * TEX_TILE, ocol[idx]};
}

/* Each chunk's step this frame, drawn or not. */
static uint8_t ch_step[CHN][CHN];

/* A vertex of chunk (ci, cj), drawn every s: on an edge shared with a
   coarser chunk it lies on that one's line. */
static RV out_seam_v(int i, int j, int ci, int cj, int s) {
    int li = i - ci * CH, lj = j - cj * CH, big;
    bool along_j = true;
    if(li == 0 && ci > 0) big = ch_step[cj][ci - 1];
    else if(li == CH && ci + 1 < CHN) big = ch_step[cj][ci + 1];
    else {
        along_j = false;
        if(lj == 0 && cj > 0) big = ch_step[cj - 1][ci];
        else if(lj == CH && cj + 1 < CHN) big = ch_step[cj + 1][ci];
        else return out_v(i, j);
    }
    int t = along_j ? lj : li;
    if(big <= s || !(t & (big - 1))) return out_v(i, j);
    int a = t & ~(big - 1);
    int k = (t - a) * 256 / big;
    RV va = along_j ? out_v(i, cj * CH + a) : out_v(ci * CH + a, j);
    RV vb = along_j ? out_v(i, cj * CH + a + big) : out_v(ci * CH + a + big, j);
    RV v = out_v(i, j);
    v.y = va.y + (vb.y - va.y) * (float)k * (1.0f / 256.0f);
    v.c = mix256(va.c, vb.c, k);
    return v;
}

/* ---- What the map hides ---- */

static void skyline_update(V3 cam) {
    /* A jump (a reset, a viewpoint, another area): nothing is known to
       be hidden until the bearings come round again. */
    V3 mv = vsub(cam, sky_from);
    if(vdot(mv, mv) > 12.0f * 12.0f)
        for(int b = 0; b < SKY_N; b++) sky_tan[b] = -1e9f;
    sky_from = cam;
    for(int n = 0; n < SKY_PER_FRAME; n++) {
        float a = ((float)sky_next + 0.5f) * (ENG_TAU / SKY_N), sx = sinf(a), sz = cosf(a), best = -1e9f;
        /* Out to the map's edge; the ground at the camera's feet is no
           skyline. */
        for(float d = SKY_STEP_M; d < 1100.0f; d += SKY_STEP_M) {
            float x = cam.x + sx * d, z = cam.z + sz * d;
            if(x <= LEVEL_ORG || x >= -LEVEL_ORG || z <= LEVEL_ORG || z >= -LEVEL_ORG) break;
            float t = (lvl_h((int)((x - LEVEL_ORG) * (1.0f / LEVEL_D)), (int)((z - LEVEL_ORG) * (1.0f / LEVEL_D))) - cam.y) / d;
            if(t > best) best = t;
        }
        sky_tan[sky_next] = best;
        sky_next = (sky_next + 1) % SKY_N;
    }
}

/* A bearing (x east, z north) as a turn of 0..SKY_N, to within a third of
   a degree: an arctangent a chunk a frame is too dear. */
static inline float bearing(float x, float z) {
    float ax = fabsf(x), az = fabsf(z), lo = ax < az ? ax : az, hi = ax < az ? az : ax;
    float q = hi > 0 ? lo / hi : 0;
    float a = q * (0.99f - 0.205f * q * q);          /* atan(q), 0..pi/4 */
    if(ax > az) a = ENG_PI * 0.5f - a;               /* from north toward east */
    if(z < 0) a = ENG_PI - a;
    if(x < 0) a = ENG_TAU - a;
    return a * (SKY_N / ENG_TAU);
}

/* Whether everything within rad of c (level with the camera or under top)
   stands below the map's skyline from the camera. */
static bool hidden(V3 cam, float cx, float cz, float rad, float top) {
    float dx = cx - cam.x, dz = cz - cam.z, d = sqrtf(dx * dx + dz * dz);
    if(d < rad + 2.0f * SKY_STEP_M) return false;
    float t = (top - cam.y) / (top > cam.y ? d - rad : d + rad) + SKY_MARGIN;
    /* The bearings it spans, and one more either side: a skyline is
       sampled, not swept. */
    float u = bearing(dx, dz), half = rad / d * (1.2f * SKY_N / ENG_TAU);
    int b0 = (int)floorf(u - half - 0.5f), b1 = (int)floorf(u + half + 0.5f);
    for(int b = b0; b <= b1; b++)
        if(sky_tan[b & (SKY_N - 1)] < t) return false;
    return true;
}

/* Row j of a chunk, fogged: a strip's upper edge, then the next one's
   lower. */
static void out_row(RV *row, int ci, int cj, int j, int step, bool seam) {
    for(int i = ci * CH; i <= ci * CH + CH; i += step, row++) {
        *row = seam ? out_seam_v(i, j, ci, cj, step) : out_v(i, j);
        row->c = out_fog(row->c, row->x, row->y, row->z);
    }
}

int outland_draw(void) {
    if(!ov) return 0;

    V3 cam = r_cam.pos;
    float half = CH * cell * 0.5f, fog_near, fog_far;
    world_fog(&fog_near, &fog_far, &fg.col);
    fg.cam = cam;
    fg.fwd = r_cam.fwd;
    fg.near = fog_near;
    fg.k = fog_far > fog_near ? 256.0f / (fog_far - fog_near) : 0;
    int sent = 0;
    skyline_update(cam);
    for(int cj = 0; cj < CHN; cj++)
        for(int ci = 0; ci < CHN; ci++) {
            float dx = cam.x - (org + ci * CH * cell + half), dz = cam.z - (org + cj * CH * cell + half);
            float d2 = dx * dx + dz * dz;
            bool map = ci * CH >= MAP0 && ci * CH < MAP1 && cj * CH >= MAP0 && cj * CH < MAP1;
            int step = map || d2 < OUT_LOD2_M * OUT_LOD2_M ? 1 : d2 < OUT_LOD4_M * OUT_LOD4_M ? 2 : 4;
            if(!map && step < 4) {
                float ok = OUT_LOD_ERR * fmaxf(sqrtf(d2) - half, 1.0f);
                if(chy[cj][ci].err[1] < ok) step = 4;
                else if(chy[cj][ci].err[0] < ok) step = 2;
            }
            ch_step[cj][ci] = (uint8_t)step;
        }
    r_hdr(h_out);
    for(int cj = 0; cj < CHN; cj++)
        for(int ci = 0; ci < CHN; ci++) {
            if(ci * CH >= MAP0 && ci * CH < MAP1 && cj * CH >= MAP0 && cj * CH < MAP1) continue;   /* the map */
            float lo = chy[cj][ci].lo, hi = chy[cj][ci].hi;
            V3 c = v3(org + ci * CH * cell + half, (lo + hi) * 0.5f, org + cj * CH * cell + half);
            float rad = chy[cj][ci].rad;
            if(!r_sphere_visible(c, rad) || hidden(cam, c.x, c.z, half * 1.42f, hi)) continue;
            int step = ch_step[cj][ci], per = CH / step + 1;
            bool seam = (ci > 0 && ch_step[cj][ci - 1] > step) || (ci + 1 < CHN && ch_step[cj][ci + 1] > step) ||
                        (cj > 0 && ch_step[cj - 1][ci] > step) || (cj + 1 < CHN && ch_step[cj + 1][ci] > step);
            RV rows[2][CH + 1], *lower = rows[0], *upper = rows[1];
            out_row(lower, ci, cj, cj * CH, step, seam);
            for(int j = cj * CH; j < cj * CH + CH; j += step) {
                out_row(upper, ci, cj, j + step, step, seam);
                RV v[2 * (CH + 1)];
                for(int k = 0; k < per; k++) {
                    v[2 * k] = upper[k];
                    v[2 * k + 1] = lower[k];
                }
                r_strip_bounded(v, 2 * per, c, rad);
                sent += 2 * per;
                RV *t = lower;
                lower = upper;
                upper = t;
            }
        }
    /* The skirts down the map's edge, each run at the step of the chunk
       it hangs from. */
    for(int side = 0; side < 4; side++)
        for(int k0 = 0; k0 < SIDE_N; k0 += CH) {
            int i, j;
            side_ij(side, k0, &i, &j);
            int ci = side == 2 ? MAP0 / CH - 1 : side == 3 ? MAP1 / CH : i / CH;
            int cj = side == 0 ? MAP0 / CH - 1 : side == 1 ? MAP1 / CH : j / CH;
            int step = ch_step[cj][ci], lv = step >> 1, i1, j1;
            side_ij(side, k0 + CH, &i1, &j1);
            V3 mid = v3(org + (i + i1) * 0.5f * cell, (chy[cj][ci].lo + chy[cj][ci].hi) * 0.5f, org + (j + j1) * 0.5f * cell);
            if(!r_sphere_visible(mid, chy[cj][ci].rad)) continue;
            RV v[2 * (CH + 1)];
            int n = 0;
            for(int k = k0; k <= k0 + CH; k += step) {
                side_ij(side, k, &i, &j);
                RV t = out_v(i, j);
                t.c = out_fog(t.c, t.x, t.y, t.z);
                v[n++] = t;
                t.y -= skirt[lv][side][k];
                v[n++] = t;
            }
            r_strip(v, n);
            sent += n;
        }
    return sent;
}

/* The rivers, as the map's water looks from afar: its colour over the
   bed, the ripples running. They lie flat: where the fog is at its most
   there is nothing of them to see. */
void outland_draw_tr(void) {
    if(!ov) return;
    V3 cam = r_cam.pos;
    float far = world_fog_far() + 20.0f, scroll = RIVER_FLOW * game.time * RIPPLE_ALONG;
    scroll -= floorf(scroll);
    r_hdr(h_river);
    for(int k = 0; k < nruns; k++)
        for(int p0 = runs[k].first, end = runs[k].first + runs[k].n; p0 + 1 < end; p0 += RIB_RUN) {
            int p1 = p0 + RIB_RUN < end - 1 ? p0 + RIB_RUN : end - 1;
            const OutRib *a = &rib[p0], *b = &rib[p1];
            V3 c = v3((a->xl + b->xr) * 0.5f, a->y, (a->zl + b->zr) * 0.5f);
            float dx = cam.x - c.x, dz = cam.z - c.z, rad = RIB_RUN * cell * 0.75f;
            if(sqrtf(dx * dx + dz * dz) - rad > far || !r_sphere_visible(c, rad) || hidden(cam, c.x, c.z, rad, c.y + 2.0f)) continue;
            RV v[2 * (RIB_RUN + 1)];
            int n = 0;
            for(int p = p0; p <= p1; p++) {
                const OutRib *q = &rib[p];
                /* (the texture from each strip's own start: big coordinates swim on the console) */
                float along = (float)(p - p0) * cell * RIPPLE_ALONG + scroll;
                uint32_t cl = (out_fog(rib_col, q->xl, q->y, q->zl) & 0xffffffu) | (rib_col & 0xff000000u);
                uint32_t cr = (out_fog(rib_col, q->xr, q->y, q->zr) & 0xffffffu) | (rib_col & 0xff000000u);
                v[n++] = (RV){q->xl, q->y, q->zl, 0, along, cl};
                v[n++] = (RV){q->xr, q->y, q->zr, 24.0f * RIPPLE_ACROSS, along, cr};
            }
            r_strip(v, n);
        }
}
