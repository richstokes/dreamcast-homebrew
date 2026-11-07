/* Ridgeline terrain (terrain.h). */
#include "terrain.h"
#include <malloc.h>

extern const uint8_t rl_level_bin[];

const GroundVert *lvl_ground;
const float *lvl_water;
const uint8_t *lvl_surf;

/* Friction from the usual off-road figures: dry rock grips best, mud and
   snow worst; mud also drags the tyres and swallows them. A sliding tyre
   on loose ground keeps nearly all its grip (it ploughs the surface), so
   slides there build gently; dry rock and dirt let go more sharply. */
const SurfProps surf_props[SURF_COUNT] = {
    [SURF_GRASS] = {0.72f, 0.030f, 0.02f, 0.015f, 0.93f, 0xffb8a070, "GRASS"},
    [SURF_FOREST] = {0.68f, 0.035f, 0.03f, 0.02f, 0.93f, 0xff7a6a4a, "FOREST"},
    [SURF_DIRT] = {0.80f, 0.022f, 0.00f, 0.015f, 0.88f, 0xffc0a078, "DIRT"},
    [SURF_GRAVEL] = {0.64f, 0.028f, 0.00f, 0.025f, 0.95f, 0xffc8bca4, "GRAVEL"},
    [SURF_ROCK] = {0.95f, 0.012f, 0.00f, 0.03f, 0.82f, 0xffa8a8a8, "ROCK"},
    [SURF_MUD] = {0.45f, 0.110f, 0.09f, 0.01f, 0.96f, 0xff5c4430, "MUD"},
    [SURF_SCREE] = {0.52f, 0.050f, 0.03f, 0.03f, 0.95f, 0xffb0aaa0, "SCREE"},
    [SURF_SNOW] = {0.33f, 0.040f, 0.06f, 0.005f, 0.95f, 0xfff0f4f8, "SNOW"},
    [SURF_WETROCK] = {0.46f, 0.015f, 0.00f, 0.03f, 0.85f, 0xff8a9496, "WET ROCK"},
};

/* ---- Obstacles ---- */

enum { OB_SPHERE, OB_CAPSULE, OB_BOX, OB_TREE };
#define OB_CELL 8.0f
#define OB_N ((int)(LEVEL_SIZE / OB_CELL) + 1)
static uint16_t *ob_first, *ob_count, *ob_list;
/* Boxes: the inverse turn, cached. */
static float box_c[LVL_BOXES], box_s[LVL_BOXES];

static void ob_bounds(int type, int i, float *x0, float *z0, float *x1, float *z1) {
    switch(type) {
        case OB_SPHERE: {
            const lvl_sphere_t *s = &lvl_spheres[i];
            *x0 = s->x - s->r; *x1 = s->x + s->r; *z0 = s->z - s->r; *z1 = s->z + s->r;
            break;
        }
        case OB_CAPSULE: {
            const lvl_capsule_t *c = &lvl_capsules[i];
            *x0 = fminf(c->ax, c->bx) - c->r; *x1 = fmaxf(c->ax, c->bx) + c->r;
            *z0 = fminf(c->az, c->bz) - c->r; *z1 = fmaxf(c->az, c->bz) + c->r;
            break;
        }
        case OB_BOX: {
            const lvl_box_t *b = &lvl_boxes[i];
            float r = sqrtf(b->hx * b->hx + b->hz * b->hz);
            *x0 = b->x - r; *x1 = b->x + r; *z0 = b->z - r; *z1 = b->z + r;
            break;
        }
        default: {
            const lvl_tree_t *t = &lvl_trees[i];
            *x0 = t->x - 1; *x1 = t->x + 1; *z0 = t->z - 1; *z1 = t->z + 1;
            break;
        }
    }
}

static void ob_grid_build(void) {
    static const int counts[4] = {LVL_SPHERES, LVL_CAPSULES, LVL_BOXES, LVL_TREES};
    ob_first = calloc(OB_N * OB_N, sizeof(uint16_t));
    ob_count = calloc(OB_N * OB_N, sizeof(uint16_t));
    for(int pass = 0; pass < 2; pass++) {
        int total = 0;
        if(pass == 1) {
            for(int c = 0; c < OB_N * OB_N; c++) {
                ob_first[c] = (uint16_t)total;
                total += ob_count[c];
                ob_count[c] = 0;
            }
            ob_list = malloc(sizeof(uint16_t) * (total + 1));
        }
        for(int type = 0; type < 4; type++)
            for(int i = 0; i < counts[type]; i++) {
                float x0, z0, x1, z1;
                ob_bounds(type, i, &x0, &z0, &x1, &z1);
                int i0 = (int)((x0 - LEVEL_ORG) / OB_CELL), i1 = (int)((x1 - LEVEL_ORG) / OB_CELL);
                int j0 = (int)((z0 - LEVEL_ORG) / OB_CELL), j1 = (int)((z1 - LEVEL_ORG) / OB_CELL);
                for(int j = j0; j <= j1; j++)
                    for(int k = i0; k <= i1; k++) {
                        if(k < 0 || j < 0 || k >= OB_N || j >= OB_N) continue;
                        int c = j * OB_N + k;
                        if(pass == 1) ob_list[ob_first[c] + ob_count[c]] = (uint16_t)(type << 13 | i);
                        ob_count[c]++;
                    }
            }
    }
    for(int i = 0; i < LVL_BOXES; i++) {
        box_c[i] = cosf(lvl_boxes[i].yaw);
        box_s[i] = sinf(lvl_boxes[i].yaw);
    }
}

static inline int ob_cell(float x, float z) {
    int i = (int)((x - LEVEL_ORG) / OB_CELL), j = (int)((z - LEVEL_ORG) / OB_CELL);
    if(i < 0 || j < 0 || i >= OB_N || j >= OB_N) return -1;
    return j * OB_N + i;
}

/* A box's local frame: +X along its width (right of the yaw), +Z along it. */
static inline void box_local(int i, float x, float z, float *lx, float *lz) {
    const lvl_box_t *b = &lvl_boxes[i];
    float dx = x - b->x, dz = z - b->z;
    float c = box_c[i], s = box_s[i];
    *lx = dx * c - dz * s;
    *lz = dx * s + dz * c;
}

void terrain_init(void) {
    lvl_ground = (const GroundVert *)(rl_level_bin + LEVEL_BIN_GROUND);
    lvl_water = (const float *)(rl_level_bin + LEVEL_BIN_WATER);
    lvl_surf = rl_level_bin + LEVEL_BIN_SURF;
    ob_grid_build();
    printf("Ridgeline terrain: %d boulders, %d logs, %d ledges, %d trees\n", LVL_SPHERES, LVL_CAPSULES, LVL_BOXES,
           LVL_TREES);
}

/* ---- Queries ---- */

float terrain_height_n(float x, float z, V3 *n) {
    float gx = (x - LEVEL_ORG) * (1.0f / LEVEL_D), gz = (z - LEVEL_ORG) * (1.0f / LEVEL_D);
    gx = clampf(gx, 0, LEVEL_N - 1.001f);
    gz = clampf(gz, 0, LEVEL_N - 1.001f);
    int i = (int)gx, j = (int)gz;
    float fx = gx - i, fz = gz - j;
    const GroundVert *r0 = lvl_ground + j * LEVEL_N + i, *r1 = r0 + LEVEL_N;
    float h00 = r0[0].h, h10 = r0[1].h, h01 = r1[0].h, h11 = r1[1].h;
    /* Split along (i, j)-(i+1, j+1), as the strips are drawn. */
    float dx, dz, h;
    if(fx >= fz) {
        dx = h10 - h00;
        dz = h11 - h10;
    }
    else {
        dx = h11 - h01;
        dz = h01 - h00;
    }
    h = h00 + dx * fx + dz * fz;
    if(n) *n = vnorm(v3(-dx, LEVEL_D, -dz));
    return h;
}

float terrain_height(float x, float z) {
    return terrain_height_n(x, z, NULL);
}

int surface_at(float x, float z) {
    int i = lvl_clampi((int)((x - LEVEL_ORG) / LEVEL_D + 0.5f)), j = lvl_clampi((int)((z - LEVEL_ORG) / LEVEL_D + 0.5f));
    return lvl_surf[j * LEVEL_N + i];
}

float water_at(float x, float z) {
    int i = lvl_clampi((int)((x - LEVEL_ORG) / LEVEL_D + 0.5f)), j = lvl_clampi((int)((z - LEVEL_ORG) / LEVEL_D + 0.5f));
    return lvl_water[j * LEVEL_N + i];
}

float ground_at(float x, float z, V3 *n, int *surf) {
    V3 tn;
    float h = terrain_height_n(x, z, &tn);
    int sf = -1;
    int c = ob_cell(x, z);
    if(c >= 0) {
        const uint16_t *l = ob_list + ob_first[c];
        for(int k = 0; k < ob_count[c]; k++) {
            int type = l[k] >> 13, i = l[k] & 0x1fff;
            float top;
            V3 on;
            if(type == OB_SPHERE) {
                const lvl_sphere_t *s = &lvl_spheres[i];
                float dx = x - s->x, dz = z - s->z, d2 = dx * dx + dz * dz;
                if(d2 >= s->r * s->r) continue;
                float dy = sqrtf(s->r * s->r - d2);
                top = s->y + dy;
                on = v3(dx / s->r, dy / s->r, dz / s->r);
            }
            else if(type == OB_CAPSULE) {
                const lvl_capsule_t *cp = &lvl_capsules[i];
                float ex = cp->bx - cp->ax, ez = cp->bz - cp->az;
                float t = clampf(((x - cp->ax) * ex + (z - cp->az) * ez) / (ex * ex + ez * ez), 0, 1);
                float qx = cp->ax + ex * t, qz = cp->az + ez * t, qy = cp->ay + (cp->by - cp->ay) * t;
                float dx = x - qx, dz = z - qz, d2 = dx * dx + dz * dz;
                if(d2 >= cp->r * cp->r) continue;
                float dy = sqrtf(cp->r * cp->r - d2);
                top = qy + dy;
                on = v3(dx / cp->r, dy / cp->r, dz / cp->r);
            }
            else if(type == OB_BOX) {
                const lvl_box_t *b = &lvl_boxes[i];
                float lx, lz;
                box_local(i, x, z, &lx, &lz);
                if(fabsf(lx) >= b->hx || fabsf(lz) >= b->hz) continue;
                top = b->y + b->hy;
                on = v3(0, 1, 0);
            }
            else continue;
            if(top > h) {
                h = top;
                tn = on;
                sf = SURF_ROCK;
            }
        }
    }
    if(n) *n = tn;
    if(surf) *surf = sf >= 0 ? sf : surface_at(x, z);
    return h;
}

int obstacle_contacts(V3 p, float r, ObsContact *out, int max) {
    int n = 0;
    int c = ob_cell(p.x, p.z);
    if(c < 0) return 0;
    const uint16_t *l = ob_list + ob_first[c];
    for(int k = 0; k < ob_count[c] && n < max; k++) {
        int type = l[k] >> 13, i = l[k] & 0x1fff;
        V3 q;   /* the closest point of the obstacle */
        bool inside = false;
        V3 in_n = v3(0, 1, 0);
        float in_d = 0;
        if(type == OB_SPHERE) {
            const lvl_sphere_t *s = &lvl_spheres[i];
            V3 d = vsub(p, v3(s->x, s->y, s->z));
            float dl = vlen(d);
            if(dl >= s->r + r || dl < 1e-4f) continue;
            out[n++] = (ObsContact){vscale(d, 1.0f / dl), s->r + r - dl};
            continue;
        }
        else if(type == OB_CAPSULE) {
            const lvl_capsule_t *cp = &lvl_capsules[i];
            V3 a = v3(cp->ax, cp->ay, cp->az), e = v3(cp->bx - cp->ax, cp->by - cp->ay, cp->bz - cp->az);
            float t = clampf(vdot(vsub(p, a), e) / vdot(e, e), 0, 1);
            V3 d = vsub(p, vmad(a, e, t));
            float dl = vlen(d);
            if(dl >= cp->r + r || dl < 1e-4f) continue;
            out[n++] = (ObsContact){vscale(d, 1.0f / dl), cp->r + r - dl};
            continue;
        }
        else if(type == OB_BOX) {
            const lvl_box_t *b = &lvl_boxes[i];
            float lx, lz;
            box_local(i, p.x, p.z, &lx, &lz);
            float ly = p.y - b->y;
            float cx = clampf(lx, -b->hx, b->hx), cy = clampf(ly, -b->hy, b->hy), cz = clampf(lz, -b->hz, b->hz);
            if(cx == lx && cy == ly && cz == lz) {
                /* Inside: out through the nearest face. */
                inside = true;
                float fx = b->hx - fabsf(lx), fy = b->hy - ly, fz = b->hz - fabsf(lz);
                V3 ln;
                if(fy <= fx && fy <= fz) { ln = v3(0, 1, 0); in_d = fy; }
                else if(fx <= fz) { ln = v3(signf(lx), 0, 0); in_d = fx; }
                else { ln = v3(0, 0, signf(lz)); in_d = fz; }
                float c0 = box_c[i], s0 = box_s[i];
                in_n = v3(ln.x * c0 + ln.z * s0, ln.y, -ln.x * s0 + ln.z * c0);
                out[n++] = (ObsContact){in_n, in_d + r};
                continue;
            }
            float dx = lx - cx, dy = ly - cy, dz = lz - cz;
            float d2 = dx * dx + dy * dy + dz * dz;
            if(d2 >= r * r || d2 < 1e-8f) continue;
            float dl = sqrtf(d2);
            float c0 = box_c[i], s0 = box_s[i];
            V3 ln = v3(dx / dl, dy / dl, dz / dl);
            out[n++] = (ObsContact){v3(ln.x * c0 + ln.z * s0, ln.y, -ln.x * s0 + ln.z * c0), r - dl};
            continue;
        }
        else {
            const lvl_tree_t *t = &lvl_trees[i];
            if(p.y > t->y + 4.0f || p.y < t->y - 1.0f) continue;
            float tr = 0.30f * t->scale;
            float dx = p.x - t->x, dz = p.z - t->z, d2 = dx * dx + dz * dz;
            if(d2 >= (tr + r) * (tr + r) || d2 < 1e-6f) continue;
            float dl = sqrtf(d2);
            q = v3(dx / dl, 0, dz / dl);
            out[n++] = (ObsContact){q, tr + r - dl};
        }
        (void)inside;
    }
    return n;
}

/* ---- Building the terrain's scenery ---- */

#define CHUNK_CELLS 16
#define CHUNKS ((LEVEL_N - 1) / CHUNK_CELLS)

static inline V3 grid_p(int i, int j) {
    return v3(LEVEL_ORG + i * LEVEL_D, lvl_h(i, j), LEVEL_ORG + j * LEVEL_D);
}

static inline uint32_t grid_c(int i, int j) {
    return lvl_c(i, j);
}

#define TEX_TILE (1.0f / 9.0f)

static void tv(Scenery *sc, int i, int j, bool join) {
    V3 p = grid_p(i, j);
    if(join) sc_join(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, grid_c(i, j));
    else sc_vert(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, grid_c(i, j));
}

/* A chunk's skirt along one edge: the edge's vertices and a copy dropped
   below, hiding the cracks where a coarser neighbour meets it. */
static void skirt(Scenery *sc, int i0, int j0, int di, int dj, int steps, int step, float drop) {
    for(int k = 0; k <= steps; k++) {
        int i = i0 + di * k * step, j = j0 + dj * k * step;
        V3 p = grid_p(i, j);
        uint32_t c = color_scale(grid_c(i, j), 0.82f);
        V3 q = v3(p.x, p.y - drop, p.z);
        if(k == 0) sc_join(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, c);
        else sc_vert(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, c);
        sc_vert(sc, q, p.x * TEX_TILE, p.z * TEX_TILE + drop * TEX_TILE, c);
    }
}

/* Levels of detail by distance from the centre of the 96 m block a chunk
   lies in, so a block's levels switch together and never overlap or leave
   a gap: 3 m and 6 m grids in 48 m chunks near, 12 m and 24 m grids over
   whole blocks further out. The near two are built each frame straight
   from the heightmap (prebuilt, they would take ~5 MB); the far two are
   prebuilt scenery. Skirts hang under every chunk's edges to hide the
   cracks where levels meet. */
#define BLOCK_CELLS 32
#define BLOCKS ((LEVEL_N - 1) / BLOCK_CELLS)
static const float lod_far[4] = {100.0f, 180.0f, 300.0f, 620.0f};
/* The 3 m grid only for chunks whose centre is within this (horizontally). */
#define NEAR_FINE 50.0f
/* The far blocks' skirt chunks: [block][12 m, 24 m][edge: -z, +x, +z, -x]. */
static uint16_t far_skirt[BLOCKS * BLOCKS][2][4];
static V3 block_c[BLOCKS * BLOCKS];
/* Near chunks: a sphere per chunk and per strip at each near level (four
   strips of four rows at 3 m, two of four rows at 6 m). */
#define NCHUNK (BLOCKS * BLOCKS * 4)
typedef struct { V3 c; float r; } Sphere;
static Sphere nc_sphere[NCHUNK], nc_strip[NCHUNK][2][4];
/* Each near level's strips within a chunk: cell offset and size. Squarish
   strips have small spheres, so more of them are wholly on screen and take
   the renderer's fast path (no per-vertex clip tests). */
static const int near_strips[2] = {4, 2};
static const int8_t near_rect[2][4][4] = {
    {{0, 0, 8, 8}, {8, 0, 8, 8}, {0, 8, 8, 8}, {8, 8, 8, 8}},   /* 3 m: 8 x 8 cells, 162 vertices */
    {{0, 0, 16, 8}, {0, 8, 16, 8}},                              /* 6 m: 8 x 4 quads, 78 vertices */
};
static int nc_i0[NCHUNK], nc_j0[NCHUNK];

static Sphere cells_sphere(int i0, int j0, int ni, int nj) {
    float lo = 1e9f, hi = -1e9f;
    for(int j = j0; j <= j0 + nj; j++)
        for(int i = i0; i <= i0 + ni; i++) {
            float h = lvl_h(i, j);
            lo = fminf(lo, h);
            hi = fmaxf(hi, h);
        }
    lo -= 13.0f;   /* the skirts */
    float hx = ni * LEVEL_D * 0.5f, hz = nj * LEVEL_D * 0.5f, hy = (hi - lo) * 0.5f;
    return (Sphere){v3(LEVEL_ORG + (i0 + ni * 0.5f) * LEVEL_D, (lo + hi) * 0.5f, LEVEL_ORG + (j0 + nj * 0.5f) * LEVEL_D),
                    sqrtf(hx * hx + hy * hy + hz * hz) + 0.1f};
}

static void terrain_chunk(Scenery *sc, const pvr_poly_hdr_t *h, int lod, int i0, int j0, int cells, V3 lod_p) {
    int step = 1 << lod, i1 = i0 + cells, j1 = j0 + cells;
    sc_chunk_begin(sc, HF_OP, lod ? lod_far[lod - 1] : 0.0f, lod_far[lod], TAG_TERRAIN);
    sc_chunk_lod_point(sc, lod_p);
    sc_strip_begin(sc, h);
    for(int j = j0, r = 0; j < j1; j += step, r++)
        for(int i = i0; i <= i1; i += step) {
            tv(sc, i, j + step, i == i0 && r != 0);
            tv(sc, i, j, false);
        }
    sc_strip_end(sc);
    sc_chunk_end(sc);
    /* Each edge's skirt is a chunk of its own, drawn only where the block
       beside it has another grid (terrain_draw_far_skirts). */
    float drop = 1.5f * step + 1.0f;
    int steps = cells / step;
    static const int e0[4][4] = {{0, 0, 1, 0}, {1, 0, 0, 1}, {1, 1, -1, 0}, {0, 1, 0, -1}};
    int b = (j0 / BLOCK_CELLS) * BLOCKS + i0 / BLOCK_CELLS;
    for(int e = 0; e < 4; e++) {
        far_skirt[b][lod - 2][e] = sc->chunk_n;
        sc_chunk_begin(sc, HF_OP, lod_far[lod - 1], lod_far[lod], TAG_SKIRT);
        sc_chunk_lod_point(sc, lod_p);
        sc_strip_begin(sc, h);
        skirt(sc, i0 + e0[e][0] * cells, j0 + e0[e][1] * cells, e0[e][2], e0[e][3], steps, step, drop);
        sc_strip_end(sc);
        sc_chunk_end(sc);
    }
}

static const pvr_poly_hdr_t *h_ground;

void terrain_build(Scenery *sc) {
    const pvr_poly_hdr_t *h = h_ground = r_header(TEX_GRASS, HF_OP);
    for(int bj = 0; bj < BLOCKS; bj++)
        for(int bi = 0; bi < BLOCKS; bi++) {
            int i0 = bi * BLOCK_CELLS, j0 = bj * BLOCK_CELLS;
            V3 c = block_c[bj * BLOCKS + bi] = grid_p(i0 + BLOCK_CELLS / 2, j0 + BLOCK_CELLS / 2);
            for(int k = 0; k < 4; k++) {
                int n = (bj * BLOCKS + bi) * 4 + k;
                int ci = i0 + (k & 1) * CHUNK_CELLS, cj = j0 + (k >> 1) * CHUNK_CELLS;
                nc_i0[n] = ci;
                nc_j0[n] = cj;
                nc_sphere[n] = cells_sphere(ci, cj, CHUNK_CELLS, CHUNK_CELLS);
                for(int lod = 0; lod < 2; lod++)
                    for(int s = 0; s < near_strips[lod]; s++) {
                        const int8_t *q = near_rect[lod][s];
                        nc_strip[n][lod][s] = cells_sphere(ci + q[0], cj + q[1], q[2], q[3]);
                    }
            }
            terrain_chunk(sc, h, 2, i0, j0, BLOCK_CELLS, c);
            terrain_chunk(sc, h, 3, i0, j0, BLOCK_CELLS, c);
        }

    /* The water: translucent, its colour deepening with depth. */
    const pvr_poly_hdr_t *hw = r_header(TEX_NONE, HF_TR);
    for(int cj = 0; cj < CHUNKS; cj++)
        for(int ci = 0; ci < CHUNKS; ci++) {
            int i0 = ci * CHUNK_CELLS, j0 = cj * CHUNK_CELLS;
            bool any = false;
            for(int j = j0; j <= j0 + CHUNK_CELLS && !any; j++)
                for(int i = i0; i <= i0 + CHUNK_CELLS; i++)
                    if(lvl_water[lvl_clampi(j) * LEVEL_N + lvl_clampi(i)] > -999.0f) {
                        any = true;
                        break;
                    }
            if(!any) continue;
            sc_chunk_begin(sc, HF_TR, 0, 900.0f, TAG_WATER);
            sc_strip_begin(sc, hw);
            for(int j = j0; j < j0 + CHUNK_CELLS; j++) {
                int run = -1;
                for(int i = i0; i < i0 + CHUNK_CELLS; i++) {
                    bool wet = false;
                    float level = -1000.0f;
                    for(int k = 0; k < 4; k++) {
                        float w = lvl_water[lvl_clampi(j + (k >> 1)) * LEVEL_N + lvl_clampi(i + (k & 1))];
                        if(w > level) level = w;
                    }
                    wet = level > -999.0f;
                    if(!wet) {
                        run = -1;
                        continue;
                    }
                    /* A quad per wet cell, joined into the chunk's strip. */
                    for(int k = 0; k < 4; k++) {
                        int ii = i + (k >> 1), jj = j + ((k & 1) ? 0 : 1);
                        float w = lvl_water[lvl_clampi(jj) * LEVEL_N + lvl_clampi(ii)];
                        float g = lvl_h(ii, jj);
                        float y = w > -999.0f ? w : level - 0.25f;
                        float depth = clampf((y - g) / 1.6f, 0, 1);
                        uint32_t c = color_lerp(0xff9ed2cc, 0xff2e6a74, depth);
                        c = color_alpha(c, 0.42f + 0.38f * depth);
                        V3 p = v3(LEVEL_ORG + ii * LEVEL_D, y, LEVEL_ORG + jj * LEVEL_D);
                        if(k == 0 && run < 0) sc_join(sc, p, 0, 0, c);
                        else if(k >= 2 || run < 0) sc_vert(sc, p, 0, 0, c);
                    }
                    run = i;
                }
            }
            sc_strip_end(sc);
            sc_chunk_end(sc);
        }
}

void terrain_draw_water(const Scenery *sc) {
    /* Few chunks: draw the visible ones far to near. */
    int idx[64], n = 0;
    float d2s[64];
    V3 cam_p = r_cam.pos;
    for(uint32_t i = 0; i < sc->chunk_n && n < 64; i++) {
        const ScChunk *c = &sc->chunks[i];
        if(c->tag != TAG_WATER) continue;
        V3 d = vsub(c->c, cam_p);
        float d2 = vdot(d, d);
        if(d2 >= c->far2 || !r_sphere_visible(c->c, c->r)) continue;
        int k = n++;
        while(k > 0 && d2s[k - 1] < d2) {
            idx[k] = idx[k - 1];
            d2s[k] = d2s[k - 1];
            k--;
        }
        idx[k] = (int)i;
        d2s[k] = d2;
    }
    for(int k = 0; k < n; k++) {
        const ScChunk *c = &sc->chunks[idx[k]];
        for(int s = 0; s < c->nstrips; s++) {
            const ScStrip *t = &sc->strips[c->first_strip + s];
            r_hdr(t->hdr);
            r_strip_bounded(sc->pool + t->first, t->count, t->c, t->r);
        }
    }
}

/* ---- The near terrain, built as it is drawn ---- */

static RV near_buf[512] __attribute__((aligned(32)));

static inline RV near_v(int i, int j) {
    int ci = lvl_clampi(i), cj = lvl_clampi(j);
    float x = LEVEL_ORG + i * LEVEL_D, z = LEVEL_ORG + j * LEVEL_D;
    const GroundVert *g = &lvl_ground[cj * LEVEL_N + ci];
    return (RV){x, g->h, z, x * TEX_TILE, z * TEX_TILE, g->c};
}

/* The near chunks' grids this frame, on the 16 x 16 chunk grid: -1 for
   chunks drawn from the prebuilt far blocks. */
static int8_t chunk_lod[BLOCKS * 2][BLOCKS * 2];

#ifdef RL_AUTOTEST
int ter_count[4];   /* vertices a frame: near 3 m, near 6 m, skirts, far (world.c) */
#define TER_COUNT(i, n) (ter_count[i] += (n))
#else
#define TER_COUNT(i, n) ((void)0)
#endif

static void near_chunk(int n, int lod, int cgx, int cgz) {
    int step = 1 << lod, i0 = nc_i0[n], j0 = nc_j0[n];
    for(int s = 0; s < near_strips[lod]; s++) {
        const Sphere *sp = &nc_strip[n][lod][s];
        if(!r_sphere_visible(sp->c, sp->r)) continue;
        const int8_t *q = near_rect[lod][s];
        int a0 = i0 + q[0], a1 = a0 + q[2], b0 = j0 + q[1], b1 = b0 + q[3];
        int k = 0;
        for(int j = b0; j < b1; j += step) {
            if(k) {
                near_buf[k] = near_buf[k - 1];
                k++;
                near_buf[k++] = near_v(a0, j + step);
            }
            for(int i = a0; i <= a1; i += step) {
                near_buf[k++] = near_v(i, j + step);
                near_buf[k++] = near_v(i, j);
            }
        }
        r_strip_bounded(near_buf, k, sp->c, sp->r);
        TER_COUNT(lod, k);
    }
    /* Skirts on the edges whose neighbour has another grid: a copy of the
       edge dropped below it. Same-grid neighbours share their edge exactly. */
    const Sphere *sp = &nc_sphere[n];
    float drop = 1.5f * step + 1.0f;
    int k = 0;
    static const int dir[4][4] = {{0, 0, 1, 0}, {1, 0, 0, 1}, {1, 1, -1, 0}, {0, 1, 0, -1}};
    static const int nb[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    for(int e = 0; e < 4; e++) {
        int nx = cgx + nb[e][0], nz = cgz + nb[e][1];
        if(nx >= 0 && nz >= 0 && nx < BLOCKS * 2 && nz < BLOCKS * 2 && chunk_lod[nz][nx] == lod) continue;
        int ei = i0 + dir[e][0] * CHUNK_CELLS, ej = j0 + dir[e][1] * CHUNK_CELLS;
        for(int q = 0; q <= CHUNK_CELLS / step; q++) {
            RV v = near_v(ei + dir[e][2] * q * step, ej + dir[e][3] * q * step);
            v.c = color_scale(v.c, 0.82f);
            if(q == 0 && k) {
                near_buf[k] = near_buf[k - 1];
                k++;
                near_buf[k++] = v;
            }
            near_buf[k++] = v;
            v.y -= drop;
            v.v += drop * TEX_TILE;
            near_buf[k++] = v;
        }
    }
    if(k) r_strip_bounded(near_buf, k, sp->c, sp->r);
    TER_COUNT(2, k);
}

int terrain_draw_near(void) {
    V3 cam_p = r_cam.pos;
    int drawn = 0;
    r_hdr(h_ground);
    /* Each near chunk's grid first, so skirts know their neighbours'. */
    for(int b = 0; b < BLOCKS * BLOCKS; b++) {
        V3 d = vsub(block_c[b], cam_p);
        bool near = vdot(d, d) < lod_far[1] * lod_far[1];
        for(int k = 0; k < 4; k++) {
            int n = b * 4 + k;
            int cgx = (b % BLOCKS) * 2 + (k & 1), cgz = (b / BLOCKS) * 2 + (k >> 1);
            /* Within a near block each chunk picks its own grid. */
            V3 dc = vsub(nc_sphere[n].c, cam_p);
            float dc2 = dc.x * dc.x + dc.z * dc.z;
            chunk_lod[cgz][cgx] = !near ? -1 : dc2 < NEAR_FINE * NEAR_FINE ? 0 : 1;
        }
    }
    for(int b = 0; b < BLOCKS * BLOCKS; b++)
        for(int k = 0; k < 4; k++) {
            int cgx = (b % BLOCKS) * 2 + (k & 1), cgz = (b / BLOCKS) * 2 + (k >> 1);
            int lod = chunk_lod[cgz][cgx];
            int n = b * 4 + k;
            if(lod < 0 || !r_sphere_visible(nc_sphere[n].c, nc_sphere[n].r)) continue;
            near_chunk(n, lod, cgx, cgz);
            drawn++;
        }
    return drawn;
}

/* The far blocks' skirts, only on edges whose neighbour block has another
   grid: between two blocks at one grid the edge vertices match exactly,
   and those skirts were half of a 24 m block's vertices. */
int terrain_draw_far_skirts(const Scenery *sc) {
    int8_t lod[BLOCKS * BLOCKS];
    V3 cam_p = r_cam.pos;
    for(int b = 0; b < BLOCKS * BLOCKS; b++) {
        V3 d = vsub(block_c[b], cam_p);
        float d2 = vdot(d, d);
        lod[b] = d2 < lod_far[1] * lod_far[1] ? 1 : d2 < lod_far[2] * lod_far[2] ? 2 : d2 < lod_far[3] * lod_far[3] ? 3 : -1;
    }
    static const int nb[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    int sent = 0;
    for(int b = 0; b < BLOCKS * BLOCKS; b++) {
        if(lod[b] < 2) continue;
        int bi = b % BLOCKS, bj = b / BLOCKS;
        for(int e = 0; e < 4; e++) {
            int ni = bi + nb[e][0], nj = bj + nb[e][1];
            if(ni >= 0 && nj >= 0 && ni < BLOCKS && nj < BLOCKS && lod[nj * BLOCKS + ni] == lod[b]) continue;
            sent += sc_draw_chunk(sc, far_skirt[b][lod[b] - 2][e]);
        }
    }
    return sent;
}
