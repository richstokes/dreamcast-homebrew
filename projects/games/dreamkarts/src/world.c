/* Track scenery: Emerald Beach, Coral Crossing, Palm Harbour, Temple Hill,
   Neon Skyway, Cyberstone, Spiral Canyon, Tsudoken Drift and Apple Valley.

   Static geometry (track surfaces, terrain, props, palm trees) is generated
   once into a pool of world-space triangle strips grouped into chunks with
   bounding spheres; each frame only visible chunks are transformed. The sea,
   sky, clouds, shore foam and dash panels animate, so they are emitted per
   frame.

   This file builds and draws it; each track's own scenery is built in
   world_<track>.c (world_int.h lists who does what). */
#include "world_int.h"
#include "prof.h"
#include <malloc.h>
#include <sh4zam/shz_sh4zam.h>

V3 world_sun_dir;
float world_time;
bool world_split;
bool world_night;
bool world_canyon;
/* Tsudoken Drift: a flat circuit on the plain under Mt Fuji. */
static bool world_tk;
/* Apple Valley: a kart club circuit in the Mojave desert. */
static bool world_av;
/* Golden hour (Spiral Canyon, Tsudoken Drift): a low sun, everything
   graded warm; the canyon's is a retrowave sun ahead down the start
   straight, Tsudoken's sets beside Fuji in the south-west. */
bool world_sunset;
static V3 sun_disc_dir;   /* where the sun's disc and lens flare sit */
/* Spiral Canyon's sun, along the start straight (sun_ahead) at
   DR_SUN_EL up, sinks by up to 35% of that over a race. */
#define DR_SUN_EL   0.11f
#define DR_SUN_SINK 0.35f
static V3 sun_ahead;
uint16_t world_light[3] = {256, 256, 256};

#ifdef DK_CHUNK_STATS
/* Which kind of scenery each chunk is (by build order), and what each
   kind submitted: strips and vertices whose sphere is on screen. */
int ck_first[CK_N + 1];
static uint32_t ck_strips[CK_N], ck_verts[CK_N], ck_frames;
static V3 ck_cam;
extern float world_time;
static int ck_kind(int i) {
    for(int k = CK_N - 1; k >= 0; k--)
        if(i >= ck_first[k]) return k;
    return 0;
}
void world_chunk_stats(bool print) {
    ck_frames = print ? 120 : 0;
    if(!print) {
        memset(ck_strips, 0, sizeof ck_strips);
        memset(ck_verts, 0, sizeof ck_verts);
    }
}
static void ck_frame(void) {
    if(++ck_frames < 120) return;
    static const char *const names[CK_N] = {"ground", "deck", "rock", "props", "torch"};
    char line[256], *o = line;
    o += sprintf(o, "DK CHUNKS t=%.0f cam (%.0f, %.0f) per frame:", (double)world_time, (double)ck_cam.x, (double)ck_cam.z);
    for(int k = 0; k < CK_N; k++) o += sprintf(o, " %s %lu/%lu", names[k], (unsigned long)(ck_strips[k] / ck_frames), (unsigned long)(ck_verts[k] / ck_frames));
    puts(line);
    memset(ck_strips, 0, sizeof ck_strips);
    memset(ck_verts, 0, sizeof ck_verts);
    ck_frames = 0;
}
#endif

/* Static, not malloc'd: from the heap, the pool once left it within a
   megabyte of the top of RAM (the music was linked in then), and KOS
   copies every sound effect through a temporary heap block its own size,
   so the bigger effects failed to load. In .bss it costs the same RAM but
   leaves the heap to the small, short-lived things. */
static RV pool_mem[MAX_POOL] __attribute__((aligned(32)));
RV *pool;
int pool_n;
Strip strips[MAX_STRIPS];
int strip_n;
Chunk chunks[MAX_CHUNKS];
int chunk_n;
Palm palms[MAX_PALMS];
int palm_n;
V3 far_sprites[MAX_SPRITES];
float far_sprite_h[MAX_SPRITES];
int far_sprite_n;
bool built_ok = true;
V3 foam_p[MAX_FOAM], foam_n[MAX_FOAM];
float foam_u[MAX_FOAM];
bool foam_on[MAX_FOAM];
int foam_count;
static V3 sky_dir[7][13];
V3 ring_dir[25];

/* Headers. */
const pvr_poly_hdr_t *h_road, *h_board, *h_curb, *h_sand, *h_grass, *h_barrier, *h_wood, *h_rock, *h_bark, *h_checker,
    *h_thatch, *h_crowd, *h_banner, *h_col, *h_water, *h_frond, *h_sky, *h_glow_add, *h_dash_glow, *h_stone, *h_flame,
    *h_hills, *h_islandrock, *h_far_sprite;
static const pvr_poly_hdr_t *h_dash, *h_palm_far, *h_cloud, *h_mount, *h_foam, *h_flare, *h_nightsky, *h_cloudsea,
    *h_retrosun, *h_sky_nz, *h_sundisc;
static const pvr_poly_hdr_t *h_sparkle;
const pvr_poly_hdr_t *h_skyroad[SKYROAD_PHASES], *h_canyonroad;
pvr_poly_hdr_t sk_road_hdr __attribute__((aligned(32)));

int world_gates;
float world_gate_s[WORLD_MAX_GATES];

static float beach_ground_y(float x, float z, int hint) {
    TrackPos tp;
    track_locate(v3(x, 0, z), hint, &tp);
    float db = tp.d > 0 ? tp.d - track_wall_out_at(&tp) : -tp.d - track_wall_in_at(&tp);
    return db > 0 ? terrain_height(v3(x, 0, z), db, track.smp[tp.idx].shore) : 0.0f;
}

float world_ground_y(float x, float z) {
    switch(track.id) {
        case TRACK_BEACH: return beach_ground_y(x, z, -1);
        case TRACK_FIGURE8:
        case TRACK_HARBOUR: return f8_ground_y(x, z);
        case TRACK_TEMPLE: return th_terrain(x, z) + F8_GRASS_Y;
        case TRACK_CANYON: return dr_terrain(x, z);
        case TRACK_TSUDOKEN: return tk_terrain(x, z);
        case TRACK_APPLE: return av_terrain(x, z);
        default: return -100.0f;   /* clouds */
    }
}

void world_init(void) {
    world_sun_dir = vnorm(v3(0.30f, 0.55f, 0.78f));
    pool = pool_mem;
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
    h_water = r_header(TEX_WATER, HF_OP | HF_SPEC);
    h_sparkle = r_header(TEX_STAR, HF_TR | HF_ADD | HF_NOFOG);
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
    h_cloudsea = r_header(TEX_CLOUDSEA, HF_OP);
    sk_init();
    h_canyonroad = r_header(TEX_CANYONROAD, HF_OP);
    h_hills = r_header(TEX_NONE, HF_TR | HF_NOFOG);
    h_islandrock = r_header(TEX_ISLANDROCK, HF_OP);
    dr_init();
    h_retrosun = r_header(TEX_RETROSUN, HF_TR | HF_NOFOG);
    h_sky_nz = r_header(TEX_NONE, HF_OP | HF_NOFOG | HF_NOZWRITE);
    tk_init();
    h_sundisc = r_header(TEX_SUNDISC, HF_TR | HF_NOFOG);
    h_far_sprite = h_palm_far;
    av_init();

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

/* The road ribbon's geometry, kept for world_surface_y: each chunk's
   sample distances (f8_chunk_svals, the list every road ribbon is built
   from; Emerald Beach draws the plain samples, which is the same list on
   its flat road), the ribbon's edge heights at each, and how many pieces
   each quad is drawn in (road_pieces). */
#define SURF_CHUNKS (TRACK_SAMPLES / 8)
static float surf_s[SURF_CHUNKS][MAX_SVALS];
static float surf_y[SURF_CHUNKS][MAX_SVALS][2];   /* left edge, right edge */
static uint8_t surf_m[SURF_CHUNKS][MAX_SVALS];    /* pieces from sample k to k+1 */
static uint8_t surf_n[SURF_CHUNKS];
static float surf_half;
static bool surf_built;

static void surface_table_build(void) {
    surf_half = track.in_sky && !world_canyon ? SK_EDGE : 8.0f;
    for(int c = 0; c < SURF_CHUNKS; c++) {
        int n = f8_chunk_svals(c, surf_s[c]);
        surf_n[c] = (uint8_t)n;
        for(int k = 0; k < n; k++) {
            surf_y[c][k][0] = track_road_y(surf_s[c][k], -surf_half);
            surf_y[c][k][1] = track_road_y(surf_s[c][k], surf_half);
            surf_m[c][k] = (uint8_t)(k + 1 < n ? road_pieces(surf_s[c][k], surf_s[c][k + 1]) : 1);
        }
    }
    surf_built = true;
}

float world_surface_y(float s, float d) {
    if(!surf_built) return track_road_y(s, d);
    s = track_wrap(s);
    int c = (int)(s / track.ds) / 8;
    if(c >= SURF_CHUNKS) c = SURF_CHUNKS - 1;
    const float *sv = surf_s[c];
    int n = surf_n[c], k = 0;
    while(k + 2 < n && s >= sv[k + 1]) k++;
    float u = clampf((s - sv[k]) / (sv[k + 1] - sv[k]), 0.0f, 1.0f);
    /* The piece of the quad s is in, and where in it; its corners lie on
       the quad's edges. */
    int m = surf_m[c][k], j = (int)(u * (float)m);
    if(j >= m) j = m - 1;
    u = u * (float)m - (float)j;
    float t0 = (float)j / (float)m, t1 = (float)(j + 1) / (float)m;
    float y00 = lerpf(surf_y[c][k][0], surf_y[c][k + 1][0], t0), y10 = lerpf(surf_y[c][k][0], surf_y[c][k + 1][0], t1);
    float y01 = lerpf(surf_y[c][k][1], surf_y[c][k + 1][1], t0), y11 = lerpf(surf_y[c][k][1], surf_y[c][k + 1][1], t1);
    /* Beyond the ribbon (curbs, shoulders, against a wall) its triangles
       are extended: the neighbouring strips share the samples and, near
       enough, their banking. */
    float v = clampf((d + surf_half) / (2.0f * surf_half), -0.5f, 1.5f);
    /* The strip runs left, right at each sample, so its triangles are
       (left k, right k, left k+1) and (right k, left k+1, right k+1). */
    if(u + v <= 1.0f) return y00 + (y10 - y00) * u + (y01 - y00) * v;
    return y11 + (y01 - y11) * (1.0f - u) + (y10 - y11) * (1.0f - v);
}

void world_build(void) {
    world_gates = 0;
    if(!pool) return;
    pool_n = strip_n = chunk_n = palm_n = far_sprite_n = foam_count = 0;
    cur_chunk = NULL;
    built_ok = true;
    world_canyon = track.id == TRACK_CANYON;
    world_tk = track.id == TRACK_TSUDOKEN;
    world_av = track.id == TRACK_APPLE;
    world_night = track.in_sky && !world_canyon;
#ifdef DK_ROCK_TINT
    world_sunset = false;   /* the leak test reads colours */
#else
    world_sunset = world_canyon || (world_tk && !TK_AFTERNOON);
#endif
    h_far_sprite = h_palm_far;   /* Tsudoken's and Apple Valley's builds set their own */
    th_flame_n = 0;
    /* The sun, or at night the moon (north-west, low over the waves). */
    world_sun_dir = world_night ? vnorm(v3(-0.50f, 0.33f, 0.80f)) : vnorm(v3(0.30f, 0.55f, 0.78f));
    sun_disc_dir = world_sun_dir;
    /* Moonlight on the karts: dimmer and bluer (applied as they are painted). */
    world_light[0] = world_night ? 176 : 256;
    world_light[1] = world_night ? 168 : 256;
    world_light[2] = world_night ? 240 : 256;
    if(world_tk && TK_AFTERNOON) {
        world_sun_dir = vnorm(v3(sinf(220.0f * DK_PI / 180.0f), 1.05f, cosf(220.0f * DK_PI / 180.0f)));
        sun_disc_dir = world_sun_dir;
    }
    if(world_av) {
        /* A bright desert afternoon, the sun in the south-west. */
        world_sun_dir = vnorm(v3(sinf(AV_SUN_DIR), 0.95f, cosf(AV_SUN_DIR)));
        sun_disc_dir = world_sun_dir;
    }
    if(world_sunset) {
        /* Spiral Canyon: dead ahead along the start straight, the light
           from ~20 degrees up, the disc low enough for the far hills to cut
           off its foot. Tsudoken: in the south-west beside Fuji, lower. */
        V3 a = track_point(track_wrap(-30.0f), 0.0f), b = track_point(30.0f, 0.0f);
        V3 f = vnorm(v3(b.x - a.x, 0.0f, b.z - a.z));
        if(world_tk) f = v3(sinf(TK_SUN_DIR), 0.0f, cosf(TK_SUN_DIR));
        world_sun_dir = vnorm(vadd(f, v3(0, world_tk ? 0.30f : 0.36f, 0)));
        sun_disc_dir = vnorm(vadd(f, v3(0, world_tk ? 0.075f : DR_SUN_EL, 0)));
        sun_ahead = f;
        world_light[0] = 256;   /* no higher: the kart paint doesn't clamp */
        world_light[1] = 209;
        world_light[2] = 172;
    }
    if(track.id == TRACK_FIGURE8) build_figure8();
    else if(track.id == TRACK_HARBOUR) build_harbour();
    else if(track.id == TRACK_TEMPLE) build_temple();
    else if(track.id == TRACK_CANYON) build_canyon();
    else if(track.id == TRACK_TSUDOKEN) build_tsudoken();
    else if(track.id == TRACK_APPLE) build_apple();
    else if(track.in_sky) build_skyway();
    else build_beach();
    surface_table_build();
    /* Release and console builds log only the first build: under dcload-ip
       each printf stalls the console with IRQs off. */
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    bool log = true;
#else
    static bool logged;
    bool log = !logged;
    logged = true;
#endif
    if(!log) return;
#ifdef DK_AUTOTEST
    /* Heap headroom: the break against the top of the 16 MB. */
    extern void *sbrk(intptr_t);
    struct mallinfo mi = mallinfo();
    printf("DreamKarts memory: heap %lu KiB in use, %lu KiB free in it, %lu KiB above the break\n",
           (unsigned long)mi.uordblks / 1024, (unsigned long)mi.fordblks / 1024,
           (unsigned long)(0x8d000000u - (uintptr_t)sbrk(0)) / 1024);
#endif
    printf("DreamKarts world: %s, %d chunks, %d strips, %d vertices (%lu KiB), %d palms%s\n", track.name, chunk_n,
           strip_n, pool_n, (unsigned long)(pool_n * sizeof(RV) / 1024), palm_n, built_ok ? "" : " [POOL OVERFLOW]");
}

void world_update(float dt) {
    world_time += dt;
#ifdef DK_CHUNK_STATS
    if(world_canyon || world_tk || world_av) ck_frame();
#endif
    /* The road's lights run forward a repeat (about 8 m) every 0.55 s. */
    if(world_canyon) sk_road_hdr = *h_canyonroad;
    else if(world_night) {
        int phase = (int)(world_time * (SKYROAD_PHASES / 0.55f)) % SKYROAD_PHASES;
        sk_road_hdr = *h_skyroad[phase];
    }
}

void world_race_progress(float frac) {
    if(!world_canyon || !world_sunset) return;
    /* Only the disc: the light stays put, as the scenery's shading is
       baked from it. */
    float el = DR_SUN_EL * (1.0f - DR_SUN_SINK * clampf(frac, 0.0f, 1.0f));
    sun_disc_dir = vnorm(vadd(sun_ahead, v3(0, el, 0)));
}

#define SPLIT_NEAR 25.0f   /* split screen: the road in full detail this near */

static void draw_chunks(int list) {
    V3 cam = r_cam.pos;
    for(int i = 0; i < chunk_n; i++) {
        const Chunk *c = &chunks[i];
        if(c->list != list) continue;
        if(world_split && c->far) continue;
        /* A split screen draws a light copy of the road beyond SPLIT_NEAR
           (its pair's sphere decides, so exactly one of the two is drawn);
           the full screen only ever the detailed one. */
        if(c->lod == 3 && !world_split) continue;   /* split-screen only */
        if(c->lod == 1 || c->lod == 2) {
            if(!world_split) {
                if(c->lod == 2) continue;
            }
            else {
                const Chunk *f = c->lod == 1 ? c : c - 1;
                V3 df = vsub(f->c, cam);
                float lim = SPLIT_NEAR + f->r;
                bool near = vdot(df, df) < lim * lim;
                if(near != (c->lod == 1)) continue;
            }
        }
#ifdef DK_ROCK_TINT
        if(world_canyon && (i >= diag_ref_first) != (diag_mode == 1)) continue;
#endif
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
#ifdef DK_CHUNK_STATS
            if((world_canyon || world_tk || world_av) && r_sphere_visible(st->c, st->r)) {
                int k = ck_kind(i);
                ck_cam = r_cam.pos;
                ck_strips[k]++;
                ck_verts[k] += st->count;
            }
#endif
            r_hdr(st->hdr);
            r_strip_bounded(pool + st->first, st->count, st->c, st->r);
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
/* The golden-hour sky: orange under the sun, pink and purple round the
   rest of the horizon, up to indigo overhead. */
static void draw_sunset_sky(void) {
    static const uint32_t dr_sun_side[7] = {
        0xffffb070, 0xffffb070, 0xffff9060, 0xffff6878, 0xffc04898, 0xff5a2c8c, 0xff261c5e
    };
    static const uint32_t dr_far_side[7] = {
        0xfff08088, 0xfff08088, 0xffd86090, 0xffa04490, 0xff642e86, 0xff3a2272, 0xff261c5e
    };
    /* Tsudoken: gold under the sun through peach to lavender and a dusky
       blue overhead, the far side rose into violet. */
    static const uint32_t tk_sun_side[7] = {
        0xffffd690, 0xffffd690, 0xffffb478, 0xffec8c82, 0xffbe78a0, 0xff785aa0, 0xff2c2c60
    };
    static const uint32_t tk_far_side[7] = {
        0xfff4b896, 0xfff4b896, 0xffdc9696, 0xffaa78a0, 0xff786096, 0xff504888, 0xff2c2c60
    };
    const uint32_t *sun_side = world_tk ? tk_sun_side : dr_sun_side;
    const uint32_t *far_side = world_tk ? tk_far_side : dr_far_side;
    const int az = 12;
    const float R = 1500.0f;
    V3 c = r_cam.pos, sd = vnorm(v3(sun_disc_dir.x, 0, sun_disc_dir.z));
    /* No depth, so the sun's glow (as far out as the dome) is never cut
       by the dome's facets. */
    r_hdr(h_sky_nz);
    for(int e = 0; e < 6; e++) {
        RV v[2 * 13];
        int n = 0;
        for(int i = 0; i <= az; i++) {
            for(int k = 1; k >= 0; k--) {
                V3 d = sky_dir[e + k][i];
                float t = 0.5f + 0.5f * (d.x * sd.x + d.z * sd.z) / fmaxf(0.01f, sqrtf(d.x * d.x + d.z * d.z));
                v[n++] = (RV){c.x + d.x * R, c.y + d.y * R, c.z + d.z * R, 0, 0,
                              color_lerp(far_side[e + k], sun_side[e + k], t * t)};
            }
        }
        r_strip(v, n);
    }
}

static void draw_sky(void) {
    if(world_night) {
        draw_night_sky();
        return;
    }
    if(world_sunset) {
        draw_sunset_sky();
        return;
    }
    static const uint32_t day_cols[] = {
        0xffbde6f8, 0xffbde6f8, 0xff98d2f6, 0xff6cb6f0, 0xff3f90e4, 0xff2a74d8, 0xff2266d0
    };
    /* Apple Valley: the desert's washed-out horizon under a deep blue. */
    static const uint32_t av_cols[] = {
        0xffcedce8, 0xffcedce8, 0xffa8c8e8, 0xff74a8e2, 0xff4686d8, 0xff2a68cc, 0xff1e56c0
    };
    const uint32_t *cols = world_av ? av_cols : day_cols;
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

/* The sea's and the cloud sea's vertex colours depend only on where the
   camera-centred grid lies, which changes when the camera crosses into
   another cell: keep the colours of the last two grids (one per
   split-screen view). Row j's colours at z = za and zb, column i. A colour
   function that changed over time would need this cache dropped. */
#define GRID_MAX 16
typedef uint32_t (*GridColorFn)(float x, float z);
typedef struct {
    float cx, cz, cell;
    GridColorFn fn;
    uint32_t za[GRID_MAX][GRID_MAX + 1], zb[GRID_MAX][GRID_MAX + 1];
} GridColors;

static const GridColors *grid_colors(float cx, float cz, float cell, int n, float x0, float z0, GridColorFn fn) {
    static GridColors cache[2];
    static int next;
    for(int c = 0; c < 2; c++)
        if(cache[c].fn == fn && cache[c].cx == cx && cache[c].cz == cz && cache[c].cell == cell) return &cache[c];
    GridColors *g = &cache[next];
    next ^= 1;
    g->fn = fn;
    g->cx = cx;
    g->cz = cz;
    g->cell = cell;
    for(int j = 0; j < n; j++) {
        float za = z0 + j * cell, zb = za + cell;
        for(int i = 0; i <= n; i++) {
            float x = x0 + i * cell;
            g->zb[j][i] = fn(x, zb);
            g->za[j][i] = fn(x, za);
        }
    }
    return g;
}

static void draw_clouds_below(void) {
    const float cell = world_split ? 90.0f : 64.0f;
    const int n = world_split ? 10 : 14;
    float cx = floorf(r_cam.pos.x / cell) * cell, cz = floorf(r_cam.pos.z / cell) * cell;
    float x0 = cx - n * cell * 0.5f, z0 = cz - n * cell * 0.5f;
    float su = fmodf(world_time * 0.006f, 1.0f), sv2 = fmodf(world_time * 0.004f, 1.0f);
    const float uvs = 1.0f / 110.0f;
    r_hdr(h_cloudsea);
    GridColorFn fn = cloud_color;
    const GridColors *gc = grid_colors(cx, cz, cell, n, x0, z0, fn);
    RV row[2 * (14 + 1)];
    for(int j = 0; j < n; j++) {
        float za = z0 + j * cell, zb = za + cell;
        V3 mid = v3(cx, CLOUD_Y, (za + zb) * 0.5f);
        if(!r_sphere_visible(mid, n * cell * 0.75f)) continue;
        int k = 0;
        for(int i = 0; i <= n; i++) {
            float x = x0 + i * cell;
            row[k++] = (RV){x, CLOUD_Y, zb, x * uvs + su, zb * uvs + sv2, gc->zb[j][i]};
            row[k++] = (RV){x, CLOUD_Y, za, x * uvs + su, za * uvs + sv2, gc->za[j][i]};
#ifdef DK_XF_CHECK
            extern uint32_t xf_checked, xf_mismatch;
            xf_checked += 2;
            xf_mismatch += (fn(x, zb) != gc->zb[j][i]) + (fn(x, za) != gc->za[j][i]);
#endif
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

/* Sun glints, after Shenmue II's sea (s2/world_render.cpp
   UpdateWaveNormals, which writes pow(N.L, 42.5) into each wave vertex's
   offset colour): the grid's vertices take their normal from a few
   swells' slopes, and a Blinn highlight toward the camera goes in the
   offset colour, which the PVR adds after the texture. The swells alias
   on the 45 m grid, so in effect each vertex twinkles on its own clock;
   the half vector keeps the glints on the path toward the sun. */
#define GLINT_SQUARINGS 4     /* exponent 2^n */
#define GLINT_GAIN 0.5f      /* the sheen; the sparkles carry the glints */
#define GLINT_CUT 0.8f        /* N.H below this adds nothing worth sending */
static uint32_t glint[GRID_MAX + 1][GRID_MAX + 1];
static bool glint_done[GRID_MAX + 1];

/* Sparkles: the per-vertex sheen is 45 m across, too broad to twinkle, so
   small stars flash on and off around the vertices glinting brightest.
   The sea pass gathers those (a reservoir sample, so far rows count as
   much as near ones); the translucent pass spawns stars among them. */
#define SPARKLE_N 40         /* spawned on screen only */
#define SPARKLE_MIN 40        /* vertex glint (0-255) worth a star */
#define SPARKLE_SIZE 0.011f   /* metres a metre away: ~5 px on screen */
#define SPARKLE_CANDS 96
static struct { float x, z; float l; } sp_cand[SPARKLE_CANDS];
static int sp_cand_n, sp_seen;
static float sp_cell;
typedef struct { float x, z, t, life, l; } Sparkle;
static Sparkle sparkle[SPARKLE_N];
static float sparkle_t;
static uint32_t sp_seed = 0x5eaf00du;   /* its own: dk_rand drives the race */

static float sp_rand(void) {
    sp_seed = sp_seed * 1664525u + 1013904223u;
    return (float)(sp_seed >> 8) * (1.0f / 16777216.0f);
}

/* Direction (x, z), wavenumber (radians a metre, 2 pi / wavelength) and
   peak slope; each runs at the deep-water sqrt(9.8 k). */
static const struct { float dx, dz, k, slope; } swell[] = {
    {0.96f, 0.28f, 0.170f, 0.13f}, {-0.42f, 0.91f, 0.273f, 0.10f},
    {0.62f, -0.78f, 0.103f, 0.09f}, {-0.81f, -0.59f, 0.898f, 0.06f},
};
#define SWELLS 4
/* The half vector's least height that the steepest swell (slopes summed,
   ~21 degrees) can still tip past GLINT_CUT (0.8: ~37 degrees). */
#define GLINT_MIN_HY 0.53f
static float swell_t[SWELLS];
/* Each swell's phase k (dx x + dz z) - t splits into a column part and a
   row part, so cos(A + B) = cos A cos B - sin A sin B from tables: per
   column (times the slope) once a view, per row in glint_row. */
static float swell_ca[SWELLS][GRID_MAX + 1], swell_sa[SWELLS][GRID_MAX + 1];

static void swell_columns(float x0, float cell, int n) {
    for(int k = 0; k < SWELLS; k++) {
        float t = sqrtf(9.8f * swell[k].k) * world_time;
        swell_t[k] = t - DK_TAU * (float)(int)(t * (1.0f / DK_TAU));
        /* Column to column the angle steps by k dx cell: rotate. */
        shz_sincos_t a = shz_sincosf(swell[k].k * swell[k].dx * x0), st = shz_sincosf(swell[k].k * swell[k].dx * cell);
        float c = a.cos, s = a.sin;
        for(int i = 0; i <= n; i++) {
            swell_ca[k][i] = swell[k].slope * c;
            swell_sa[k][i] = swell[k].slope * s;
            float nc = c * st.cos - s * st.sin;
            s = s * st.cos + c * st.sin;
            c = nc;
        }
    }
}

/* Row j's offset colours, at z0 + j * cell. */
static void glint_row(int j, float x0, float z0, float cell, int n) {
    glint_done[j] = true;
    V3 L = world_sun_dir, c = r_cam.pos;
    float z = z0 + j * cell, vy = c.y - WATER_Y, vz = c.z - z;
    float cb[SWELLS], sb[SWELLS];
    for(int k = 0; k < SWELLS; k++) {
        shz_sincos_t b = shz_sincosf(swell[k].k * swell[k].dz * z - swell_t[k]);
        cb[k] = b.cos;
        sb[k] = b.sin;
    }
    /* r_sphere_visible's test, stepped along the row: each column moves
       the vertex cell * the camera's axes' x in view space. Only vertices
       of triangles that can be on screen get a glint, and every vertex of
       one lies within a cell's diagonal of any point of it. */
    float tx, ty;
    r_frustum_slopes(&tx, &ty);
    V3 d0 = v3(x0 - c.x, WATER_Y - c.y, z - c.z);
    float qz = vdot(d0, r_cam.fwd), qx = vdot(d0, r_cam.right), qy = vdot(d0, r_cam.up);
    float sz = cell * r_cam.fwd.x, sx = cell * r_cam.right.x, sy = cell * r_cam.up.x;
    const float rad = cell * 1.5f, srad = cell * 0.75f;
    for(int i = 0; i <= n; i++, qz += sz, qx += sx, qy += sy) {
        float x = x0 + i * cell, vx = c.x - x;
        glint[j][i] = 0;
        if(qz + rad < R_NEAR_W || fabsf(qx) > qz * tx + rad * 1.6f || fabsf(qy) > qz * ty + rad * 1.4f) continue;
        float iv = shz_inv_sqrtf(vx * vx + vy * vy + vz * vz);
        float hx = L.x + vx * iv, hy = L.y + vy * iv, hz = L.z + vz * iv;
        float ih = shz_inv_sqrtf(hx * hx + hy * hy + hz * hz);
        if(hy * ih < GLINT_MIN_HY) continue;
        float gx = 0, gz = 0;
        for(int k = 0; k < SWELLS; k++) {
            float cs = swell_ca[k][i] * cb[k] - swell_sa[k][i] * sb[k];
            gx += swell[k].dx * cs;
            gz += swell[k].dz * cs;
        }
        /* N = (-gx, 1, -gz) / |..| */
        float d = (hy - gx * hx - gz * hz) * ih * shz_inv_sqrtf(1.0f + gx * gx + gz * gz);
        if(d <= GLINT_CUT) continue;
        for(int s = 0; s < GLINT_SQUARINGS; s++) d *= d;
        int l = (int)(fminf(d * GLINT_GAIN, 1.0f) * 255.0f), raw = (int)(fminf(d, 1.0f) * 255.0f);
        if(l > 2) glint[j][i] = (uint32_t)l << 16 | (uint32_t)(l * 244 >> 8) << 8 | (uint32_t)(l * 214 >> 8);
        if(raw >= SPARKLE_MIN && qz + srad >= R_NEAR_W && fabsf(qx) <= qz * tx + srad * 1.6f &&
           fabsf(qy) <= qz * ty + srad * 1.4f) {
            int slot = sp_cand_n < SPARKLE_CANDS ? sp_cand_n++ : (int)(sp_rand() * (float)(sp_seen + 1));
            if(slot < SPARKLE_CANDS) {
                sp_cand[slot].x = x;
                sp_cand[slot].z = z;
                sp_cand[slot].l = (float)raw * (1.0f / 255.0f);
            }
            sp_seen++;
        }
    }
}

/* Animated sea: a camera-centred grid out to ~360 m, then wide bands to
   the horizon. Only the northern half-plane is ever visible. */
static void draw_sea(void) {
    if(world_night) {
        draw_clouds_below();
        return;
    }
    if(world_canyon || world_tk || world_av) return;   /* the valley floor, the plain and the desert are static scenery */
    const float cell = world_split ? 60.0f : 45.0f;
    const int n = world_split ? 10 : 16;
    float cx = floorf(r_cam.pos.x / cell) * cell, cz = floorf(r_cam.pos.z / cell) * cell;
    float x0 = cx - n * cell * 0.5f, z0 = cz - n * cell * 0.5f;
    float su = fmodf(world_time * 0.018f, 1.0f), sv2 = fmodf(world_time * 0.011f, 1.0f);
    r_hdr(h_water);
    const GridColors *gc = grid_colors(cx, cz, cell, n, x0, z0, sea_color);
    RV row[2 * (16 + 1)];
    uint32_t spec[2 * (16 + 1)];
    memset(glint_done, 0, sizeof(glint_done));
    sp_cand_n = sp_seen = 0;
    sp_cell = cell;
    swell_columns(x0, cell, n);
    for(int j = 0; j < n; j++) {
        float za = z0 + j * cell, zb = za + cell;
        if(zb < -60.0f) continue;
        V3 mid = v3(cx, WATER_Y, (za + zb) * 0.5f);
        if(!r_sphere_visible(mid, n * cell * 0.75f)) continue;
        int k = 0;
        if(!glint_done[j]) glint_row(j, x0, z0, cell, n);
        if(!glint_done[j + 1]) glint_row(j + 1, x0, z0, cell, n);
        for(int i = 0; i <= n; i++) {
            float x = x0 + i * cell;
            spec[k] = glint[j + 1][i];
            spec[k + 1] = glint[j][i];
            row[k++] = (RV){x, WATER_Y, zb, x / 26.0f + su, zb / 26.0f + sv2, gc->zb[j][i]};
            row[k++] = (RV){x, WATER_Y, za, x / 26.0f + su, za / 26.0f + sv2, gc->za[j][i]};
#ifdef DK_XF_CHECK
            extern uint32_t xf_checked, xf_mismatch;
            xf_checked += 2;
            xf_mismatch += (sea_color(x, zb) != gc->zb[j][i]) + (sea_color(x, za) != gc->za[j][i]);
#endif
        }
        r_strip_spec(row, spec, k);
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

/* The dash pads' chevrons point toward v = 0, at s1, and run forward that
   way. The scroll stays inside one repeat of the texture: the real PVR
   loses texture precision on large UVs, so a scroll that grew with
   world_time broke the chevrons up after a long session (Flycast does not
   show it). */
static void draw_dash_pads(void) {
    float scroll = fmodf(world_time * 2.2f, 1.0f);
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

/* Shenmue's fog table (s1/task/wexe.cpp): the PVR's 128 entries run from
   far (entry 0) toward the eye, sixteen to each halving of the distance,
   and fog falls by an even step per entry. So the haze starts at near,
   thickens quickly at first then ever more gently, and reaches cap at far
   rather than full, leaving distant shapes as tinted outlines. */
static void fog_table_octaves(float near, float far, float cap) {
    float t[129];
    int n = (int)(16.0f * log2f(far / near));
    for(int k = 0; k <= 128; k++) t[k] = k < n ? cap * (float)(n - k) / (float)n : 0.0f;
    pvr_fog_far_depth(far);
    pvr_fog_table_custom(t);
}

void world_set_fog(bool split) {
    /* The fog table is global: pull it in for the shorter split-screen view.
       The PVR reads the table while it renders, so the table only changes
       once the previous frame's render has finished, before a new scene.
       At night it fades to the purple of the sky's horizon. */
    static int fog_mode = -1;
    int mode = (int)split | (world_night ? 2 : 0) | (world_canyon ? 4 : 0) | (world_sunset ? 8 : 0) | (world_tk ? 16 : 0) |
               (world_av ? 32 : 0);
    if(fog_mode == mode) return;
    fog_mode = mode;
    pvr_wait_ready();
    if(world_night) {
        pvr_fog_table_color(1.0f, 20 / 255.0f, 16 / 255.0f, 56 / 255.0f);
        pvr_set_bg_color(20 / 255.0f, 16 / 255.0f, 56 / 255.0f);
    }
    else if(world_av) {
        /* The desert's pale haze, the sky's horizon. */
        pvr_fog_table_color(1.0f, 206 / 255.0f, 220 / 255.0f, 232 / 255.0f);
        pvr_set_bg_color(206 / 255.0f, 220 / 255.0f, 232 / 255.0f);
    }
    else if(world_tk && TK_AFTERNOON) {
        /* The day sky's horizon: a light blue haze. */
        pvr_fog_table_color(1.0f, 0.74f, 0.90f, 0.97f);
        pvr_set_bg_color(0.74f, 0.90f, 0.97f);
    }
    else if(world_tk) {
        /* A peach haze, the colour of the horizon away from the sun. */
        pvr_fog_table_color(1.0f, 0.955f, 0.72f, 0.59f);
        pvr_set_bg_color(0.955f, 0.72f, 0.59f);
    }
    else if(world_sunset) {
        /* A rose haze, between the horizon's orange and its pink. */
        pvr_fog_table_color(1.0f, 0.97f, 0.56f, 0.50f);
        pvr_set_bg_color(0.97f, 0.56f, 0.50f);
    }
    else {
        pvr_fog_table_color(1.0f, 0.74f, 0.89f, 0.98f);
#ifdef DK_ROCK_TINT
        if(world_canyon) pvr_set_bg_color(0, 0, 0);
        else
#endif
        pvr_set_bg_color(0.55f, 0.82f, 0.98f);
    }
    /* The two open plains: a haze from close in. Tsudoken's is light, so
       the cherry trees keep their pink, and the desert has only a thin
       heat haze over the sand. */
    if(world_tk || world_av) {
        float k = split ? 0.5f : 1.0f;
        if(world_tk) fog_table_octaves(50.0f * k, 1100.0f * k, 0.85f);
        else fog_table_octaves(30.0f * k, 900.0f * k, 0.45f);
        return;
    }
    /* The sky tracks see further (Spiral Canyon by day too). */
    bool sky = track.in_sky;
    if(split) pvr_fog_table_linear(sky ? 140.0f : 110.0f, sky ? 520.0f : 400.0f);
    else pvr_fog_table_linear(sky ? 240.0f : 160.0f, sky ? 1300.0f : 820.0f);
}

/* True where p is in solid scenery, or less than margin metres from it:
   Spiral Canyon's rock (not a tunnel's bore), Tsudoken's ground, raised
   road, barriers and props. For keeping the spectator's cameras and their
   sight lines clear of it; other tracks have none. */
bool world_solid(V3 p, float margin) {
    if(world_tk) return tk_in_solid(p, margin);
    return world_canyon && dr_in_rock(p, margin);
}

/* True when nothing solid stands between a and b (world_solid). */
bool world_sight_clear(V3 a, V3 b) {
    if(world_tk) return tk_sight_clear(a, b);
    if(!world_canyon) return true;
    int steps = (int)(vlen(vsub(b, a)) / 2.0f);
    for(int i = 1; i < steps; i++)
        if(dr_in_rock(vlerp(a, b, (float)i / steps), 0.3f)) return false;
    return true;
}

void world_draw_op(void) {
    r_load_vp();
    PROF_BEGIN(pm);
    if(!(dk_probe_off & PROBE_SKY)) draw_sky();
    if(world_tk && !(dk_probe_off & PROBE_DISTANT)) tk_draw_fuji();
    if(world_av && !(dk_probe_off & PROBE_DISTANT)) av_draw_peak();
    PROF_LAP(PS_SKY, pm);
    if(!(dk_probe_off & PROBE_SEA)) draw_sea();
    PROF_LAP(PS_SEA, pm);
    if(!(dk_probe_off & PROBE_WORLD)) {
        draw_chunks(HF_OP);
        PROF_LAP(PS_CHUNKS, pm);
        draw_dash_pads();
        if(world_av) av_draw_windsock();
    }
}

void world_draw_pt(void) {
    r_load_vp();
    draw_chunks(HF_PT);
#ifdef DK_ROCK_TINT
    if(world_canyon) return;   /* the leak test: no sprites over the rock */
#endif
    dr_draw_pt();
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
        r_billboard_y(palms[i].base, palms[i].height * 0.62f, palms[i].height * 1.25f, 0, 0, 1, 1, sunset_grade(0xffffffff));
    }
    r_hdr(h_far_sprite);
    for(int i = 0; i < far_sprite_n; i += world_split ? 2 : 1) {
        V3 mid = vadd(far_sprites[i], v3(0, far_sprite_h[i] * 0.6f, 0));
        if(!r_sphere_visible(mid, far_sprite_h[i])) continue;
        r_billboard_y(far_sprites[i], far_sprite_h[i] * 0.62f, far_sprite_h[i] * 1.25f, 0, 0, 1, 1, sunset_grade(0xffd8e8d8));
    }
    if(world_av) av_draw_tumbleweeds();
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

static void draw_clouds_and_mountains(void) {
    if(world_night) {
        draw_night_distant();
        return;
    }
    V3 c = r_cam.pos;
    if(world_tk) {
        /* The low sun first, then Fuji, then the hills in front of both. */
        V3 sp = vmad(c, sun_disc_dir, 1460.0f);
        if(TK_AFTERNOON) {
            if(r_sphere_visible(sp, 200.0f)) {
                r_hdr(h_glow_add);
                r_billboard(sp, 240.0f, 240.0f, 0, 0, 0, 1, 1, argb(200, 255, 250, 210));
                r_billboard(sp, 80.0f, 80.0f, 0, 0, 0, 1, 1, argb(255, 255, 255, 240));
            }
        }
        else if(r_sphere_visible(sp, 500.0f)) {
            r_hdr(h_glow_add);
            r_billboard(sp, 1000.0f, 700.0f, 0, 0, 0, 1, 1, argb(130, 255, 150, 90));
            r_hdr(h_sundisc);
            r_billboard(sp, 170.0f, 170.0f, 0, 0, 0, 1, 1, 0xffffffff);
        }
        tk_draw_mist();    /* Fuji itself was drawn with the opaque scenery */
        tk_draw_hills();
    }
    else if(world_canyon) {
        draw_canyon_hills();
    }
    else if(world_av) {
        av_draw_ranges();   /* the brown mountain was drawn with the opaque scenery */
    }
    else {
        /* Distant mountains ring. */
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
    }

    /* Clouds drift round the sky at depths from 950 to 1500 m, drawn far
       to near since the translucent list isn't sorted. All sit at about the
       same altitude and move with the same wind, so a nearer cloud is
       bigger, higher and crosses the sky faster. They stay centred on the
       camera: shifting them as the kart moves made them seem to swing
       round. t runs 0 (near) to 1 (far). */
    static const uint8_t far_first[11] = {3, 6, 9, 1, 4, 7, 10, 2, 5, 8, 0};
    r_hdr(h_cloud);
    for(int k = 0; k < 11; k++) {
        int i = far_first[k];
        if(world_split && (i & 1)) continue;
        float t = (float)((i * 7) % 11) / 10.0f;
        float d = 950.0f + 550.0f * t;
        float a = (float)i * 0.63f * DK_PI * 0.9f + world_time * 0.006f * 950.0f / d;
        float el = 0.07f + 0.05f * (float)(i % 4) / 3.0f + (i & 1) * 0.04f;
        float sa = sinf(a), ca = cosf(a), r = cosf(el) * d;
        V3 p = v3(c.x + sa * r, sinf(el) * 1250.0f + 20.0f, c.z + ca * r);
        if(!r_sphere_visible(p, 260.0f)) continue;
        float w = 150.0f + (float)(i % 3) * 50.0f;
        /* Four cumulus in the texture's quarters. */
        float u0 = (i & 1) ? 0.5f : 0.0f, v0 = (i & 2) ? 0.5f : 0.0f;
        float alpha = 0.92f - 0.15f * t;   /* the far ones a little hazier */
        uint32_t col = color_alpha(0xffffffff, alpha);
        if(world_sunset || world_tk) {
            /* None in front of the low sun: a cloud drifting within ~35
               degrees of it fades out (gone by ~30). */
            float fade = 1.0f;
            if(world_sunset) {
                float sd = (sa * sun_disc_dir.x + ca * sun_disc_dir.z) /
                           fmaxf(0.01f, sqrtf(sun_disc_dir.x * sun_disc_dir.x + sun_disc_dir.z * sun_disc_dir.z));
                fade = clampf((0.87f - sd) / 0.05f, 0.0f, 1.0f);
            }
            /* Tsudoken: nor in front of Fuji. */
            if(world_tk) {
                float fd = sa * sinf(TK_FUJI_DIR) + ca * cosf(TK_FUJI_DIR);
                fade = fminf(fade, clampf((0.90f - fd) / 0.05f, 0.0f, 1.0f));
            }
            if(fade <= 0.0f) continue;
            col = world_sunset ? argb((int)(240.0f * alpha * fade), 255, 168, 156) : color_alpha(0xffffffff, alpha * fade);
        }
        r_billboard(p, w, w * 0.5f, 0, u0, v0, u0 + 0.5f, v0 + 0.5f, col);
    }

    if(world_tk) return;
    if(world_sunset) {
        /* The retrowave sun, beyond the hills so they hide its foot, in a
           wide pink glow. */
        V3 sp = vmad(c, sun_disc_dir, 1450.0f);
        if(r_sphere_visible(sp, 420.0f)) {
            r_hdr(h_glow_add);
            r_billboard(sp, 900.0f, 620.0f, 0, 0, 0, 1, 1, argb(150, 255, 110, 120));
            r_hdr(h_retrosun);
            r_billboard(sp, 340.0f, 340.0f, 0, 0, 0, 1, 1, 0xffffffff);
        }
        return;
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
    if(world_sunset) return;   /* the sun is low and ahead: it would flare through the tunnels */
    V3 sp = vmad(r_cam.pos, sun_disc_dir, 1300.0f);
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

/* Open water there, deep enough that a sparkle can't poke up through the
   shore or the grass the sea runs under. A whole-lap track_locate is ~30 us
   on the console, so Emerald Beach finds the nearest of every 8th sample
   and lets the hinted search finish. */
static bool sea_open(float x, float z) {
    float g;
    if(track.id == TRACK_BEACH) {
        float best = 1e30f;
        int hint = 0;
        for(int i = 0; i < TRACK_SAMPLES; i += 8) {
            float dx = x - track.smp[i].p.x, dz = z - track.smp[i].p.z, d2 = dx * dx + dz * dz;
            if(d2 < best) {
                best = d2;
                hint = i;
            }
        }
        g = beach_ground_y(x, z, hint);
    } else {
        g = world_ground_y(x, z);
    }
    return g < WATER_Y - 0.3f;
}

static void draw_sea_sparkles(void) {
    /* One view's candidates at a time: split screen keeps the sheen only. */
    if(world_split) return;
    float dt = clampf(world_time - sparkle_t, 0, 0.1f);
    sparkle_t = world_time;
    V3 c = r_cam.pos;
    r_hdr(h_sparkle);
    for(int i = 0; i < SPARKLE_N; i++) {
        Sparkle *s = &sparkle[i];
        if((s->t += dt) >= s->life) {
            s->t = 0;
            s->l = 0;
            s->life = 0.05f + sp_rand() * 0.25f;   /* dark a moment, then try again */
            if(sp_cand_n > 0) {
                int k = (int)(sp_rand() * (float)sp_cand_n);
                if(sp_rand() < sp_cand[k].l * 1.5f) {
                    s->x = sp_cand[k].x + (sp_rand() - 0.5f) * sp_cell;
                    s->z = sp_cand[k].z + (sp_rand() - 0.5f) * sp_cell * 1.5f;
                    if(sea_open(s->x, s->z)) {
                        s->l = sp_cand[k].l * (0.6f + 0.4f * sp_rand());
                        s->life = 0.2f + sp_rand() * 0.3f;
                    }
                }
            }
        }
        if(s->l <= 0) continue;
        float e = sinf(DK_PI * s->t / s->life);
        float dx = s->x - c.x, dz = s->z - c.z, dy = WATER_Y - c.y;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        float a = e * e * fminf(s->l * 1.6f, 1.0f) * clampf((600.0f - d) / 350.0f, 0, 1);
        if(a < 0.02f) continue;
        /* Smaller on screen further out, and flattened as water seen
           edge-on flattens them, so the far row stays below the horizon. */
        float size = d * SPARKLE_SIZE * (0.7f + 0.6f * e) * clampf(1.3f - d * (1.0f / 400.0f), 0.45f, 1.0f);
        float h = size * 0.45f;
        V3 p = v3(s->x, WATER_Y + h * 0.5f * fabsf(r_cam.up.y) + 0.05f, s->z);
        if(!r_sphere_visible(p, size)) continue;
        r_billboard(p, size, h, 0, 0, 0, 1, 1, argb((int)(a * 255.0f), 255, 246, 222));
    }
}

void world_draw_tr(void) {
    r_load_vp();
    if(!(dk_probe_off & PROBE_DISTANT)) draw_clouds_and_mountains();
    /* Far out in the desert, so straight after the sky (the list draws in
       the order sent). */
    if(world_av && !(dk_probe_off & PROBE_DISTANT)) av_draw_dust_devils();
    if(dk_probe_off & PROBE_FOAM) {
        if(!(dk_probe_off & PROBE_DISTANT) && !world_night) draw_lens_flare();
        return;
    }
    if(track.in_sky) {
        /* Neon glows are additive, so they need no sorting. */
        if(!(dk_probe_off & PROBE_WORLD)) draw_chunks(HF_TR);
        draw_sparkles();
        dr_draw_waterfalls();
    }
    PROF_BEGIN(pm);
    draw_shore_foam();
    draw_sea_sparkles();
    PROF_LAP(PS_FOAM, pm);
    th_draw_flames();
    if(world_tk) {
        tk_draw_glows();
        tk_draw_petals();
    }
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
