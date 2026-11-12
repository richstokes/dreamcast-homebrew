/* Ridgeline: showers (rain.h).

   A shower is mostly light and air: the sun goes, the sky and everything
   under it turn the same grey, the far hills go into the haze and the
   ground darkens. All of that is the hour's palette changed before the
   world is relit to it, so it costs nothing a frame. The rain itself is a
   hundred-odd streaks in a box of air that travels with the camera, and
   rings where drops meet water near it. */
#include "rain.h"
#include "level.h"
#include "terrain.h"
#include "tod.h"
#include "lamps.h"

/* ---- When ---- */

/* Three a day, clear of sunrise and sunset (the light then is the best of
   the day): a light one in the morning, the day's heaviest after noon,
   one in the dark for the headlights. Each comes on and goes over
   RAIN_EASE of an hour (24 s). */
static const struct { float at, half, k; } showers[] = {{7.9f, 0.75f, 0.7f}, {13.3f, 1.0f, 1.0f}, {21.3f, 1.0f, 0.85f}};
#define SHOWERS (int)(sizeof(showers) / sizeof(showers[0]))
#define RAIN_EASE 0.4f
#define RAIN_DRY 1.6f     /* hours the ground takes to dry */

static inline float smooth(float t) {
    t = clampf(t, 0, 1);
    return t * t * (3.0f - 2.0f * t);
}

/* Hours from a to b going forward round the clock. */
static inline float after(float a, float b) {
    float d = b - a;
    return d < 0 ? d + 24.0f : d;
}

float rain_shower(float hour) {
    if(look->rain <= 0) return 0;
#ifdef RL_AT_RAIN
    return RL_AT_RAIN;   /* a test build's weather */
#endif
    float most = 0;
    for(int i = 0; i < SHOWERS; i++) {
        float d = fabsf(hour - showers[i].at);
        if(d > 12.0f) d = 24.0f - d;
        float e = showers[i].k * smooth((showers[i].half - d) * (1.0f / RAIN_EASE));
        if(e > most) most = e;
    }
    return most * look->rain;
}

float rain_wet(float hour) {
    if(look->rain <= 0) return 0;
#ifdef RL_AT_RAIN
    return RL_AT_RAIN > 0 ? 1.0f : 0;
#endif
    float most = 0;
    for(int i = 0; i < SHOWERS; i++) {
        /* Wet within a few minutes of its start, drying from its end. */
        float in = after(showers[i].at - showers[i].half, hour), out = after(showers[i].at + showers[i].half, hour);
        float w = in < 2.0f * showers[i].half ? smooth(in * (1.0f / RAIN_EASE)) : 1.0f - smooth(out * (1.0f / RAIN_DRY));
        if(out > 12.0f && in >= 2.0f * showers[i].half) w = 0;
        if(w > most) most = w;
    }
    return most;
}

/* ---- The light ---- */

/* An overcast's colour for one of the clear day's: its brightness, a
   shade cool. */
static uint32_t overcast(uint32_t c, float k) {
    float l = (((c >> 16) & 255) * 5 + ((c >> 8) & 255) * 9 + (c & 255) * 2) * (k / 16.0f);
    int r = (int)(l * 0.93f), g = (int)(l * 0.97f), b = (int)(l * 1.03f);
    return 0xff000000u | (uint32_t)(r > 255 ? 255 : r) << 16 | (uint32_t)(g > 255 ? 255 : g) << 8 | (uint32_t)(b > 255 ? 255 : b);
}

void rain_palette(Palette *p, float hour) {
    float a = rain_shower(hour);
    p->wet = rain_wet(hour);
    if(a <= 0) return;
    float c = 0.9f * a;
    /* One sky from the hills to overhead, the cloud a little darker than
       the gaps it has closed, no glow left where the sun is. */
    uint32_t low = overcast(p->sky_horizon, 0.9f);
    p->sky_top = color_lerp(p->sky_top, overcast(p->sky_horizon, 0.72f), c);
    p->sky_horizon = color_lerp(p->sky_horizon, low, c);
    p->haze = color_lerp(p->haze, overcast(p->haze, 0.92f), c);
    p->cloud_col = color_lerp(p->cloud_col, overcast(p->cloud_col, 0.8f), c);
    p->glow_col = color_lerp(p->glow_col, p->haze, c);
    p->glow_k *= 1.0f - 0.85f * a;
    p->stars *= 1.0f - a;
    /* The sun is behind it: most of the light comes from the whole sky,
       which is brighter for that and has no colour to speak of. */
    float sky = (p->sky_k[0] + p->sky_k[1] + p->sky_k[2]) * (1.0f / 3.0f);
    float amb = (p->amb_k[0] + p->amb_k[1] + p->amb_k[2]) * (1.0f / 3.0f);
    float sun = (p->sun_k[0] + p->sun_k[1] + p->sun_k[2]) * (1.0f / 3.0f);
    static const float cool[3] = {0.95f, 1.0f, 1.06f};
    for(int i = 0; i < 3; i++) {
        p->sky_k[i] = lerpf(p->sky_k[i], (sky + 0.22f * sun) * cool[i], 0.8f * a);
        p->amb_k[i] = lerpf(p->amb_k[i], amb * cool[i], 0.8f * a);
        /* (by night less is taken: a moon behind cloud still lights the
           hill, and the dark is already as deep as is any use) */
        p->sun_k[i] *= 1.0f - 0.82f * a * (1.0f - 0.5f * p->dark);
    }
    /* And the air is full of it: the far side of the corrie goes. */
    p->fog_near *= 1.0f - 0.6f * a;
    p->fog_far = lerpf(p->fog_far, fminf(p->fog_far, 430.0f), a);
    p->water_k *= 1.0f - 0.18f * a;
}

float rain_overcast(void) {
    return rain_shower(tod.hour);
}

float rain_now(void) {
    if(look->rain <= 0) return 0;
    float a = rain_shower(tod.hour) * (0.72f + 0.28f * clampf((cam.pos.y - 10.0f) * (1.0f / 110.0f), 0, 1));
    return clampf(a + 0.4f * look->rain * world_mist(), 0, 1);
}

/* ---- The rain in the air ----

   Each drop is a fixed place in a box of air DX by DY by DZ that repeats
   through the world; the box that is drawn is the one round a point ahead
   of the camera, so the drops stay put as the camera turns and are always
   there to see, with nothing kept from frame to frame but the time. They
   fall at FALL down the wind, drawn as the streak a drop makes in a
   fifteenth of a second: its head bright, its tail gone. */
#define DROPS 150
#define DX 20.0f
#define DY 11.0f
#define FALL 9.5f
#define DRIFT 3.2f          /* m/s downwind */
#define STREAK 0.62f
#define RAIN_FAR 19.0f
#define BEAM_DROPS 14

static float drop_u[DROPS][3];
static float rain_t, rain_a;
static const pvr_poly_hdr_t *h_rain;

/* ---- Rings on the water ---- */
#define RINGS 28
#define RING_SEG 8
#define RING_LIFE 0.7f
typedef struct { float x, y, z, age, size; } Ring;
static Ring rings[RINGS];
static int n_rings;

static uint32_t seed = 0x7a11e5u;
static inline float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8 & 0xffff) * (1.0f / 65536.0f);
}

void rain_update(float dt) {
    rain_a = rain_now();
    if(rain_a <= 0.01f && !n_rings) return;
    if(!h_rain) {
        h_rain = r_header(TEX_NONE, HF_TR | HF_NOFOG);
        for(int i = 0; i < DROPS; i++)
            for(int k = 0; k < 3; k++) drop_u[i][k] = rnd();
        for(int i = 0; i < RINGS; i++) rings[i].age = RING_LIFE;
    }
    /* (paused, it hangs in the air as everything else does) */
    if(game_frozen()) return;
    rain_t += dt;
    /* A few new rings a frame, each tried at one place ahead: most of the
       Highlands are not water. */
    V3 f = v3(cam.fwd.x, 0, cam.fwd.z);
    float fl = sqrtf(f.x * f.x + f.z * f.z);
    n_rings = 0;
    for(int i = 0; i < RINGS; i++) {
        Ring *r = &rings[i];
        if(r->age < RING_LIFE) {
            r->age += dt;
            n_rings++;
            continue;
        }
        if(fl < 0.01f || rnd() > rain_a * dt * 14.0f) continue;
        float along = 3.0f + 26.0f * rnd(), side = (rnd() - 0.5f) * (4.0f + along * 1.2f);
        float x = cam.pos.x + (f.x * along - f.z * side) / fl, z = cam.pos.z + (f.z * along + f.x * side) / fl;
        float w = water_at(x, z);
        if(w <= -999.0f || w < terrain_height(x, z) + 0.04f) continue;
        *r = (Ring){x, w + 0.03f, z, 0, 0.3f + 0.3f * rnd()};
        n_rings++;
    }
}

#define BUF 240
static RV buf[BUF + 8];
static int nbuf;

static void flush(void) {
    if(nbuf) r_strip(buf, nbuf);
    nbuf = 0;
}

/* n vertices into the strip, joined on to what is there. */
static RV *strip(int n) {
    if(nbuf + n + 2 > BUF) flush();
    if(nbuf) {
        buf[nbuf] = buf[nbuf - 1];
        nbuf += 2;   /* the second of the join is the strip's own first, written by the caller's copy below */
    }
    RV *o = buf + nbuf;
    nbuf += n;
    return o;
}

/* u's fraction, 0 .. 1 (a cast, not floorf: that is a call on the SH-4,
   and there are three of these a drop). */
static inline float wrap(float u) {
    u -= (float)(int)u;
    return u < 0 ? u + 1.0f : u;
}

void rain_draw_tr(void) {
    if(!h_rain || (rain_a <= 0.01f && !n_rings)) return;
    V3 c = r_cam.pos, fw = r_cam.fwd;
    if(rain_a > 0.01f) {
        /* The box's middle: ahead of the camera, a little above it. */
        float fl = sqrtf(fw.x * fw.x + fw.z * fw.z);
        V3 mid = v3(c.x + (fl > 0.01f ? fw.x / fl : 0) * 8.5f, c.y + 1.5f, c.z + (fl > 0.01f ? fw.z / fl : 0) * 8.5f);
        V3 vel = v3(WIND_X * DRIFT, -FALL, WIND_Z * DRIFT);
        V3 dir = vnorm(vel);
        /* Lit by the sky as everything is. */
        uint32_t day = world_lit(0xffc4d0dc, v3(0, 1, 0));
        float base = 0.20f + 0.26f * rain_a;
        int n = (int)(DROPS * (0.25f + 0.75f * rain_a));
        r_hdr(h_rain);
        for(int i = 0; i < n; i++) {
            const float *u = drop_u[i];
            /* Where it is in the box round mid now. */
            V3 p = v3(mid.x + (wrap(u[0] + (vel.x * rain_t - mid.x) * (1.0f / DX)) - 0.5f) * DX,
                      mid.y + (wrap(u[1] + (vel.y * rain_t - mid.y) * (1.0f / DY)) - 0.5f) * DY,
                      mid.z + (wrap(u[2] + (vel.z * rain_t - mid.z) * (1.0f / DX)) - 0.5f) * DX);
            V3 d = vsub(p, c);
            float z = vdot(d, fw);
            if(z < 0.8f || z > RAIN_FAR || fabsf(vdot(d, r_cam.right)) > z * 0.8f + 0.6f) continue;
            /* A line a pixel wide breaks into dashes: far ones are held at
               a pixel and a half and fade instead. */
            float hw = fmaxf(0.006f, 0.0014f * z), a = base * (1.0f - z * (1.0f / RAIN_FAR)) * clampf((z - 0.8f) * (1.0f / 3.0f), 0, 1);
            uint32_t col = day;
            V3 side = vcross(dir, d);
            float sl = vlen(side);
            if(sl < 1e-4f) continue;
            side = vscale(side, hw / sl);
            V3 tail = vmad(p, dir, -STREAK);
            uint32_t head = color_alpha(col, a), gone = col & 0xffffffu;
            RV *o = strip(4);
            o[0] = (RV){tail.x - side.x, tail.y - side.y, tail.z - side.z, 0, 0, gone};
            o[1] = (RV){tail.x + side.x, tail.y + side.y, tail.z + side.z, 0, 0, gone};
            o[2] = (RV){p.x - side.x, p.y - side.y, p.z - side.z, 0, 0, head};
            o[3] = (RV){p.x + side.x, p.y + side.y, p.z + side.z, 0, 0, head};
            if(o != buf) o[-1] = o[0];
        }
        /* And rain in the headlights: the box above is too thin a rain
           for more than a drop or two to be in a beam, and it is the
           beams that show a night's rain. Each beam has BEAM_DROPS of its
           own, falling through the cone at fixed places along it. */
        int nb = pal->dark > 0.25f ? lamps_beams() : 0;
        for(int li = 0; li < nb; li++) {
            V3 o, f, r, u;
            float th, tv, range;
            lamps_beam(li, &o, &f, &r, &u, &th, &tv, &range);
            V3 to = vsub(vmad(o, f, range * 0.4f), c);
            if(vdot(to, to) > 45.0f * 45.0f || vdot(to, fw) < -range * 0.5f) continue;
            for(int i = 0; i < BEAM_DROPS; i++) {
                const float *q = drop_u[(li * BEAM_DROPS + i) % DROPS];
                float along = 2.0f + (range * 0.75f - 2.0f) * q[0], tall = fmaxf(along * tv * 1.6f, 1.2f);
                float side = (2.0f * q[2] - 1.0f) * along * th * 0.9f, up = (0.5f - wrap(q[1] + rain_t * (FALL / tall))) * tall;
                V3 p = vmad(vmad(vmad(o, f, along), r, side), u, up);
                float k = lamps_light(p, 1 << li, -1, NULL);
                if(k < 0.04f) continue;
                V3 d = vsub(p, c);
                float z = vdot(d, fw);
                if(z < 0.8f || z > 60.0f) continue;
                V3 sd = vcross(dir, d);
                float sl = vlen(sd);
                if(sl < 1e-4f) continue;
                sd = vscale(sd, fmaxf(0.008f, 0.0016f * z) / sl);
                V3 tail = vmad(p, dir, -0.4f);
                uint32_t head = color_alpha(0xfffff2dc, clampf(k, 0, 1) * (0.35f + 0.5f * rain_a)), gone = 0x00fff2dcu;
                RV *v = strip(4);
                v[0] = (RV){tail.x - sd.x, tail.y - sd.y, tail.z - sd.z, 0, 0, gone};
                v[1] = (RV){tail.x + sd.x, tail.y + sd.y, tail.z + sd.z, 0, 0, gone};
                v[2] = (RV){p.x - sd.x, p.y - sd.y, p.z - sd.z, 0, 0, head};
                v[3] = (RV){p.x + sd.x, p.y + sd.y, p.z + sd.z, 0, 0, head};
                if(v != buf) v[-1] = v[0];
            }
        }
        flush();
    }
    if(n_rings) {
        /* Each ring a band on the water, widening and fading. */
        static float sa[RING_SEG + 1], ca[RING_SEG + 1];
        if(ca[0] == 0)
            for(int k = 0; k <= RING_SEG; k++) {
                sa[k] = sinf(ENG_TAU * k / RING_SEG);
                ca[k] = cosf(ENG_TAU * k / RING_SEG);
            }
        uint32_t lit = world_lit(0xffe6eef4, v3(0, 1, 0));
        r_hdr(h_rain);
        for(int i = 0; i < RINGS; i++) {
            const Ring *r = &rings[i];
            if(r->age >= RING_LIFE) continue;
            V3 d = v3(r->x - c.x, r->y - c.y, r->z - c.z);
            float z = vdot(d, fw);
            if(z < 1.0f || z > 34.0f || fabsf(vdot(d, r_cam.right)) > z * 0.8f + 1.0f) continue;
            float t = r->age * (1.0f / RING_LIFE), rad = r->size * (0.25f + 0.75f * t);
            float band = fmaxf(0.04f, 0.003f * z), in = fmaxf(rad - band, 0);
            uint32_t col = color_alpha(lit, 0.8f * (1.0f - t) * clampf(1.25f - z * (1.0f / 30.0f), 0, 1));
            RV *o = strip(2 * (RING_SEG + 1));
            for(int k = 0; k <= RING_SEG; k++) {
                o[2 * k] = (RV){r->x + sa[k] * in, r->y, r->z + ca[k] * in, 0, 0, col};
                o[2 * k + 1] = (RV){r->x + sa[k] * rad, r->y, r->z + ca[k] * rad, 0, 0, col};
            }
            if(o != buf) o[-1] = o[0];
        }
        flush();
    }
}
