/* Ridgeline: small things in the air about the camera (air.h).

   Each kind lives in a fixed pool and only where it belongs: leaves fall
   from the larches and maples within reach, motes hang in the forest
   while the sun is up, fireflies come out over the grass after dusk, and
   snow blows about the top of the mountain. All of them are a few pixels
   on the screen, so they cost the PVR next to nothing; each kind goes in
   one strip a frame. Their randomness is their own, so a demo or a bench
   that seeds the game's generator plays out the same. */
#include "air.h"
#include "world.h"
#include "terrain.h"
#include "tod.h"

#define LEAVES 64
#define MOTES 24
#define FLIES 24
#define FLAKES 48
#define NEAR_TREES 24

typedef struct {
    V3 p, v;
    float age, life;
    float spin, tumble, rate, size;
    float gy;              /* the ground under it, looked up every eighth frame */
    uint8_t live, kind;    /* kind: the tree it fell from (larch, maple) */
    uint8_t landed;
} Bit;

static Bit leaves[LEAVES], motes[MOTES], flies[FLIES], flakes[FLAKES];
static const pvr_poly_hdr_t *h_leaf, *h_add, *h_snow;
static float air_t;
static unsigned air_frame;

/* Each bit looks the ground up on its own eighth frame: a lookup is a few
   cache misses into the level, and a bit moves centimetres in between. */
static inline float ground(Bit *b, int i) {
    if(!((i + air_frame) & 7)) b->gy = terrain_height(b->p.x, b->p.z);
    return b->gy;
}

static uint32_t seed = 0x51ed27u;
static inline float rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return (float)(seed >> 8 & 0xffff) * (1.0f / 65535.0f);
}
static inline float rnd_s(void) { return rnd() * 2.0f - 1.0f; }

/* The larches and maples near the camera, found a slice of the forest a
   frame: a sweep of the whole forest takes about a quarter of a second. */
static uint16_t decid[LVL_MAX_TREES];
static int ndecid;
static uint16_t near_t[NEAR_TREES], scan_t[NEAR_TREES];
static int nnear, nscan, scan_at, forest_n, scan_forest;
static float leaf_acc;
#define NEAR_LEAF 40.0f       /* trees that drop leaves within this */
#define SCAN_STEP 48

/* Trees that drop leaves: the mountain's larches and maples, the
   desert's cottonwoods (its junipers and pinyons keep theirs). */
static inline bool air_sheds(int kind) {
    return lvl_area == AREA_MOAB ? kind == 1 : kind == 1 || kind == 2;
}

void air_init(void) {
    h_leaf = r_header(TEX_NONE, HF_OP);
    h_add = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    h_snow = r_header(TEX_SUN, HF_TR | HF_NOFOG);   /* a flat disc, soft at its rim */
    ndecid = 0;
    for(int i = 0; i < LVL_TREES; i++)
        if(air_sheds(lvl_trees[i].kind)) decid[ndecid++] = (uint16_t)i;
    nnear = nscan = scan_at = forest_n = scan_forest = 0;
    for(int i = 0; i < LEAVES; i++) leaves[i].live = 0;
    for(int i = 0; i < MOTES; i++) motes[i].live = 0;
    for(int i = 0; i < FLIES; i++) flies[i].live = 0;
    for(int i = 0; i < FLAKES; i++) flakes[i].live = 0;
}

/* One slice of the sweep: the deciduous trees within reach, and how many
   trees of any kind stand close (motes need a forest round them). */
static void scan_trees(V3 c) {
    int to = scan_at + SCAN_STEP;
    if(to > LVL_TREES) to = LVL_TREES;
    for(int i = scan_at; i < to; i++) {
        const lvl_tree_t *t = &lvl_trees[i];
        float dx = t->x - c.x, dz = t->z - c.z, d2 = dx * dx + dz * dz;
        if(d2 < 22.0f * 22.0f) scan_forest++;
        if(air_sheds(t->kind) && d2 < NEAR_LEAF * NEAR_LEAF && nscan < NEAR_TREES) scan_t[nscan++] = (uint16_t)i;
    }
    scan_at = to;
    if(scan_at >= LVL_TREES) {
        memcpy(near_t, scan_t, sizeof(near_t));
        nnear = nscan;
        forest_n = scan_forest;
        nscan = scan_at = scan_forest = 0;
    }
}

static Bit *free_bit(Bit *pool, int n) {
    for(int i = 0; i < n; i++)
        if(!pool[i].live) return &pool[i];
    return NULL;
}

/* A point on the ground ahead of the camera, between near and far metres. */
static V3 ahead(V3 c, float near, float far, float spread) {
    V3 f = cam.fwd;
    float fl = sqrtf(f.x * f.x + f.z * f.z);
    float fx = fl > 0.01f ? f.x / fl : 0, fz = fl > 0.01f ? f.z / fl : 1;
    float d = near + (far - near) * rnd(), side = spread * rnd_s() * d;
    return v3(c.x + fx * d - fz * side, c.y, c.z + fz * d + fx * side);
}

/* ---- Leaves ---- */

static int n_leaves;

static void leaves_update(float dt, V3 c) {
    if(!nnear && !n_leaves) return;
    /* A leaf every few seconds from each tree in reach, more in a gust. */
    float gust = 0.6f + 0.4f * sinf(air_t * 0.37f) * sinf(air_t * 0.11f + 1.0f);
    leaf_acc += dt * nnear * 0.5f * gust;
    while(leaf_acc >= 1.0f && nnear) {
        leaf_acc -= 1.0f;
        Bit *b = free_bit(leaves, LEAVES);
        if(!b) {
            leaf_acc = 0;
            break;
        }
        /* Mostly from the trees in front of the camera. */
        const lvl_tree_t *t = &lvl_trees[near_t[(int)(rnd() * 0.999f * nnear)]];
        for(int k = 0; k < 3 && (t->x - c.x) * cam.fwd.x + (t->z - c.z) * cam.fwd.z < 0; k++)
            t = &lvl_trees[near_t[(int)(rnd() * 0.999f * nnear)]];
        float h = (t->kind == 2 ? 5.0f : 10.0f) * t->scale, r = (t->kind == 2 ? 2.0f : 1.8f) * t->scale;
        float a = rnd() * ENG_TAU, rr = r * (0.3f + 0.7f * rnd());
        b->p = v3(t->x + sinf(a) * rr, t->y + h * (0.35f + 0.45f * rnd()), t->z + cosf(a) * rr);
        b->v = v3(WIND_X * 1.2f, -0.4f, WIND_Z * 1.2f);
        b->age = 0;
        b->life = 30.0f;
        b->spin = rnd() * ENG_TAU;
        b->tumble = rnd() * ENG_TAU;
        b->rate = 2.0f + 3.0f * rnd();
        b->size = (t->kind == 2 ? 0.17f : 0.13f) * (0.8f + 0.4f * rnd());   /* chunky, as the trees are */
        b->kind = t->kind;
        b->gy = terrain_height(b->p.x, b->p.z);
        b->landed = 0;
        b->live = 1;
    }
    n_leaves = 0;
    for(int i = 0; i < LEAVES; i++) {
        Bit *b = &leaves[i];
        if(!b->live) continue;
        b->age += dt;
        float dx = b->p.x - c.x, dz = b->p.z - c.z;
        if(b->age > b->life || dx * dx + dz * dz > 55.0f * 55.0f) {
            b->live = 0;
            continue;
        }
        n_leaves++;
        if(b->landed) continue;
        /* Drifting downwind, falling at a leaf's pace, swinging side to
           side as it tumbles. */
        float sw = sinf(b->tumble);
        float wind = 1.4f * gust;
        b->v.x += (WIND_X * wind + 0.9f * sw * WIND_Z - b->v.x) * dt * 2.0f;
        b->v.z += (WIND_Z * wind - 0.9f * sw * WIND_X - b->v.z) * dt * 2.0f;
        b->v.y += (-0.75f + 0.35f * cosf(b->tumble * 2.0f) - b->v.y) * dt * 3.0f;
        b->p = vmad(b->p, b->v, dt);
        b->spin += b->rate * dt;
        b->tumble += b->rate * 0.6f * dt;
        float g = ground(b, i);
        if(b->p.y < g + 0.03f) {
            /* On the ground a while, then gone. */
            b->p.y = g + 0.03f;
            b->landed = 1;
            b->life = b->age + 4.0f + 3.0f * rnd();
        }
    }
}

/* ---- Motes, fireflies and snow ---- */

/* How many of each kind are alive: a kind with none and no call for any
   is skipped whole. */
static int n_motes, n_flies, n_flakes;

static void motes_update(float dt, V3 c, float want) {
    if(want <= 0 && !n_motes) return;
    n_motes = 0;
    for(int i = 0; i < MOTES; i++) {
        Bit *b = &motes[i];
        if(!b->live) {
            if(want <= 0 || rnd() > dt * 2.0f) continue;
            b->p = ahead(c, 3.0f, 14.0f, 0.7f);
            b->p.y = terrain_height(b->p.x, b->p.z) + 0.5f + 3.0f * rnd();
            b->v = v3(rnd_s() * 0.1f, rnd_s() * 0.05f, rnd_s() * 0.1f);
            b->age = 0;
            b->life = 6.0f + 6.0f * rnd();
            b->tumble = rnd() * ENG_TAU;
            b->size = 0.025f + 0.02f * rnd();
            b->live = 1;
            n_motes++;
            continue;
        }
        b->age += dt;
        if(b->age > b->life) {
            b->live = 0;
            continue;
        }
        b->tumble += dt * 0.7f;
        b->v.x += (0.12f * sinf(b->tumble) - b->v.x) * dt;
        b->v.y += (0.05f * cosf(b->tumble * 1.3f) - b->v.y) * dt;
        b->p = vmad(b->p, b->v, dt);
        n_motes++;
    }
}

static void flies_update(float dt, V3 c, float want) {
    if(want <= 0 && !n_flies) return;
    n_flies = 0;
    for(int i = 0; i < FLIES; i++) {
        Bit *b = &flies[i];
        if(!b->live) {
            if(want <= 0 || rnd() > dt * 1.5f) continue;
            V3 p = ahead(c, 5.0f, 32.0f, 0.9f);
            if(surface_at(p.x, p.z) != SURF_GRASS || water_at(p.x, p.z) > -999.0f) continue;
            b->gy = terrain_height(p.x, p.z);
            b->p = v3(p.x, b->gy + 0.4f + 1.8f * rnd(), p.z);
            b->v = v3(0, 0, 0);
            b->age = 0;
            b->life = 10.0f + 10.0f * rnd();
            b->tumble = rnd() * ENG_TAU;
            b->spin = rnd() * ENG_TAU;    /* when it glows */
            b->rate = 0.6f + 0.5f * rnd();
            b->live = 1;
            n_flies++;
            continue;
        }
        b->age += dt;
        if(b->age > b->life) {
            b->live = 0;
            continue;
        }
        /* A lazy wander, staying near the grass. */
        b->tumble += dt * 0.9f;
        float up = (ground(b, i) + 1.2f - b->p.y) * 0.5f;
        b->v.x += (0.5f * sinf(b->tumble) - b->v.x) * dt;
        b->v.z += (0.5f * cosf(b->tumble * 0.8f) - b->v.z) * dt;
        b->v.y += (up + 0.2f * sinf(b->tumble * 2.3f) - b->v.y) * dt;
        b->p = vmad(b->p, b->v, dt);
        b->spin += b->rate * dt;
        n_flies++;
    }
}

static void snow_update(float dt, V3 c, float want) {
    if(want <= 0 && !n_flakes) return;
    n_flakes = 0;
    for(int i = 0; i < FLAKES; i++) {
        Bit *b = &flakes[i];
        if(!b->live) {
            if(want <= 0 || rnd() > want * dt * 6.0f) continue;
            /* In view (the wind would carry flakes from further off past
               the screen before they were seen). */
            b->p = ahead(c, 2.0f, 14.0f, 0.6f);
            b->p.y = c.y - 1.5f + 5.0f * rnd();
            b->gy = terrain_height(b->p.x, b->p.z);
            b->v = v3(WIND_X * 3.0f, -1.1f, WIND_Z * 3.0f);
            b->age = 0;
            b->life = 3.0f + 2.0f * rnd();
            b->tumble = rnd() * ENG_TAU;
            b->size = 0.035f + 0.025f * rnd();
            b->live = 1;
            n_flakes++;
            continue;
        }
        b->age += dt;
        b->tumble += dt * 2.1f;
        b->p.x += (b->v.x + 0.6f * sinf(b->tumble)) * dt;
        b->p.z += (b->v.z + 0.6f * cosf(b->tumble * 0.7f)) * dt;
        b->p.y += b->v.y * dt;
        if(b->age > b->life || b->p.y < ground(b, i)) b->live = 0;
        else n_flakes++;
    }
}

void air_update(float dt) {
    if(game_frozen()) return;
    air_t += dt;
    air_frame++;
    V3 c = cam.pos;
    scan_trees(c);
    leaves_update(dt, c);
    /* Motes: in the forest, while the sun is up and clear of the cloud. */
    float sun = (pal->sun_k[0] + pal->sun_k[1] + pal->sun_k[2]) * (1.0f / 3.0f) * (1.0f - pal->dark);
    motes_update(dt, c, forest_n >= 6 && !tod.moonlit && world_mist() < 0.3f ? sun : 0);
    /* Fireflies: after dusk, low on the mountain. */
    flies_update(dt, c, look->fireflies && pal->dark > 0.45f && c.y < 60.0f ? 1.0f : 0);
    /* Snow: blowing about the top of the mountain. */
    snow_update(dt, c, look->snow ? clampf((c.y - 112.0f) * (1.0f / 20.0f), 0, 1) : 0);
}

/* ---- Drawing ---- */

#define BUF 256
static RV buf[BUF + 8];
static int nbuf;

static void flush(void) {
    if(nbuf) r_strip(buf, nbuf);
    nbuf = 0;
}

/* Four corners into the strip, joined on to what is there. */
static void quad(const RV *q) {
    if(nbuf + 6 > BUF) flush();
    if(nbuf) {
        buf[nbuf] = buf[nbuf - 1];
        buf[nbuf + 1] = q[0];
        nbuf += 2;
    }
    for(int k = 0; k < 4; k++) buf[nbuf++] = q[k];
}

static void sprite(V3 p, float s, uint32_t col) {
    V3 rx = vscale(r_cam.right, s), ry = vscale(r_cam.up, s);
    RV q[4] = {{p.x - rx.x + ry.x, p.y - rx.y + ry.y, p.z - rx.z + ry.z, 0, 0, col},
               {p.x + rx.x + ry.x, p.y + rx.y + ry.y, p.z + rx.z + ry.z, 1, 0, col},
               {p.x - rx.x - ry.x, p.y - rx.y - ry.y, p.z - rx.z - ry.z, 0, 1, col},
               {p.x + rx.x - ry.x, p.y + rx.y - ry.y, p.z + rx.z - ry.z, 1, 1, col}};
    quad(q);
}

/* In front of the camera between near and far metres, and on the screen
   roughly. */
static inline bool in_view(V3 p, float near, float far) {
    V3 d = vsub(p, r_cam.pos);
    float z = vdot(d, r_cam.fwd);
    return z > near && z < far && fabsf(vdot(d, r_cam.right)) < z * 0.8f + 1.0f;
}

void air_draw_op(void) {
    /* Each leaf a little diamond tumbling in the air: its two faces lit
       a shade apart, so the flutter shows as it turns. */
    uint32_t lit[2][2];
    static const uint32_t alb[2] = {0xffe4b23a, 0xffd8642e};
    for(int k = 0; k < 2; k++) {
        lit[k][0] = world_lit(alb[k], v3(0, 1, 0));
        lit[k][1] = color_scale(lit[k][0], 0.7f);
    }
    r_load_vp();   /* a model drawn before may have left its matrix loaded */
    r_hdr(h_leaf);
    for(int i = 0; i < LEAVES; i++) {
        const Bit *b = &leaves[i];
        if(!b->live || !in_view(b->p, 0.3f, 45.0f)) continue;
        float s = b->size;
        if(b->landed) s *= clampf((b->life - b->age) * 0.5f, 0, 1);
        float cs = cosf(b->spin), sn = sinf(b->spin), ct = cosf(b->tumble), st = sinf(b->tumble);
        V3 a = v3(cs * s * 1.4f, st * s * 0.6f, sn * s * 1.4f);
        float wk = (ct < 0 ? -0.35f : 0.35f) + 0.65f * ct;   /* never quite edge on */
        V3 w = v3(-sn * wk * s * 0.7f, (b->landed ? 0 : st * s * 0.7f), cs * wk * s * 0.7f);
        uint32_t col = lit[b->kind == 2][ct < 0];
        V3 p = b->p;
        RV q[4] = {{p.x + a.x, p.y + a.y, p.z + a.z, 0, 0, col}, {p.x + w.x, p.y + w.y, p.z + w.z, 0, 0, col},
                   {p.x - w.x, p.y - w.y, p.z - w.z, 0, 0, col}, {p.x - a.x, p.y - a.y, p.z - a.z, 0, 0, col}};
        quad(q);
    }
    flush();
}

void air_draw_tr(void) {
    /* Motes catch the light most against the sun. */
    float sunv = (pal->sun_k[0] + pal->sun_k[1] + pal->sun_k[2]) * (1.0f / 3.0f) * (1.0f - pal->dark);
    r_load_vp();
    r_hdr(h_add);
    for(int i = 0; i < MOTES; i++) {
        const Bit *b = &motes[i];
        if(!b->live || !in_view(b->p, 0.3f, 16.0f)) continue;
        float fade = clampf(b->age, 0, 1) * clampf(b->life - b->age, 0, 1);
        V3 d = vnorm(vsub(b->p, r_cam.pos));
        float back = clampf(vdot(d, sun_dir), 0, 1);
        float a = fade * sunv * (0.25f + 0.75f * back * back);
        if(a < 0.02f) continue;
        sprite(b->p, b->size, color_alpha(0xfffff0d0, a));
    }
    /* Fireflies: a slow pulse of yellow-green, a soft halo round it. */
    for(int i = 0; i < FLIES; i++) {
        const Bit *b = &flies[i];
        if(!b->live || !in_view(b->p, 0.3f, 40.0f)) continue;
        float fade = clampf(b->age * 0.5f, 0, 1) * clampf((b->life - b->age) * 0.5f, 0, 1);
        float pulse = sinf(b->spin);
        pulse = pulse > 0 ? pulse * pulse * pulse : 0;
        float a = fade * pulse * pal->dark;
        if(a < 0.02f) continue;
        sprite(b->p, 0.32f, color_alpha(0xffb8ff50, 0.45f * a));
        sprite(b->p, 0.07f, color_alpha(0xffeeffb0, a));
    }
    flush();
    /* Snow: lit by the sky, fogged by hand (the PVR's fog would fog the
       clear corners too). */
    r_hdr(h_snow);
    uint32_t white = world_lit(0xfff4f8ff, v3(0, 1, 0));
    for(int i = 0; i < FLAKES; i++) {
        const Bit *b = &flakes[i];
        if(!b->live || !in_view(b->p, 1.5f, 20.0f)) continue;
        float a = clampf(b->age * 2.0f, 0, 1) * clampf((b->life - b->age) * 2.0f, 0, 1);
        sprite(b->p, b->size, color_alpha(white, a));
    }
    flush();
}
