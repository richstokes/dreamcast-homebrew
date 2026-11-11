/* Ridgeline: the dunes' wires and blown sand (dunes.h).

   The wires: three to a span, each a sagging run of thin ribbon between
   one H-frame's crossarm and the next. A wire is a few centimetres
   thick and would come and go between the screen's lines, so a ribbon
   is drawn a pixel or two wide wherever it is, turned to face the
   camera: a line of even weight against the sky from under it or from
   half a kilometre off. Far off the three would run together into a
   dark band, so only the middle one is drawn there, and translucent:
   wide enough never to break into dashes, faint enough to read as thin.

   The sand: when the wind gusts, the crests smoke. Every brink in the
   area (a vertex the ground climbs to gently with the wind and falls
   steeply from) is found once; the ones about the camera are kept in a
   short list, and in a gust wisps start from them, low and fast
   downwind, spreading and thinning over the slip face. Soft sprites are
   what the PVR is slow at when they are near and big, so a wisp thins
   out close to the camera and there are never many.

   And the whips: every rig that goes into dunes carries a flag on a
   long whip, to be seen coming over a crest, so here every truck has
   one off its tail. The tip is a weight on a spring over the mount, so
   the whip lays back as the truck pulls away, nods over a bump and
   quivers after a landing; the pennant streams on the wind the truck
   makes and the wind there is.

   Its randomness is its own, as the desert's is, so a demo or a bench
   that seeds the game's generator plays out the same. */
#include "dunes.h"
#include "world.h"
#include "terrain.h"
#include "tod.h"
#include "truck.h"

#define SPANS 16
#define WIRE_SEG 8
#define WIRE_SAG 0.022f         /* of the span */
#define WIRE_FAR 520.0f
#define ARM_Y 13.05f            /* where a wire hangs, over the frame's foot (world.c's pylon) */
#define ARM_X 4.2f

#define BRINKS 6000
#define NEAR_MAX 160
#define NEAR_M 95.0f
#define WISPS 36

#define WHIP_LEN 2.7f
#define WHIP_SEG 5
typedef struct {
    V3 tip, vel;
    bool live;
} Whip;
static Whip whips[MAX_TRUCKS];

typedef struct { V3 a, b; } Span;
static Span spans[SPANS * 3];
static int nspan;
static V3 span_c[SPANS];
static float span_r[SPANS];

static uint16_t (*brink)[2];
static int nbrink;
static uint16_t near_b[NEAR_MAX];
static int nnear, near_tick;

typedef struct {
    V3 p, v;
    float age, life, size;
} Wisp;
static Wisp wisps[WISPS];
static float wind_t, spawn_acc, gust;
static const pvr_poly_hdr_t *h_wire, *h_wire_far, *h_sand;
static bool on;

static uint32_t seed = 0x51d0beefu;
static inline float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8 & 0xffff) * (1.0f / 65535.0f);
}

void dunes_init(void) {
    h_wire = r_header(TEX_NONE, HF_OP);
    h_wire_far = r_header(TEX_NONE, HF_TR);
    h_sand = r_header(TEX_PUFF, HF_TR);
    on = lvl_area == AREA_DUNES;
    nspan = nbrink = nnear = 0;
    for(int i = 0; i < WISPS; i++) wisps[i].life = 0;
    for(int i = 0; i < MAX_TRUCKS; i++) whips[i].live = false;
    if(!on) return;
    /* The frames in the order the level lists them, which is along the
       line; a last span runs on from each end, off the map. */
    V3 arm[SPANS + 2][3];
    int n = 0;
    for(int i = 0; i < LVL_PROPS && n < SPANS; i++) {
        const lvl_prop_t *p = &lvl_props[i];
        if(p->kind != PROP_PYLON) continue;
        float cy = cosf(p->yaw), sy = sinf(p->yaw);
        for(int k = 0; k < 3; k++)
            arm[n + 1][k] = v3(p->x + cy * (k - 1) * ARM_X, p->y + ARM_Y, p->z - sy * (k - 1) * ARM_X);
        n++;
    }
    if(n >= 2) {
        for(int k = 0; k < 3; k++) {
            V3 d0 = vsub(arm[1][k], arm[2][k]), d1 = vsub(arm[n][k], arm[n - 1][k]);
            arm[0][k] = vadd(arm[1][k], v3(d0.x, -2.0f, d0.z));
            arm[n + 1][k] = vadd(arm[n][k], v3(d1.x, 0, d1.z));
        }
        for(int s = 0; s <= n && nspan < SPANS; s++) {
            for(int k = 0; k < 3; k++) spans[nspan * 3 + k] = (Span){arm[s][k], arm[s + 1][k]};
            span_c[nspan] = vscale(vadd(arm[s][1], arm[s + 1][1]), 0.5f);
            span_r[nspan] = vlen(vsub(arm[s][1], arm[s + 1][1])) * 0.5f + ARM_X + 1.0f;
            nspan++;
        }
    }
    /* Brinks: by the slope coming up to a vertex with the wind and the
       slope going on from it. */
    if(!brink) brink = spent_alloc(sizeof(*brink) * BRINKS);
    if(!brink) return;
    for(int j = 2; j < LEVEL_N - 2; j++)
        for(int i = 2; i < LEVEL_N - 2 && nbrink < BRINKS; i++) {
            float h = lvl_h(i, j);
            float up = ((h - lvl_h(i - 1, j)) * WIND_X + (h - lvl_h(i, j - 1)) * WIND_Z) * (1.0f / LEVEL_D);
            float dn = ((lvl_h(i + 1, j) - h) * WIND_X + (lvl_h(i, j + 1) - h) * WIND_Z) * (1.0f / LEVEL_D);
            if(dn > -0.3f || up < -0.2f) continue;
            int sf = lvl_surf[j * LEVEL_N + i];
            if(sf != SURF_SAND && sf != SURF_DEEPSAND) continue;
            /* (the edge of a trail's cut is no dune) */
            if(h - lvl_h(i + 2, j + 2) < 2.2f) continue;
            brink[nbrink][0] = (uint16_t)i;
            brink[nbrink][1] = (uint16_t)j;
            nbrink++;
        }
    printf("Ridgeline dunes: %d spans of wire, %d brinks\n", nspan, nbrink);
}

/* Where a truck's whip is bolted on: the tail's left corner, half way up. */
static V3 whip_mount(const Truck *t) {
    const float *c = t->spec.hull_c, *h = t->spec.hull_h;
    return truck_to_world(t, v3(c[0] - h[0] * 0.7f, c[1] + h[1] * 0.2f, c[2] - h[2] * 0.88f));
}

void dunes_update(float dt) {
    if(!on || game_frozen()) return;
    wind_t += dt;
    for(int i = 0; i < game.ntrucks && i < MAX_TRUCKS; i++) {
        const Truck *t = &trucks[i];
        Whip *w = &whips[i];
        if(t->absent) {
            w->live = false;
            continue;
        }
        V3 base = whip_mount(t), rest = vmad(base, t->ax[1], WHIP_LEN);
        V3 off = vsub(w->tip, rest);
        if(!w->live || vdot(off, off) > 9.0f) {   /* new here, or the truck was put somewhere else */
            w->tip = rest;
            w->vel = t->vel;
            w->live = true;
            continue;
        }
        /* A stiff spring to where it would stand, and the wind on the flag. */
        float k = dt > 0.02f ? 0.5f : 1.0f;   /* (a slow frame: softer, so it cannot ring) */
        V3 acc = vadd(vscale(off, -85.0f * k), vscale(vsub(w->vel, t->vel), -6.5f));
        acc = vadd(acc, v3(WIND_X * 2.0f * gust, 0, WIND_Z * 2.0f * gust));
        w->vel = vmad(w->vel, acc, dt);
        w->tip = vmad(w->tip, w->vel, dt);
        /* It bends; it does not stretch. */
        V3 d = vsub(w->tip, base);
        float l = vlen(d);
        if(l > 0.01f) w->tip = vmad(base, d, WHIP_LEN / l);
        V3 lean = vsub(w->tip, rest);
        float ll = vlen(lean);
        if(ll > 1.3f) w->tip = vmad(rest, lean, 1.3f / ll);
    }
    /* The desert's gusts (desert.c), a little stronger at the top of each. */
    gust = 0.55f + 0.45f * sinf(wind_t * 0.23f) * sinf(wind_t * 0.071f + 1.3f);
    float blow = clampf((gust - 0.62f) * (1.0f / 0.3f), 0, 1);
    V3 c = cam.pos;
    /* The brinks near the camera, found again every half second. */
    if(nbrink && ++near_tick >= 30) {
        near_tick = 0;
        nnear = 0;
        for(int k = 0; k < nbrink && nnear < NEAR_MAX; k++) {
            float dx = LEVEL_ORG + brink[k][0] * LEVEL_D - c.x, dz = LEVEL_ORG + brink[k][1] * LEVEL_D - c.z;
            if(dx * dx + dz * dz < NEAR_M * NEAR_M) near_b[nnear++] = (uint16_t)k;
        }
    }
    spawn_acc += dt * 26.0f * blow * blow;
    while(spawn_acc >= 1.0f) {
        spawn_acc -= 1.0f;
        if(!nnear) break;
        const uint16_t *b = brink[near_b[(int)(rnd() * 0.999f * (float)nnear)]];
        float x = LEVEL_ORG + b[0] * LEVEL_D + (rnd() - 0.5f) * 3.0f, z = LEVEL_ORG + b[1] * LEVEL_D + (rnd() - 0.5f) * 3.0f;
        if((x - c.x) * cam.fwd.x + (z - c.z) * cam.fwd.z < -6.0f) continue;
        for(int i = 0; i < WISPS; i++) {
            Wisp *w = &wisps[i];
            if(w->life > 0) continue;
            float sp = 6.0f + 4.0f * rnd();
            *w = (Wisp){.p = v3(x, lvl_h(b[0], b[1]) + 0.15f, z), .v = v3(WIND_X * sp, 0.5f + 0.5f * rnd(), WIND_Z * sp),
                        .life = 1.1f + 0.9f * rnd(), .size = 0.7f + 0.5f * rnd()};
            break;
        }
    }
    for(int i = 0; i < WISPS; i++) {
        Wisp *w = &wisps[i];
        if(w->life <= 0) continue;
        w->age += dt;
        if(w->age >= w->life) {
            w->life = 0;
            continue;
        }
        /* It sails out level over the slip face and sinks, never into it. */
        w->v.y -= 0.9f * dt;
        w->p = vmad(w->p, w->v, dt);
        float g = terrain_height(w->p.x, w->p.z) + 0.2f;
        if(w->p.y < g) w->p.y = g;
    }
}

static void whips_draw(void) {
    V3 c = r_cam.pos;
    r_hdr(h_wire);
    uint32_t pole = world_lit(0xffe6e4da, v3(0, 1, 0)), cloth = world_lit(0xffff6a1c, v3(0, 1, 0));
    for(int i = 0; i < game.ntrucks && i < MAX_TRUCKS; i++) {
        const Truck *t = &trucks[i];
        const Whip *w = &whips[i];
        if(!w->live || t->absent) continue;
        V3 base = whip_mount(t), mid = vmad(base, t->ax[1], WHIP_LEN * 0.55f);
        V3 to = vsub(base, c);
        float dist = vlen(to);
        if(dist > 160.0f || !r_sphere_visible(mid, WHIP_LEN)) continue;
        /* The whip: a curve from the mount, straight up out of it, to the tip. */
        RV v[2 * (WHIP_SEG + 1)];
        V3 p = base, below = base;
        float wd = fmaxf(0.011f, dist * 0.0007f);
        for(int s = 0; s <= WHIP_SEG; s++) {
            float u = (float)s / WHIP_SEG, a = (1 - u) * (1 - u), b = 2 * u * (1 - u), d = u * u;
            below = p;
            p = v3(base.x * a + mid.x * b + w->tip.x * d, base.y * a + mid.y * b + w->tip.y * d, base.z * a + mid.z * b + w->tip.z * d);
            V3 side = vcross(vsub(w->tip, base), vsub(p, c));
            float sl = vlen(side);
            side = sl > 1e-4f ? vscale(side, wd / sl) : v3(wd, 0, 0);
            v[2 * s] = (RV){p.x + side.x, p.y + side.y, p.z + side.z, 0, 0, pole};
            v[2 * s + 1] = (RV){p.x - side.x, p.y - side.y, p.z - side.z, 0, 0, pole};
        }
        r_strip(v, 2 * (WHIP_SEG + 1));
        /* The pennant, on the wind the truck feels. */
        V3 air = v3(WIND_X * (2.5f + 3.5f * gust) - t->vel.x, 0, WIND_Z * (2.5f + 3.5f * gust) - t->vel.z);
        float al = vlen(air);
        air = al > 0.3f ? vscale(air, 1.0f / al) : v3(WIND_X, 0, WIND_Z);
        float fl = wind_t * (9.0f + fminf(al, 12.0f)) + i * 2.1f, stream = clampf(al * 0.25f, 0.25f, 1.0f);
        V3 fly = v3(p.x + air.x * 0.5f * stream - air.z * 0.07f * sinf(fl), p.y - 0.14f - 0.3f * (1.0f - stream) + 0.05f * sinf(fl * 1.3f),
                    p.z + air.z * 0.5f * stream + air.x * 0.07f * sinf(fl));
        RV f[3] = {{p.x, p.y, p.z, 0, 0, cloth}, {below.x, below.y - 0.1f, below.z, 0, 0, color_scale(cloth, 0.9f)},
                   {fly.x, fly.y, fly.z, 0, 0, cloth}};
        r_strip(f, 3);
    }
}

#define WIRE_ONE_M 170.0f       /* past this a span is one faint wire */

/* The spans near the camera (three solid wires) or the ones further off. */
static void wires_draw(bool far) {
    V3 c = r_cam.pos;
    bool hdr = false;
    uint32_t col = world_lit(0xff34363c, v3(0, 1, 0));
    if(far) col = color_alpha(col, 0.5f);
    for(int s = 0; s < nspan; s++) {
        V3 d = vsub(span_c[s], c);
        float d2 = vdot(d, d);
        if((d2 > WIRE_ONE_M * WIRE_ONE_M) != far) continue;
        if(d2 > (WIRE_FAR + span_r[s]) * (WIRE_FAR + span_r[s]) || !r_sphere_visible(span_c[s], span_r[s])) continue;
        if(!hdr) {
            r_hdr(far ? h_wire_far : h_wire);
            hdr = true;
        }
        for(int k = far ? 1 : 0; k < (far ? 2 : 3); k++) {
            const Span *sp = &spans[s * 3 + k];
            V3 run = vsub(sp->b, sp->a);
            float sag = vlen(run) * WIRE_SAG;
            RV v[2 * (WIRE_SEG + 1)];
            for(int i = 0; i <= WIRE_SEG; i++) {
                float t = (float)i / WIRE_SEG;
                V3 p = vmad(sp->a, run, t);
                p.y -= sag * 4.0f * t * (1.0f - t);
                /* Across the wire and the line of sight. */
                V3 to = vsub(p, c);
                float dist = vlen(to);
                V3 side = vcross(run, to);
                float sl = vlen(side);
                float w = fmaxf(0.015f, dist * (far ? 0.0013f : 0.001f));
                side = sl > 1e-4f ? vscale(side, w / sl) : v3(0, w, 0);
                v[2 * i] = (RV){p.x + side.x, p.y + side.y, p.z + side.z, 0, 0, col};
                v[2 * i + 1] = (RV){p.x - side.x, p.y - side.y, p.z - side.z, 0, 0, col};
            }
            r_strip(v, 2 * (WIRE_SEG + 1));
        }
    }
}

void dunes_draw_op(void) {
    if(!on) return;
    whips_draw();
    if(nspan) wires_draw(false);
}

float dunes_wind(void) {
    return on ? 0.22f + 0.4f * clampf((gust - 0.35f) * (1.0f / 0.6f), 0, 1) : 0;
}

void dunes_draw_tr(void) {
    if(!on) return;
    if(nspan) wires_draw(true);
    V3 c = r_cam.pos;
    /* Sand in the sun: paler than the dune it left. */
    uint32_t col = world_lit(0xffd6ccb2, v3(0, 1, 0));
    bool hdr = false;
    for(int i = 0; i < WISPS; i++) {
        const Wisp *w = &wisps[i];
        if(w->life <= 0) continue;
        float t = w->age / w->life, size = w->size * (1.0f + 3.2f * t);
        if(!r_sphere_visible(w->p, size)) continue;
        float dist = vlen(vsub(w->p, c));
        float a = 0.42f * fminf(t * 6.0f, 1.0f) * (1.0f - t) * (1.0f - t) * clampf((dist - 7.0f) * (1.0f / 14.0f), 0, 1) *
                  (1.0f - 0.6f * pal->dark);
        if(a < 0.02f) continue;
        if(!hdr) {
            r_hdr(h_sand);
            hdr = true;
        }
        /* Drawn out along the wind: a streamer more than a puff. */
        r_billboard(w->p, size * 1.7f, size * 0.6f, 0.12f, 0, 0, 1, 1, color_alpha(col, a));
    }
}
