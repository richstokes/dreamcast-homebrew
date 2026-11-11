/* Ridgeline time of day (tod.h).

   The day is a set of palette keys by the hour (night, first light,
   sunrise, morning, noon, afternoon, sunset, dusk) blended as the clock
   runs. The sun climbs an arc from the east-north-east over the south to
   set in the west-north-west, low enough all day to keep the long
   shadows the painted look wants; the moon is full and opposite it, so
   it rises as the sun sets and lights the night from the south.

   Nothing in the world is lit per frame. The ground, the trees, the rocks
   and the props keep their vertex colours, and the clock relights them in
   slices: a pass over the ground grid, then the far terrain copied from
   it, then the scenery, a few thousand vertices a frame, so a whole pass
   takes about a second and a half while driving. The sun moves a third
   of a degree in that time, which the eye never sees; a jump of the
   clock (the menu, photo mode) shows as a short fade instead. */
#include "tod.h"
#include "world.h"
#include "terrain.h"
#include "outland.h"

Tod tod;
static Palette cur;
const Palette *pal = &cur;
uint64_t tod_us;

const char *const tod_names[TOD_COUNT] = {"DAWN", "MORNING", "AFTERNOON", "DUSK", "NIGHT"};
const float tod_hours[TOD_COUNT] = {5.6f, 9.0f, 17.0f, 18.0f, 23.0f};

/* ---- The sun and the moon ---- */

#define SUNRISE 6.0f
#define SUNSET 18.5f
#define SUN_PEAK (42.0f * ENG_PI / 180.0f)
#define NIGHT_DIP (50.0f * ENG_PI / 180.0f)

/* Bearing (radians from north) and elevation of the sun at an hour. By
   day the bearing sweeps 60 to 330 degrees and the elevation is a flat
   hump; at 17:00 it stands where the fixed sun used to (300 degrees, 21
   up). By night it sinks on round through the north, so it never jumps. */
static void sun_at(float hour, float *az, float *el) {
    float day = SUNSET - SUNRISE, night = 24.0f - day;
    if(hour >= SUNRISE && hour <= SUNSET) {
        float t = (hour - SUNRISE) / day;
        *az = (60.0f + 270.0f * t) * ENG_PI / 180.0f;
        *el = SUN_PEAK * powf(sinf(ENG_PI * t), 0.7f);
    }
    else {
        float t = (hour > SUNSET ? hour - SUNSET : hour + 24.0f - SUNSET) / night;
        *az = (330.0f + 90.0f * t) * ENG_PI / 180.0f;
        *el = -NIGHT_DIP * sinf(ENG_PI * t);
    }
}

static V3 dir_of(float az, float el) {
    float ce = cosf(el);
    return v3(sinf(az) * ce, sinf(el), cosf(az) * ce);
}

/* ---- The palette by the hour ---- */

typedef struct {
    float hour;
    uint32_t sky_top, sky_horizon, haze, sun_col, glow_col, cloud_col;
    float glow_k, fog_near, fog_far;
    float sun_k[3], sky_k[3], amb_k[3];
    float stars, dark, water_k;
} Key;

/* The 17:00 key is the autumn afternoon the game was painted in: mustard
   gold, forest green, cool grey, pale sky. Night is moonlit blue rather
   than black; a television crushes anything darker. */
static const Key keys[] = {
    {0.0f, 0xff070e1e, 0xff16243a, 0xff141f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 120, 700,
     {0.17f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {4.6f, 0xff070e1e, 0xff16243a, 0xff141f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 120, 700,
     {0.17f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    /* First light: a band of orange low in the east under a deep blue. */
    {5.5f, 0xff1a2a4a, 0xff6e5a6a, 0xff3e3e56, 0xffe8ecf4, 0xffd87a50, 0xff5a5068, 0.85f, 90, 560,
     {0.10f, 0.12f, 0.18f}, {0.14f, 0.16f, 0.24f}, {0.08f, 0.09f, 0.13f}, 0.5f, 0.8f, 0.45f},
    /* Sunrise: a low orange sun, mist in the valleys. */
    {6.3f, 0xff5a86b0, 0xfff0c090, 0xffe0b494, 0xffffb060, 0xffffc070, 0xffffc8a0, 1.0f, 60, 480,
     {0.75f, 0.42f, 0.22f}, {0.34f, 0.34f, 0.42f}, {0.14f, 0.15f, 0.20f}, 0.0f, 0.25f, 0.75f},
    {8.0f, 0xff7fb0d8, 0xffe4ecf0, 0xffdde6ea, 0xfffff4dc, 0xfff8e8c8, 0xfffafcfc, 0.5f, 110, 680,
     {0.78f, 0.70f, 0.58f}, {0.40f, 0.46f, 0.54f}, {0.20f, 0.21f, 0.24f}, 0.0f, 0.0f, 1.0f},
    {12.5f, 0xff7cb4e0, 0xffd8e8f0, 0xffd4e2ea, 0xfffffaf0, 0xfff4ecd8, 0xfffcfdfe, 0.3f, 160, 820,
     {0.86f, 0.82f, 0.74f}, {0.42f, 0.48f, 0.56f}, {0.23f, 0.23f, 0.25f}, 0.0f, 0.0f, 1.0f},
    {17.0f, 0xff8fbcd8, 0xffdce9ee, 0xffd6e2e8, 0xfffff2d8, 0xfff6e2c4, 0xfff8fbfc, 0.55f, 140, 760,
     {0.80f, 0.70f, 0.55f}, {0.42f, 0.47f, 0.54f}, {0.22f, 0.22f, 0.24f}, 0.0f, 0.0f, 1.0f},
    /* Sunset: the sun goes orange and the whole west with it. */
    {18.3f, 0xff5878a8, 0xfff0a060, 0xffe0a888, 0xffff9040, 0xffff9a50, 0xffffb088, 1.0f, 110, 640,
     {0.70f, 0.36f, 0.16f}, {0.30f, 0.30f, 0.42f}, {0.14f, 0.13f, 0.17f}, 0.0f, 0.3f, 0.8f},
    /* Dusk: the sun gone, an ember of it left on the horizon, first stars. */
    {19.0f, 0xff1c2c50, 0xff8a5a60, 0xff5a4a62, 0xffff9040, 0xffe07040, 0xff584860, 0.8f, 100, 560,
     {0.0f, 0.0f, 0.0f}, {0.14f, 0.15f, 0.24f}, {0.08f, 0.09f, 0.13f}, 0.5f, 0.8f, 0.45f},
    {20.2f, 0xff070e1e, 0xff16243a, 0xff141f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 120, 700,
     {0.17f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {24.0f, 0xff070e1e, 0xff16243a, 0xff141f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 120, 700,
     {0.17f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
};
#define KEYS (int)(sizeof(keys) / sizeof(keys[0]))

/* The desert's day, hour for hour with the mountain's: drier air, so a
   deeper blue overhead and a haze of dust rather than damp, the low sun
   redder on rock that is red already, and a long violet dusk. The fog
   stands further off than the mountain's: what there is to see here is
   in the distance (terrain.c draws the ground out to match). */
static const Key keys_desert[] = {
    {0.0f, 0xff060c1c, 0xff18223a, 0xff161e30, 0xffe8ecf4, 0xff2c3858, 0xff2a3448, 0.35f, 120, 700,
     {0.18f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {4.6f, 0xff060c1c, 0xff18223a, 0xff161e30, 0xffe8ecf4, 0xff2c3858, 0xff2a3448, 0.35f, 120, 700,
     {0.18f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {5.5f, 0xff182a52, 0xff8a5a66, 0xff4a4056, 0xffe8ecf4, 0xffe08050, 0xff6a5068, 0.9f, 110, 680,
     {0.10f, 0.11f, 0.17f}, {0.16f, 0.16f, 0.22f}, {0.10f, 0.09f, 0.12f}, 0.5f, 0.8f, 0.45f},
    {6.3f, 0xff4a7cc0, 0xfff4b888, 0xffe0a890, 0xffffa850, 0xffffb868, 0xffffc8a0, 1.0f, 110, 720,
     {0.86f, 0.46f, 0.22f}, {0.36f, 0.32f, 0.36f}, {0.17f, 0.14f, 0.16f}, 0.0f, 0.25f, 0.75f},
    {8.0f, 0xff4c92dc, 0xffcfe2f0, 0xffdccac4, 0xfffff4dc, 0xfff8e4c0, 0xfffafcfc, 0.5f, 180, 940,
     {0.90f, 0.78f, 0.62f}, {0.44f, 0.43f, 0.47f}, {0.24f, 0.22f, 0.21f}, 0.0f, 0.0f, 1.0f},
    {12.5f, 0xff3f88dc, 0xffc4dcf0, 0xffd8c8c6, 0xfffffaf0, 0xfff4ecd8, 0xfffcfdfe, 0.3f, 200, 1000,
     {0.98f, 0.90f, 0.78f}, {0.46f, 0.45f, 0.48f}, {0.26f, 0.24f, 0.22f}, 0.0f, 0.0f, 1.0f},
    {17.0f, 0xff4a8cd4, 0xffd4e0ea, 0xffdcc2b6, 0xfffff0d0, 0xfff8dcb4, 0xfff8fbfc, 0.6f, 190, 960,
     {0.98f, 0.78f, 0.54f}, {0.46f, 0.43f, 0.45f}, {0.25f, 0.22f, 0.20f}, 0.0f, 0.0f, 1.0f},
    {18.3f, 0xff4a68a8, 0xfff49a58, 0xffe09878, 0xffff8838, 0xffff9048, 0xffffa880, 1.0f, 140, 820,
     {0.86f, 0.40f, 0.15f}, {0.34f, 0.28f, 0.36f}, {0.16f, 0.13f, 0.15f}, 0.0f, 0.3f, 0.8f},
    {19.0f, 0xff1e2a58, 0xff9a5a6a, 0xff66486a, 0xffff9040, 0xffe86a44, 0xff684868, 0.85f, 120, 700,
     {0.0f, 0.0f, 0.0f}, {0.16f, 0.15f, 0.23f}, {0.10f, 0.09f, 0.12f}, 0.5f, 0.8f, 0.45f},
    {20.2f, 0xff060c1c, 0xff18223a, 0xff161e30, 0xffe8ecf4, 0xff2c3858, 0xff2a3448, 0.35f, 120, 700,
     {0.18f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {24.0f, 0xff060c1c, 0xff18223a, 0xff161e30, 0xffe8ecf4, 0xff2c3858, 0xff2a3448, 0.35f, 120, 700,
     {0.18f, 0.20f, 0.28f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
};
_Static_assert(sizeof(keys_desert) == sizeof(keys), "an hour for an hour");

/* The dunes' day: the Columbia Basin's big dry sky. Grey sand takes
   whatever colour the light is, so the light does the work: a strong
   warm sun against a cool sky, which leaves every slip face blue-grey in
   shadow beside a back the colour of straw; gold at both ends of the day
   (pale sand throws the low sun's colour back up, so the ground glows
   when the sun is too low to light it much), and a pale dusty haze a
   long way off. */
static const Key keys_basin[] = {
    {0.0f, 0xff060c1c, 0xff17233a, 0xff151f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 130, 760,
     {0.19f, 0.22f, 0.30f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {4.6f, 0xff060c1c, 0xff17233a, 0xff151f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 130, 760,
     {0.19f, 0.22f, 0.30f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {5.5f, 0xff1a2c52, 0xff866068, 0xff484458, 0xffe8ecf4, 0xffe08656, 0xff665468, 0.9f, 110, 660,
     {0.10f, 0.12f, 0.18f}, {0.15f, 0.17f, 0.24f}, {0.09f, 0.09f, 0.13f}, 0.5f, 0.8f, 0.45f},
    {6.3f, 0xff5284c0, 0xfff4c090, 0xffe2b89a, 0xffffac58, 0xffffbe6c, 0xffffcca4, 1.0f, 100, 700,
     {0.96f, 0.56f, 0.27f}, {0.42f, 0.37f, 0.42f}, {0.25f, 0.20f, 0.20f}, 0.0f, 0.25f, 0.75f},
    {8.0f, 0xff5a9ee0, 0xffd6e6f0, 0xffd6dcdc, 0xfffff4dc, 0xfff8e6c4, 0xfffafcfc, 0.5f, 180, 960,
     {0.94f, 0.82f, 0.64f}, {0.40f, 0.46f, 0.56f}, {0.19f, 0.20f, 0.24f}, 0.0f, 0.0f, 1.0f},
    {12.5f, 0xff4c94e4, 0xffcce0f0, 0xffd2dade, 0xfffffaf0, 0xfff4ecd8, 0xfffcfdfe, 0.3f, 210, 1040,
     {1.00f, 0.93f, 0.80f}, {0.42f, 0.48f, 0.58f}, {0.21f, 0.22f, 0.25f}, 0.0f, 0.0f, 1.0f},
    {17.0f, 0xff5a98dc, 0xffdce6ea, 0xffdcd4c4, 0xfffff0d0, 0xfffadcae, 0xfff8fbfc, 0.65f, 190, 980,
     {1.00f, 0.80f, 0.54f}, {0.40f, 0.45f, 0.55f}, {0.20f, 0.21f, 0.24f}, 0.0f, 0.0f, 1.0f},
    {18.3f, 0xff5070b0, 0xfff8a45c, 0xffe4a67e, 0xffff8c3c, 0xffff964c, 0xffffb084, 1.0f, 140, 840,
     {0.98f, 0.50f, 0.20f}, {0.40f, 0.33f, 0.40f}, {0.25f, 0.19f, 0.19f}, 0.0f, 0.3f, 0.8f},
    {19.0f, 0xff1e2c5a, 0xff9a5e68, 0xff624c68, 0xffff9040, 0xffe87044, 0xff644a66, 0.85f, 120, 720,
     {0.0f, 0.0f, 0.0f}, {0.19f, 0.17f, 0.25f}, {0.13f, 0.11f, 0.14f}, 0.5f, 0.8f, 0.45f},
    {20.2f, 0xff060c1c, 0xff17233a, 0xff151f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 130, 760,
     {0.19f, 0.22f, 0.30f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
    {24.0f, 0xff060c1c, 0xff17233a, 0xff151f30, 0xffe8ecf4, 0xff2a3a58, 0xff2a3448, 0.35f, 130, 760,
     {0.19f, 0.22f, 0.30f}, {0.09f, 0.11f, 0.17f}, {0.07f, 0.08f, 0.12f}, 1.0f, 1.0f, 0.30f},
};
_Static_assert(sizeof(keys_basin) == sizeof(keys), "an hour for an hour");

/* Moonlight: a cool, faint sun from the moon's direction. */
static const float moon_k[3] = {0.17f, 0.20f, 0.28f};

static void lerp3(float *o, const float *a, const float *b, float t) {
    for(int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * t;
}

static void palette_at(float hour, Palette *p) {
    const Key *ks = lvl_area == AREA_MOAB ? keys_desert : lvl_area == AREA_DUNES ? keys_basin : keys;
    int k = 0;
    while(k + 2 < KEYS && hour >= ks[k + 1].hour) k++;
    const Key *a = &ks[k], *b = &ks[k + 1];
    float t = clampf((hour - a->hour) / (b->hour - a->hour), 0, 1);
    t = t * t * (3.0f - 2.0f * t);
    p->sky_top = color_lerp(a->sky_top, b->sky_top, t);
    p->sky_horizon = color_lerp(a->sky_horizon, b->sky_horizon, t);
    p->haze = color_lerp(a->haze, b->haze, t);
    p->sun_col = color_lerp(a->sun_col, b->sun_col, t);
    p->glow_col = color_lerp(a->glow_col, b->glow_col, t);
    p->cloud_col = color_lerp(a->cloud_col, b->cloud_col, t);
    p->glow_k = lerpf(a->glow_k, b->glow_k, t);
    p->fog_near = lerpf(a->fog_near, b->fog_near, t);
    p->fog_far = lerpf(a->fog_far, b->fog_far, t);
    lerp3(p->sun_k, a->sun_k, b->sun_k, t);
    lerp3(p->sky_k, a->sky_k, b->sky_k, t);
    lerp3(p->amb_k, a->amb_k, b->amb_k, t);
    p->stars = lerpf(a->stars, b->stars, t);
    p->dark = lerpf(a->dark, b->dark, t);
    p->water_k = lerpf(a->water_k, b->water_k, t);
}

/* A light's share as it crosses the horizon: nothing from 3 degrees under. */
static float horizon_fade(float el) {
    return clampf((el + 3.0f * ENG_PI / 180.0f) / (5.0f * ENG_PI / 180.0f), 0, 1);
}

static void eval(void) {
    float az, el;
    sun_at(tod.hour, &az, &el);
    tod.sun = dir_of(az, el);
    tod.sun_el = el;
    tod.moon = vscale(tod.sun, -1.0f);
    tod.moon_el = -el;
    palette_at(tod.hour, &cur);
    /* The world is lit by one light: the sun while it is up, else the
       moon. Both fade out at the horizon, so the hand-over is in the dark. */
    float sf = horizon_fade(el), mf = horizon_fade(-el);
    float sun_w = (cur.sun_k[0] + cur.sun_k[1] + cur.sun_k[2]) * sf;
    float moon_w = (moon_k[0] + moon_k[1] + moon_k[2]) * mf;
    tod.moonlit = moon_w > sun_w;
    if(tod.moonlit) {
        sun_dir = tod.moon;
        for(int i = 0; i < 3; i++) cur.sun_k[i] = moon_k[i] * mf;
    }
    else {
        sun_dir = tod.sun;
        for(int i = 0; i < 3; i++) cur.sun_k[i] *= sf;
    }
    tod.glow_az = el > -10.0f * ENG_PI / 180.0f ? az : az + ENG_PI;
}

/* ---- Relighting ---- */

/* A pass: the ground grid, the far terrain and water in the scenery
   pool, then the trees, rocks and props. A frame does budget vertices'
   worth, in the proportion each phase costs. */
enum { PH_GRID, PH_OUTLAND, PH_POOL, PH_SCENERY, PH_DONE };
static struct {
    int phase, at;
    Palette snap;         /* the light this pass is for */
    V3 snap_dir;
    bool have_snap;
} rl;
static bool relight_needed;

static bool same_light(const Palette *a, const Palette *b, V3 da, V3 db) {
    for(int i = 0; i < 3; i++)
        if(a->sun_k[i] != b->sun_k[i] || a->sky_k[i] != b->sky_k[i] || a->amb_k[i] != b->amb_k[i]) return false;
    return a->water_k == b->water_k && da.x == db.x && da.y == db.y && da.z == db.z;
}

static void pass_begin(void) {
    rl.snap = cur;
    rl.snap_dir = sun_dir;
    rl.have_snap = true;
    rl.phase = PH_GRID;
    rl.at = 0;
    terrain_relight_setup(&rl.snap, rl.snap_dir);
    outland_relight_setup(&rl.snap, rl.snap_dir);
    world_relight_setup(&rl.snap, rl.snap_dir);
}

/* Relight up to n vertices' worth of work; returns what is left of n. */
static int relight_work(int n) {
    while(n > 0 && rl.phase != PH_DONE) {
        int total, step;
        switch(rl.phase) {
            case PH_GRID: total = LEVEL_N * LEVEL_N; step = n; break;
            case PH_OUTLAND: total = outland_relight_n(); step = n; break;
            case PH_POOL: total = terrain_pool_n(); step = n * 3; break;   /* a copy: cheap */
            default: total = world_relight_n(); step = n; break;
        }
        int to = rl.at + step;
        if(to > total) to = total;
        if(to > rl.at) {
            switch(rl.phase) {
                case PH_GRID: terrain_relight(rl.at, to); n -= to - rl.at; break;
                case PH_OUTLAND: outland_relight(rl.at, to); n -= to - rl.at; break;
                case PH_POOL: terrain_relight_pool(&scenery, rl.at, to); n -= (to - rl.at + 2) / 3; break;
                default: world_relight(rl.at, to); n -= to - rl.at; break;
            }
        }
        rl.at = to;
        if(rl.at >= total) {
            rl.phase++;
            rl.at = 0;
        }
    }
    return n;
}

static void relight_step(float budget) {
    uint64_t t0 = timer_us_gettime64();
    int n = (int)(1100.0f * budget);
    /* A jump of the clock starts over at once; otherwise a new pass only
       when the light has moved since the last. */
    if(relight_needed) pass_begin();
    else if(rl.phase == PH_DONE || !rl.have_snap) {
        if(rl.have_snap && same_light(&rl.snap, &cur, rl.snap_dir, sun_dir)) return;
        pass_begin();
    }
    relight_needed = false;
    relight_work(n);
    tod_us += timer_us_gettime64() - t0;
}

void tod_relight_all(void) {
    pass_begin();
    while(rl.phase != PH_DONE) relight_work(1 << 20);
    relight_needed = false;
}

/* ---- The clock ---- */

void tod_init(float hour) {
    tod.hour = hour;
    eval();
    /* The ground's colours come from the level lit by a fixed sun: light
       them for this hour before the far terrain is built from them. */
    terrain_relight_setup(&cur, sun_dir);
    terrain_relight(0, LEVEL_N * LEVEL_N);
    rl.phase = PH_DONE;
    rl.have_snap = false;
}

void tod_set_hour(float hour) {
    hour = fmodf(hour, 24.0f);
    if(hour < 0) hour += 24.0f;
    tod.hour = hour;
    eval();
    relight_needed = true;
}

void tod_update(float dt, bool run_clock, float budget) {
    if(run_clock) {
        tod.hour += dt / TOD_SECONDS_PER_HOUR;
        if(tod.hour >= 24.0f) tod.hour -= 24.0f;
    }
    eval();
    relight_step(budget);
}

void tod_format(float hour, char *out, int size) {
    int m = (int)(hour * 60.0f + 0.5f) % (24 * 60);
    snprintf(out, size, "%d:%02d", m / 60, m % 60);
}
