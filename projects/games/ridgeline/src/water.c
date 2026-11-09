/* Ridgeline water (water.h): the lake, the pond, the creek and its falls.

   Built once with the terrain into the scenery pool (translucent cells,
   deepening in colour with depth, white where the water falls or a trail
   fords it, a ripple texture turned along each stretch's flow); relit with
   the terrain; drawn each frame with the ripples scrolling downstream and,
   near the camera, the sky and the sun's glint on it. */
#include "water.h"
#include "terrain.h"
#include <malloc.h>

#define W_CHUNK_CELLS 16      /* as the terrain's chunks */
#define W_CHUNKS ((LEVEL_N - 1) / W_CHUNK_CELLS)

/* The water's vertices: each grid vertex of a wet cell once, with how
   deep the water is there and how much foam it carries; the pool's water
   vertices point here (terrain_pool_src), so a frame works out the moving parts
   (glint, sky, sparkle) once a vertex rather than once a cell corner. */
typedef struct { float x, y, z; uint8_t depth, foam; uint16_t pad; } WetV;
static WetV *wv;
static uint32_t *wv_col;      /* lit, by the relight */
static int wv_n;
/* Each water chunk's flow: unit direction downstream and speed (m/s);
   still water drifts slowly with the wind. */
typedef struct { uint16_t chunk; float fx, fz, speed; V3 o; } WChunk;
#define WCHUNKS 64
static WChunk wch[WCHUNKS];
static int wch_n;

static inline uint32_t water_colour(float depth, float foam, float k) {
    uint32_t c = color_lerp(0xff9ed2cc, 0xff2e6a74, depth);
    c = color_lerp(c, 0xffeef6f4, foam * 0.85f);
    return color_alpha(color_scale(c, k), 0.42f + 0.38f * depth + 0.4f * foam);
}

/* ---- Water, built ---- */

#define WET_MAX 2048
static inline float lvl_wat(int i, int j) {
    return lvl_water[lvl_clampi(j) * LEVEL_N + lvl_clampi(i)];
}

/* Texture across and along the flow (ripples stretch downstream), about
   the chunk's middle so the coordinates stay small (the PVR swims on big
   ones). */
#define RIPPLE_ACROSS (1.0f / 4.0f)
#define RIPPLE_ALONG (1.0f / 7.0f)
static inline void water_uv(const WChunk *w, V3 p, float *u, float *v) {
    float dx = p.x - w->o.x, dz = p.z - w->o.z;
    *u = (dz * w->fx - dx * w->fz) * RIPPLE_ACROSS;
    *v = -(dx * w->fx + dz * w->fz) * RIPPLE_ALONG;
}

/* Foam: where the water's surface drops (falls and riffles), and where a
   trail fords running water shallow enough to wade. */
static float water_foam(int i, int j, float depth) {
    float w = lvl_wat(i, j);
    if(w <= -999.0f) return 0;
    float drop = 0;
    static const int nb[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for(int k = 0; k < 4; k++) {
        float n = lvl_wat(i + nb[k][0], j + nb[k][1]);
        if(n > -999.0f) drop = fmaxf(drop, fabsf(n - w));
    }
    float foam = clampf((drop - 0.3f) * (1.0f / 0.4f), 0, 1);
    if(drop > 0.02f && depth < 0.45f) {
        float x = LEVEL_ORG + i * LEVEL_D, z = LEVEL_ORG + j * LEVEL_D;
        for(int r = 0; r < LVL_ROUTES && foam < 0.55f; r++)
            for(int k = 0; k < lvl_routes[r].n; k++) {
                const lvl_pt_t *q = &lvl_routes[r].pts[k];
                float dx = q->x - x, dz = q->z - z, rr = lvl_routes[r].half + 1.5f;
                if(dx * dx + dz * dz < rr * rr) {
                    foam = fmaxf(foam, 0.55f);
                    break;
                }
            }
    }
    return foam;
}

/* The id of grid vertex (i, j) of a wet cell whose highest water is
   level, made on first sight. A dry corner sits a little under the
   cell's water, under the bank. */
static int wet_id(int16_t *idmap, int i, int j, float level) {
    int g = lvl_clampi(j) * LEVEL_N + lvl_clampi(i);
    if(idmap[g] >= 0) return idmap[g];
    if(wv_n >= WET_MAX) return wv_n - 1;
    float w = lvl_wat(i, j), gnd = lvl_h(i, j);
    float y = w > -999.0f ? w : level - 0.25f;
    float depth = clampf((y - gnd) / 1.6f, 0, 1);
    int id = wv_n++;
    WetV *v = &wv[id];
    *v = (WetV){LEVEL_ORG + i * LEVEL_D, y, LEVEL_ORG + j * LEVEL_D, (uint8_t)(depth * 255.0f),
                (uint8_t)(water_foam(i, j, y - gnd) * 255.0f), 0};
    wv_col[id] = water_colour(depth, v->foam * (1.0f / 255), pal->water_k);
    idmap[g] = (int16_t)id;
    return id;
}

/* A chunk's wet cells, step grid cells a side, into its strips: each
   cell a quad joined on to the run before it; at step 1 a new strip every
   few rows, so a small strip's sphere lets the draw find the water near
   the camera without walking the rest. */
#define WATER_LOD_M 150.0f
static void water_cells(Scenery *sc, void (*mark)(const Scenery *, uint32_t), uint32_t tag, int16_t *idmap, const WChunk *wc,
                        const pvr_poly_hdr_t *hw, int i0, int j0, int step) {
    sc_strip_begin(sc, hw);
    for(int j = j0; j < j0 + W_CHUNK_CELLS; j += step) {
        if(step == 1 && j > j0 && !((j - j0) & 3)) {
            sc_strip_end(sc);
            sc_strip_begin(sc, hw);
        }
        int run = -1;
        for(int i = i0; i < i0 + W_CHUNK_CELLS; i += step) {
            float level = -1000.0f;
            for(int b = 0; b <= step; b++)
                for(int a = 0; a <= step; a++) {
                    float w = lvl_wat(i + a, j + b);
                    if(w > level) level = w;
                }
            if(level <= -999.0f) {
                run = -1;
                continue;
            }
            for(int k = 0; k < 4; k++) {
                int ii = i + (k >> 1) * step, jj = j + ((k & 1) ? 0 : step);
                int id = wet_id(idmap, ii, jj, level);
                const WetV *v = &wv[id];
                V3 p = v3(v->x, v->y, v->z);
                float u0, v0;
                water_uv(wc, p, &u0, &v0);
                if(k == 0 && run < 0) sc_join(sc, p, u0, v0, wv_col[id]);
                else if(k >= 2 || run < 0) sc_vert(sc, p, u0, v0, wv_col[id]);
                mark(sc, tag | (uint32_t)id);
            }
            run = i;
        }
    }
    sc_strip_end(sc);
}

void water_build(Scenery *sc, void (*mark)(const Scenery *, uint32_t), uint32_t tag) {
    const pvr_poly_hdr_t *hw = r_header(TEX_RIPPLE, HF_TR);
    int16_t *idmap = malloc(sizeof(int16_t) * LEVEL_N * LEVEL_N);
    if(!wv) {
        wv = malloc(sizeof(WetV) * WET_MAX);
        wv_col = malloc(sizeof(uint32_t) * WET_MAX);
    }
    wv_n = 0;
    wch_n = 0;
    if(idmap) memset(idmap, 0xff, sizeof(int16_t) * LEVEL_N * LEVEL_N);
    for(int cj = 0; cj < W_CHUNKS && idmap && wv && wv_col; cj++)
        for(int ci = 0; ci < W_CHUNKS; ci++) {
            int i0 = ci * W_CHUNK_CELLS, j0 = cj * W_CHUNK_CELLS;
            /* Wet anywhere, and which way it runs: down the water's
               surface, summed over the chunk. */
            bool any = false;
            float gx = 0, gz = 0, wy = 0;
            int wn = 0;
            for(int j = j0; j <= j0 + W_CHUNK_CELLS; j++)
                for(int i = i0; i <= i0 + W_CHUNK_CELLS; i++) {
                    float w = lvl_wat(i, j);
                    if(w <= -999.0f) continue;
                    any = true;
                    wy += w;
                    wn++;
                    float e = lvl_wat(i + 1, j), wst = lvl_wat(i - 1, j), n = lvl_wat(i, j + 1), so = lvl_wat(i, j - 1);
                    if(e > -999.0f && wst > -999.0f) gx += e - wst;
                    if(n > -999.0f && so > -999.0f) gz += n - so;
                }
            if(!any) continue;
            float gl = sqrtf(gx * gx + gz * gz);
            /* Its middle at the water's mean height: where the near and far
               grids are measured from. */
            V3 o = v3(LEVEL_ORG + (i0 + W_CHUNK_CELLS / 2) * LEVEL_D, wy / wn, LEVEL_ORG + (j0 + W_CHUNK_CELLS / 2) * LEVEL_D);
            float fx = WIND_X, fz = WIND_Z, speed = 0.15f;
            if(gl > 0.5f) {
                fx = -gx / gl;
                fz = -gz / gl;
                speed = 0.9f;
            }
            /* Near, on the 3 m grid in strips of a few rows; far, a 6 m
               grid in long strips (a quarter of the vertices, and the fog
               hides the difference). */
            for(int lod = 0; lod < 2; lod++) {
                WChunk w = {(uint16_t)sc->chunk_n, fx, fz, speed, o};
                if(wch_n < WCHUNKS) wch[wch_n++] = w;
                sc_chunk_begin(sc, HF_TR, lod ? WATER_LOD_M : 0, lod ? 900.0f : WATER_LOD_M, TAG_WATER);
                sc_chunk_lod_point(sc, o);
                water_cells(sc, mark, tag, idmap, &w, hw, i0, j0, lod ? 2 : 1);
                sc_chunk_end(sc);
            }
        }
    free(idmap);
    printf("Ridgeline water: %d vertices, %d chunks\n", wv_n, wch_n);
}

uint32_t water_relit(uint32_t id, float water_k) {
    const WetV *w = &wv[id];
    return wv_col[id] = water_colour(w->depth * (1.0f / 255), w->foam * (1.0f / 255), water_k);
}

/* ---- Water, drawn ----

   Every visible strip of water goes through a buffer to scroll its
   ripples downstream. Within WATER_NEAR of the camera each wet vertex
   also takes, once a frame however many cells share it, more of the sky
   at a low angle (and more opaque, as water is) and the sun's glint off
   a rippled surface in the offset colour, both fading out with distance
   so there is no seam where they stop. The brightest glints throw small
   sparkles. */
#define WATER_NEAR 40.0f
#define GLINT_NEAR 35.0f
#define SPARKS 16
#define SPARK_CANDS 48
static uint32_t *wv_dc, *wv_sp;     /* this frame's colour and glint */
static uint16_t *wv_stamp, wv_frame;
static struct { float x, y, z; } sp_cand[SPARK_CANDS];
static int sp_cand_n, sp_seen;
typedef struct { float x, y, z, age, life, size; } Spark;
static Spark sparks[SPARKS];
static uint32_t sp_seed = 0x6a7e12u;   /* its own: demos replay the game's */
static float wat_t;

static inline float sp_rand(void) {
    sp_seed = sp_seed * 1664525u + 1013904223u;
    return (float)(sp_seed >> 8 & 0xffff) * (1.0f / 65535.0f);
}

static inline uint32_t mix256w(uint32_t a, uint32_t b, int t) {
    uint32_t rb = ((a & 0xff00ffu) * (uint32_t)(256 - t) + (b & 0xff00ffu) * (uint32_t)t) >> 8;
    uint32_t ag = (((a >> 8) & 0xff00ffu) * (uint32_t)(256 - t) + ((b >> 8) & 0xff00ffu) * (uint32_t)t) >> 8;
    return (rb & 0xff00ffu) | ((ag & 0xff00ffu) << 8);
}

/* The frame's light on the water, set once a frame. */
static struct { V3 c, L; uint32_t sky; float glint[3]; } wl;

static void wet_dyn(int id) {
    const WetV *w = &wv[id];
    wv_stamp[id] = wv_frame;
    float dx = wl.c.x - w->x, dy = wl.c.y - w->y, dz = wl.c.z - w->z;
    float d2 = dx * dx + dy * dy + dz * dz;
    uint32_t base = wv_col[id];
    wv_sp[id] = 0;
    if(d2 >= WATER_NEAR * WATER_NEAR) {
        wv_dc[id] = base;
        return;
    }
    float il = 1.0f / sqrtf(d2 + 0.01f), d = d2 * il;
    float near = clampf((WATER_NEAR - d) * (1.0f / 15.0f), 0, 1);
    float foam = w->foam * (1.0f / 255);
    /* Grazing: the sky's colour, and the water turns to a mirror. */
    float vy = clampf(dy * il, 0, 1), fr = 1.0f - vy;
    fr = fr * fr * fr * near * (1.0f - foam);
    int a = (int)(base >> 24);
    a += (int)((235 - a) * fr);
    wv_dc[id] = (mix256w(base, wl.sky, (int)(fr * 170.0f)) & 0xffffffu) | (uint32_t)a << 24;
    /* The glint: a Blinn highlight off a surface rippled by a few slow
       waves (after DreamKarts' sea), strongest along the path to the sun. */
    float hx = wl.L.x + dx * il, hy = wl.L.y + dy * il, hz = wl.L.z + dz * il;
    float ih = 1.0f / sqrtf(hx * hx + hy * hy + hz * hz);
    if(d > GLINT_NEAR || hy * ih < 0.55f || wl.glint[0] + wl.glint[1] + wl.glint[2] < 3.0f) return;
    float gx = 0.13f * sinf(0.8f * w->x + 0.5f * w->z + 2.1f * wat_t);
    float gz = 0.13f * cosf(0.9f * w->z - 0.4f * w->x + 1.9f * wat_t);
    float n = (hy - gx * hx - gz * hz) * ih / sqrtf(1.0f + gx * gx + gz * gz);
    if(n < 0.85f) return;
    n *= n;
    n *= n;
    n *= n;
    n *= n;
    n *= n;   /* ^32 */
    float k = n * near * (1.0f - 0.7f * foam);
    int r = (int)(wl.glint[0] * k), g = (int)(wl.glint[1] * k), b = (int)(wl.glint[2] * k);
    wv_sp[id] = (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
    if(k > 0.45f) {
        int slot = sp_cand_n < SPARK_CANDS ? sp_cand_n++ : (int)(sp_rand() * (float)(sp_seen + 1));
        if(slot < SPARK_CANDS) {
            sp_cand[slot].x = w->x;
            sp_cand[slot].y = w->y;
            sp_cand[slot].z = w->z;
        }
        sp_seen++;
    }
}

static const WChunk *water_chunk(uint32_t ci) {
    for(int k = 0; k < wch_n; k++)
        if(wch[k].chunk == ci) return &wch[k];
    return NULL;
}

void water_draw(const Scenery *sc) {
    static const pvr_poly_hdr_t *h_water, *h_spark;
    if(!h_water) {
        h_water = r_header(TEX_RIPPLE, HF_TR | HF_SPEC);
        h_spark = r_header(TEX_SUN, HF_TR | HF_ADD | HF_NOFOG);
        wv_dc = malloc(sizeof(uint32_t) * WET_MAX);
        wv_sp = malloc(sizeof(uint32_t) * WET_MAX);
        wv_stamp = calloc(WET_MAX, sizeof(uint16_t));
    }
    const uint32_t *psrc = terrain_pool_src();
    if(!wv || !wv_dc || !wv_sp || !wv_stamp || !psrc) return;
    float dt = clampf(game.time - wat_t, 0, 0.1f);
    wat_t = game.time;
    if(++wv_frame == 0) {
        memset(wv_stamp, 0, sizeof(uint16_t) * WET_MAX);
        wv_frame = 1;
    }
    wl.c = r_cam.pos;
    wl.L = sun_dir;
    wl.sky = pal->sky_horizon;
    for(int i = 0; i < 3; i++)
        wl.glint[i] = 255.0f * 1.6f * pal->sun_k[i] * (((pal->sun_col >> (16 - 8 * i)) & 255) * (1.0f / 255));
    sp_cand_n = sp_seen = 0;
    float fog_far = world_fog_far();

    /* Few chunks: draw the visible ones far to near. */
    int idx[64], n = 0;
    float d2s[64];
    V3 cam_p = r_cam.pos;
    for(uint32_t i = 0; i < sc->chunk_n && n < 64; i++) {
        const ScChunk *c = &sc->chunks[i];
        if(c->tag != TAG_WATER) continue;
        V3 d = vsub(c->lc, cam_p);
        float d2 = vdot(d, d);
        if(d2 < c->near2) continue;
        /* Past the fog's far end water is the fog's colour on ground of
           the same colour: nothing to draw. */
        float beyond = fog_far + c->r;
        if(d2 >= c->far2 || d2 >= beyond * beyond || !r_sphere_visible(c->c, c->r)) continue;
        int k = n++;
        while(k > 0 && d2s[k - 1] < d2) {
            idx[k] = idx[k - 1];
            d2s[k] = d2s[k - 1];
            k--;
        }
        idx[k] = (int)i;
        d2s[k] = d2;
    }
    static RV buf[512] __attribute__((aligned(32)));
    static uint32_t spec[512];
    r_hdr(h_water);
    for(int k = 0; k < n; k++) {
        const ScChunk *c = &sc->chunks[idx[k]];
        const WChunk *wc = water_chunk((uint32_t)idx[k]);
        float scroll = wc ? wc->speed * wat_t * RIPPLE_ALONG : 0;
        scroll -= floorf(scroll);
        for(int s = 0; s < c->nstrips; s++) {
            const ScStrip *t = &sc->strips[c->first_strip + s];
            if(!r_sphere_visible(t->c, t->r)) continue;
            const RV *src = sc->pool + t->first;
            /* Far off, the strip as built: the ripples still, the light
               the relight gave it (the fog hides the difference). */
            float sd = vlen(vsub(t->c, cam_p)) - t->r;
            if(sd >= fog_far) continue;
            if(sd >= WATER_NEAR) {
                r_strip_bounded(src, t->count, t->c, t->r);
                continue;
            }
            const uint32_t *ids = psrc + t->first;
            for(int v = 0; v < t->count; v++) {
                buf[v] = src[v];
                buf[v].v += scroll;
                int id = (int)(ids[v] & 0x3fffffffu);
                if(wv_stamp[id] != wv_frame) wet_dyn(id);
                buf[v].c = wv_dc[id];
                spec[v] = wv_sp[id];
            }
            r_strip_spec(buf, spec, t->count);
        }
    }

    /* Sparkles: brief stars among the brightest glints. */
    for(int i = 0; i < SPARKS; i++) {
        Spark *p = &sparks[i];
        if(p->life > 0) {
            p->age += dt;
            if(p->age >= p->life) p->life = 0;
            continue;
        }
        if(!sp_cand_n || sp_rand() > 0.2f) continue;
        int ci = (int)(sp_rand() * 0.999f * (float)sp_cand_n);
        p->x = sp_cand[ci].x + (sp_rand() - 0.5f) * 2.5f;
        p->y = sp_cand[ci].y + 0.03f;
        p->z = sp_cand[ci].z + (sp_rand() - 0.5f) * 2.5f;
        p->age = 0;
        p->life = 0.12f + 0.2f * sp_rand();
        p->size = 0.6f + 0.6f * sp_rand();   /* pixels' worth: a few a metre away per 0.01 */
    }
    uint32_t col = pal->sun_col;
    r_hdr(h_spark);
    for(int i = 0; i < SPARKS; i++) {
        const Spark *p = &sparks[i];
        if(p->life <= 0) continue;
        float f = sinf(ENG_PI * p->age / p->life);
        V3 q = v3(p->x, p->y, p->z);
        float d = vlen(vsub(q, cam_p));
        float sz = p->size * 0.004f * d;   /* ~2-4 pixels at any distance */
        r_billboard(q, sz, sz, 0, 0, 0, 1, 1, color_alpha(col, f));
    }
}

