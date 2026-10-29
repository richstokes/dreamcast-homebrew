/* DreamKarts world internals, shared by the scenery's source files:
   world.c builds and draws the scenery, through the builder in
   world_mesh.c; world_road.c builds the road the tracks share and
   world_props.c the props several of them place; each track's own
   scenery is in world_<track>.c. */
#ifndef DK_WORLD_INT_H
#define DK_WORLD_INT_H

#include "world.h"
#include "render.h"
#include "track.h"
#include "texture_assets.h"

/* ---- The scenery (world.c) ---- */

typedef struct {
    const pvr_poly_hdr_t *hdr;
    uint16_t first, count;
    V3 c;              /* bounding sphere */
    float r;
} Strip;

typedef struct {
    V3 c;
    float r;
    float lod_far;     /* not drawn beyond this distance */
    uint16_t first_strip, nstrips;
    uint8_t list;
    uint8_t far;       /* distant terrain or fine detail: skipped in a split screen */
    uint8_t lod;       /* split screen: 1 drawn near the camera, 2 (the next chunk) beyond; 3 only in a split screen */
} Chunk;

typedef struct {
    V3 base;
    float height;
} Palm;

/* The vertex pool holds one track's static scenery (rebuilt on each load).
   Its size is not the budget to design to: 60000 vertices is 1.4 MB of
   the ~9 MB free since the music moved to the disc. The budget is per
   frame, what survives culling: ~0.41 us a vertex and 1.6 us a strip on
   the console, and in a split screen the Tile Accelerator's vertex buffer
   (pvr_init in render.c), which a flat track that shows most of its lap
   can overrun. Check new scenery with -DDK_CHUNK_STATS and a split-screen
   make hwbench. Strip.first is 16 bits: past 65535 it needs widening.
   Tracks so far: ~31800 at most (Apple Valley, Tsudoken Drift). */
#define MAX_POOL   60000
#define MAX_STRIPS 4600
#define MAX_CHUNKS 512   /* Apple Valley ~420 */
#define MAX_PALMS  48
#define MAX_SPRITES 64
/* Shore foam: waterline points with a seaward normal; off = break. */
#define MAX_FOAM (TRACK_SAMPLES / 2 + 2)

extern RV *pool;
extern int pool_n;
extern Strip strips[MAX_STRIPS];
extern int strip_n;
extern Chunk chunks[MAX_CHUNKS];
extern int chunk_n;
extern bool built_ok;
extern Palm palms[MAX_PALMS];
extern int palm_n;
extern V3 far_sprites[MAX_SPRITES];
extern float far_sprite_h[MAX_SPRITES];
extern int far_sprite_n;
extern V3 foam_p[MAX_FOAM], foam_n[MAX_FOAM];
extern float foam_u[MAX_FOAM];
extern bool foam_on[MAX_FOAM];
extern int foam_count;
extern V3 ring_dir[25];
extern bool world_sunset;   /* golden hour (Spiral Canyon, Tsudoken Drift) */
/* The asphalt's header (made with HF_SPEC): its strips get the sun's sheen
   when drawn. A track with its own road texture sets it with f8m.road. */
extern const pvr_poly_hdr_t *h_sheen;

/* The golden-hour grade, applied to vertex colours as they are built and
   to the few sprites coloured at run time. */
static inline uint32_t sunset_grade(uint32_t c) {
    if(!world_sunset) return c;
    unsigned r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
    r = r * 278 >> 8;
    if(r > 255) r = 255;
    g = g * 206 >> 8;
    b = b * 158 >> 8;
    return (c & 0xff000000u) | (r << 16) | (g << 8) | b;
}

/* Headers more than one file draws with, made by world_init; a track's
   own are made by its *_init. The far sprites' is set by the track's
   build. */
extern const pvr_poly_hdr_t *h_road, *h_board, *h_curb, *h_sand, *h_grass, *h_barrier, *h_wood, *h_rock, *h_bark,
    *h_checker, *h_thatch, *h_crowd, *h_banner, *h_col, *h_water, *h_frond, *h_sky, *h_glow_add, *h_dash_glow,
    *h_stone, *h_flame, *h_hills, *h_islandrock, *h_far_sprite;
/* The sky roads (Neon Skyway's neon deck, Spiral Canyon's ribbon): the
   strips point at one header whose texture is swapped each frame through
   the phases, so the lights run along it. */
#define SKYROAD_PHASES 16
extern const pvr_poly_hdr_t *h_skyroad[SKYROAD_PHASES], *h_canyonroad;
extern pvr_poly_hdr_t sk_road_hdr;

#define WATER_Y (-0.35f)
/* Metres of lap per repeat of the asphalt textures, which are 16 m across.
   Square texels in metres: the chase camera squashes the road along the
   lap, and the PVR picks a mip level from that squashed side, so texels
   twice as dense along (8 m) cost the edge lines and grooves a level. */
#define ROAD_V_M 16.0f
#define F8_GRASS_Y (-0.06f)
#define MAX_SVALS 72
#define SK_EDGE     9.6f       /* the sky decks' road edge (deck to 8 m, then the light strip) */
#define SK_RAIL     10.2f      /* glass rails on the closed sides */
#define SK_DEPTH    0.7f       /* deck thickness */
#define CLOUD_Y     (-60.0f)
#define SK_SPARKLES 40
#define TH_FLAMES   96         /* Temple Hill's braziers, Spiral Canyon's torches */
#define TK_FUJI_DIR   (212.0f * DK_PI / 180.0f)   /* bearing, 0 = north */
#define TK_SUN_DIR    (240.0f * DK_PI / 180.0f)
/* Tsudoken is lit for early afternoon: a high white sun in the
   south-west, the day sky, a blue haze on the hills and Fuji.
   -DDK_TK_GOLDEN_HOUR builds it at golden hour instead (the warm grade,
   the low sun setting beside Fuji, the lanterns lit). */
#ifdef DK_TK_GOLDEN_HOUR
#define TK_AFTERNOON 0
#else
#define TK_AFTERNOON 1
#endif
#define AV_SUN_DIR    (235.0f * DK_PI / 180.0f)

#ifdef DK_CHUNK_STATS
/* Which kind of scenery each chunk is (by build order). */
enum { CK_GROUND, CK_DECK, CK_ROCK, CK_PROPS, CK_TORCH, CK_N };
extern int ck_first[CK_N + 1];
#endif

/* ---- The builder (world_mesh.c) ---- */

extern Chunk *cur_chunk;
extern int cur_strip_first;
extern const pvr_poly_hdr_t *strip_hdr;
extern uint32_t (*svp_tint)(V3 p, uint32_t c);

void chunk_begin(int list, float lod_far);
void chunk_end(void);
static inline void strip_begin(const pvr_poly_hdr_t *h) {
    strip_hdr = h;
    cur_strip_first = pool_n;
}
void strip_end(void);
void svp(V3 p, float u, float v, uint32_t c);
void svj(V3 p, float u, float v, uint32_t c);
uint32_t shade(uint32_t c, V3 n);
void quad(const pvr_poly_hdr_t *h, V3 a, V3 b, V3 c, V3 d, float u0, float v0, float u1, float v1, uint32_t col);
void box(const pvr_poly_hdr_t *h, V3 c, V3 half, float yaw, uint32_t col, float uv_scale);
void cylinder(const pvr_poly_hdr_t *h, V3 base, float r0, float r1, float height, int sides, uint32_t col0,
              uint32_t col1, float u_rep, float v_rep);
void cone(const pvr_poly_hdr_t *h, V3 apex, float radius, float drop, int sides, uint32_t ca, uint32_t cb);
uint32_t ground_color(float h, float db, bool grassy);

static inline float smooth01(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

static inline float seg_dist(V3 p, V3 a, V3 b) {
    float sx = b.x - a.x, sz = b.z - a.z;
    float t = clampf(((p.x - a.x) * sx + (p.z - a.z) * sz) / (sx * sx + sz * sz), 0.0f, 1.0f);
    float dx = p.x - a.x - sx * t, dz = p.z - a.z - sz * t;
    return sqrtf(dx * dx + dz * dz);
}

#ifdef DK_ROCK_TINT
/* Spiral Canyon's leak test (world_canyon.c). */
extern int rock_pass;
extern bool diag_on;
extern int diag_mode, diag_ref_first;
uint32_t diag_colour(V3 p, uint32_t c);
#endif

/* ---- Props (world_props.c) ---- */

void add_palm(V3 base, float height, float lean_yaw, float lean, uint32_t seed);
void add_umbrella(V3 base, uint32_t ca, uint32_t cb);
void add_hut(V3 c, float yaw);
void add_rock(V3 c, float r, float hgt, uint32_t seed, bool grass_top);
void add_arch(V3 c, float yaw);
void add_lighthouse(V3 c);
void add_boat(V3 c, float yaw, uint32_t hull);
void add_gantry(void);
void add_grandstand(float x0, float x1, float zf);
void add_sign(V3 c, float yaw);
void add_pier(float x, float z0);
void add_pond(V3 c, float rx, float rz);
#ifdef DK_PROP_TYPES
/* A file that places baked props includes their header before this one. */
void prop_place(const dk_prop_t *m, const pvr_poly_hdr_t *const *mats, uint32_t pt_mask, V3 p, float yaw, float scale,
                float lod_far, bool far);
#endif

/* ---- The road (world_road.c) ---- */

/* The materials of a day track's road (Coral Crossing, Temple Hill,
   Tsudoken Drift): set before its chunks are built. */
typedef struct {
    const pvr_poly_hdr_t *road, *curb, *shoulder, *barrier, *skirt;
    uint32_t cap, stripe_a, stripe_b;   /* barrier top, the jump faces' hazard stripes */
    float shoulder_uv;
    float wall_h, barrier_u;            /* barrier height, metres of lap per repeat of its texture */
    bool split_lod;                     /* the chunk is the near half of a split-screen pair (a coarse copy follows) */
    void (*extra)(const float *sv, int n);   /* more ribbons in the chunk (the verges) */
    bool skirt_ground;                  /* skirts wherever the road stands above f8m.ground, down to it */
    float half, edge;                   /* the road's half width and the curbs' outer edge */
    const pvr_poly_hdr_t *verge;        /* f8_verges and the coarse deck: from the barrier out over the ground */
    float (*ground)(float x, float z);  /* the ground's height there */
} RoadStyle;
extern RoadStyle f8m;
enum { RB_WALL0 = 1, RB_WALL1 = 2 };
void f8m_default(void);
int f8_chunk_svals(int c, float *out);
V3 f8_pt(float s, float d, float dy);
float f8_wall(float s);
void f8_ribbon(const float *sv, int n, const pvr_poly_hdr_t *h, float d0, float d1, float dy0, float dy1, int flags,
               float uscale, float vscale, bool world_uv, uint32_t c0, uint32_t c1);
int road_pieces(float sa, float sb);
void f8_road_ribbon(const float *sv, int n, const pvr_poly_hdr_t *h, float half, float vscale);
bool in_bridge(float s);
void f8_build_track_chunk(int c);
void f8_jump_face(float s, float dir);
void f8_verges(const float *sv, int n);
void f8_build_deck_coarse(int c);
float sk_road_dist(V3 p);

/* ---- The tracks ---- */

/* Emerald Beach (world_beach.c) */
float terrain_height(V3 p, float db, float shore);
void build_beach(void);

/* Coral Crossing (world_coral.c), whose headland Palm Harbour and Temple
   Hill share */
extern float gnd_xw, gnd_zn, gnd_zs;
float f8_ground(float x, float z);
float f8_ground_y(float x, float z);
void f8_build_ground(void);
void f8_foam(void);
void build_figure8(void);

/* Palm Harbour (world_harbour.c) */
bool hb_clear(V3 p, float margin);
void build_harbour(void);

/* Temple Hill (world_temple.c); its flames are Spiral Canyon's torches too */
extern V3 th_flame_p[TH_FLAMES];
extern int th_flame_n;
float th_terrain(float x, float z);
void th_banks(const float *sv, int n);
void build_temple(void);
void th_draw_flames(void);

/* Neon Skyway and Cyberstone (world_skyway.c), whose sky deck Spiral
   Canyon's road is built on */
extern float sk_vscale;
extern uint8_t sk_sparkle_kind[SK_SPARKLES];
extern int sk_beam_n;
void sk_init(void);
void sk_build_track_chunk(int c);
void sk_place_props(void);
void build_skyway(void);
void draw_night_distant(void);
void draw_sparkles(void);

/* Spiral Canyon (world_canyon.c) */
void dr_init(void);
float dr_terrain(float x, float z);
void dr_skirts(const float *sv, int n);
void dr_build_deck_coarse(int c);
void dr_hoop(float s);
void dr_place_props(void);
void build_canyon(void);
bool dr_in_rock(V3 p, float margin);
void dr_draw_pt(void);
void dr_draw_waterfalls(void);
void draw_canyon_hills(void);

/* Tsudoken Drift (world_tsudoken.c) */
void tk_init(void);
float tk_terrain(float x, float z);
void build_tsudoken(void);
bool tk_in_solid(V3 p, float margin);
bool tk_sight_clear(V3 a, V3 b);
void tk_draw_fuji(void);
void tk_draw_mist(void);
void tk_draw_hills(void);
void tk_draw_petals(void);
void tk_draw_glows(void);

/* Apple Valley (world_apple.c) */
void av_init(void);
float av_terrain(float x, float z);
void build_apple(void);
void av_draw_peak(void);
void av_draw_windsock(void);
void av_draw_tumbleweeds(void);
void av_draw_dust_devils(void);
void av_draw_ranges(void);

#endif
