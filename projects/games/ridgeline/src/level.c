/* Ridgeline levels (level.h): the areas' tables and unpacking a grid.

   A packed grid (tools/levelkit.py pack_level) is "RLVL", a version, the
   heights' base and step, five packed sizes, then five LZ4 blocks: the
   heights and the water's surface as u16 planes, the surface ids, the
   relighting inputs as seven planes and the horizon map. All but the
   surfaces are stored as what is left after predicting each value from
   its left, upper and upper-left neighbours, which is what makes them
   pack to a third of their size. */
#include "level.h"
#include "terrain.h"
#include "texture_assets.h"
#include <string.h>

#include "level_cascade.h"
#include "level_moab.h"
#include "level_dunes.h"

extern const uint8_t rl_level_cascade_lvl[], rl_level_moab_lvl[], rl_level_dunes_lvl[];

const RlLevel *const rl_levels[AREA_COUNT] = {&rl_level_cascade, &rl_level_moab, &rl_level_dunes};
static const uint8_t *const packs[AREA_COUNT] = {rl_level_cascade_lvl, rl_level_moab_lvl, rl_level_dunes_lvl};
const RlLevel *lvl = &rl_level_cascade;
int lvl_area;

static const LvlLook looks[AREA_COUNT] = {
    [AREA_CASCADE] = {
        .ground_tex = TEX_GRASS, .water_shallow = 0xff9ed2cc, .water_deep = 0xff2e6a74, .wet = {52, 92, 98},
        .boulder = {0xff9aa0a6, 0xff9aa0a6, 0xff9aa0a6}, .ledge = 0xff8c9298, .dust = 0,
        .snow = true, .fireflies = true, .cloud_cap = true, .woods = 1.0f,
    },
    /* Red rock: a silt-green river, sandstone boulders, red dust off
       everything, and a quiet the forest never has. */
    [AREA_MOAB] = {
        .ground_tex = TEX_SANDSTONE, .water_shallow = 0xffb8b48c, .water_deep = 0xff5a6a48, .wet = {96, 100, 66},
        .boulder = {0xffc86c3e, 0xffb45a30, 0xffd27c4a}, .ledge = 0xffd07844, .dust = 0xffd8946a,
        .snow = false, .fireflies = false, .cloud_cap = false, .woods = 0.3f,
    },
    /* Grey basalt sand: a lake the sky's own blue, black rimrock, pale
       dust, and the wind for company. */
    [AREA_DUNES] = {
        .ground_tex = TEX_DUNE, .water_shallow = 0xff8cb0b4, .water_deep = 0xff2c5a72, .wet = {58, 84, 92},
        .boulder = {0xff4c4440, 0xff584c44, 0xff443e3c}, .ledge = 0xff4c4440, .dust = 0xffcfc4ae,
        .snow = false, .fireflies = false, .cloud_cap = false, .woods = 0.12f, .lake = true,
    },
};
const LvlLook *look = &looks[AREA_CASCADE];

#define NN (LEVEL_N * LEVEL_N)

/* An LZ4 block into dst; the bytes written, or -1 if it is damaged. */
static int lz4_block(const uint8_t *s, int n, uint8_t *dst, int cap) {
    const uint8_t *se = s + n;
    uint8_t *d = dst, *de = dst + cap;
    while(s < se) {
        int tok = *s++, lit = tok >> 4, b;
        if(lit == 15) do {
                if(s >= se) return -1;
                lit += b = *s++;
            } while(b == 255);
        if(lit > de - d || lit > se - s) return -1;
        memcpy(d, s, (size_t)lit);
        d += lit;
        s += lit;
        if(s >= se) break;   /* the last sequence is literals only */
        if(se - s < 2) return -1;
        int off = s[0] | s[1] << 8, len = (tok & 15) + 4;
        s += 2;
        if((tok & 15) == 15) do {
                if(s >= se) return -1;
                len += b = *s++;
            } while(b == 255);
        if(off == 0 || off > d - dst || len > de - d) return -1;
        const uint8_t *m = d - off;
        while(len--) *d++ = *m++;   /* byte by byte: a match may run into itself */
    }
    return (int)(d - dst);
}

/* A plane of w x h bytes of residuals back to its values, in place. */
static void unpredict8(uint8_t *p, int w, int h) {
    for(int i = 1; i < w; i++) p[i] = (uint8_t)(p[i] + p[i - 1]);
    for(int j = 1; j < h; j++) {
        uint8_t *r = p + j * w, *u = r - w;
        r[0] = (uint8_t)(r[0] + u[0]);
        for(int i = 1; i < w; i++) r[i] = (uint8_t)(r[i] + r[i - 1] + u[i] - u[i - 1]);
    }
}

/* The u16 grid packed as zigzagged residuals in two byte planes at src,
   one value at a time: row above kept in up[]. */
typedef struct {
    const uint8_t *lo, *hi;
    int32_t up[LEVEL_N];
    int32_t left, upleft;
    int i, j;
} U16Grid;

static void u16_begin(U16Grid *g, const uint8_t *src) {
    g->lo = src;
    g->hi = src + NN;
    g->i = g->j = 0;
}

static int32_t u16_next(U16Grid *g) {
    int k = g->j * LEVEL_N + g->i;
    uint32_t zz = (uint32_t)g->lo[k] | (uint32_t)g->hi[k] << 8;
    int32_t r = (int32_t)(zz >> 1) ^ -(int32_t)(zz & 1), v;
    if(g->j == 0) v = g->i ? r + g->left : r;
    else if(g->i == 0) v = r + g->up[0];
    else v = r + g->left + g->up[g->i] - g->upleft;
    g->upleft = g->up[g->i];
    g->up[g->i] = v;
    g->left = v;
    if(++g->i == LEVEL_N) {
        g->i = 0;
        g->j++;
    }
    return v;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool level_load(int area, void *buf, LvlGrid *out) {
    lvl = rl_levels[area];
    look = &looks[area];
    lvl_area = area;
    const uint8_t *pk = packs[area];
    if(memcmp(pk, "RLVL", 4) || rd32(pk + 4) != 1) return false;
    float base, step;
    memcpy(&base, pk + 8, 4);
    memcpy(&step, pk + 12, 4);
    const uint8_t *sec[5];
    int len[5];
    const uint8_t *p = pk + 16 + 5 * 4;
    for(int k = 0; k < 5; k++) {
        len[k] = (int)rd32(pk + 16 + 4 * k);
        sec[k] = p;
        p += (len[k] + 3) & ~3;
    }
    /* Where each part lives in buf. Unpacking borrows the parts not yet
       filled as scratch: the light's planes pass through the ground's
       room, the heights through the water's, the water through the
       horizon map's. */
    uint8_t *b = buf;
    GroundVert *ground = (GroundVert *)b;
    float *water = (float *)(b + NN * 8);
    LvlLight *light = (LvlLight *)(b + NN * 12);
    uint8_t *surf = b + NN * 20;
    uint8_t *hz = surf + ((NN + 3) & ~3);
    const int hz_bytes = LEVEL_HZ_DIRS * LEVEL_HZ_N * LEVEL_HZ_N;

    uint8_t *planes = (uint8_t *)ground;
    if(lz4_block(sec[3], len[3], planes, NN * 7) != NN * 7) return false;
    for(int k = 0; k < 7; k++) unpredict8(planes + k * NN, LEVEL_N, LEVEL_N);
    for(int i = 0; i < NN; i++) {
        LvlLight *l = &light[i];
        l->r = planes[i];
        l->g = planes[NN + i];
        l->b = planes[2 * NN + i];
        l->nx = (int8_t)planes[3 * NN + i];
        l->nz = (int8_t)planes[4 * NN + i];
        l->deep = planes[5 * NN + i];
        l->flags = planes[6 * NN + i];
        l->pad = 0;
    }
    U16Grid g;
    uint8_t *tmp = (uint8_t *)water;
    if(lz4_block(sec[0], len[0], tmp, NN * 2) != NN * 2) return false;
    u16_begin(&g, tmp);
    for(int i = 0; i < NN; i++) {
        ground[i].h = base + step * (float)u16_next(&g);
        ground[i].c = 0xff808080u;
    }
    tmp = hz;
    if(lz4_block(sec[1], len[1], tmp, NN * 2) != NN * 2) return false;
    u16_begin(&g, tmp);
    for(int i = 0; i < NN; i++) {
        int32_t q = u16_next(&g);
        water[i] = q ? base + step * (float)(q - 1) : -1000.0f;
    }
    if(lz4_block(sec[2], len[2], surf, NN) != NN) return false;
    if(lz4_block(sec[4], len[4], hz, hz_bytes) != hz_bytes) return false;
    for(int d = 0; d < LEVEL_HZ_DIRS; d++) unpredict8(hz + d * LEVEL_HZ_N * LEVEL_HZ_N, LEVEL_HZ_N, LEVEL_HZ_N);
    out->ground = ground;
    out->water = water;
    out->surf = surf;
    out->light = light;
    out->hz = hz;
    return true;
}
