/* Emerald Beach's tide.

   On the seaward run out of turn 1 the sea comes in over the outside of
   the road and goes out again, on a fixed cycle from the green light, so
   it can be learned and seen coming. The racing line holds the inside
   there and the water stops short of it even at the top of the tide:
   it only ever costs a kart that has run wide. Water slows a kart as a
   curb slows it, only more; a boost cuts through it as through sand.

   It runs on the race clock, not world_time: online that is the server's
   green-light clock, so every console sees the same water. */
#include "tide.h"
#include "fx.h"
#include "texture_assets.h"
#include "engine/render.h"
#include "track.h"
#include "world_int.h"

#define TIDE_S0     165.0f     /* the stretch, in lap distance */
#define TIDE_S1     244.0f
#define TIDE_RAMP   24.0f      /* tapering in and out at each end */
#define TIDE_PERIOD 26.0f      /* seconds; a lap is ~30, so it shifts lap to lap */
#define TIDE_REACH  1.5f       /* the highest it comes, 7 m clear of the line */

static const pvr_poly_hdr_t *h_tide, *h_tide_foam;
static float splash_t[MAX_KARTS];

void tide_init(void) {
    h_tide = r_header(TEX_WATER, HF_TR);
    h_tide_foam = r_header(TEX_FOAM, HF_TR);
}

bool tide_on(void) {
    return track.id == TRACK_BEACH;
}

bool tide_open(float s) {
    return s >= TIDE_S0 && s <= TIDE_S1;
}

static float tide_level(void) {
    float c = 0.5f - 0.5f * cosf(game.race_time * (DK_TAU / TIDE_PERIOD));
    return c * c * (3.0f - 2.0f * c);   /* lingering at high and low water */
}

static float edge_at(float s, float wall, float level) {
    if(s < TIDE_S0 || s > TIDE_S1) return wall;
    float t = (s - TIDE_S0) / TIDE_RAMP, u = (TIDE_S1 - s) / TIDE_RAMP, k = clampf(fminf(t, u), 0, 1);
    return lerpf(wall, TIDE_REACH, level * k * k * (3.0f - 2.0f * k));
}

float tide_edge(float s) {
    return edge_at(s, 100.0f, tide_level());
}

bool tide_wet(const Kart *k) {
    return k->grounded && tide_on() && k->tp.d > tide_edge(k->tp.s);
}

void tide_update(float dt) {
    if(!tide_on()) return;
    for(int i = 0; i < MAX_KARTS; i++) {
        const Kart *k = &karts[i];
        if(tide_wet(k) && fabsf(k->speed) > 8.0f && (splash_t[i] -= dt) <= 0) {
            fx_splash(k->pos, 0.45f);
            splash_t[i] = 0.14f;
        }
    }
}

/* The sand along the tide's stretch as the outside terrain mesh draws
   it: flat to the outer wall, then straight between its band edges
   (sand_h[i][b], bands[b] beyond the wall), and where it goes under the
   sea. Built with the beach, so drawing never evaluates the terrain. */
static const float bands[] = {0.0f, 5.0f, 12.0f, 22.0f};
enum { NB = sizeof(bands) / sizeof(bands[0]), MAX = 34 };
static float sand_h[MAX][NB], shore_at[MAX];
static int tide_i0;

void tide_build(void) {
    tide_i0 = (int)(TIDE_S0 / track.ds);
    for(int n = 0; n < MAX; n++) {
        const TrackSample *t = &track.smp[(tide_i0 + n) % TRACK_SAMPLES];
        shore_at[n] = bands[NB - 1];
        bool found = false;
        for(int b = 0; b < NB; b++) {
            sand_h[n][b] = b ? terrain_height(vmad(t->p, t->r, t->wall_out + bands[b]), bands[b], t->shore) : 0.0f;
            if(!found && b && sand_h[n][b] < WATER_Y) {
                float y0 = sand_h[n][b - 1], y1 = sand_h[n][b];
                shore_at[n] = lerpf(bands[b - 1], bands[b], (y0 - WATER_Y) / (y0 - y1));
                found = true;
            }
        }
    }
}

static float sand_y(int n, float db) {
    if(db <= 0) return 0.0f;
    for(int b = 1; b < NB; b++)
        if(db <= bands[b]) return lerpf(sand_h[n][b - 1], sand_h[n][b], (db - bands[b - 1]) / (bands[b] - bands[b - 1]));
    return sand_h[n][NB - 1];
}

/* The water lies just over the sand, meeting the sea at the shore. */
static V3 wet_point(const TrackSample *t, int n, float d, float lift) {
    V3 p = vmad(t->p, t->r, d);
    float db = d - t->wall_out;
    p.y = db >= shore_at[n] ? WATER_Y + 0.02f : sand_y(n, db) + lift;
    return p;
}

/* The sea washes up the open beach from the shoreline, over the sand and
   onto the road, and back. */
void tide_draw(void) {
    float level = tide_level();
    if(!tide_on() || level < 0.01f || !r_sphere_visible(v3(130.0f, 0, 36.0f), 80.0f)) return;
    enum { COLS = 4 };
    RV w[COLS - 1][2 * MAX], f[2 * MAX];
    int n = 0;
    float scroll = fmodf(world_time * 0.03f, 1.0f), wash = 0.5f + 0.5f * sinf(world_time * 1.3f);
    uint32_t ca = color_alpha(0xffffffff, 0.8f + 0.2f * wash), cb = color_alpha(0xffffffff, 0.0f);
    uint32_t tint = argb(205, 120, 205, 225), sea_tint = argb(0, 120, 205, 225);
    for(int i = tide_i0; i * track.ds <= TIDE_S1 && n < MAX; i++, n++) {
        const TrackSample *t = &track.smp[i];
        float sea = t->wall_out + shore_at[n];
        float d = edge_at(t->s, sea, level) + 0.3f * sinf(world_time * 1.1f + t->s * 0.23f);
        d = fminf(d, sea - 0.6f);
        float col[COLS] = {d, fmaxf(d, t->wall_out), fmaxf(d, t->wall_out + bands[1]), sea};
        V3 p[COLS];
        for(int k = 0; k < COLS; k++) p[k] = wet_point(t, n, col[k], 0.05f);
        for(int k = 0; k + 1 < COLS; k++) {
            /* Fading into the sea at the shore rather than ending on a line. */
            uint32_t c1 = k + 2 == COLS ? sea_tint : tint;
            w[k][2 * n] = (RV){p[k].x, p[k].y, p[k].z, p[k].x / 8.0f + scroll, p[k].z / 8.0f, tint};
            w[k][2 * n + 1] = (RV){p[k + 1].x, p[k + 1].y, p[k + 1].z, p[k + 1].x / 8.0f + scroll, p[k + 1].z / 8.0f, c1};
        }
        V3 c = wet_point(t, n, d - 1.4f, 0.07f), e = wet_point(t, n, fminf(d + 1.8f, sea), 0.07f);
        f[2 * n] = (RV){c.x, c.y, c.z, t->s / 6.0f, scroll * 3.0f, cb};
        f[2 * n + 1] = (RV){e.x, e.y, e.z, t->s / 6.0f, scroll * 3.0f + 1.0f, ca};
    }
    r_hdr(h_tide);
    for(int k = 0; k + 1 < COLS; k++) r_strip(w[k], 2 * n);
    r_hdr(h_tide_foam);
    r_strip(f, 2 * n);
}
