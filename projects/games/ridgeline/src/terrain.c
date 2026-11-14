/* Ridgeline terrain (terrain.h). */
#include "terrain.h"
#include "water.h"
#include <malloc.h>

const GroundVert *lvl_ground;
const float *lvl_water;
const uint8_t *lvl_surf;

/* The loaded area's grid (level.h). The clock relights the ground's
   colours in place (terrain_relight) from each vertex's albedo, normal
   and depth under water, and the horizon map. */
static GroundVert *ground_ram;
static const LvlLight *lvl_light;
static const uint8_t *lvl_hz;
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
    /* The desert's: dry sand drags and swallows the tyres but ploughs
       rather than lets go; slickrock is sandstone as rough as sandpaper,
       the best grip there is, and lets go all at once. */
    [SURF_SAND] = {0.58f, 0.085f, 0.07f, 0.01f, 0.96f, 0xffd8a070, "SAND"},
    [SURF_SLICKROCK] = {1.05f, 0.011f, 0.00f, 0.02f, 0.80f, 0xffd09870, "SLICKROCK"},
    /* The dunes': sand packed and damp is as good as a dirt road; dry
       and deep (a slip face, a bowl, anywhere off the beaten track on a
       big dune) it takes the tyres to the rims and gives half the push
       back: one in five is about the steepest it can be climbed from a
       crawl with every wheel driving, where plain sand allows nearly
       one in two, and on the level it costs a third of the pace. */
    [SURF_HARDPACK] = {0.76f, 0.026f, 0.01f, 0.012f, 0.90f, 0xffc8bca4, "PACKED SAND"},
    [SURF_DEEPSAND] = {0.36f, 0.210f, 0.16f, 0.008f, 0.97f, 0xffd8ccb4, "DEEP SAND"},
};

/* ---- Obstacles ---- */

enum { OB_SPHERE, OB_CAPSULE, OB_BOX, OB_TREE };
#define OB_CELL 8.0f
#define OB_N ((int)(LEVEL_SIZE / OB_CELL) + 1)
static uint16_t *ob_first, *ob_count, *ob_list;
/* Boxes: the inverse turn, cached. */
static float box_c[LVL_MAX_BOXES], box_s[LVL_MAX_BOXES];

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
    const int counts[4] = {LVL_SPHERES, LVL_CAPSULES, LVL_BOXES, LVL_TREES};
    if(!ob_first) ob_first = malloc(OB_N * OB_N * sizeof(uint16_t));
    if(!ob_count) ob_count = malloc(OB_N * OB_N * sizeof(uint16_t));
    memset(ob_first, 0, OB_N * OB_N * sizeof(uint16_t));
    memset(ob_count, 0, OB_N * OB_N * sizeof(uint16_t));
    free(ob_list);
    ob_list = NULL;
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

/* Which diagonal splits a cell of the ground in two. A ridge that runs
   across the split is drawn as a row of teeth, a vertex up and a vertex
   down along it; one that runs with it is an edge. A cell is split along
   (i, j)-(i+1, j+1) unless the other two corners are nearer each other's
   height (by more than the slack, so level ground does not dither), where
   an area asks for it (LvlLook.fit_split): the dunes, whose crests and
   slip faces are all edges. Every grid the ground is drawn on and the
   height under a wheel split a cell the same way.

   In a strip the vertices come in pairs, one from each of two rows, and
   which of a pair goes first picks the diagonal of the cell before it;
   to change it the vertex two back goes in again (a triangle of no
   area).

   The 3 m and 6 m grids are built every frame, so their cells' splits are
   worked out once, at load: a bit a cell, set where it is crossed, a byte
   for each near tile's row of eight 3 m cells and for each pair of tiles'
   row of eight 6 m ones. */
#define FIT_SLACK 0.04f    /* m of height a metre of cell */
static bool ter_fit;                 /* the area asks for it */
static float ter_slack = 1e9f;       /* FIT_SLACK, or so much that no cell is crossed */
#define XCELLS (LEVEL_N - 1)
static uint8_t ter_x3[XCELLS][XCELLS / 8], ter_x6[XCELLS / 2][XCELLS / 16];

static inline bool cell_cross(float h00, float h10, float h01, float h11, float slack) {
    return fabsf(h10 - h01) + slack < fabsf(h00 - h11);
}

static void cross_build(void) {
    memset(ter_x3, 0, sizeof(ter_x3));
    memset(ter_x6, 0, sizeof(ter_x6));
    for(int step = 1; step <= 2; step++)
        for(int j = 0; j < XCELLS; j += step)
            for(int i = 0; i < XCELLS; i += step) {
                if(!cell_cross(lvl_h(i, j), lvl_h(i + step, j), lvl_h(i, j + step), lvl_h(i + step, j + step),
                               ter_slack * LEVEL_D * step))
                    continue;
                if(step == 1) ter_x3[j][i >> 3] |= 1 << (i & 7);
                else ter_x6[j >> 1][i >> 4] |= 1 << (i >> 1 & 7);
            }
}

/* The cells' splits along a row of a near strip: bit c for the c'th cell
   from a0, cells of them, on the grid of step cells. */
static inline unsigned cross_bits(int a0, int j, int cells, int step) {
    unsigned b = step == 1 ? ter_x3[j][a0 >> 3] : (unsigned)ter_x6[j >> 1][a0 >> 4] >> (a0 >> 1 & 7);
    return b & ((1u << cells) - 1);
}

bool terrain_load(int area, void *buf) {
    LvlGrid g;
    if(!level_load(area, buf, &g)) {
        printf("Ridgeline terrain: area %d's grid is damaged\n", area);
        return false;
    }
    lvl_ground = ground_ram = g.ground;
    lvl_water = g.water;
    lvl_surf = g.surf;
    lvl_light = g.light;
    lvl_hz = g.hz;
    ter_fit = look->fit_split;
    ter_slack = ter_fit ? FIT_SLACK : 1e9f;
    cross_build();
    ob_grid_build();
    printf("Ridgeline terrain: %s, %d boulders, %d logs, %d ledges, %d trees\n", lvl->name, LVL_SPHERES, LVL_CAPSULES,
           LVL_BOXES, LVL_TREES);
    return true;
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
    /* Split as the strips are drawn (ter_x3). */
    float dx, dz, h;
    if(ter_x3[j][i >> 3] >> (i & 7) & 1) {
        if(fx + fz <= 1.0f) {
            dx = h10 - h00;
            dz = h01 - h00;
            h = h00 + dx * fx + dz * fz;
        }
        else {
            dx = h11 - h01;
            dz = h11 - h10;
            h = h10 + h01 - h11 + dx * fx + dz * fz;
        }
    }
    else {
        if(fx >= fz) {
            dx = h10 - h00;
            dz = h11 - h10;
        }
        else {
            dx = h11 - h01;
            dz = h01 - h00;
        }
        h = h00 + dx * fx + dz * fz;
    }
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
    return ground_below(x, z, 1e9f, n, surf);
}

float ground_below(float x, float z, float ymax, V3 *n, int *surf) {
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
            if(top > h && top <= ymax) {
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

/* Skirts hide the cracks where two grids meet: along a shared edge the
   finer one's extra vertices leave the coarser one's straight line. On
   rolling ground a couple of metres of skirt covers that; across a cliff
   the two can part by the cliff's height. So where an area has cliffs
   each vertex knows how far below it the lowest ground lies within the
   coarsest grid's step along the grid lines (half metres), and its skirt
   hangs that much further. */
static uint8_t *edge_drop;

static void edge_drop_build(void) {
    static uint8_t *room;
    if(lvl_area == AREA_CASCADE) {   /* no cliffs: its skirts stay as they were measured */
        edge_drop = NULL;
        return;
    }
    if(!room) room = spent_alloc(LEVEL_N * LEVEL_N);
    edge_drop = room;
    if(!edge_drop) {
        rl_no_memory("the far ground's skirts");
        return;
    }
    for(int j = 0; j < LEVEL_N; j++)
        for(int i = 0; i < LEVEL_N; i++) {
            float h = lvl_h(i, j), lo = h;
            for(int d = -8; d <= 8; d++) {
                lo = fminf(lo, lvl_h(i + d, j));
                lo = fminf(lo, lvl_h(i, j + d));
            }
            float v = (h - lo) * 2.0f;
            edge_drop[j * LEVEL_N + i] = (uint8_t)(v > 255.0f ? 255.0f : v);
        }
}

static inline float skirt_drop(int i, int j, float base) {
    return edge_drop ? base + edge_drop[lvl_clampi(j) * LEVEL_N + lvl_clampi(i)] * 0.5f : base;
}

/* Where each terrain pool vertex gets its colour when relit: a grid
   vertex, a grid vertex darkened for a skirt, or the water at a depth. */
enum { SRC_GRID, SRC_SKIRT, SRC_WATER };
/* The table is kept for good at more than any area needs (Loch Lowland's
   far ground and water are ~53,000 vertices), in the room the textures
   were linked into. (Sized to the whole pool and cut back each time, it
   wanted a megabyte, which is more than a second area's load has free:
   the water went missing and the far ground was lit from garbage.) */
#define TER_SRC_MAX 64000
static uint32_t *ter_src;
static uint32_t ter_src_n, ter_marked;
static bool ter_src_short;

static void src_mark(const Scenery *sc, uint32_t src) {
    if(ter_src_short) return;
    if(!ter_src || sc->pool_n > TER_SRC_MAX) {
        ter_src_short = true;
        rl_no_memory("the far ground's relighting");
        return;
    }
    uint32_t n = sc->pool_n;
    /* A join appends the previous vertex again before this one twice. */
    for(uint32_t i = ter_marked; i < n; i++) ter_src[i] = src;
    if(n - ter_marked == 3) ter_src[ter_marked] = ter_src[ter_marked - 1];
    ter_marked = n;
}

/* The grid vertex a far block's vertex takes its colour from: itself,
   unless a cliff's face passes within half a step of it. A coarse grid
   draws a cliff as one ramp between two of its vertices, and coloured as
   the bench above and the talus below it would fade from one to the
   other; from the face's own vertices it stays a wall. */
static int far_colour_src(int i, int j, int step) {
    int best = lvl_clampi(j) * LEVEL_N + lvl_clampi(i), bd = 1 << 30, r = step / 2;
    if(lvl_light[best].flags & LVL_FACE) return best;
    for(int dj = -r; dj <= r; dj++)
        for(int di = -r; di <= r; di++) {
            int a = i + di, b = j + dj;
            if(a < 0 || b < 0 || a >= LEVEL_N || b >= LEVEL_N) continue;
            int d = di * di + dj * dj;
            if(d < bd && (lvl_light[b * LEVEL_N + a].flags & LVL_FACE)) {
                bd = d;
                best = b * LEVEL_N + a;
            }
        }
    return best;
}

static int tv_step;   /* the grid step of the far block being built */

static void tv(Scenery *sc, int i, int j, bool join) {
    V3 p = grid_p(i, j);
    int src = far_colour_src(i, j, tv_step);
    if(join) sc_join(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, lvl_ground[src].c);
    else sc_vert(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, lvl_ground[src].c);
    src_mark(sc, SRC_GRID << 30 | (uint32_t)src);
}

/* A chunk's skirt along one edge: the edge's vertices and a copy dropped
   below, hiding the cracks where a coarser neighbour meets it. */
static void skirt(Scenery *sc, int i0, int j0, int di, int dj, int steps, int step, float drop) {
    for(int k = 0; k <= steps; k++) {
        int i = i0 + di * k * step, j = j0 + dj * k * step;
        V3 p = grid_p(i, j);
        /* A skirt hung down a cliff shows beside the face: its colour
           from the face, as the block's vertices there. */
        int src = far_colour_src(i, j, step);
        uint32_t c = color_scale(lvl_ground[src].c, 0.82f);
        float dr = skirt_drop(i, j, drop);
        V3 q = v3(p.x, p.y - dr, p.z);
        if(k == 0) sc_join(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, c);
        else sc_vert(sc, p, p.x * TEX_TILE, p.z * TEX_TILE, c);
        sc_vert(sc, q, p.x * TEX_TILE, p.z * TEX_TILE + dr * TEX_TILE, c);
        src_mark(sc, SRC_SKIRT << 30 | (uint32_t)src);
    }
}

/* Levels of detail by distance from the centre of the 96 m block a chunk
   lies in, so a block's levels switch together and never overlap or leave
   a gap: 3 m and 6 m grids in 24 m tiles near, 12 m and 24 m grids over
   whole blocks further out. The near two are built each frame straight
   from the heightmap (prebuilt, they would take ~5 MB); the far two are
   prebuilt scenery. Skirts hang under the edges where levels meet to hide
   the cracks. */
#define BLOCK_CELLS 32
#define BLOCKS ((LEVEL_N - 1) / BLOCK_CELLS)
static float lod_far[4] = {100.0f, 180.0f, 300.0f, 620.0f};
/* The 3 m grid for tiles whose centre is within NEAR_FINE (horizontally),
   and out to TRAIL_FINE for tiles a route runs through: a trail is two
   cells wide, and on the 6 m grid its cut into a slope turned into long
   triangles slashed across the hill. */
#define NEAR_FINE 50.0f
#define TRAIL_FINE 100.0f
/* The far blocks' chunks, [block][12 m, 24 m], and their skirts' on each
   edge (-z, +x, +z, -x): drawn by index, as the scenery's own walk reads
   every chunk's record to find the few in range. */
static uint16_t far_chunk[BLOCKS * BLOCKS][2];
static uint16_t far_skirt[BLOCKS * BLOCKS][2][4];
static V3 block_c[BLOCKS * BLOCKS];
/* Near chunks (48 m, four tiles each): a sphere per chunk, per tile, and
   per pair of tiles side by side, which the 6 m grid draws as one strip
   where both have it. Squarish strips have small spheres, so more of them
   are wholly on screen and take the renderer's fast path (no per-vertex
   clip tests). A tile is 162 vertices at 3 m, 46 at 6 m; a pair 78. */
#define NCHUNK (BLOCKS * BLOCKS * 4)
#define TILE_CELLS 8
#define TILES (BLOCKS * 4)
typedef struct { V3 c; float r; } Sphere;
static Sphere nc_sphere[NCHUNK], tile_sp[TILES * TILES], pair_sp[TILES * TILES / 2];
static uint8_t tile_trail[TILES * TILES];

static Sphere cells_sphere(int i0, int j0, int ni, int nj) {
    float lo = 1e9f, hi = -1e9f, deep = 0;
    for(int j = j0; j <= j0 + nj; j++)
        for(int i = i0; i <= i0 + ni; i++) {
            float h = lvl_h(i, j);
            lo = fminf(lo, h);
            hi = fmaxf(hi, h);
            deep = fmaxf(deep, skirt_drop(i, j, 0));
        }
    lo -= 13.0f + deep;   /* the skirts */
    float hx = ni * LEVEL_D * 0.5f, hz = nj * LEVEL_D * 0.5f, hy = (hi - lo) * 0.5f;
    return (Sphere){v3(LEVEL_ORG + (i0 + ni * 0.5f) * LEVEL_D, (lo + hi) * 0.5f, LEVEL_ORG + (j0 + nj * 0.5f) * LEVEL_D),
                    sqrtf(hx * hx + hy * hy + hz * hz) + 0.1f};
}

static void terrain_chunk(Scenery *sc, const pvr_poly_hdr_t *h, int lod, int i0, int j0, int cells, V3 lod_p) {
    int step = 1 << lod, i1 = i0 + cells, j1 = j0 + cells;
    int b = (j0 / BLOCK_CELLS) * BLOCKS + i0 / BLOCK_CELLS;
    far_chunk[b][lod - 2] = sc->chunk_n;
    sc_chunk_begin(sc, HF_OP, lod ? lod_far[lod - 1] : 0.0f, lod_far[lod], TAG_TERRAIN);
    sc_chunk_lod_point(sc, lod_p);
    sc_strip_begin(sc, h);
    tv_step = step;
    float slack = ter_slack * LEVEL_D * step;
    for(int j = j0, r = 0; j < j1; j += step, r++) {
        /* low: the pairs go in lower row first, crossing the cell behind
           (cell_cross). */
        int low = cell_cross(lvl_h(i0, j), lvl_h(i0 + step, j), lvl_h(i0, j + step), lvl_h(i0 + step, j + step), slack);
        for(int i = i0; i <= i1; i += step) {
            if(i > i0 &&
               low != cell_cross(lvl_h(i - step, j), lvl_h(i, j), lvl_h(i - step, j + step), lvl_h(i, j + step), slack)) {
                tv(sc, i - step, low ? j : j + step, false);
                low ^= 1;
            }
            tv(sc, i, low ? j : j + step, i == i0 && r != 0);
            tv(sc, i, low ? j + step : j, false);
        }
    }
    sc_strip_end(sc);
    sc_chunk_end(sc);
    /* Each edge's skirt is a chunk of its own, drawn only where the block
       beside it has another grid (terrain_draw_far). */
    float drop = 1.5f * step + 1.0f;
    int steps = cells / step;
    static const int e0[4][4] = {{0, 0, 1, 0}, {1, 0, 0, 1}, {1, 1, -1, 0}, {0, 1, 0, -1}};
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
    const pvr_poly_hdr_t *h = h_ground = r_header(look->ground_tex, HF_OP);
    /* The mountain hides its own far side; the other areas are seen whole
       from their heights, so their ground is drawn out to the fog. The
       Highlands have no forest to pay for and crags to show across a
       corrie, so their finer grids reach further too. */
    lod_far[1] = look->near_m;
    lod_far[2] = look->mid_m;
    lod_far[3] = look->far_m;
    edge_drop_build();
    if(!ter_src) ter_src = spent_alloc(sizeof(uint32_t) * TER_SRC_MAX);
    ter_marked = sc->pool_n;
    ter_src_short = false;
    memset(tile_trail, 0, sizeof(tile_trail));
    for(int tj = 0; tj < TILES; tj++)
        for(int ti = 0; ti < TILES; ti++) {
            tile_sp[tj * TILES + ti] = cells_sphere(ti * TILE_CELLS, tj * TILE_CELLS, TILE_CELLS, TILE_CELLS);
            if(!(ti & 1))
                pair_sp[(tj * TILES + ti) / 2] = cells_sphere(ti * TILE_CELLS, tj * TILE_CELLS, TILE_CELLS * 2, TILE_CELLS);
        }
    /* The tiles under each route and its cut banks: within this of a
       point (they are ~4 m apart, so the circles leave no gaps). */
    const float ts = TILE_CELLS * LEVEL_D;
    for(int r = 0; r < LVL_ROUTES; r++)
        for(int k = 0; k < lvl_routes[r].n; k++) {
            const lvl_pt_t *q = &lvl_routes[r].pts[k];
            float pad = lvl_routes[r].half + 6.0f, x = q->x - LEVEL_ORG, z = q->z - LEVEL_ORG;
            for(int j = (int)((z - pad) / ts); j <= (int)((z + pad) / ts); j++)
                for(int i = (int)((x - pad) / ts); i <= (int)((x + pad) / ts); i++) {
                    if(i < 0 || j < 0 || i >= TILES || j >= TILES) continue;
                    float dx = x - clampf(x, i * ts, (i + 1) * ts), dz = z - clampf(z, j * ts, (j + 1) * ts);
                    if(dx * dx + dz * dz < pad * pad) tile_trail[j * TILES + i] = 1;
                }
        }
    for(int bj = 0; bj < BLOCKS; bj++)
        for(int bi = 0; bi < BLOCKS; bi++) {
            int i0 = bi * BLOCK_CELLS, j0 = bj * BLOCK_CELLS;
            V3 c = block_c[bj * BLOCKS + bi] = grid_p(i0 + BLOCK_CELLS / 2, j0 + BLOCK_CELLS / 2);
            for(int k = 0; k < 4; k++) {
                int n = (bj * BLOCKS + bi) * 4 + k;
                int ci = i0 + (k & 1) * CHUNK_CELLS, cj = j0 + (k >> 1) * CHUNK_CELLS;
                nc_sphere[n] = cells_sphere(ci, cj, CHUNK_CELLS, CHUNK_CELLS);
            }
            terrain_chunk(sc, h, 2, i0, j0, BLOCK_CELLS, c);
            terrain_chunk(sc, h, 3, i0, j0, BLOCK_CELLS, c);
        }

    water_build(sc, src_mark, SRC_WATER << 30);
    /* Without its table the far ground and the water keep the light they
       were built in, and the water is not drawn (water.c). */
    ter_src_n = ter_src_short ? 0 : sc->pool_n;
}

/* ---- Relighting ---- */

/* The light a relight pass is for: the sky's and the sun's weight per
   channel, the sun's direction, and where it sits in the horizon map. */
static struct {
    float sky[3], sun[3];
    V3 L;
    const uint8_t *hz0, *hz1;   /* the two bearings either side of the sun's */
    float hz_t;                 /* and how far between them */
    float el2;                  /* the sun's elevation in the map's half-degrees */
    float water_k;
} rlt;

void terrain_light(const Palette *p, float sky[3], float sun[3]) {
    /* The ground was painted a little flatter than the trees: the same
       sky and sun, scaled to the level tool's weights. */
    static const float sun_scale[3] = {0.674f, 0.717f, 0.784f};
    /* Wet ground is darker ground: turf, peat and rock alike. */
    float wet = 1.0f - 0.2f * p->wet;
    for(int i = 0; i < 3; i++) {
        sky[i] = 0.96f * wet * (p->amb_k[i] + p->sky_k[i]);
        sun[i] = p->sun_k[i] * 1.15f * wet * sun_scale[i];
    }
}

void terrain_relight_setup(const Palette *p, V3 light) {
    terrain_light(p, rlt.sky, rlt.sun);
    rlt.L = light;
    rlt.water_k = p->water_k;
    water_relight_begin();
    float az = atan2f(light.x, light.z) * 180.0f / ENG_PI;
    if(az < 0) az += 360.0f;
    float u = clampf((az - LEVEL_HZ_AZ0) / LEVEL_HZ_STEP, 0, LEVEL_HZ_DIRS - 1.001f);
    int d = (int)u;
    rlt.hz_t = u - d;
    rlt.hz0 = lvl_hz + d * LEVEL_HZ_N * LEVEL_HZ_N;
    rlt.hz1 = rlt.hz0 + LEVEL_HZ_N * LEVEL_HZ_N;
    rlt.el2 = asinf(clampf(light.y, -1, 1)) * (360.0f / ENG_PI);
}

/* The skyline's elevation toward the sun from grid vertex (i, j): the map
   is on every other vertex, so the odd ones take the mean of their
   neighbours'. */
static inline float horizon_at(int i, int j) {
    int hi = i >> 1, hj = j >> 1;
    int hi1 = (i & 1) && hi + 1 < LEVEL_HZ_N ? hi + 1 : hi, hj1 = (j & 1) && hj + 1 < LEVEL_HZ_N ? hj + 1 : hj;
    int a = hj * LEVEL_HZ_N + hi, b = hj * LEVEL_HZ_N + hi1, c = hj1 * LEVEL_HZ_N + hi, d = hj1 * LEVEL_HZ_N + hi1;
    float h0 = 0.25f * (rlt.hz0[a] + rlt.hz0[b] + rlt.hz0[c] + rlt.hz0[d]);
    float h1 = 0.25f * (rlt.hz1[a] + rlt.hz1[b] + rlt.hz1[c] + rlt.hz1[d]);
    return h0 + (h1 - h0) * rlt.hz_t;
}

void terrain_relight(int from, int to) {
    /* Under the water the bed tints toward the water's colour. */
    const float wet[3] = {look->wet[0] * rlt.water_k, look->wet[1] * rlt.water_k, look->wet[2] * rlt.water_k};
    int i = from % LEVEL_N, j = from / LEVEL_N;
    for(int idx = from; idx < to; idx++) {
        const LvlLight *l = &lvl_light[idx];
        float nx = l->nx * (1.0f / 127.0f), nz = l->nz * (1.0f / 127.0f);
        float ny = sqrtf(fmaxf(1.0f - nx * nx - nz * nz, 0.0f));
        float d = nx * rlt.L.x + ny * rlt.L.y + nz * rlt.L.z;
        if(d > 0) {
            /* In a hill's shadow where the sun sits under the skyline;
               soft over 3 degrees. */
            float sh = clampf((rlt.el2 - horizon_at(i, j)) * (1.0f / 6.0f) + 0.5f, 0, 1);
            d *= sh;
        }
        else d = 0;
        float up = 0.55f + 0.45f * ny;
        float deep = l->deep * (0.5f / 255.0f), dry = 1.0f - deep;
        int r = (int)(l->r * (rlt.sky[0] * up + rlt.sun[0] * d) * dry + wet[0] * deep);
        int g = (int)(l->g * (rlt.sky[1] * up + rlt.sun[1] * d) * dry + wet[1] * deep);
        int b = (int)(l->b * (rlt.sky[2] * up + rlt.sun[2] * d) * dry + wet[2] * deep);
        if(r > 255) r = 255;
        if(g > 255) g = 255;
        if(b > 255) b = 255;
        ground_ram[idx].c = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
        if(++i == LEVEL_N) {
            i = 0;
            j++;
        }
    }
}

int terrain_pool_n(void) {
    return (int)ter_src_n;
}

const uint32_t *terrain_pool_src(void) {
    return ter_src_short ? NULL : ter_src;
}

int terrain_water_n(void) {
    return water_wet_n();
}

void terrain_relight_water(int from, int to) {
    for(int i = from; i < to; i++) water_relit((uint32_t)i, rlt.water_k);
}

void terrain_relight_pool(Scenery *sc, int from, int to) {
    RV *pool = sc->pool;
    for(int i = from; i < to; i++) {
        uint32_t s = ter_src[i], v = s & 0x3fffffffu;
        switch(s >> 30) {
            case SRC_GRID: pool[i].c = ground_ram[v].c; break;
            case SRC_SKIRT: pool[i].c = color_scale(ground_ram[v].c, 0.82f); break;
            default: pool[i].c = water_relit(v, rlt.water_k); break;
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

/* The near tiles' grids this frame: 0 for 3 m, 1 for 6 m, -1 for tiles
   drawn from the prebuilt far blocks. */
static int8_t tile_lod[TILES][TILES];

#ifdef RL_AUTOTEST
int ter_count[4];   /* vertices a frame: near 3 m, near 6 m, skirts, far (world.c) */
#define TER_COUNT(i, n) (ter_count[i] += (n))
#else
#define TER_COUNT(i, n) ((void)0)
#endif

/* near_cells where cells are split the way their heights lie. */
static void near_cells_fit(int a0, int b0, int ni, int nj, int step, const Sphere *sp) {
    int a1 = a0 + ni, b1 = b0 + nj;
    int cells = ni / step;
    RV *p = near_buf;
    for(int j = b0; j < b1; j += step) {
        /* low: as terrain_chunk's. A pair's upper vertex is p[low]. */
        unsigned bits = cross_bits(a0, j, cells, step);
        int low = bits & 1;
        if(p != near_buf) {
            *p = p[-1];
            p++;
            *p++ = near_v(a0, low ? j : j + step);
        }
        if(bits == 0 || bits == (1u << cells) - 1) {
            /* Most rows: split one way from end to end. */
            RV *pu = p + low, *pl = p + (low ^ 1);
            for(int i = a0; i <= a1; i += step, pu += 2, pl += 2) {
                *pu = near_v(i, j + step);
                *pl = near_v(i, j);
            }
            p += 2 * (cells + 1);
        }
        else {
            p[low] = near_v(a0, j + step);
            p[low ^ 1] = near_v(a0, j);
            p += 2;
            for(int i = a0 + step; i <= a1; i += step, p += 2, bits >>= 1) {
                if((int)(bits & 1) != low) {
                    *p = p[-2];
                    p++;
                    low ^= 1;
                }
                p[low] = near_v(i, j + step);
                p[low ^ 1] = near_v(i, j);
            }
        }
    }
    int k = (int)(p - near_buf);
    r_strip_bounded(near_buf, k, sp->c, sp->r);
    TER_COUNT(step >> 1, k);
}

/* A strip of cells [a0, a0 + ni) x [b0, b0 + nj) on a grid of step cells. */
static void near_cells(int a0, int b0, int ni, int nj, int step, const Sphere *sp) {
    if(!r_sphere_visible(sp->c, sp->r)) return;
    if(ter_fit) {
        near_cells_fit(a0, b0, ni, nj, step, sp);
        return;
    }
    int a1 = a0 + ni, b1 = b0 + nj, k = 0;
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
    TER_COUNT(step >> 1, k);
}

/* near_tile_stitched where cells are split the way their heights lie. */
static void near_stitched_fit(int a0, int b0, int edges, const Sphere *sp) {
    int a1 = a0 + TILE_CELLS, b1 = b0 + TILE_CELLS, k = 0;
    for(int jb = b0; jb < b1; jb++) {
        int jt = jb + 1;
        /* The left and right edges' rows, folded down to even ones. */
        int lt = (edges & 8) && (jt & 1) ? jb : jt, lb = (edges & 8) && (jb & 1) ? jb - 1 : jb;
        int rt = (edges & 2) && (jt & 1) ? jb : jt, rb = (edges & 2) && (jb & 1) ? jb - 1 : jb;
        if(k) {
            near_buf[k] = near_buf[k - 1];
            k++;
            near_buf[k++] = near_v(a0, lt);
        }
        /* As near_cells: a folded cell is split as it would have been. */
        unsigned bits = cross_bits(a0, jb, TILE_CELLS, 1);
        int low = 0;
        near_buf[k++] = near_v(a0, lt);
        near_buf[k++] = near_v(a0, lb);
        bool ft = (edges & 4) && jt == b1, fb = (edges & 1) && jb == b0;
        for(int i = a0 + 1; i <= a1; i++, k += 2, bits >>= 1) {
            /* The pair before this one, wherever a change of split leaves it. */
            const RV *pu = &near_buf[k - 2 + low], *pl = &near_buf[k - 1 - low];
            if((int)(bits & 1) != low) {
                near_buf[k] = near_buf[k - 2];
                k++;
                low ^= 1;
            }
            if(i == a1) {
                near_buf[k + low] = near_v(a1, rt);
                near_buf[k + (low ^ 1)] = near_v(a1, rb);
            }
            else {
                /* An odd vertex on the top or bottom edge: the even one before it. */
                near_buf[k + low] = ft && (i & 1) ? *pu : near_v(i, jt);
                near_buf[k + (low ^ 1)] = fb && (i & 1) ? *pl : near_v(i, jb);
            }
        }
    }
    r_strip_bounded(near_buf, k, sp->c, sp->r);
    TER_COUNT(0, k);
}

/* A 3 m tile beside 6 m ones: along each edge (bit e, as in nb[] below) it
   shares with a 6 m tile, the odd vertices take their even neighbour's
   place, so the edge is the 6 m grid's exactly (no T-junctions to crack,
   no skirt) and the triangles there fold into fans. */
static void near_tile_stitched(int a0, int b0, int edges, const Sphere *sp) {
    if(!r_sphere_visible(sp->c, sp->r)) return;
    if(ter_fit) {
        near_stitched_fit(a0, b0, edges, sp);
        return;
    }
    int a1 = a0 + TILE_CELLS, b1 = b0 + TILE_CELLS, k = 0;
    for(int jb = b0; jb < b1; jb++) {
        int jt = jb + 1;
        /* The left and right edges' rows, folded down to even ones. */
        int lt = (edges & 8) && (jt & 1) ? jb : jt, lb = (edges & 8) && (jb & 1) ? jb - 1 : jb;
        int rt = (edges & 2) && (jt & 1) ? jb : jt, rb = (edges & 2) && (jb & 1) ? jb - 1 : jb;
        if(k) {
            near_buf[k] = near_buf[k - 1];
            k++;
            near_buf[k++] = near_v(a0, lt);
        }
        near_buf[k++] = near_v(a0, lt);
        near_buf[k++] = near_v(a0, lb);
        bool ft = (edges & 4) && jt == b1, fb = (edges & 1) && jb == b0;
        if(ft || fb)
            for(int i = a0 + 1; i < a1; i++) {
                /* An odd vertex on the top or bottom edge: the even one before it. */
                near_buf[k] = ft && (i & 1) ? near_buf[k - 2] : near_v(i, jt);
                k++;
                near_buf[k] = fb && (i & 1) ? near_buf[k - 2] : near_v(i, jb);
                k++;
            }
        else
            for(int i = a0 + 1; i < a1; i++) {
                near_buf[k++] = near_v(i, jt);
                near_buf[k++] = near_v(i, jb);
            }
        near_buf[k++] = near_v(a1, rt);
        near_buf[k++] = near_v(a1, rb);
    }
    r_strip_bounded(near_buf, k, sp->c, sp->r);
    TER_COUNT(0, k);
}

static const int nb[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

static inline int lod_at(int x, int z) {
    return x >= 0 && z >= 0 && x < TILES && z < TILES ? tile_lod[z][x] : -1;
}

static void near_tile(int x, int z) {
    int lod = tile_lod[z][x];
    const Sphere *sp = &tile_sp[z * TILES + x];
    if(lod == 0) {
        int edges = 0;
        for(int e = 0; e < 4; e++)
            if(lod_at(x + nb[e][0], z + nb[e][1]) == 1) edges |= 1 << e;
        if(edges) {
            near_tile_stitched(x * TILE_CELLS, z * TILE_CELLS, edges, sp);
            return;
        }
    }
    near_cells(x * TILE_CELLS, z * TILE_CELLS, TILE_CELLS, TILE_CELLS, 1 << lod, sp);
}

static void near_chunk(int n, int tx, int tz) {
    for(int r = 0; r < 2; r++) {
        int z = tz + r;
        if(tile_lod[z][tx] == 1 && tile_lod[z][tx + 1] == 1)
            near_cells(tx * TILE_CELLS, z * TILE_CELLS, TILE_CELLS * 2, TILE_CELLS, 2, &pair_sp[(z * TILES + tx) / 2]);
        else {
            near_tile(tx, z);
            near_tile(tx + 1, z);
        }
    }
    /* Skirts where a tile meets the prebuilt far blocks: a copy of the
       edge dropped below it. */
    const Sphere *sp = &nc_sphere[n];
    int k = 0;
    static const int dir[4][4] = {{0, 0, 1, 0}, {1, 0, 0, 1}, {1, 1, -1, 0}, {0, 1, 0, -1}};
    for(int t = 0; t < 4; t++) {
        int x = tx + (t & 1), z = tz + (t >> 1), step = 1 << tile_lod[z][x];
        float drop = 1.5f * step + 1.0f;
        int i0 = x * TILE_CELLS, j0 = z * TILE_CELLS;
        for(int e = 0; e < 4; e++) {
            if(lod_at(x + nb[e][0], z + nb[e][1]) >= 0) continue;
            int ei = i0 + dir[e][0] * TILE_CELLS, ej = j0 + dir[e][1] * TILE_CELLS;
            for(int q = 0; q <= TILE_CELLS / step; q++) {
                int vi = ei + dir[e][2] * q * step, vj = ej + dir[e][3] * q * step;
                RV v = near_v(vi, vj);
                float vdrop = skirt_drop(vi, vj, drop);
                v.c = color_scale(v.c, 0.82f);
                if(q == 0 && k) {
                    near_buf[k] = near_buf[k - 1];
                    k++;
                    near_buf[k++] = v;
                }
                near_buf[k++] = v;
                v.y -= vdrop;
                v.v += vdrop * TEX_TILE;
                near_buf[k++] = v;
            }
        }
    }
    if(k) r_strip_bounded(near_buf, k, sp->c, sp->r);
    TER_COUNT(2, k);
}

int terrain_draw_near(void) {
    V3 cam_p = r_cam.pos;
    int drawn = 0;
    r_hdr(h_ground);
    /* Each near tile's grid first, so skirts know their neighbours'. */
    for(int b = 0; b < BLOCKS * BLOCKS; b++) {
        V3 d = vsub(block_c[b], cam_p);
        bool near = vdot(d, d) < lod_far[1] * lod_far[1];
        int x0 = (b % BLOCKS) * 4, z0 = (b / BLOCKS) * 4;
        for(int t = 0; t < 16; t++) {
            int x = x0 + (t & 3), z = z0 + (t >> 2);
            int8_t lod = -1;
            if(near) {
                /* Within a near block each tile picks its own grid. */
                V3 dc = vsub(tile_sp[z * TILES + x].c, cam_p);
                float dc2 = dc.x * dc.x + dc.z * dc.z;
                float fine = tile_trail[z * TILES + x] ? TRAIL_FINE : NEAR_FINE;
                lod = dc2 < fine * fine ? 0 : 1;
            }
            tile_lod[z][x] = lod;
        }
    }
    for(int b = 0; b < BLOCKS * BLOCKS; b++) {
        int x0 = (b % BLOCKS) * 4, z0 = (b / BLOCKS) * 4;
        if(tile_lod[z0][x0] < 0) continue;
        for(int k = 0; k < 4; k++) {
            int n = b * 4 + k;
            if(!r_sphere_visible(nc_sphere[n].c, nc_sphere[n].r)) continue;
            near_chunk(n, x0 + (k & 1) * 2, z0 + (k >> 1) * 2);
            drawn++;
        }
    }
    return drawn;
}

/* A vertex of the grid a near tile was drawn on: tile (a0, b0)'s, with
   the edges it shares with 6 m tiles folded as near_tile_stitched folds
   them. */
static inline void decal_fold(int *i, int *j, int a0, int b0, int edges) {
    if((*i == a0 && (edges & 8)) || (*i == a0 + TILE_CELLS && (edges & 2))) *j &= ~1;
    if((*j == b0 && (edges & 1)) || (*j == b0 + TILE_CELLS && (edges & 4))) *i &= ~1;
}

int terrain_decal(const TerDecal *d) {
    const float (*quad)[2] = d->quad;
    const float alpha = (float)(d->col >> 24);
    const uint32_t rgb = d->col & 0xffffffu;
    float x0 = 1e9f, x1 = -1e9f, z0 = 1e9f, z1 = -1e9f;
    for(int e = 0; e < 4; e++) {
        x0 = fminf(x0, quad[e][0]);
        x1 = fmaxf(x1, quad[e][0]);
        z0 = fminf(z0, quad[e][1]);
        z1 = fmaxf(z1, quad[e][1]);
    }
    const float inv = 1.0f / (LEVEL_D * TILE_CELLS);
    int tx0 = (int)floorf((x0 - LEVEL_ORG) * inv), tx1 = (int)floorf((x1 - LEVEL_ORG) * inv);
    int tz0 = (int)floorf((z0 - LEVEL_ORG) * inv), tz1 = (int)floorf((z1 - LEVEL_ORG) * inv);
    if(tx0 < 0) tx0 = 0;
    if(tz0 < 0) tz0 = 0;
    if(tx1 > TILES - 1) tx1 = TILES - 1;
    if(tz1 > TILES - 1) tz1 = TILES - 1;
    int sent = 0;
    for(int tz = tz0; tz <= tz1; tz++)
        for(int tx = tx0; tx <= tx1; tx++) {
            int lod = tile_lod[tz][tx];
            if(lod < 0) continue;
            int step = 1 << lod, a0 = tx * TILE_CELLS, b0 = tz * TILE_CELLS, edges = 0;
            if(lod == 0)
                for(int e = 0; e < 4; e++)
                    if(lod_at(tx + nb[e][0], tz + nb[e][1]) == 1) edges |= 1 << e;
            for(int j = b0; j < b0 + TILE_CELLS; j += step) {
                /* The quad's reach across this row of cells. */
                float za = LEVEL_ORG + j * LEVEL_D, zb = za + step * LEVEL_D;
                if(zb < z0 || za > z1) continue;
                float xa = 1e9f, xb = -1e9f;
                for(int e = 0; e < 4; e++) {
                    const float *q = quad[e], *n = quad[(e + 1) & 3];
                    if(q[1] >= za && q[1] <= zb) {
                        xa = fminf(xa, q[0]);
                        xb = fmaxf(xb, q[0]);
                    }
                    for(int s = 0; s < 2; s++) {
                        float zc = s ? zb : za;
                        if((q[1] - zc) * (n[1] - zc) >= 0) continue;
                        float x = q[0] + (n[0] - q[0]) * (zc - q[1]) / (n[1] - q[1]);
                        xa = fminf(xa, x);
                        xb = fmaxf(xb, x);
                    }
                }
                if(xa > xb) continue;
                /* (a folded edge's triangles reach a cell further) */
                int pad = edges ? 1 : 0;
                int ia = ((int)floorf((xa - LEVEL_ORG) * (1.0f / LEVEL_D)) - pad) & ~(step - 1);
                int ib = ((int)floorf((xb - LEVEL_ORG) * (1.0f / LEVEL_D)) + pad) & ~(step - 1);
                if(ia < a0) ia = a0;
                if(ib > a0 + TILE_CELLS - step) ib = a0 + TILE_CELLS - step;
                if(ia > ib) continue;
                /* The cells' splits, as near_cells_fit lays them. */
                unsigned bits = cross_bits(a0, j, TILE_CELLS / step, step) >> ((ia - a0) / step);
                int low = bits & 1;
                RV *p = near_buf;
                float lo = 1e9f, hi = -1e9f, lit = 0;
                for(int i = ia; i <= ib + step; i += step) {
                    if(i > ia) {
                        if((int)(bits & 1) != low) {
                            *p = p[-2];
                            p++;
                            low ^= 1;
                        }
                        bits >>= 1;
                    }
                    for(int s = 0; s < 2; s++) {
                        int vi = i, vj = s ? j : j + step;
                        if(edges) decal_fold(&vi, &vj, a0, b0, edges);
                        float x = LEVEL_ORG + vi * LEVEL_D, z = LEVEL_ORG + vj * LEVEL_D;
                        float h = lvl_ground[lvl_clampi(vj) * LEVEL_N + lvl_clampi(vi)].h;
                        lo = fminf(lo, h);
                        hi = fmaxf(hi, h);
                        /* (1 / sqrt squared: the SH-4 has that in one instruction, and no quick divide) */
                        float off = fabsf(h - (d->axis[0] + d->axis[1] * x + d->axis[2] * z));
                        float rs = 1.0f / sqrtf(fmaxf(d->reach[0] + d->reach[1] * x + d->reach[2] * z, 0.5f));
                        float k = clampf(2.0f - off * rs * rs, 0, 1);
                        lit += k;
                        p[s ^ low] = (RV){x, h + d->lift, z, d->u[0] + d->u[1] * x + d->u[2] * z, d->v[0] + d->v[1] * x + d->v[2] * z,
                                          rgb | (uint32_t)(alpha * k) << 24};
                    }
                    p += 2;
                }
                if(lit <= 0) continue;
                /* A strip a row, each with a sphere of its own: all but
                   the rows under the camera are then wholly in front of
                   it, and go the renderer's quick way. */
                int k = (int)(p - near_buf);
                float hx = (ib + step - ia) * LEVEL_D * 0.5f, hz = step * LEVEL_D * 0.5f, hy = (hi - lo) * 0.5f;
                r_strip_bounded(near_buf, k, v3(LEVEL_ORG + ia * LEVEL_D + hx, (lo + hi) * 0.5f + d->lift, za + hz),
                                sqrtf(hx * hx + hy * hy + hz * hz) + (edges ? LEVEL_D : 0.01f));
                sent += k;
            }
        }
    return sent;
}

/* The far blocks, each at its distance's grid, and their skirts, only on
   edges whose neighbour block has another grid: between two blocks at one
   grid the edge vertices match exactly, and those skirts were half of a
   24 m block's vertices. Returns the blocks' vertices, the skirts' in
   *skirts. */
int terrain_draw_far(const Scenery *sc, int *skirts) {
    int8_t lod[BLOCKS * BLOCKS];
    V3 cam_p = r_cam.pos;
    for(int b = 0; b < BLOCKS * BLOCKS; b++) {
        V3 d = vsub(block_c[b], cam_p);
        float d2 = vdot(d, d);
        lod[b] = d2 < lod_far[1] * lod_far[1] ? 1 : d2 < lod_far[2] * lod_far[2] ? 2 : d2 < lod_far[3] * lod_far[3] ? 3 : -1;
    }
    int sent = 0, sk = 0;
    for(int b = 0; b < BLOCKS * BLOCKS; b++) {
        if(lod[b] < 2) continue;
        sent += sc_draw_chunk(sc, far_chunk[b][lod[b] - 2]);
        int bi = b % BLOCKS, bj = b / BLOCKS;
        for(int e = 0; e < 4; e++) {
            int ni = bi + nb[e][0], nj = bj + nb[e][1];
            if(ni >= 0 && nj >= 0 && ni < BLOCKS && nj < BLOCKS && lod[nj * BLOCKS + ni] == lod[b]) continue;
            sk += sc_draw_chunk(sc, far_skirt[b][lod[b] - 2][e]);
        }
    }
    *skirts = sk;
    return sent;
}
