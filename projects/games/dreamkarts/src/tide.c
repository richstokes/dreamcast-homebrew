/* Emerald Beach's tide.

   On the seaward run out of turn 1 the sea comes in over the outside of
   the road and goes out again, on a fixed cycle from the green light, so
   it can be learned and seen coming. The racing line holds the inside
   there and the water stops short of it even at the top of the tide:
   it only ever costs a kart that has run wide. Water slows a kart as a
   curb slows it, only more; a boost cuts through it as through sand.

   It runs on the race clock, not world_time, and changes how karts drive,
   so it stays offline until the consoles' build carries it (net_version.h). */
#include "tide.h"
#include "fx.h"
#include "texture_assets.h"
#include "render.h"
#include "track.h"
#include "world.h"

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
    return track.id == TRACK_BEACH && !game.online && game.mode != MODE_TT;
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

void tide_draw(void) {
    float level = tide_level();
    if(!tide_on() || level < 0.01f || !r_sphere_visible(v3(135.0f, 0, 32.0f), 70.0f)) return;
    enum { MAX = 34 };
    RV w[2 * MAX], f[2 * MAX];
    int n = 0;
    float scroll = fmodf(world_time * 0.03f, 1.0f), wash = 0.5f + 0.5f * sinf(world_time * 1.3f);
    uint32_t ca = color_alpha(0xffffffff, 0.8f + 0.2f * wash), cb = color_alpha(0xffffffff, 0.0f);
    uint32_t tint = argb(205, 120, 205, 225);
    for(int i = (int)(TIDE_S0 / track.ds); i * track.ds <= TIDE_S1 && n < MAX; i++, n++) {
        const TrackSample *t = &track.smp[i];
        float d = edge_at(t->s, t->wall_out, level) + 0.3f * sinf(world_time * 1.1f + t->s * 0.23f);
        d = fminf(d, t->wall_out);
        V3 a = vmad(t->p, t->r, d), b = vmad(t->p, t->r, t->wall_out), c = vmad(t->p, t->r, d - 1.4f);
        V3 e = vmad(t->p, t->r, d + 1.8f);
        a.y = b.y = 0.05f;
        c.y = e.y = 0.07f;
        w[2 * n] = (RV){a.x, a.y, a.z, a.x / 8.0f + scroll, a.z / 8.0f, tint};
        w[2 * n + 1] = (RV){b.x, b.y, b.z, b.x / 8.0f + scroll, b.z / 8.0f, tint};
        f[2 * n] = (RV){c.x, c.y, c.z, t->s / 6.0f, scroll * 3.0f, cb};
        f[2 * n + 1] = (RV){e.x, e.y, e.z, t->s / 6.0f, scroll * 3.0f + 1.0f, ca};
    }
    r_hdr(h_tide);
    r_strip(w, 2 * n);
    r_hdr(h_tide_foam);
    r_strip(f, 2 * n);
}
