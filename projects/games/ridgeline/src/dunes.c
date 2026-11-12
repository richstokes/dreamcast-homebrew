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

   After dark the whips are lit, as they are out there: a strip of
   LEDs up each one, a colour to a rig, so a truck coming over a crest
   shows as a stick of light before its lamps do. The ones stood about
   the camp and on the parked quads are lit too, and the ones marking
   the places to find burn amber. Each is a soft additive ribbon round a
   hot core, held a pixel or two wide from far off.

   And the slides: sand lies on a slip face at the steepest angle it
   can, so a tyre crossing one sets it going. A sheet breaks away under
   the wheel and runs down the face as a tongue, spreading, slowing
   where the slope eases, and lies there paler than the face until the
   wind has smoothed it. Each is a ribbon laid on the ground down the
   line of steepest fall, its texture run along it while it moves.

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

/* The whips stood in the sand and on what is parked at the camp. */
#define FLAGS 40
#define LED_FAR 460.0f
typedef struct {
    V3 base, top;
    uint32_t led;
} Flag;
static Flag flags[FLAGS];
static int nflag;
static const uint32_t led_cols[6] = {0xff28d0ff, 0xffff40c0, 0xff58ff48, 0xff4868ff, 0xffff4428, 0xffffb428};
#define LED_AMBER 0xffff9a28

/* Sand sliding down a slip face. */
#define SLIDES 28
#define SL_PTS 9
#define SL_STEP 1.3f            /* metres between a slide's points */
#define SL_STEEP 0.42f          /* the slope (rise over run) a tyre can set going */
#define SL_FAR 95.0f
typedef struct {
    V3 p[SL_PTS], side[SL_PTS]; /* down its middle from where it broke away; across it there, level */
    V3 head, dir, nrm;          /* its front, the way it runs (flat), the face's normal */
    int n;
    float run, len, speed, width, flow, rest;
    bool live, moving;
} Slide;
static Slide slides[SLIDES];
static float slide_acc[MAX_TRUCKS], slide_wait[MAX_TRUCKS];
static int slide_wheel;

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
static const pvr_poly_hdr_t *h_wire, *h_wire_far, *h_sand, *h_led, *h_slide;
static bool on;

static uint32_t seed = 0x51d0beefu;
static inline float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8 & 0xffff) * (1.0f / 65535.0f);
}

bool dunes_whip_of(const lvl_prop_t *p, V3 *base, V3 *top) {
    float sy = sinf(p->yaw), cy = cosf(p->yaw), right, up, fwd, len;
    switch(p->kind) {
        case PROP_FLAG: right = 0, up = -0.2f, fwd = 0, len = 3.7f; break;       /* its foot in the sand */
        case PROP_QUAD: right = 0.42f, up = 0.7f, fwd = -0.92f, len = 2.7f; break;   /* a corner of the tail */
        case PROP_RAIL: right = -0.44f, up = 1.5f, fwd = -0.62f, len = 2.3f; break;  /* the back of the cage */
        default: return false;
    }
    *base = v3(p->x + cy * right + sy * fwd, p->y + up, p->z - sy * right + cy * fwd);
    *top = v3(base->x + WIND_X * 0.3f, base->y + len, base->z + WIND_Z * 0.3f);
    return true;
}

void dunes_init(void) {
    h_wire = r_header(TEX_NONE, HF_OP);
    h_wire_far = r_header(TEX_NONE, HF_TR);
    h_sand = r_header(TEX_PUFF, HF_TR);
    h_led = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    h_slide = r_header(TEX_SLIDE, HF_TR);
    on = lvl_area == AREA_DUNES;
    nspan = nbrink = nnear = nflag = 0;
    for(int i = 0; i < WISPS; i++) wisps[i].life = 0;
    for(int i = 0; i < MAX_TRUCKS; i++) whips[i].live = false;
    for(int i = 0; i < SLIDES; i++) slides[i].live = false;
    if(!on) return;
    /* The camp's whips are whatever colour their owners liked; out in
       the dunes a whip marks a place, and those are all amber. */
    for(int i = 0; i < LVL_PROPS && nflag < FLAGS; i++) {
        const lvl_prop_t *p = &lvl_props[i];
        Flag *f = &flags[nflag];
        if(!dunes_whip_of(p, &f->base, &f->top)) continue;
        float dx = p->x - lvl_spawns[0][0], dz = p->z - lvl_spawns[0][2];
        bool camp = p->kind != PROP_FLAG || dx * dx + dz * dz < 110.0f * 110.0f;
        f->led = camp ? led_cols[((((uint32_t)(p->x * 31.0f) ^ (uint32_t)(p->z * 17.0f)) * 2654435761u) >> 16) % 6] : LED_AMBER;
        nflag++;
    }
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
    printf("Ridgeline dunes: %d spans of wire, %d brinks, %d whips\n", nspan, nbrink, nflag);
}

/* Where a truck's whip is bolted on: the tail's left corner, half way up. */
static V3 whip_mount(const Truck *t) {
    const float *c = t->spec.hull_c, *h = t->spec.hull_h;
    return truck_to_world(t, v3(c[0] - h[0] * 0.7f, c[1] + h[1] * 0.2f, c[2] - h[2] * 0.88f));
}

/* A sheet of sand breaking away at a point on a slip face whose normal
   is n, shoved off at push metres a second. */
static void slide_start(V3 at, V3 n, float push, float width) {
    float hl = sqrtf(n.x * n.x + n.z * n.z);
    if(hl < 0.05f) return;
    Slide *s = NULL;
    for(int i = 0; i < SLIDES && !s; i++)
        if(!slides[i].live) s = &slides[i];
    if(!s) return;
    s->dir = v3(n.x / hl, 0, n.z / hl);
    s->nrm = n;
    s->p[0] = s->head = at;
    s->side[0] = v3(s->dir.z, 0, -s->dir.x);
    s->n = 1;
    s->run = s->len = s->flow = s->rest = 0;
    s->speed = 1.6f + push;
    s->width = width;
    s->live = s->moving = true;
}

static void slides_update(float dt, float blow) {
    /* What the trucks set going: a wheel of each is looked at a frame. */
    slide_wheel = (slide_wheel + 1) & 3;
    for(int i = 0; i < game.ntrucks && i < MAX_TRUCKS; i++) {
        const Truck *t = &trucks[i];
        slide_wait[i] -= dt;
        if(t->absent) continue;
        const Wheel *w = &t->wh[slide_wheel];
        if(!w->contact || (w->surf != SURF_SAND && w->surf != SURF_DEEPSAND)) continue;
        V3 dc = vsub(w->cp, cam.pos);
        if(vdot(dc, dc) > 80.0f * 80.0f) continue;
        V3 n;
        terrain_height_n(w->cp.x, w->cp.z, &n);
        float hl = sqrtf(n.x * n.x + n.z * n.z);
        if(n.y < 0.5f || hl < SL_STEEP * n.y) continue;
        /* By the ground the tyre has covered and what it has churned. */
        slide_acc[i] += (vlen(t->vel) + 0.6f * w->slip) * dt;
        if(slide_acc[i] < 0.7f || slide_wait[i] > 0) continue;
        slide_acc[i] = 0;
        slide_wait[i] = 0.42f;
        V3 down = v3(n.x / hl, 0, n.z / hl);
        float push = clampf(vdot(t->vel, down) * 0.7f, 0, 6.0f) + fminf(w->slip * 0.25f, 1.5f);
        V3 at = v3(w->cp.x + down.x * 0.35f, 0, w->cp.z + down.z * 0.35f);
        at.y = terrain_height(at.x, at.z);
        slide_start(at, n, push, (w->surf == SURF_DEEPSAND ? 1.25f : 1.0f) * (0.8f + 0.5f * rnd()));
    }
    for(int i = 0; i < SLIDES; i++) {
        Slide *s = &slides[i];
        if(!s->live) continue;
        if(!s->moving) {
            /* Lying where it stopped: the wind takes it, quicker in a gust. */
            s->rest += dt * (0.55f + 1.7f * blow);
            if(s->rest > 9.0f) s->live = false;
            continue;
        }
        V3 n;
        float gy = terrain_height_n(s->head.x, s->head.z, &n);
        float hl = sqrtf(n.x * n.x + n.z * n.z);
        if(hl > 0.05f) {
            /* It turns down the fall line as the face under it turns. */
            float k = fminf(dt * 4.0f, 1.0f);
            V3 d = v3(s->dir.x + (n.x / hl - s->dir.x) * k, 0, s->dir.z + (n.z / hl - s->dir.z) * k);
            float dl = sqrtf(d.x * d.x + d.z * d.z);
            if(dl > 0.01f) s->dir = v3(d.x / dl, 0, d.z / dl);
        }
        /* As fast as the slope will carry it: nothing under about 19
           degrees, four metres a second at the angle of repose. */
        float want = clampf((hl / fmaxf(n.y, 0.1f) - 0.34f) * 14.0f, 0, 4.2f);
        s->speed += (want - s->speed) * fminf(dt * 2.2f, 1.0f);
        float step = s->speed * dt;
        s->head = v3(s->head.x + s->dir.x * step, gy, s->head.z + s->dir.z * step);
        s->head.y = terrain_height(s->head.x, s->head.z);
        s->run += step;
        s->len += step;
        s->flow += step;
        if(s->run >= SL_STEP) {
            int sf = surface_at(s->head.x, s->head.z);
            if(s->n >= SL_PTS - 1 || (sf != SURF_SAND && sf != SURF_DEEPSAND) || water_at(s->head.x, s->head.z) > -999.0f)
                s->moving = false;
            else {
                s->p[s->n] = s->head;
                s->side[s->n] = v3(s->dir.z, 0, -s->dir.x);
                s->n++;
                s->run = 0;
            }
        }
        if(s->speed < 0.3f) s->moving = false;
    }
}

float dunes_wear(void) {
    return on ? 0.5f + 2.2f * clampf((gust - 0.62f) * (1.0f / 0.3f), 0, 1) : 1.0f;
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
    slides_update(dt, blow);
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

/* A truck's whip from its mount to its tip: WHIP_SEG + 1 points up a
   curve that leaves the mount straight. */
static void whip_curve(const Truck *t, const Whip *w, V3 *p) {
    V3 base = whip_mount(t), mid = vmad(base, t->ax[1], WHIP_LEN * 0.55f);
    for(int s = 0; s <= WHIP_SEG; s++) {
        float u = (float)s / WHIP_SEG, a = (1 - u) * (1 - u), b = 2 * u * (1 - u), d = u * u;
        p[s] = v3(base.x * a + mid.x * b + w->tip.x * d, base.y * a + mid.y * b + w->tip.y * d, base.z * a + mid.z * b + w->tip.z * d);
    }
}

/* A lit whip along n points: its glow, then its core. */
static void led_stick(const V3 *p, int n, uint32_t led, float dist, float k) {
    V3 c = r_cam.pos, axis = vsub(p[n - 1], p[0]);
    /* Drawn a hand's width nearer than the whip itself, which would
       otherwise show through its light in dashes. */
    float lift = 0.07f / fmaxf(dist, 0.5f);
    for(int pass = 0; pass < 2; pass++) {
        float hw = pass ? fmaxf(0.024f, dist * 0.0016f) : fmaxf(0.16f, dist * 0.0042f);
        uint32_t col = pass ? color_alpha(color_lerp(led, 0xffffffff, 0.6f), 0.95f * k) : color_alpha(led, 0.5f * k);
        RV v[2 * (WHIP_SEG + 1)];
        for(int s = 0; s < n; s++) {
            V3 to = vsub(p[s], c), q = vmad(p[s], to, -lift);
            V3 side = vcross(axis, to);
            float sl = vlen(side);
            side = sl > 1e-4f ? vscale(side, hw / sl) : v3(hw, 0, 0);
            /* (the core is the middle of the glow's texture: all colour) */
            v[2 * s] = (RV){q.x + side.x, q.y + side.y, q.z + side.z, pass ? 0.5f : 0.0f, 0.5f, col};
            v[2 * s + 1] = (RV){q.x - side.x, q.y - side.y, q.z - side.z, pass ? 0.5f : 1.0f, 0.5f, col};
        }
        r_strip(v, 2 * n);
    }
    /* The glow closed off round its tip. */
    float hs = fmaxf(0.2f, dist * 0.005f);
    V3 rx = vscale(r_cam.right, hs), ry = vscale(r_cam.up, hs), q = p[n - 1];
    uint32_t col = color_alpha(led, 0.45f * k);
    RV t[4] = {{q.x - rx.x + ry.x, q.y - rx.y + ry.y, q.z - rx.z + ry.z, 0, 0, col},
               {q.x + rx.x + ry.x, q.y + rx.y + ry.y, q.z + rx.z + ry.z, 1, 0, col},
               {q.x - rx.x - ry.x, q.y - rx.y - ry.y, q.z - rx.z - ry.z, 0, 1, col},
               {q.x + rx.x - ry.x, q.y + rx.y - ry.y, q.z + rx.z - ry.z, 1, 1, col}};
    r_strip(t, 4);
}

/* The whips' light, once it is dark enough to show. */
static void leds_draw(void) {
    /* Not through dusk: a glow too faint to see costs the PVR as much
       as a bright one. */
    float dark = clampf((pal->dark - 0.3f) * 2.0f, 0, 1);
    if(dark < 0.03f) return;
    V3 c = r_cam.pos;
    float fog_far = world_fog_far();
    r_hdr(h_led);
    for(int i = 0; i < game.ntrucks && i < MAX_TRUCKS; i++) {
        const Truck *t = &trucks[i];
        const Whip *w = &whips[i];
        if(!w->live || t->absent) continue;
        V3 p[WHIP_SEG + 1];
        whip_curve(t, w, p);
        float dist = vlen(vsub(p[0], c));
        if(dist > LED_FAR || !r_sphere_visible(p[WHIP_SEG / 2], WHIP_LEN + 3.5f)) continue;
        float k = dark * clampf(1.0f - dist / fog_far, 0, 1);
        if(k < 0.02f) continue;
        uint32_t led = led_cols[(unsigned)(t->model * 5 + t->livery * 3 + 1) % 6];
        led_stick(p, WHIP_SEG + 1, led, dist, k);
        if(dist > 45.0f) continue;
        /* Its colour on the sand round the truck's tail. */
        V3 n, g = v3(p[0].x, 0, p[0].z);
        g.y = terrain_height_n(g.x, g.z, &n) + 0.09f;
        V3 ta = vnorm(vcross(n, v3(0, 0, 1))), tb = vcross(n, ta);
        ta = vscale(ta, 2.5f);
        tb = vscale(tb, 2.5f);
        uint32_t col = color_alpha(led, 0.24f * k);
        RV q[4] = {{g.x - ta.x + tb.x, g.y - ta.y + tb.y, g.z - ta.z + tb.z, 0, 0, col},
                   {g.x + ta.x + tb.x, g.y + ta.y + tb.y, g.z + ta.z + tb.z, 1, 0, col},
                   {g.x - ta.x - tb.x, g.y - ta.y - tb.y, g.z - ta.z - tb.z, 0, 1, col},
                   {g.x + ta.x - tb.x, g.y + ta.y - tb.y, g.z + ta.z - tb.z, 1, 1, col}};
        r_strip(q, 4);
    }
    for(int i = 0; i < nflag; i++) {
        const Flag *f = &flags[i];
        V3 d = vsub(f->base, c);
        float d2 = vdot(d, d);
        if(d2 > LED_FAR * LED_FAR || !r_sphere_visible(f->top, 4.5f)) continue;
        float dist = sqrtf(d2), k = dark * clampf(1.0f - dist / fog_far, 0, 1);
        if(k < 0.02f) continue;
        V3 p[2] = {f->base, f->top};
        led_stick(p, 2, f->led, dist, k);
    }
}

/* The slides: each a ribbon from where it broke away to its front. */
static void slides_draw(void) {
    V3 c = r_cam.pos;
    bool hdr = false;
    for(int i = 0; i < SLIDES; i++) {
        const Slide *s = &slides[i];
        if(!s->live) continue;
        V3 mid = vscale(vadd(s->p[0], s->head), 0.5f);
        float dist = vlen(vsub(mid, c));
        if(dist > SL_FAR || !r_sphere_visible(mid, s->len * 0.5f + s->width + 0.5f)) continue;
        float fade = (1.0f - clampf((s->rest - 2.5f) * (1.0f / 6.5f), 0, 1)) * clampf((SL_FAR - dist) * (1.0f / 30.0f), 0, 1);
        if(fade < 0.02f) continue;
        if(!hdr) {
            r_hdr(h_slide);
            hdr = true;
        }
        /* In the face's own colour as it is lit now: darker where it
           broke away and left the damp sand under the crust showing,
           paler where the loose dry sand has piled up at its front. */
        uint32_t face = lvl_c((int)((mid.x - LEVEL_ORG) / LEVEL_D + 0.5f), (int)((mid.z - LEVEL_ORG) / LEVEL_D + 0.5f)) | 0xff000000u;
        uint32_t damp = color_scale(face, 0.7f), pale = color_lerp(face, 0xffffffff, 0.2f);
        float ilen = 1.0f / fmaxf(s->len, 1.0f);
        RV v[2 * (SL_PTS + 1)];
        int n = 0;
        bool tip = s->run > 0.08f || s->n < 2;
        for(int k = 0; k < s->n + (tip ? 1 : 0); k++) {
            bool head = k == s->n;
            V3 p = head ? s->head : s->p[k];
            V3 side = head ? v3(s->dir.z, 0, -s->dir.x) : s->side[k];
            float at = head ? s->len : k * SL_STEP, ahead = s->len - at;
            /* A tongue: narrow where it broke away, widest behind its
               rounded front. */
            float w = s->width * (0.3f + 0.7f * clampf(at * (1.0f / 2.6f), 0, 1)) * sqrtf(clampf(ahead * (1.0f / 1.5f), 0.02f, 1.0f));
            float a = 0.72f * fade * clampf(at * (1.0f / 1.2f), 0, 1) * (0.45f + 0.55f * clampf(ahead * (1.0f / 0.8f), 0, 1));
            float tv = (at - s->flow) * 0.45f;
            float toe = at * ilen;
            uint32_t cc = color_alpha(color_lerp(damp, pale, toe * toe * (3.0f - 2.0f * toe)), a);
            v[n++] = (RV){p.x - side.x * w, p.y + 0.05f, p.z - side.z * w, 0, tv, cc};
            v[n++] = (RV){p.x + side.x * w, p.y + 0.05f, p.z + side.z * w, 1, tv, cc};
        }
        if(n >= 4) r_strip(v, n);
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
    slides_draw();
    leds_draw();
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
