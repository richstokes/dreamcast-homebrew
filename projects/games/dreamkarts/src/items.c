/* DreamKarts power-ups: see items.h. */
#include "items.h"
#include "fx.h"
#include "hud.h"
#include "models.h"
#include "render.h"
#include "texture_assets.h"
#include "world.h"

/* A taken emerald bursts and pops straight back in a new colour; while it
   is away the next kart through still gets one, so a whole pack passing a
   band in the same second each gets a go. The limits are the bands
   themselves and the one slot you have to drive through. */
#define EMERALD_RESPAWN   0.3f
#define EMERALD_HOVER     0.85f   /* gem centre above the road */
#define EMERALD_SCALE     2.28f   /* about 1.1 m tall */
#define HELD_SCALE        0.62f
#define PICK_ALONG        0.9f    /* metres either side of the band */
#define PICK_LATERAL      1.45f
#define PICK_HEIGHT       1.3f    /* a kart flying over does not take it */
#define SNEAKERS_MIN      2.1f    /* boost seconds in 1st place... */
#define SNEAKERS_MAX      3.1f    /* ...and in 6th */
#define SPEAR_SPEED_MUL   1.6f    /* times the class top speed */
#define SPEAR_RANGE       60.0f   /* lock-on range */
#define SPEAR_LIFE        4.0f
#define SPEAR_TURN        8.0f    /* lateral m/s it can follow a swerve at */
#define SPEAR_HIT_ALONG   1.3f
#define SPEAR_HIT_LATERAL 1.4f
#define SPEAR_HIT_HEIGHT  1.8f
#define RAISE_LEN         0.45f   /* the Chao holds the gem up on use */
/* CPU rivals leave the leading human alone on their final lap. */
#define AI_SPARE_LEADER_FINAL_LAP 1
/* Serial log of pickups, throws and hits (DK ITEM): autotests and the demo
   recording build, never on hardware. */
#if (defined(DK_AUTOTEST) || defined(DK_RECORD)) && !defined(DK_HW)
#define ITEMS_LOG 1
#else
#define ITEMS_LOG 0
#endif

typedef struct {
    int lit;             /* slot holding the emerald this race */
    int item;            /* its colour */
    float taken_t;       /* seconds until it is back (0 = there) */
    float age;           /* seconds since it appeared (pop-in) */
} Emerald;

typedef struct {
    bool active;
    bool remote;         /* run from a rival's packets; hits judged here only on local karts */
    int owner, target;   /* kart indices, target -1 none */
    int seq, dir;        /* 1-7 and +1 forward / -1 back */
    float s, d, life, speed;
    V3 pos;
} Spear;

static Emerald emeralds[MAX_ITEM_BANDS];
static Spear spears[MAX_KARTS];
static int remote_seq[MAX_KARTS];     /* a rival's last spear sequence, to spawn each once */
static bool remote_spun[MAX_KARTS], remote_shield[MAX_KARTS];
static float last_s[MAX_KARTS];
static uint32_t rng_state;
static const pvr_poly_hdr_t *h_back, *h_front, *h_glow, *h_star, *h_flat2d, *h_pool;
int items_got, items_used, items_fired, items_hits, items_spins;

/* Slot lateral positions as a fraction of the road's half width. */
static const float slot_frac[ITEM_SLOTS] = {-0.72f, -0.24f, 0.24f, 0.72f};

/* ------------------------------------------------------------------ */
/* The gem: a hexagonal bipyramid with a flat table, 24 flat-shaded facets
   (each with its own three vertices). Built once in model space. */

#define GEM_FACETS 24
#define GEM_VERTS (GEM_FACETS * 3)
static RV gem_v[GEM_VERTS] __attribute__((aligned(32)));
static float gem_n[GEM_FACETS][3];
static uint16_t gem_idx[GEM_VERTS], gem_idx_back[GEM_VERTS];

static void gem_facet(int f, V3 a, V3 b, V3 c) {
    /* Outward normal: flip the winding if it points at the centre. */
    V3 n = vnorm(vcross(vsub(b, a), vsub(c, a)));
    V3 mid = vscale(vadd(vadd(a, b), c), 1.0f / 3.0f);
    if(vdot(n, mid) < 0) {
        V3 t = b;
        b = c;
        c = t;
        n = vscale(n, -1.0f);
    }
    gem_n[f][0] = n.x;
    gem_n[f][1] = n.y;
    gem_n[f][2] = n.z;
    RV *v = &gem_v[f * 3];
    v[0] = (RV){a.x, a.y, a.z, 0, 0, 0xffffffff};
    v[1] = (RV){b.x, b.y, b.z, 0, 0, 0xffffffff};
    v[2] = (RV){c.x, c.y, c.z, 0, 0, 0xffffffff};
    gem_idx[f * 3] = (uint16_t)(f * 3);
    gem_idx[f * 3 + 1] = (uint16_t)(f * 3 + 1);
    gem_idx[f * 3 + 2] = (uint16_t)(f * 3 + 2);
    gem_idx_back[f * 3] = (uint16_t)(f * 3);
    gem_idx_back[f * 3 + 1] = (uint16_t)(f * 3 + 2);
    gem_idx_back[f * 3 + 2] = (uint16_t)(f * 3 + 1);
}

static void gem_build(void) {
    const float tr = 0.11f, ty = 0.17f, gr = 0.22f, gy = 0.0f, cy = -0.31f;
    V3 t[7], g[7];
    for(int i = 0; i <= 6; i++) {
        float a = (float)i * (DK_TAU / 6.0f);
        t[i] = v3(sinf(a) * tr, ty, cosf(a) * tr);
        g[i] = v3(sinf(a + DK_PI / 6.0f) * gr, gy, cosf(a + DK_PI / 6.0f) * gr);
    }
    V3 top = v3(0, ty, 0), culet = v3(0, cy, 0);
    int f = 0;
    for(int i = 0; i < 6; i++) gem_facet(f++, top, t[i], t[i + 1]);                 /* table */
    for(int i = 0; i < 6; i++) {                                                       /* crown */
        gem_facet(f++, t[i], g[i], t[i + 1]);
        gem_facet(f++, t[i + 1], g[i], g[i + 1]);
    }
    for(int i = 0; i < 6; i++) gem_facet(f++, g[i], culet, g[i + 1]);                /* pavilion */
}

uint32_t items_colour(int item) {
    switch(item) {
        case ITEM_SNEAKERS: return argb(255, 60, 150, 255);
        case ITEM_SPEAR: return argb(255, 255, 60, 70);
        case ITEM_SHIELD: return argb(255, 70, 240, 120);
        default: return argb(255, 230, 230, 230);
    }
}

const char *items_name(int item) {
    switch(item) {
        case ITEM_SNEAKERS: return "POWER SNEAKERS";
        case ITEM_SPEAR: return "CHAOS SPEAR";
        case ITEM_SHIELD: return "BARRIER";
        default: return "";
    }
}

/* Draw one gem at p, turned yaw, size sc: the far facets glow through
   (additive, no depth write), then the near ones over them (alpha, depth
   written so the particles sort against it), then a sparkle. */
static void gem_draw(V3 p, float yaw, float sc, int item, float alpha) {
    float m[16];
    m4_translate_rotate(m, p, yaw, 0, 0);
    m4_scale_apply(m, sc, sc, sc);
    uint32_t base = items_colour(item);
    float br = (float)((base >> 16) & 255), bg = (float)((base >> 8) & 255), bb = (float)(base & 255);
    float cy = cosf(yaw), sy = sinf(yaw);
    V3 L = world_sun_dir;
    V3 V = vnorm(vsub(r_cam.pos, p));
    V3 H = vnorm(vadd(L, V));
    float night = world_night ? 1.0f : 0.0f;
    static uint32_t front[GEM_FACETS], back[GEM_FACETS];
    for(int f = 0; f < GEM_FACETS; f++) {
        const float *n0 = gem_n[f];
        V3 n = v3(n0[0] * cy + n0[2] * sy, n0[1], -n0[0] * sy + n0[2] * cy);
        float diff = fmaxf(vdot(n, L), 0.0f);
        float sp = fmaxf(vdot(n, H), 0.0f);
        sp *= sp; sp *= sp; sp *= sp; sp *= sp;          /* ^16 */
        float lit = 0.30f + 0.75f * diff + 0.25f * night;
        float r = br * lit + 255.0f * sp, g = bg * lit + 255.0f * sp, b = bb * lit + 255.0f * sp;
        front[f] = argb((int)(alpha * (165.0f + 40.0f * night)), r > 255 ? 255 : (int)r, g > 255 ? 255 : (int)g,
                        b > 255 ? 255 : (int)b);
        float bl = 0.45f + 0.25f * night + 0.30f * (1.0f - diff);
        back[f] = argb((int)(alpha * 190.0f), (int)fminf(br * bl, 255), (int)fminf(bg * bl, 255), (int)fminf(bb * bl, 255));
    }
    r_load_model(m);
    for(int f = 0; f < GEM_FACETS; f++) gem_v[f * 3].c = gem_v[f * 3 + 1].c = gem_v[f * 3 + 2].c = back[f];
    r_hdr(h_back);
    r_mesh(gem_v, GEM_VERTS, gem_idx_back, GEM_VERTS, true);
    for(int f = 0; f < GEM_FACETS; f++) gem_v[f * 3].c = gem_v[f * 3 + 1].c = gem_v[f * 3 + 2].c = front[f];
    r_hdr(h_front);
    r_mesh(gem_v, GEM_VERTS, gem_idx, GEM_VERTS, true);
    r_load_vp();
    /* A sparkle that wanders round the top edge as the gem turns. */
    float tw = 0.5f + 0.5f * sinf(world_time * 9.0f + yaw * 2.0f);
    V3 sp = vadd(p, v3(sinf(yaw * 3.0f) * 0.12f * sc, 0.14f * sc, cosf(yaw * 3.0f) * 0.12f * sc));
    r_hdr(h_star);
    r_billboard(sp, 0.16f * sc * (0.6f + tw), 0.16f * sc * (0.6f + tw), world_time * 2.0f, 0, 0, 1, 1,
                color_alpha(0xffffffff, alpha * (0.35f + 0.65f * tw)));
    r_hdr(h_glow);
    r_billboard(p, 0.42f * sc, 0.42f * sc, 0, 0, 0, 1, 1, color_alpha(base, alpha * (0.30f + 0.15f * night)));
}

/* ------------------------------------------------------------------ */

void items_init(void) {
    gem_build();
    h_back = r_header(TEX_NONE, HF_TR | HF_ADD);
    h_front = r_header(TEX_NONE, HF_TR | HF_ZWRITE);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD);
    h_pool = r_header(TEX_GLOW, HF_TR | HF_ADD | HF_NOFOG);
    h_star = r_header(TEX_STAR, HF_TR | HF_ADD);
    h_flat2d = r_header(TEX_NONE, HF_TR | HF_NOFOG | HF_NOZTEST);
}

static uint32_t irand(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

/* Blue is common, red less so, green rare. */
static int roll_item(void) {
    uint32_t r = irand() % 100;
    return r < 45 ? ITEM_SNEAKERS : r < 80 ? ITEM_SPEAR : ITEM_SHIELD;
}

float items_slot_d(int band, int slot) {
    float s = track.bands[band].s;
    int i = (int)(s / track.ds) % TRACK_SAMPLES;
    return slot_frac[slot & 3] * track.smp[i].half;
}

bool items_emerald(int band, V3 *pos, int *item) {
    const Emerald *e = &emeralds[band];
    if(band < 0 || band >= track.nbands || e->taken_t > 0) return false;
    float s = track.bands[band].s, d = items_slot_d(band, e->lit);
    V3 p = track_point(s, d);
    p.y = track_road_y(s, d) + EMERALD_HOVER + 0.06f * sinf(world_time * 2.4f + (float)band);
    if(pos) *pos = p;
    if(item) *item = e->item;
    return true;
}

void items_reset(uint32_t seed) {
    rng_state = seed * 2654435761u + 0x9e3779b9u;
    for(int b = 0; b < MAX_ITEM_BANDS; b++) {
        emeralds[b].lit = (int)(irand() % ITEM_SLOTS);
        emeralds[b].item = roll_item();
        emeralds[b].taken_t = 0;
        emeralds[b].age = 10.0f;
    }
    memset(spears, 0, sizeof(spears));
    memset(remote_seq, 0, sizeof(remote_seq));
    memset(remote_spun, 0, sizeof(remote_spun));
    memset(remote_shield, 0, sizeof(remote_shield));
    for(int i = 0; i < MAX_KARTS; i++) {
        last_s[i] = karts[i].tp.s;
        karts[i].item = ITEM_NONE;
        karts[i].item_t = 0;
        karts[i].spin_t = 0;
        karts[i].shield_t = 0;
        karts[i].raise_t = 0;
        karts[i].spear_seq = 0;
    }
    items_got = items_used = items_fired = items_hits = items_spins = 0;
#if ITEMS_LOG
    if(game.items)
        for(int b = 0; b < track.nbands; b++)
            printf("DK ITEM band %d at %.0f m (%.0f m to the line), slot %d d %.1f: %s\n", b,
                   (double)track.bands[b].s, (double)(track.length - track.bands[b].s), emeralds[b].lit,
                   (double)items_slot_d(b, emeralds[b].lit), items_name(emeralds[b].item));
#endif
}

/* ------------------------------------------------------------------ */
/* Spears                                                               */

static float spear_y(float s, float d) {
    if(track.has_jump && track_in_gap(s)) return track.jump.lip_h + 0.55f;
    return track_road_y(s, d) + 0.55f;
}

/* The kart dir (+1 ahead / -1 behind) of k within range on the same
   stretch of road, nearest first, or -1. */
static int pick_target(const Kart *k, int dir, float range) {
    int best = -1;
    float best_d = range;
    for(int j = 0; j < kart_count; j++) {
        const Kart *o = &karts[j];
        if(o == k || o->finished) continue;
        float ahead = track_delta(k->tp.s, o->tp.s) * (float)dir;
        if(ahead <= 1.0f || ahead >= best_d) continue;
        if(fabsf(o->pos.y - k->pos.y) > 6.0f) continue;   /* the other leg of a crossing */
        best_d = ahead;
        best = j;
    }
    return best;
}

static void spear_launch(Kart *k, int dir, int seq, int target, float s, float d, bool remote) {
    int idx = (int)(k - karts);
    Spear *sp = &spears[idx];
    sp->active = true;
    sp->remote = remote;
    sp->owner = idx;
    sp->target = target;
    sp->seq = seq;
    sp->dir = dir;
    sp->s = track_wrap(s);
    sp->d = d;
    sp->life = SPEAR_LIFE;
    sp->speed = cc_params[game.cc].top_speed * SPEAR_SPEED_MUL;
    sp->pos = track_point(sp->s, sp->d);
    sp->pos.y = spear_y(sp->s, sp->d);
    items_fired++;
#if ITEMS_LOG
    printf("DK ITEM kart %d fires a spear %s at %s (seq %d%s)\n", idx, dir > 0 ? "ahead" : "behind",
           target >= 0 ? karts[target].name : "nobody", seq, remote ? ", from the network" : "");
#endif
}

static void spear_end(Spear *sp, bool burst) {
    if(burst) fx_burst(sp->pos, 14, argb(255, 255, 120, 60), 5.0f);
    sp->active = false;
}

/* A spear reaches a kart this console drives: it spins out, or its
   barrier pops. (A rival's fate is its own console's call: a spear flies
   through the rival drawn here, and ends when that console's packets say
   it spun out or lost its barrier.) */
static void spear_hit(Spear *sp, Kart *o) {
    int oi = (int)(o - karts);
    (void)oi;
    items_hits++;
    if(o->shield_t > 0) {
        o->shield_t = 0;
        o->events |= EV_SHIELD_POP;
        fx_burst(vadd(o->pos, v3(0, 0.8f, 0)), 18, items_colour(ITEM_SHIELD), 6.0f);
#if ITEMS_LOG
        printf("DK ITEM spear from %d pops kart %d's barrier\n", sp->owner, oi);
#endif
        spear_end(sp, true);
        return;
    }
    kart_spin_out(o, sp->dir > 0 ? 1 : -1);
    items_spins++;
#if ITEMS_LOG
    printf("DK ITEM spear from %d spins kart %d (%s) at %.1f s\n", sp->owner, oi, o->name, (double)game.race_time);
#endif
    spear_end(sp, true);
}

static void spear_step(Spear *sp, float dt) {
    float move = sp->speed * dt * (float)sp->dir;
    float s0 = sp->s;
    sp->s = track_wrap(sp->s + move);
    /* Home on the target's lateral offset, no faster than it can turn. */
    if(sp->target >= 0 && sp->target < kart_count) {
        const Kart *t = &karts[sp->target];
        float ahead = track_delta(sp->s, t->tp.s) * (float)sp->dir;
        if(ahead > -3.0f && ahead < SPEAR_RANGE + 20.0f && !t->finished) sp->d = approach(sp->d, t->tp.d, SPEAR_TURN * dt);
        else sp->target = -1;
    }
    int si = (int)(sp->s / track.ds) % TRACK_SAMPLES;
    float lim = track.smp[si].half + 0.5f;
    sp->d = clampf(sp->d, -lim, lim);
    sp->pos = track_point(sp->s, sp->d);
    sp->pos.y = spear_y(sp->s, sp->d);
    sp->life -= dt;
    if(sp->life <= 0) {
        spear_end(sp, true);
        return;
    }
    V3 back = vscale(vnorm(vsub(sp->pos, track_point(s0, sp->d))), -1.0f);
    fx_trail(sp->pos, vmad(vscale(back, 6.0f), v3(frand_s(), 1.0f + frand(), frand_s()), 2.5f),
             argb(255, 255, 150, 60), dt);
    /* Hits: anyone but the thrower, within the band it swept this step. */
    for(int j = 0; j < kart_count; j++) {
        Kart *o = &karts[j];
        if(j == sp->owner || o->remote) continue;
        float along = track_delta(sp->s, o->tp.s);
        float swept = fabsf(move) + SPEAR_HIT_ALONG;
        bool in_s = sp->dir > 0 ? (along > -swept && along < SPEAR_HIT_ALONG) : (along < swept && along > -SPEAR_HIT_ALONG);
        if(!in_s || fabsf(sp->d - o->tp.d) > SPEAR_HIT_LATERAL || fabsf(o->pos.y - sp->pos.y) > SPEAR_HIT_HEIGHT) continue;
        spear_hit(sp, o);
        return;
    }
}

float items_threat(const Kart *k) {
    int idx = (int)(k - karts);
    float best = 1e9f;
    for(int i = 0; i < MAX_KARTS; i++) {
        const Spear *sp = &spears[i];
        if(!sp->active || sp->owner == idx) continue;
        float ahead = track_delta(sp->s, k->tp.s) * (float)sp->dir;   /* kart ahead of the spear */
        if(ahead < -2.0f || ahead > SPEAR_RANGE + 20.0f) continue;
        if(sp->target != idx && fabsf(sp->d - k->tp.d) > 3.0f) continue;
        if(ahead < best) best = ahead;
    }
    return best;
}

/* ------------------------------------------------------------------ */

void items_update(float dt) {
    if(!game.items) return;
    for(int b = 0; b < track.nbands; b++) {
        Emerald *e = &emeralds[b];
        e->age += dt;
        if(e->taken_t > 0) {
            e->taken_t -= dt;
            if(e->taken_t <= 0) {
                e->taken_t = 0;
                e->age = 0;
            }
        }
    }
    for(int i = 0; i < MAX_KARTS; i++)
        if(spears[i].active) spear_step(&spears[i], dt);
}

static void take_emerald(int band, Kart *k, bool give) {
    Emerald *e = &emeralds[band];
    V3 p;
    items_emerald(band, &p, NULL);
    fx_burst(p, 12, items_colour(e->item), 4.5f);
    if(give) {
        k->item = e->item;
        k->item_t = 0;
        k->ai_item_delay = 0.35f + frand() * 0.7f;
        k->events |= EV_ITEM_GET;
        items_got++;
#if ITEMS_LOG
        printf("DK ITEM kart %d (%s) takes %s at band %d, %.1f s\n", (int)(k - karts), k->name, items_name(e->item), band,
               (double)game.race_time);
#endif
    }
    /* The next one through finds a new colour. */
    e->item = roll_item();
    e->taken_t = EMERALD_RESPAWN;
}

/* Did k cross band b's emerald slot this step? */
static bool crossed(const Kart *k, int b, float prev_s) {
    const Emerald *e = &emeralds[b];
    float bs = track.bands[b].s;
    float now = track_delta(bs, k->tp.s), before = track_delta(bs, prev_s);
    bool over = fabsf(now) < PICK_ALONG || (before < -PICK_ALONG && now >= -PICK_ALONG && now < 12.0f) ||
                (before > PICK_ALONG && now <= PICK_ALONG && now > -12.0f);
    if(!over) return false;
    bool beside = fabsf(k->tp.d - items_slot_d(b, e->lit)) <= PICK_LATERAL;
    bool low = fabsf(k->pos.y - track_road_y(k->tp.s, k->tp.d)) < PICK_HEIGHT;
#if ITEMS_LOG
    if((!beside || !low) && before < -PICK_ALONG && k->item == ITEM_NONE)
        printf("DK ITEM kart %d (%s) misses band %d: d %.1f, slot d %.1f%s\n", (int)(k - karts), k->name, b,
               (double)k->tp.d, (double)items_slot_d(b, e->lit), low ? "" : ", in the air");
#endif
    return beside && low;
}

static void use_item(Kart *k, bool back) {
    int idx = (int)(k - karts);
    int item = k->item;
    k->item = ITEM_NONE;
    k->raise_t = RAISE_LEN;
    k->events |= EV_ITEM_USE;
    items_used++;
    switch(item) {
        case ITEM_SNEAKERS: {
            float f = clampf((float)(k->place - 1) / 5.0f, 0, 1);
            kart_start_boost(k, lerpf(SNEAKERS_MIN, SNEAKERS_MAX, f), 1.12f);
            k->boost_stage = 3;
            k->events |= EV_BOOST;
            break;
        }
        case ITEM_SPEAR: {
            int dir = back ? -1 : 1;
            k->spear_seq = k->spear_seq % 7 + 1;
            spear_launch(k, dir, k->spear_seq, pick_target(k, dir, SPEAR_RANGE), k->tp.s + (float)dir * 2.5f, k->tp.d,
                         false);
            k->events |= EV_SPEAR;
            break;
        }
        case ITEM_SHIELD:
            k->shield_t = SHIELD_LEN;
            k->events |= EV_SHIELD_UP;
            break;
        default:
            break;
    }
    (void)idx;
}

void items_kart(Kart *k, const KartInput *in, float dt) {
    int idx = (int)(k - karts);
    float prev_s = last_s[idx];
    last_s[idx] = k->tp.s;
    if(!game.items) return;
    if(k->shield_t > 0) k->shield_t = fmaxf(k->shield_t - dt, 0);
    if(k->raise_t > 0) k->raise_t = fmaxf(k->raise_t - dt, 0);
    if(k->item != ITEM_NONE) k->item_t += dt;
    for(int b = 0; b < track.nbands; b++)
        if(k->item == ITEM_NONE && crossed(k, b, prev_s)) take_emerald(b, k, true);
    if(in->item && k->item != ITEM_NONE && k->spin_t <= 0) use_item(k, in->item_back);
}

/* ------------------------------------------------------------------ */
/* Online                                                               */

void items_pack(const Kart *k, NetItems *out) {
    int idx = (int)(k - karts);
    memset(out, 0, sizeof(*out));
    out->item = k->item;
    out->shield = k->shield_t > 0;
    out->spun = k->spin_t > 0;
    out->spear_target = -1;
    const Spear *sp = &spears[idx];
    if(sp->active && !sp->remote) {
        out->spear_seq = sp->seq;
        out->spear_back = sp->dir < 0;
        out->spear_s = sp->s;
        out->spear_d = sp->d;
        out->spear_target = sp->target >= 0 ? karts[sp->target].net_slot : -1;
    }
}

/* A rival's packets say it spun out or lost its barrier: whichever spear
   was on it here ends in a burst. */
static void remote_struck(const Kart *k) {
    int idx = (int)(k - karts);
    for(int i = 0; i < MAX_KARTS; i++) {
        Spear *sp = &spears[i];
        if(!sp->active || sp->owner == idx) continue;
        if(sp->target != idx && fabsf(track_delta(sp->s, k->tp.s)) > 12.0f) continue;
        sp->pos = vadd(k->pos, v3(0, 0.6f, 0));
        items_hits++;
#if ITEMS_LOG
        printf("DK ITEM spear from %d struck rival %d, by its console\n", sp->owner, idx);
#endif
        spear_end(sp, true);
    }
}

void items_remote(Kart *k, const NetItems *in, float lead, float dt) {
    int idx = (int)(k - karts);
    float prev_s = last_s[idx];
    last_s[idx] = k->tp.s;
    if(!game.items) return;
#ifdef DK_AUTOTEST
    {
        static NetItems last[MAX_KARTS];
        if(memcmp(&last[idx], in, sizeof(*in)) != 0) {
            last[idx] = *in;
            printf("DK ONLINE: slot %d items: held %d shield %d spun %d spear %d (back %d, %.0f m, d %.1f, target %d)\n",
                   k->net_slot, in->item, in->shield, in->spun, in->spear_seq, in->spear_back, (double)in->spear_s,
                   (double)in->spear_d, in->spear_target);
        }
    }
#endif
    k->item = in->item;
    if(in->spun && !remote_spun[idx]) {
        kart_spin_out(k, 1);
        remote_struck(k);
    }
    remote_spun[idx] = in->spun;
    if(!in->shield && remote_shield[idx]) {
        k->events |= EV_SHIELD_POP;
        fx_burst(vadd(k->pos, v3(0, 0.8f, 0)), 18, items_colour(ITEM_SHIELD), 6.0f);
        remote_struck(k);
    }
    else if(in->shield && !remote_shield[idx]) {
        k->events |= EV_SHIELD_UP;
    }
    remote_shield[idx] = in->shield;
    k->shield_t = in->shield ? fmaxf(k->shield_t, 0.3f) : 0;
    if(k->raise_t > 0) k->raise_t = fmaxf(k->raise_t - dt, 0);
    /* The emerald bursts here as it drives through the slot; its packet
       says what it got. */
    for(int b = 0; b < track.nbands; b++)
        if(emeralds[b].taken_t <= 0 && crossed(k, b, prev_s)) take_emerald(b, k, false);
    if(in->spear_seq != 0 && in->spear_seq != remote_seq[idx]) {
        int target = -1;
        for(int j = 0; j < kart_count; j++)
            if(karts[j].net_slot == in->spear_target && (karts[j].remote || j == game.player)) target = j;
        if(!game.online) target = -1;
        int dir = in->spear_back ? -1 : 1;
        float moved = cc_params[game.cc].top_speed * SPEAR_SPEED_MUL * fmaxf(lead, 0) * (float)dir;
        spear_launch(k, dir, in->spear_seq, target, in->spear_s + moved, in->spear_d, true);
        k->raise_t = RAISE_LEN;
        k->events |= EV_SPEAR;
#ifdef DK_AUTOTEST
        printf("DK ONLINE: slot %d spear %d at %.0f m targets slot %d\n", k->net_slot, in->spear_seq, (double)in->spear_s,
               in->spear_target);
#endif
    }
    remote_seq[idx] = in->spear_seq;
}

/* ------------------------------------------------------------------ */
/* CPU drivers                                                          */

float items_ai_lane(const Kart *k, float s, float d) {
    if(!game.items || k->item != ITEM_NONE || k->drift == DRIFT_ACTIVE) return d;
    for(int b = 0; b < track.nbands; b++) {
        float ahead = track_delta(k->tp.s, track.bands[b].s);
        if(ahead < 4.0f || ahead > 75.0f) continue;
        float t_left = ahead / fmaxf(fabsf(k->speed), 5.0f);
        float sd = items_slot_d(b, emeralds[b].lit);
        /* Only if the kart can still get across to it in time. */
        if(fabsf(sd - k->tp.d) / t_left > 7.0f) continue;
#ifdef DK_ITEM_TRACE
        if((int)(k - karts) == 4 && ((int)(world_time * 60.0f) % 6) == 0)
            printf("DK ITEM TRACE kart 4 s %.0f ahead %.0f d %.2f line %.2f -> slot %.2f drift %d steer %.2f spd %.1f\n",
                   (double)k->tp.s, (double)ahead, (double)k->tp.d, (double)d, (double)sd, k->drift, (double)k->steer,
                   (double)k->speed);
#endif
        return sd;
    }
    (void)s;
    return d;
}

static bool spare(const Kart *k, int target) {
#if AI_SPARE_LEADER_FINAL_LAP
    if(target < 0 || !k->cpu) return false;
    const Kart *t = &karts[target];
    return kart_is_human(target) && t->place == 1 && t->lap >= game.laps;
#else
    (void)k; (void)target;
    return false;
#endif
}

void items_ai(Kart *k, KartInput *in, float dt) {
    (void)dt;
    if(!game.items || k->item == ITEM_NONE || k->finished || k->spin_t > 0) return;
    if(k->item_t < k->ai_item_delay) return;
    float s = k->tp.s;
    switch(k->item) {
        case ITEM_SNEAKERS: {
            bool straight = fabsf(track_curvature(s + 8.0f)) < 0.005f && fabsf(track_curvature(s + 45.0f)) < 0.005f;
            if((straight && k->grounded && k->surface != SURF_SAND) || k->item_t > 5.0f) in->item = true;
            break;
        }
        case ITEM_SPEAR: {
            int t = pick_target(k, 1, 50.0f), b = pick_target(k, -1, 14.0f);
            if(t >= 0 && !spare(k, t)) {
                in->item = true;
            }
            else if(b >= 0 && !spare(k, b)) {
                in->item = true;
                in->item_back = true;
            }
            else if(k->item_t > 6.0f) {
                /* Nobody worth it for a while: get rid of it (behind, if
                   the kart ahead is the one it leaves alone) and go for
                   the next emerald. */
                in->item = true;
                in->item_back = t < 0 || spare(k, t);
            }
            break;
        }
        case ITEM_SHIELD:
            if(items_threat(k) < 30.0f || k->item_t > 6.0f) in->item = true;
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */

typedef struct { V3 p; float yaw, sc, depth, alpha; int item; } GemDraw;

static void pool_quad(V3 c, float s, float d, float r, uint32_t col) {
    V3 t = track_tangent(s), rt = v3(t.z, 0, -t.x);
    float y = track_road_y(s, d) + 0.04f;
    V3 a = vadd(vmad(c, t, r), vscale(rt, -r)), b = vadd(vmad(c, t, r), vscale(rt, r));
    V3 e = vadd(vmad(c, t, -r), vscale(rt, -r)), f = vadd(vmad(c, t, -r), vscale(rt, r));
    RV q[4] = {{a.x, y, a.z, 0, 0, col}, {b.x, y, b.z, 1, 0, col}, {e.x, y, e.z, 0, 1, col}, {f.x, y, f.z, 1, 1, col}};
    r_strip(q, 4);
}

void items_draw(void) {
    if(!game.items) return;
    GemDraw gems[MAX_ITEM_BANDS + MAX_KARTS];
    int n = 0;
    r_load_vp();
    /* Pools of light on the road under the emeralds. */
    r_hdr(h_pool);
    for(int b = 0; b < track.nbands; b++) {
        V3 p;
        int item;
        if(!items_emerald(b, &p, &item)) continue;
        if(!r_sphere_visible(p, 2.0f)) continue;
        const Emerald *e = &emeralds[b];
        float pop = clampf(e->age / 0.35f, 0, 1);
        float a = (0.28f + 0.12f * sinf(world_time * 5.0f + (float)b)) * pop;
        pool_quad(p, track.bands[b].s, items_slot_d(b, e->lit), 1.3f, color_alpha(items_colour(item), a));
        GemDraw *g = &gems[n++];
        g->p = p;
        g->yaw = world_time * 2.2f + (float)b * 1.3f;
        g->sc = EMERALD_SCALE * (pop < 1.0f ? pop * (1.4f - 0.4f * pop) : 1.0f);
        g->item = item;
        g->alpha = 1.0f;
        g->depth = r_view_depth(p);
    }
    /* Gems trailing the karts that hold one, and held up when used. */
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->item == ITEM_NONE && k->raise_t <= 0) continue;
        V3 local = v3(0, 1.0f, -0.85f);
        float sc = HELD_SCALE;
        int item = k->item;
        if(k->raise_t > 0) {
            float f = k->raise_t / RAISE_LEN;    /* 1 at use, fading up and out */
            local = v3(0.22f, 1.25f + (1.0f - f) * 0.8f, 0.05f);
            sc = HELD_SCALE * f;
            if(item == ITEM_NONE) item = ITEM_SNEAKERS;
            if(k->shield_t > SHIELD_LEN - RAISE_LEN) item = ITEM_SHIELD;
            else if(spears[i].active && spears[i].life > SPEAR_LIFE - RAISE_LEN) item = ITEM_SPEAR;
        }
        V3 p = models_kart_point_world(k, local);
        p.y += 0.05f * sinf(world_time * 3.0f + (float)i);
        if(!r_sphere_visible(p, 0.6f)) continue;
        GemDraw *g = &gems[n++];
        g->p = p;
        g->yaw = world_time * 3.0f + (float)i;
        g->sc = sc;
        g->item = item;
        g->alpha = k->raise_t > 0 ? k->raise_t / RAISE_LEN : 1.0f;
        g->depth = r_view_depth(p);
    }
    /* Far to near: the translucent list is presorted. */
    for(int i = 1; i < n; i++) {
        GemDraw g = gems[i];
        int j = i;
        while(j > 0 && gems[j - 1].depth < g.depth) {
            gems[j] = gems[j - 1];
            j--;
        }
        gems[j] = g;
    }
    for(int i = 0; i < n; i++) gem_draw(gems[i].p, gems[i].yaw, gems[i].sc, gems[i].item, gems[i].alpha);

    /* Barriers: a faint core and a ring of sparks orbiting the kart. Fill
       is what the console pays for in the translucent list, and a big
       bubble round your own kart in the chase view covered most of the
       screen for six seconds: 20 ms renders and ~70 dropped frames a race. */
    r_hdr(h_glow);
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->shield_t <= 0) continue;
        V3 c = vadd(k->pos, v3(0, 0.7f, 0));
        if(!r_sphere_visible(c, 2.0f)) continue;
        float flick = k->shield_t < 1.2f ? 0.55f + 0.45f * sinf(world_time * 30.0f) : 1.0f;
        float a = (0.30f + 0.06f * sinf(world_time * 7.0f + (float)i)) * flick;
        r_billboard(c, 1.3f, 1.3f, 0, 0, 0, 1, 1, color_alpha(items_colour(ITEM_SHIELD), a));
    }
    r_hdr(h_star);
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(k->shield_t <= 0) continue;
        V3 c = vadd(k->pos, v3(0, 0.7f, 0));
        if(!r_sphere_visible(c, 2.0f)) continue;
        float flick = k->shield_t < 1.2f ? 0.55f + 0.45f * sinf(world_time * 30.0f) : 1.0f;
        uint32_t col = color_alpha(color_lerp(items_colour(ITEM_SHIELD), 0xffffffff, 0.35f), 0.85f * flick);
        for(int s = 0; s < 10; s++) {
            float ang = world_time * 2.6f + (float)s * (DK_TAU / 10.0f);
            /* Two interleaved tilted rings, so it reads as a sphere. */
            float tilt = (s & 1) ? 0.45f : -0.45f;
            V3 p = vadd(c, v3(cosf(ang) * 1.15f, sinf(ang) * tilt + 0.1f, sinf(ang) * 1.15f));
            r_billboard(p, 0.36f, 0.36f, ang * 2.0f, 0, 0, 1, 1, col);
        }
    }
    /* Spears: a bright head and a tail of fire. */
    for(int i = 0; i < MAX_KARTS; i++) {
        const Spear *sp = &spears[i];
        if(!sp->active || !r_sphere_visible(sp->pos, 3.0f)) continue;
        V3 t = track_tangent(sp->s);
        V3 tail = vmad(sp->pos, t, -2.6f * (float)sp->dir);
        r_hdr(h_glow);
        r_streak(sp->pos, tail, 0.32f, argb(255, 255, 200, 120), argb(0, 255, 60, 30));
        r_billboard(sp->pos, 0.6f, 0.6f, 0, 0, 0, 1, 1, argb(255, 255, 160, 90));
        r_billboard(sp->pos, 0.3f, 0.3f, 0, 0, 0, 1, 1, 0xffffffff);
        r_hdr(h_star);
        r_billboard(sp->pos, 0.9f, 0.9f, world_time * 12.0f, 0, 0, 1, 1, argb(200, 255, 90, 60));
    }
}

void items_draw_hud(int player, float x, float y, float k) {
    if(!game.items) return;
    const Kart *p = &karts[game.pl[player].kart];
    if(p->item == ITEM_NONE) return;
    uint32_t base = items_colour(p->item);
    float pop = p->item_t < 0.35f ? 1.0f + 0.7f * (1.0f - p->item_t / 0.35f) : 1.0f;
    float r = 13.0f * k * pop, rin = r * 0.5f;
    float a = p->item_t > 1.0f ? 0.85f : 1.0f;
    float rot = world_time * 1.5f;
    float ox[7], oy[7], ix[7], iy[7];
    for(int i = 0; i <= 6; i++) {
        float ang = rot + (float)i * (DK_TAU / 6.0f);
        ox[i] = x + sinf(ang) * r;
        oy[i] = y - cosf(ang) * r;
        ix[i] = x + sinf(ang) * rin;
        iy[i] = y - cosf(ang) * rin;
    }
    r_hdr(h_flat2d);
    for(int i = 0; i < 6; i++) {
        float mid = rot + ((float)i + 0.5f) * (DK_TAU / 6.0f);
        float lit = 0.45f + 0.55f * fmaxf(0.0f, cosf(mid + 2.4f));    /* light from the top left */
        uint32_t c = color_alpha(color_scale(base, lit), a);
        r_tri2d(ix[i], iy[i], ox[i], oy[i], ox[i + 1], oy[i + 1], c, c, c, 16.2f);
        r_tri2d(ix[i], iy[i], ox[i + 1], oy[i + 1], ix[i + 1], iy[i + 1], c, c, c, 16.2f);
    }
    uint32_t tc = color_alpha(color_lerp(base, 0xffffffff, 0.45f), a);
    for(int i = 0; i < 6; i++) r_tri2d(x, y, ix[i], iy[i], ix[i + 1], iy[i + 1], tc, tc, tc, 16.3f);
    /* A glint that slides round as it turns. */
    float ga = rot * 2.0f;
    uint32_t gw = color_alpha(0xffffffff, a * (0.5f + 0.5f * sinf(world_time * 6.0f)));
    r_tri2d(x + sinf(ga) * rin * 0.6f, y - cosf(ga) * rin * 0.6f, x + sinf(ga + 0.5f) * r * 0.85f,
            y - cosf(ga + 0.5f) * r * 0.85f, x + sinf(ga - 0.5f) * r * 0.85f, y - cosf(ga - 0.5f) * r * 0.85f, gw, gw,
            color_alpha(gw, 0), 16.4f);
    hud_text(x, y + r + 1.0f, 0.42f * k, ALIGN_CENTER, color_alpha(0xffffffff, a * 0.9f), "Y");
}
