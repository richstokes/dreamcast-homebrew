/* Neon Skyway's life: flying traffic, and a UFO on the final lap.

   Far below the deck, just above the cloud sea, freeways cross the city:
   long straight lanes of lights, white one way and red the other, each
   car a dot with a streak, seen between the towers like a city at night
   from a plane. They are background, never near enough to be a shape.

   On the final lap, as a human comes up the climb to the crest, a saucer
   rises out of the cloud sea beside the road, glides over it beyond the
   plunge and hangs there, rim lights chasing, its beam sweeping the deck
   and catching any kart passing under, while the city's searchlights
   swing round to it. Once a human has driven through the beam it lifts
   and shoots off ahead into the sky.

   Cosmetic only: nothing here touches a kart, and it runs on its own
   clocks, so online play is unchanged. */
#include "skylife.h"
#include "kart.h"
#include "engine/render.h"
#include "track.h"
#include "world.h"
#include "world_int.h"

#define FREEWAY_N  4
#define FW_CARS    16         /* per direction, per freeway */
#define FW_LEN     760.0f     /* across the whole city */
#define FW_SPACING (FW_LEN / FW_CARS)
#define FW_FADE    80.0f      /* lights fade over each end, so none blinks out */

/* The UFO: where it hangs, and the show's timing. */
#define UFO_S      585.0f     /* over the road after the plunge */
#define UFO_TRIG   330.0f     /* crossed on the final lap, up the climb */
#define UFO_H      26.0f      /* above the road */
#define UFO_R      18.0f      /* rim radius */
#define RISE_T     3.2f
#define DEPART_T   3.2f
#define MAX_HOVER  22.0f
#define BEAM_GRAB  28.0f      /* the beam swings onto a kart this near */

typedef struct {
    V3 o, dir;        /* its middle, and the way the white lights go */
    V3 side;          /* lane offset */
    float speed, phase;
} Freeway;

static Freeway freeways[FREEWAY_N];
static bool on;

typedef struct {
    float t;          /* since the trigger, < 0 waiting */
    float dep_t;      /* since it left, < 0 still here */
    V3 pos, vel;
    float spin, spin_v;
    float prev_s[MAX_PLAYERS];
    V3 hover, fwd, rise_from;
    float road_y;
} Ufo;

static Ufo ufo;
static void ufo_mesh_build(void);

static float hash01(int i, int k) {
    uint32_t h = (uint32_t)i * 2654435761u ^ (uint32_t)k * 40503u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (float)(h & 0xffff) / 65535.0f;
}

static float ease01(float x) {
    x = clampf(x, 0, 1);
    return x * x * (3.0f - 2.0f * x);
}

static int humans(void) {
    return game.num_players > 0 ? (game.num_players < MAX_PLAYERS ? game.num_players : MAX_PLAYERS) : 1;
}

void skylife_build(void) {
    on = track.id == TRACK_SKYWAY;
    if(!on) return;
    ufo_mesh_build();
    /* A grid round the course: two freeways running east-west past its
       north and south edges, two north-south past east and west, all out
       beyond the road so from the deck they are in the distance. */
    static const struct { float x, z, yaw; } grid[FREEWAY_N] = {
        {0.0f, 175.0f, DK_PI * 0.5f}, {0.0f, -185.0f, DK_PI * 0.5f}, {215.0f, 0.0f, 0.0f}, {-225.0f, 0.0f, 0.0f}};
    for(int i = 0; i < FREEWAY_N; i++) {
        Freeway *f = &freeways[i];
        float a = grid[i].yaw + (hash01(i, 3) - 0.5f) * 0.12f;
        f->o = v3(grid[i].x, CLOUD_Y + 3.0f + 4.0f * hash01(i, 11), grid[i].z);
        f->dir = v3(sinf(a), 0, cosf(a));
        f->side = v3(cosf(a), 0, -sinf(a));
        f->speed = 20.0f + 8.0f * hash01(i, 13);
        f->phase = hash01(i, 17) * FW_SPACING;
    }
    /* The UFO's spot over the road, and where it rises from: out over the
       cloud sea on the right. */
    float s = UFO_S;
    ufo.road_y = track_road_y(s, 0);
    ufo.hover = track_point(s, 0);
    ufo.hover.y = ufo.road_y + UFO_H;
    V3 a = track_point(s - 5.0f, 0), b = track_point(s + 5.0f, 0);
    ufo.fwd = vnorm(v3(b.x - a.x, 0, b.z - a.z));
    ufo.rise_from = track_point(s - 30.0f, 70.0f);
    ufo.rise_from.y = CLOUD_Y - 30.0f;
    skylife_reset();
}

void skylife_reset(void) {
    ufo.t = -1.0f;
    ufo.dep_t = -1.0f;
    ufo.spin = 0;
    for(int p = 0; p < MAX_PLAYERS; p++) ufo.prev_s[p] = -1.0f;
}

static void ufo_update(float dt) {
    if(ufo.t < 0) {
        for(int p = 0; p < humans(); p++) {
            const Kart *k = &karts[game.pl[p].kart];
            float s = k->tp.s;
            if(k->lap >= game.laps && ufo.prev_s[p] >= 0 && track_delta(ufo.prev_s[p], UFO_TRIG) > 0 &&
               track_delta(UFO_TRIG, s) >= 0 && track_delta(ufo.prev_s[p], s) < 20.0f)
                ufo.t = 0;
            ufo.prev_s[p] = s;
        }
        if(ufo.t < 0) return;
    }
    ufo.t += dt;
    if(ufo.dep_t >= 0) {
        ufo.dep_t += dt;
        /* A short lift, then away ahead and up, faster and faster. */
        float x = ufo.dep_t;
        V3 dir = vnorm(vadd(ufo.fwd, v3(0, 0.2f, 0)));
        float lift = 6.0f * ease01(x / 0.5f);
        float go = x > 0.45f ? 30.0f * (x - 0.45f) + 110.0f * (x - 0.45f) * (x - 0.45f) : 0.0f;
        V3 np = vadd(vmad(ufo.hover, dir, go), v3(0, lift, 0));
        ufo.vel = vscale(vsub(np, ufo.pos), 1.0f / fmaxf(dt, 0.001f));
        ufo.pos = np;
        ufo.spin_v = 9.0f;
        ufo.spin += ufo.spin_v * dt;
        if(ufo.dep_t > DEPART_T) ufo.t = 1e9f;   /* gone for this race */
        return;
    }
    if(ufo.t < RISE_T) {
        /* Up out of the clouds, then across to hang over the road. */
        float r = ufo.t / RISE_T;
        float up = 1.0f - (1.0f - r) * (1.0f - r) * (1.0f - r);
        float across = ease01((r - 0.3f) / 0.7f);
        V3 p = vlerp(ufo.rise_from, ufo.hover, across);
        p.y = lerpf(ufo.rise_from.y, ufo.hover.y, up);
        ufo.pos = p;
        ufo.spin_v = lerpf(6.0f, 1.2f, r);
    }
    else {
        float h = ufo.t - RISE_T;
        ufo.pos = vadd(ufo.hover, v3(0.8f * sinf(h * 0.9f), 0.9f * sinf(h * 1.7f), 0.6f * cosf(h * 1.1f)));
        ufo.spin_v = 1.2f;
    }
    ufo.spin += ufo.spin_v * dt;
    ufo.vel = v3(0, 0, 0);
    /* It hangs until a human has driven through the beam (or long enough),
       then shoots off ahead of them. */
    bool go = ufo.t > RISE_T + MAX_HOVER;
    for(int p = 0; p < humans(); p++) {
        float past = track_delta(UFO_S, karts[game.pl[p].kart].tp.s);
        if(ufo.t > RISE_T && past > 6.0f && past < 60.0f) go = true;
    }
    if(go) ufo.dep_t = 0;
}

void skylife_update(float dt) {
    if(!on) return;
    ufo_update(dt);
}

float skylife_ufo(V3 *pos) {
    if(!on || ufo.t < 0 || ufo.t > 1e8f) return 0;
    *pos = ufo.pos;
    /* Swinging round as it rises, letting go as it leaves. */
    float k = ease01((ufo.t - 1.0f) / 2.5f);
    return ufo.dep_t >= 0 ? k * (1.0f - ease01(ufo.dep_t / 1.2f)) : k;
}

static bool ufo_up(void) {
    return on && ufo.t >= 0 && ufo.t < 1e8f;
}

/* ---- The saucer ---- */

#define U_SEG   24
#define U_RINGS 11

/* Its profile, radius and height, top to bottom. */
static const float prof[U_RINGS][2] = {
    {0.0f, 9.0f}, {3.2f, 8.4f}, {5.2f, 6.6f}, {6.0f, 4.6f}, {9.0f, 3.6f}, {14.5f, 2.0f},
    {UFO_R, 0.0f}, {16.0f, -1.3f}, {10.0f, -2.2f}, {6.5f, -2.5f}, {0.0f, -2.9f},
};

/* Unit circle, once: the rings, the glow rings and the rim lights all
   step round it (24, 16 and 20 to a turn all divide 240). */
#define CIRC_N 240
static float circ_s[CIRC_N + 1], circ_c[CIRC_N + 1];

/* The saucer's vertices, built once: positions never change (it spins by
   its model matrix), so per frame only the colours are written, and only
   the metal rings need a dot product each, against normals kept here. */
static RV ufo_v[U_RINGS * (U_SEG + 1)] __attribute__((aligned(32)));
static uint16_t ufo_idx[(U_RINGS - 1) * U_SEG * 6];
static float ufo_norm[U_RINGS][U_SEG + 1][3];
static float ufo_band[U_RINGS][U_SEG + 1];   /* (sky + rim) * band, the part of the metal shade without the moon */

static void ufo_mesh_build(void) {
    for(int i = 0; i <= CIRC_N; i++) {
        circ_s[i] = sinf((float)i / CIRC_N * DK_TAU);
        circ_c[i] = cosf((float)i / CIRC_N * DK_TAU);
    }
    int n = 0;
    for(int r = 0; r < U_RINGS - 1; r++)
        for(int i = 0; i < U_SEG; i++) {
            uint16_t a = (uint16_t)(r * (U_SEG + 1) + i), b = (uint16_t)(a + U_SEG + 1);
            uint16_t t[6] = {a, b, (uint16_t)(a + 1), (uint16_t)(a + 1), b, (uint16_t)(b + 1)};
            for(int k = 0; k < 6; k++) ufo_idx[n++] = t[k];
        }
    n = 0;
    for(int r = 0; r < U_RINGS; r++) {
        /* The profile's normal from its neighbours. */
        int r0 = r > 0 ? r - 1 : r, r1 = r < U_RINGS - 1 ? r + 1 : r;
        float tr = prof[r1][0] - prof[r0][0], ty = prof[r1][1] - prof[r0][1];
        float nr = ty, ny = -tr, nl = sqrtf(nr * nr + ny * ny);
        nr /= nl;
        ny /= nl;
        if(r == 0) {
            nr = 0;
            ny = 1;
        }
        if(r == U_RINGS - 1) {
            nr = 0;
            ny = -1;
        }
        for(int i = 0; i <= U_SEG; i++) {
            float sa = circ_s[i * (CIRC_N / U_SEG)], ca = circ_c[i * (CIRC_N / U_SEG)];
            ufo_norm[r][i][0] = nr * sa;
            ufo_norm[r][i][1] = ny;
            ufo_norm[r][i][2] = nr * ca;
            /* Brushed metal: a white highlight along the rim, panels banded. */
            float sky = 0.2f + 0.2f * ny, band = (i & 1) ? 1.0f : 0.82f, rim = r == 6 ? 0.55f : 0.0f;
            ufo_band[r][i] = sky * band + rim;
            ufo_v[n++] = (RV){prof[r][0] * sa, prof[r][1], prof[r][0] * ca, 0, 0, 0};
        }
    }
}

static void draw_ufo_mesh(void) {
    float t = world_time;
    /* The moon from the model's frame (it spins about y). */
    float cy = cosf(-ufo.spin), sy = sinf(-ufo.spin);
    V3 L = v3(world_sun_dir.x * cy + world_sun_dir.z * sy, world_sun_dir.y, -world_sun_dir.x * sy + world_sun_dir.z * cy);
    float dome = 0.75f + 0.25f * sinf(t * 3.0f);
    float under = 0.6f + 0.4f * sinf(t * 5.0f);
    int n = 0;
    for(int r = 0; r < U_RINGS; r++) {
        if(r <= 3 || r >= 8) {
            /* The glass dome, lit from inside, and the glowing ring under
               the hull, magenta into the core: one colour a ring. */
            uint32_t c;
            if(r <= 3) c = color_lerp(argb(255, 30, 120, 170), argb(255, 190, 255, 255), clampf((r == 0 ? 1.0f : 0.85f - 0.12f * r) * dome, 0, 1));
            else c = r == 8 ? argb(255, 60, 20, 70) : color_lerp(argb(255, 120, 20, 110), argb(255, 255, 120, 240), under);
            for(int i = 0; i <= U_SEG; i++) ufo_v[n++].c = c;
            continue;
        }
        /* Metal: the moon on top, the city's neon bounced from below. */
        float ny = ufo_norm[r][0][1];
        float bounce = fmaxf(0.0f, -ny) * 0.7f;
        float br = 200.0f * bounce, bg = 40.0f * bounce, bb = 190.0f * bounce;
        for(int i = 0; i <= U_SEG; i++) {
            const float *nn = ufo_norm[r][i];
            float lam = fmaxf(0.0f, nn[0] * L.x + nn[1] * L.y + nn[2] * L.z);
            float m = ufo_band[r][i] + 0.8f * lam * ((i & 1) ? 1.0f : 0.82f);
            int cr = (int)clampf(170.0f * m + br, 0, 255);
            int cg = (int)clampf(178.0f * m + bg, 0, 255);
            int cb = (int)clampf(200.0f * m + bb, 0, 255);
            ufo_v[n++].c = argb(255, cr, cg, cb);
        }
    }
    float mt[16];
    m4_translate_rotate(mt, ufo.pos, ufo.spin, 0, 0);
    r_load_model(mt);
    r_mesh(ufo_v, n, ufo_idx, (U_RINGS - 1) * U_SEG * 6, false);
}

void skylife_draw_op(void) {
    if(!on) return;
    if(ufo_up() && r_sphere_visible(ufo.pos, UFO_R + 2.0f)) {
        r_hdr(h_col);
        draw_ufo_mesh();
    }
    r_load_vp();
}

/* A flat ring of additive light, in world space. */
static void glow_ring(V3 c, float r0, float r1, uint32_t c0, uint32_t c1) {
    RV v[2 * 17];
    int n = 0;
    for(int i = 0; i <= 16; i++) {
        float ca = circ_c[i * (CIRC_N / 16)], sa = circ_s[i * (CIRC_N / 16)];
        v[n++] = (RV){c.x + sa * r1, c.y, c.z + ca * r1, 0.5f, 0.0f, c1};
        v[n++] = (RV){c.x + sa * r0, c.y, c.z + ca * r0, 0.5f, 0.5f, c0};
    }
    r_strip(v, n);
}

/* A soft cone of light from a down to b, turned to face the camera. */
static void beam(V3 a, V3 b, float wa, float wb, uint32_t ca, uint32_t cb) {
    V3 dir = vnorm(vsub(b, a));
    V3 sa = vnorm(vcross(dir, vsub(r_cam.pos, a))), sb = vnorm(vcross(dir, vsub(r_cam.pos, b)));
    RV q[4] = {
        {a.x - sa.x * wa, a.y - sa.y * wa, a.z - sa.z * wa, 0, 0.5f, ca},
        {a.x + sa.x * wa, a.y + sa.y * wa, a.z + sa.z * wa, 1, 0.5f, ca},
        {b.x - sb.x * wb, b.y - sb.y * wb, b.z - sb.z * wb, 0, 0.5f, cb},
        {b.x + sb.x * wb, b.y + sb.y * wb, b.z + sb.z * wb, 1, 0.5f, cb},
    };
    r_strip(q, 4);
}

static void draw_ufo_lights(void) {
    float t = world_time;
    V3 c = ufo.pos;
    if(!r_sphere_visible(c, 90.0f)) return;
    float on_k = ufo.t < 1.4f ? 0.0f : ufo.t < 2.4f ? ((int)(ufo.t * 14.0f) & 1 ? 1.0f : 0.3f) : 1.0f;
    r_hdr(h_glow_add);
    /* A wide halo over the cloud sea, a hot core round the hull. */
    float pulse = 0.85f + 0.15f * sinf(t * 2.3f);
    /* Kept modest: big additive quads over the whole screen are fill
       rate the console pays for (the first cut doubled its render time). */
    bool lean = world_split;   /* two views: half the glows each */
    /* The big glows shrink as the camera comes near, so what they cover
       on screen stays bounded: each is a fill pass over its area, and
       close up they overlapped over most of the screen (console bench:
       PVR 17-21 ms, up to 21 dropped frames in 2 s). */
    V3 dc = vsub(c, r_cam.pos);
    float near_k = clampf(sqrtf(vdot(dc, dc)) / 110.0f, 0.3f, 1.0f);
    if(!lean) r_billboard(c, 56.0f * pulse * near_k, 32.0f * pulse * near_k, 0, 0, 0, 1, 1, argb((int)(70 * on_k), 120, 110, 255));
    r_billboard(vadd(c, v3(0, 0.5f, 0)), 36.0f * near_k, 12.0f * near_k, 0, 0, 0, 1, 1, argb((int)(120 * on_k), 200, 230, 255));
    r_billboard(vadd(c, v3(0, 6.8f, 0)), 18.0f * pulse * near_k, 14.0f * pulse * near_k, 0, 0, 0, 1, 1, argb((int)(170 * on_k), 120, 255, 255));
    /* Underneath: a magenta ring of light. */
    float under = 0.6f + 0.4f * sinf(t * 5.0f);
    glow_ring(vadd(c, v3(0, -3.1f, 0)), 2.0f, 12.0f, argb((int)(200 * on_k * under), 255, 90, 230), argb(0, 255, 60, 200));
    /* Rim lights chasing round: cyan, magenta and white. */
    static const uint32_t chase[3] = {0xff3cdcff, 0xffff32c8, 0xffffffff};
    float ss = sinf(ufo.spin), cs = cosf(ufo.spin);   /* the lights turn with the hull */
    for(int i = 0; i < 20; i++) {
        float sa = circ_s[i * (CIRC_N / 20)], ca = circ_c[i * (CIRC_N / 20)];
        float sr = sa * cs + ca * ss, cr = ca * cs - sa * ss;
        V3 p = vadd(c, v3(sr * (UFO_R + 0.4f), 0.1f, cr * (UFO_R + 0.4f)));
        int ph = (i + (int)(t * 10.0f)) % 3;
        float lit = ((i + (int)(t * 10.0f)) % 5 == 0) ? 1.0f : 0.55f;
        uint32_t col = chase[ph];
        r_billboard(p, 4.5f * lit * near_k, 4.5f * lit * near_k, 0, 0, 0, 1, 1, color_alpha(col, on_k * lit));
    }
    if(ufo.dep_t >= 0) {
        /* Away: a streak behind it, and a flash as it goes. */
        V3 tail = vmad(c, ufo.vel, -0.12f);
        r_streak(c, tail, 16.0f * near_k, argb(220, 220, 250, 255), argb(0, 140, 80, 255));
        float f = ufo.dep_t - (DEPART_T - 0.5f);
        if(f > 0) {
            float a = sinf(clampf(f / 0.5f, 0, 1) * DK_PI);
            float fl = (lean ? 50.0f : 80.0f) * a * near_k;
            r_billboard(c, fl, fl, 0, 0, 0, 1, 1, argb((int)(230 * a), 230, 240, 255));
        }
        return;
    }
    /* The beam, on once it hangs over the road, wandering across it. */
    float b = ease01((ufo.t - RISE_T) / 0.6f);
    if(b <= 0) return;
    float w = ufo.t - RISE_T;
    V3 spot = vmad(ufo.hover, heading_right(atan2f(ufo.fwd.x, ufo.fwd.z)), 5.0f * sinf(w * 0.8f));
    spot.y = ufo.road_y + 0.15f;
    /* A kart coming under it is caught in the beam. */
    for(int p = 0; p < humans(); p++) {
        const Kart *k = &karts[game.pl[p].kart];
        float dx = k->pos.x - ufo.hover.x, dz = k->pos.z - ufo.hover.z, q = dx * dx + dz * dz;
        if(q < BEAM_GRAB * BEAM_GRAB) {
            float g = ease01(1.0f - sqrtf(q) / BEAM_GRAB + 0.5f);
            spot = vlerp(spot, v3(k->pos.x, k->pos.y + 0.15f, k->pos.z), g);
        }
    }
    V3 top = vadd(c, v3(0, -2.6f, 0));
    beam(top, spot, 5.5f, 11.0f, argb((int)(110 * b), 140, 255, 225), argb((int)(45 * b), 120, 200, 255));
    if(!lean) beam(top, spot, 2.0f, 4.0f, argb((int)(120 * b), 230, 255, 255), argb((int)(40 * b), 200, 255, 255));
    /* Rings sliding down it, and a pool of light on the deck. */
    for(int k = 0; k < (lean ? 0 : 3); k++) {
        float f = fmodf(w * 0.7f + k / 3.0f, 1.0f);
        V3 p = vlerp(top, spot, f);
        float r = lerpf(5.5f, 11.0f, f);
        float a = sinf(f * DK_PI) * b;
        glow_ring(p, r - 0.8f, r, argb((int)(170 * a), 160, 255, 230), argb((int)(170 * a), 160, 255, 230));
    }
    float pool = 0.75f + 0.25f * sinf(w * 6.0f);
    glow_ring(spot, 0.0f, 12.0f, argb((int)(150 * b * pool), 120, 255, 220), argb(0, 80, 160, 255));
}

/* The freeways: each direction a row of lights FW_SPACING apart sliding
   along, a bright dot with a streak behind it, white coming and red
   going. No fog, so they are seen right across the city. Not in a split
   screen: on the console they cost its two views ~2-3 ms of render time
   (sk-*-2p benches, 2026-11-04) for lights the deck mostly hides. */
static void draw_freeways(void) {
    if(world_split) return;
    r_hdr(h_glow_add);
    int step = 1;
    for(int i = 0; i < FREEWAY_N; i++) {
        const Freeway *f = &freeways[i];
        if(!r_sphere_visible(f->o, FW_LEN * 0.5f)) continue;
        for(int dir = -1; dir <= 1; dir += 2) {
            V3 o = vmad(f->o, f->side, dir * 5.0f);
            V3 d = vscale(f->dir, (float)dir);
            uint32_t head = dir > 0 ? argb(180, 235, 245, 255) : argb(180, 255, 60, 60);
            uint32_t tail = dir > 0 ? argb(0, 120, 180, 255) : argb(0, 255, 20, 20);
            float run = fmodf(f->speed * world_time + f->phase, FW_SPACING);
            for(int c = 0; c < FW_CARS; c += step) {
                float u = -FW_LEN * 0.5f + run + (float)c * FW_SPACING + 4.0f * hash01(i * 2 + (dir > 0), c);
                if(u > FW_LEN * 0.5f) u -= FW_LEN;
                V3 p = vmad(o, d, u);
                if(!r_sphere_visible(p, 14.0f)) continue;
                float a = clampf((FW_LEN * 0.5f - fabsf(u)) / FW_FADE, 0.0f, 1.0f);
                uint32_t h = color_alpha(head, a);
                r_billboard(p, 2.6f, 2.6f, 0, 0, 0, 1, 1, h);
                r_streak(p, vmad(p, d, -12.0f), 1.2f, h, tail);
            }
        }
    }
}

void skylife_draw_tr(void) {
    if(!on) return;
    draw_freeways();
    if(ufo_up()) draw_ufo_lights();
}
