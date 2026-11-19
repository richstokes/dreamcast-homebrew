/* Ridgeline: damage.

   A knock is the hardest blow the body took this frame (truck.c: its
   speed into the truck, where it landed and which way it pushed). It
   scuffs the face of the body it landed on, loosens whatever is bolted to
   that face and adds to the meter. Blows from below are the skid plate's
   job: they count a little toward the meter and mark nothing. A piece that
   has had enough comes off as a box with the truck's motion and a kick
   outward, and from there it is a rigid body of its own: gravity, its
   corners against the ground, a bounce and some rub, asleep once it lies
   still. A truck driving into one sends it on its way again. A wheel goes
   the same way, and takes its corner's grip and drive with it (truck.c).

   Trucks driven elsewhere (online) arrive with their damage as their own
   console worked it out (damage_set); only the pieces fall here. */
#include "damage.h"
#include "terrain.h"
#include "fx.h"
#include "engine/sfx.h"
#include "sound_assets.h"
#include "trucks.h"

_Static_assert(RL_PIECE_COUNT == DMG_PIECES, "DMG_PIECES in damage.h");

/* Test builds drive a truck up the mountain for timings that are compared
   with older builds': nothing takes damage there unless the test is of
   damage, or online (where it is on the wire). */
#if defined(RL_AUTOTEST) && !defined(RL_AT_DAMAGE) && !defined(RL_AT_ONLINE)
#define NO_KNOCKS 1
#else
#define NO_KNOCKS 0
#endif

#define BODY_MIN 3.0f         /* m/s: a knock under this leaves no mark on the body, */
#define FIX_MIN 1.5f          /* ...and under this nothing bolted to it feels it */
#define BELLY 0.35f           /* a blow from below, on the meter */
#define FULL 45.0f            /* the meter reads 100% at this much */
#define SCRAPE_MIN 3.0f       /* m/s: sliding on the body slower than this only polishes it */
#define LAMP_TOUGH 3.5f
/* A wheel comes off only in a real smash: a log or a trunk met at a pace
   a driver might plausibly be doing (20 mph, say) bends nothing. */
#define WHEEL_MIN 9.0f        /* m/s: a wheel meeting a trunk, a log or a ledge's side slower than this shrugs it off */
#define WHEEL_TOUGH 6.0f      /* what a wheel takes over that before it comes off: one blow at 15 m/s (34 mph), or two near it */
#define WHEEL_NEAR 0.9f       /* a blow to the body this close to a wheel reaches it too, */
#define WHEEL_BODY_MIN 12.0f  /* ...if it is this hard: 18 m/s (40 mph) in one go */
static const float face_at[DMG_LEVELS - 1] = {1.5f, 5.0f, 11.0f};
/* What each piece takes before it lets go (m/s over FIX_MIN, summed). */
static const float tough[DMG_PIECES] = {
    6.0f, 6.0f,     /* the bumpers */
    1.0f, 1.0f,     /* the mirrors: any real knock on that side */
    2.5f,           /* the roof's lights or rack */
    5.0f,           /* the spare */
    4.0f, 4.0f,     /* the doors: a hard roll, or a second one */
    8.0f,           /* the hood: after the bumper has gone */
    7.0f,           /* the tailgate */
};
static const V3 face_dir[DMG_FACES] = {{0, 0, 1}, {0, 0, -1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}};

typedef struct {
    DamageLook look;
    float face[DMG_FACES], piece[DMG_PIECES], lamp[4], wheel[4];
    float wheel_cool[4], wheel_last[4];
    float total;
    float cool, last;      /* one blow lasts several frames: count it once */
    int8_t side;           /* lying on a side: -1 its left, 1 its right */
    unsigned stamp;
    bool known;            /* a remote truck's look has arrived */
} Dmg;
static Dmg dmg[MAX_TRUCKS];

typedef struct {
    Debris d;
    V3 c, half;            /* the piece's box in its own space */
    V3 cpos, vel, w;       /* the box's centre in the world, its velocity and spin */
    float age, still;
    uint32_t born;
} Loose;
static Loose loose[DMG_DEBRIS];
static uint32_t born_n;

void damage_pack(const DamageLook *d, uint8_t *b) {
    /* (ten bits of faces, with the pieces past the first eight above them) */
    uint16_t f = (uint16_t)((d->pieces >> 8) << 10);
    for(int i = 0; i < DMG_FACES; i++) f |= (uint16_t)((d->face[i] & 3) << (2 * i));
    b[0] = (uint8_t)f;
    b[1] = (uint8_t)(f >> 8);
    b[2] = (uint8_t)d->pieces;
    b[3] = d->lamps;
    b[4] = d->percent;
}

void damage_unpack(const uint8_t *b, DamageLook *d) {
    uint16_t f = (uint16_t)(b[0] | b[1] << 8);
    for(int i = 0; i < DMG_FACES; i++) d->face[i] = (f >> (2 * i)) & 3;
    d->pieces = (uint16_t)((b[2] | (f >> 10) << 8) & ((1u << DMG_BITS) - 1));
    d->lamps = b[3] & 15;
    d->percent = b[4] > 100 ? 100 : b[4];
}

const DamageLook *damage_look(int truck) {
    return &dmg[truck].look;
}

unsigned damage_stamp(int truck) {
    return dmg[truck].stamp;
}

float damage_beam(int truck) {
    int out = dmg[truck].look.lamps & (DMG_HEAD_L | DMG_HEAD_R);
    return out == 0 ? 1.0f : out == (DMG_HEAD_L | DMG_HEAD_R) ? 0.0f : 0.55f;
}

/* What the damage costs the engine: the nose (the radiator, and what is
   behind it) most, and everything else a little. */
float damage_power_loss(int truck) {
    static const float nose[DMG_LEVELS] = {0, 0.06f, 0.15f, 0.28f};
    const DamageLook *l = &dmg[truck].look;
    return nose[l->face[DMG_FRONT]] + 0.12f * (float)l->percent * 0.01f;
}

const Debris *damage_debris(int i) {
    return &loose[i].d;
}

static bool has_piece(int model, int p) {
    const float *h = rl_truck_info[model].piece[p][1];
    return h[0] > 0 || h[1] > 0 || h[2] > 0;
}

static void drop_debris(int truck) {
    for(int i = 0; i < DMG_DEBRIS; i++)
        if(loose[i].d.on && loose[i].d.truck == truck) loose[i].d.on = false;
}

void damage_repair(int truck) {
    unsigned stamp = dmg[truck].stamp + 1;
    memset(&dmg[truck], 0, sizeof(dmg[truck]));
    dmg[truck].stamp = stamp;
    trucks[truck].wheel_off = 0;
    drop_debris(truck);
}

void damage_reset(void) {
    for(int i = 0; i < MAX_TRUCKS; i++) damage_repair(i);
    memset(loose, 0, sizeof(loose));
}

/* The way a piece faces: what has to be hit to knock it off. The spare
   hangs on the back, or lies on the deck behind the seats. */
static V3 piece_out(int model, int p) {
    switch(p + RL_PART_PIECE0) {
        case RL_PART_BUMPER_F: return v3(0, 0, 1);
        case RL_PART_BUMPER_R: return v3(0, 0, -1);
        case RL_PART_MIRROR_L: return v3(-1, 0, 0);
        case RL_PART_MIRROR_R: return v3(1, 0, 0);
        case RL_PART_ROOF: return v3(0, 1, 0);
        case RL_PART_DOOR_L: return v3(-1, 0, 0);
        case RL_PART_DOOR_R: return v3(1, 0, 0);
        case RL_PART_HOOD: return v3(0, 0.6f, 0.8f);   /* a blow to the nose, or landing on its roof */
        case RL_PART_TAILGATE: return v3(0, 0, -1);
        default: {
            const rl_truck_info_t *info = &rl_truck_info[model];
            return info->piece[p][0][2] < info->body_min[2] + 0.5f ? v3(0, 0, -1) : v3(0, 1, 0);
        }
    }
}

static V3 truck_origin(const Truck *t) {
    return vsub(vsub(t->pos, vscale(t->ax[1], t->spec.cog_y)), vscale(t->ax[2], t->spec.cog_z));
}

static V3 to_world(const V3 ax[3], V3 o, V3 m) {
    return vadd(o, vadd(vadd(vscale(ax[0], m.x), vscale(ax[1], m.y)), vscale(ax[2], m.z)));
}

/* Something broke at p: bits of it, and the sound if the camera is near. */
static void broke(V3 p, V3 v, uint32_t col, float size, int n) {
    float d = vlen(vsub(p, r_cam.pos));
    if(d > 60.0f) return;
    fx_burst(p, v, col, size, n);
    if(d < 35.0f) sfx_play(RL_SFX_CRASH_LIGHT, (int)(200.0f - 4.0f * d), 128, 1.35f + 0.2f * frand());
}

/* A piece leaves truck ti. */
static void detach(int ti, int p) {
    const Truck *t = &trucks[ti];
    Dmg *d = &dmg[ti];
    if(d->look.pieces & (1u << p)) return;
    d->look.pieces |= (uint16_t)(1u << p);
    d->stamp++;
    /* A door takes its mirror with it. */
    if(p + RL_PART_PIECE0 == RL_PART_DOOR_L && has_piece(t->model, RL_PART_MIRROR_L - RL_PART_PIECE0))
        detach(ti, RL_PART_MIRROR_L - RL_PART_PIECE0);
    if(p + RL_PART_PIECE0 == RL_PART_DOOR_R && has_piece(t->model, RL_PART_MIRROR_R - RL_PART_PIECE0))
        detach(ti, RL_PART_MIRROR_R - RL_PART_PIECE0);
    /* A free slot, or the one that has lain longest. */
    Loose *l = &loose[0];
    for(int i = 0; i < DMG_DEBRIS; i++) {
        if(!loose[i].d.on) {
            l = &loose[i];
            break;
        }
        if(loose[i].born < l->born) l = &loose[i];
    }
    const float (*box)[3] = rl_truck_info[t->model].piece[p];
    memset(l, 0, sizeof(*l));
    l->d = (Debris){.on = true, .truck = (uint8_t)ti, .model = (uint8_t)t->model, .livery = (uint8_t)t->livery,
                    .piece = (uint8_t)p, .moving = true};
    for(int k = 0; k < 3; k++) l->d.ax[k] = t->ax[k];
    l->c = v3(box[0][0], box[0][1], box[0][2]);
    l->half = v3(fmaxf(box[1][0], 0.03f), fmaxf(box[1][1], 0.03f), fmaxf(box[1][2], 0.03f));
    l->cpos = to_world(t->ax, truck_origin(t), l->c);
    V3 out = piece_out(t->model, p);
    V3 outw = to_world(t->ax, v3(0, 0, 0), out);
    l->vel = vadd(t->vel, vcross(t->w, vsub(l->cpos, t->pos)));
    l->vel = vmad(l->vel, outw, 1.5f + 2.0f * frand());
    l->vel = vadd(l->vel, v3(frand_s() * 0.8f, 1.2f + 1.2f * frand(), frand_s() * 0.8f));
    l->w = v3(frand_s() * 7.0f, frand_s() * 7.0f, frand_s() * 7.0f);
    l->born = ++born_n;
    broke(l->cpos, vscale(l->vel, 0.6f), 0xff2a2a2e, 0.05f, 6);
}

/* A wheel leaves truck ti: off its hub the way it was turning, and out. */
static void wheel_off(int ti, int i) {
    Truck *t = &trucks[ti];
    Dmg *d = &dmg[ti];
    int p = DMG_WHEEL0 + i;
    if(d->look.pieces & (1u << p)) return;
    d->look.pieces |= (uint16_t)(1u << p);
    d->stamp++;
    t->wheel_off |= (uint8_t)(1u << i);
    Loose *l = &loose[0];
    for(int k = 0; k < DMG_DEBRIS; k++) {
        if(!loose[k].d.on) {
            l = &loose[k];
            break;
        }
        if(loose[k].born < l->born) l = &loose[k];
    }
    memset(l, 0, sizeof(*l));
    l->d = (Debris){.on = true, .truck = (uint8_t)ti, .model = (uint8_t)t->model, .livery = (uint8_t)t->livery,
                    .piece = (uint8_t)p, .moving = true};
    for(int k = 0; k < 3; k++) l->d.ax[k] = t->ax[k];
    l->half = v3(0.5f * t->spec.width, t->spec.radius, t->spec.radius);
    l->cpos = t->wh[i].centre;
    V3 out = vscale(t->ax[0], (i & 1) ? 1.0f : -1.0f);
    l->vel = vadd(t->vel, vcross(t->w, vsub(l->cpos, t->pos)));
    l->vel = vmad(l->vel, out, 2.0f + 2.0f * frand());
    l->vel.y += 1.5f + 1.5f * frand();
    l->w = vadd(vscale(t->ax[0], t->wh[i].omega), v3(frand_s() * 2.0f, frand_s() * 2.0f, frand_s() * 2.0f));
    l->born = ++born_n;
    broke(l->cpos, vscale(l->vel, 0.5f), 0xff3a3a40, 0.05f, 8);
}

static void lamp_out(int ti, int which) {
    const Truck *t = &trucks[ti];
    Dmg *d = &dmg[ti];
    if(d->look.lamps & (1u << which)) return;
    d->look.lamps |= (uint8_t)(1u << which);
    d->stamp++;
    const rl_truck_info_t *info = &rl_truck_info[t->model];
    const float *m = which < 2 ? info->headlight[which] : info->taillight[which - 2];
    V3 p = to_world(t->ax, truck_origin(t), v3(m[0], m[1], m[2]));
    broke(p, vscale(t->vel, 0.5f), which < 2 ? 0xfff4f6f0 : 0xffe83028, 0.035f, 8);
}

static void relook(Dmg *d) {
    for(int f = 0; f < DMG_FACES; f++) {
        int lv = 0;
        while(lv < DMG_LEVELS - 1 && d->face[f] >= face_at[lv]) lv++;
        if(lv != d->look.face[f]) {
            d->look.face[f] = (uint8_t)lv;
            d->stamp++;
        }
    }
    float pc = d->total * (100.0f / FULL) + 3.0f * __builtin_popcount(d->look.pieces) + 2.0f * __builtin_popcount(d->look.lamps);
    d->look.percent = (uint8_t)clampf(pc, 0, 100);
}

/* A blow of `body` on the body and `fix` on what is bolted to it (m/s over
   their thresholds, or that much a second times dt for a scrape), landing
   at `at` (model space) and pushing the truck along n (body axes). */
static void blow(int ti, float body, float fix, V3 at, V3 n) {
    const Truck *t = &trucks[ti];
    Dmg *d = &dmg[ti];
    V3 from = vscale(n, -1.0f);   /* out of the face it landed on */
    if(from.y < -0.6f) {
        d->total += body * BELLY;
        relook(d);
        return;
    }
    d->total += body;
    for(int f = 0; f < DMG_FACES; f++) {
        float e = vdot(from, face_dir[f]);
        if(e > 0) d->face[f] += body * e * e;
    }
    for(int p = 0; p < DMG_PIECES; p++) {
        if(!has_piece(t->model, p) || (d->look.pieces & (1u << p))) continue;
        float e = vdot(from, piece_out(t->model, p));
        if(e < 0.5f) continue;
        d->piece[p] += fix * e * e;
        if(d->piece[p] >= tough[p]) detach(ti, p);
    }
    /* A wheel beside a hard enough blow. */
    for(int i = 0; i < 4; i++) {
        V3 w = t->spec.wheel[i];
        float over = fix + FIX_MIN - WHEEL_BODY_MIN;
        if(over <= 0 || vlen(vsub(at, w)) > WHEEL_NEAR) continue;
        d->wheel[i] += over;
        if(d->wheel[i] >= WHEEL_TOUGH) wheel_off(ti, i);
    }
    /* The lamps: the pair on the end that was hit, the one on the side it
       landed more than the other. */
    float hw = t->spec.body_hi[0];
    for(int l = 0; l < 4; l++) {
        float e = l < 2 ? from.z : -from.z;
        if(e < 0.5f) continue;
        bool right = l & 1;
        float side = fabsf(at.x) < 0.25f * hw ? 0.8f : (at.x > 0) == right ? 1.0f : 0.35f;
        d->lamp[l] += fix * e * e * side;
        if(d->lamp[l] >= LAMP_TOUGH) lamp_out(ti, l);
    }
    relook(d);
}

/* (field by field: the struct has padding, which a memcmp would read) */
static bool same_look(const DamageLook *a, const DamageLook *b) {
    return memcmp(a->face, b->face, sizeof(a->face)) == 0 && a->pieces == b->pieces && a->lamps == b->lamps &&
           a->percent == b->percent;
}

void damage_set(int truck, const DamageLook *look) {
    Dmg *d = &dmg[truck];
    if(d->known && same_look(&d->look, look)) return;
    /* (a truck not seen before lost what it has lost before we saw it) */
    uint16_t had = d->known ? d->look.pieces : look->pieces;
    uint8_t had_lamps = d->known ? d->look.lamps : look->lamps;
    if(had & ~look->pieces) drop_debris(truck);   /* mended: its pieces are back on it */
    d->look = *look;
    d->known = true;
    d->stamp++;
    /* Those newly gone come off here. */
    d->look.pieces = had & look->pieces;
    d->look.lamps = had_lamps & look->lamps;
    for(int p = 0; p < DMG_PIECES; p++) {
        if(!(look->pieces & ~had & (1u << p))) continue;
        if(has_piece(trucks[truck].model, p)) detach(truck, p);
        else d->look.pieces |= (uint16_t)(1u << p);
    }
    for(int l = 0; l < 4; l++)
        if(look->lamps & ~had_lamps & (1u << l)) lamp_out(truck, l);
    for(int i = 0; i < 4; i++)
        if(look->pieces & ~had & (1u << (DMG_WHEEL0 + i))) wheel_off(truck, i);
    trucks[truck].wheel_off = (uint8_t)(d->look.pieces >> DMG_WHEEL0);
}

/* ---- Loose pieces ---- */

#define REST 0.3f        /* bounce */
#define RUB 0.55f

static void loose_step(Loose *l, float dt) {
    l->age += dt;
    /* A truck driving into it sends it on (its own, once it is clear). */
    for(int ti = 0; ti < game.ntrucks; ti++) {
        const Truck *t = &trucks[ti];
        if(t->absent || (ti == l->d.truck && l->age < 1.0f)) continue;
        float sp = vlen(t->vel);
        if(sp < 1.0f) continue;
        const TruckSpec *s = &t->spec;
        V3 hc = truck_to_world(t, v3(s->hull_c[0], s->hull_c[1], s->hull_c[2]));
        V3 d = vsub(l->cpos, hc);
        if(fabsf(vdot(d, t->ax[0])) > s->hull_h[0] + 0.15f || fabsf(vdot(d, t->ax[1])) > s->hull_h[1] + 0.15f ||
           fabsf(vdot(d, t->ax[2])) > s->hull_h[2] + 0.15f)
            continue;
        if(vdot(vsub(l->vel, t->vel), t->vel) > 0) continue;   /* already going faster that way */
        l->vel = vadd(vscale(t->vel, 1.15f), v3(frand_s() * 1.5f, 1.0f + 0.15f * sp, frand_s() * 1.5f));
        l->w = v3(frand_s() * 8.0f, frand_s() * 8.0f, frand_s() * 8.0f);
        l->d.moving = true;
        l->still = 0;
    }
    if(!l->d.moving) return;
    V3 *ax = l->d.ax;
    l->vel.y -= GRAVITY * dt;
    l->cpos = vmad(l->cpos, l->vel, dt);
    for(int k = 0; k < 3; k++) ax[k] = vmad(ax[k], vcross(l->w, ax[k]), dt);
    ax[2] = vnorm(ax[2]);
    ax[0] = vnorm(vcross(ax[1], ax[2]));
    ax[1] = vcross(ax[2], ax[0]);
    /* Its corners against the ground: an impulse at each that is under and
       still going down, as a body of unit mass and one moment of inertia. */
    V3 h = l->half;
    float inertia = (h.x * h.x + h.y * h.y + h.z * h.z) * (1.0f / 3.0f) + 0.002f;
    float deepest = 0;
    bool touching = false;
    for(int c = 0; c < 8; c++) {
        V3 r = to_world(ax, v3(0, 0, 0), v3((c & 1) ? h.x : -h.x, (c & 2) ? h.y : -h.y, (c & 4) ? h.z : -h.z));
        V3 p = vadd(l->cpos, r), n;
        float g = ground_below(p.x, p.z, p.y + 0.6f, &n, NULL);
        if(p.y >= g) continue;
        touching = true;
        if(g - p.y > deepest) deepest = g - p.y;
        V3 vp = vadd(l->vel, vcross(l->w, r));
        float vn = vdot(vp, n);
        if(vn >= 0) continue;
        V3 rn = vcross(r, n);
        float j = -(1.0f + REST) * vn / (1.0f + vdot(rn, rn) / inertia);
        V3 imp = vscale(n, j);
        V3 vt = vsub(vp, vscale(n, vn));
        float vtl = vlen(vt);
        if(vtl > 0.01f) imp = vmad(imp, vt, -fminf(RUB * j, vtl * 0.5f) / vtl);
        l->vel = vadd(l->vel, imp);
        l->w = vadd(l->w, vscale(vcross(r, imp), 1.0f / inertia));
    }
    l->cpos.y += deepest * 0.8f;
    /* Trunks and rock sides: back out, and off them. */
    ObsContact oc[2];
    int nc = obstacle_contacts(l->cpos, fmaxf(h.x, fmaxf(h.y, h.z)) * 0.7f, oc, 2);
    for(int c = 0; c < nc; c++) {
        l->cpos = vmad(l->cpos, oc[c].n, oc[c].depth);
        float vn = vdot(l->vel, oc[c].n);
        if(vn < 0) l->vel = vmad(l->vel, oc[c].n, -(1.0f + REST) * vn);
    }
    float wl = l->w.x * l->w.x + l->w.y * l->w.y + l->w.z * l->w.z;
    if(wl > 30.0f * 30.0f) l->w = vscale(l->w, 30.0f / sqrtf(wl));
    /* Into water: gone, with a splash. */
    float wt = water_at(l->cpos.x, l->cpos.z);
    if(wt > l->cpos.y + 0.2f) {
        fx_splash(v3(l->cpos.x, wt, l->cpos.z), 0.7f);
        l->d.on = false;
        return;
    }
    if(touching) {
        l->w = vscale(l->w, 1.0f - 2.0f * dt);
        if(vdot(l->vel, l->vel) < 0.35f * 0.35f && wl < 1.0f) l->still += dt;
        else l->still = 0;
        if(l->still > 0.4f) {
            l->d.moving = false;
            l->vel = l->w = v3(0, 0, 0);
        }
    }
    else l->still = 0;
    if(l->cpos.y < -500.0f) l->d.on = false;
}

/* A bare hub dragging: sparks off rock and gravel, a little dirt off the rest. */
static void hub_sparks(const Truck *t, int i) {
    const TruckSpec *s = &t->spec;
    V3 hub = truck_to_world(t, v3(s->wheel[i].x, s->wheel[i].y - s->sag * s->travel - 0.14f, s->wheel[i].z));
    if(vlen(vsub(hub, r_cam.pos)) > 60.0f || vdot(t->vel, t->vel) < 1.5f * 1.5f || frand() > 0.5f) return;
    int sf;
    float g = ground_below(hub.x, hub.z, hub.y + 0.5f, NULL, &sf);
    if(hub.y - g > 0.06f) return;
    bool hard = sf == SURF_ROCK || sf == SURF_GRAVEL;
    fx_burst(v3(hub.x, g + 0.03f, hub.z), vscale(t->vel, hard ? 0.5f : 0.2f), hard ? 0xffffc860 : 0xff5a4a38, hard ? 0.02f : 0.04f, 1);
}

void damage_update(float dt) {
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        Dmg *d = &dmg[ti];
        if(t->absent) continue;
        /* Stood somewhere afresh (a recover): its wheels are back on. */
        if(t->placed && !t->remote && (d->look.pieces >> DMG_WHEEL0)) {
            d->look.pieces &= (1u << DMG_WHEEL0) - 1;
            memset(d->wheel, 0, sizeof(d->wheel));
            for(int i = 0; i < DMG_DEBRIS; i++)
                if(loose[i].d.on && loose[i].d.truck == ti && loose[i].d.piece >= DMG_WHEEL0) loose[i].d.on = false;
            d->stamp++;
            relook(d);
        }
        t->placed = false;
        t->wheel_off = (uint8_t)(d->look.pieces >> DMG_WHEEL0);
        for(int i = 0; i < 4; i++)
            if(t->wheel_off & (1u << i)) hub_sparks(t, i);
        /* What it costs to drive (the player: the CPU's trucks have nobody
           to mend them, and a climb they could no longer make would have
           them recovering for ever), and steam from a nose badly enough in. */
        t->power_loss = t->cpu ? 0 : damage_power_loss(ti);
        t->steer_pull = t->cpu ? 0 : 0.012f * (float)((int)d->look.face[DMG_RIGHT] - (int)d->look.face[DMG_LEFT]);
        int nose = d->look.face[DMG_FRONT];
        if(nose >= 2 && !t->stalled && frand() < (nose == 3 ? 6.0f : 2.5f) * dt && vlen(vsub(t->pos, r_cam.pos)) < 50.0f)
            fx_steam(truck_to_world(t, v3(frand_s() * 0.3f, t->spec.body_hi[1] * 0.62f, t->spec.body_hi[2] - 0.35f)), vscale(t->vel, 0.6f));
        if(t->remote || game.damage_off || NO_KNOCKS) continue;
        for(int i = 0; i < 4; i++) {
            d->wheel_cool[i] -= dt;
            float h = t->wheel_hit[i];
            if(h > WHEEL_MIN && (d->wheel_cool[i] <= 0 || h > d->wheel_last[i] * 1.5f)) {
                d->wheel[i] += h - WHEEL_MIN;
                d->wheel_last[i] = h;
                d->wheel_cool[i] = 0.3f;
                d->total += 0.5f * (h - WHEEL_MIN);
                if(d->wheel[i] >= WHEEL_TOUGH) wheel_off(ti, i);
                relook(d);
            }
        }
        d->cool -= dt;
        if(t->knock > FIX_MIN && (d->cool <= 0 || t->knock > d->last * 1.5f)) {
            blow(ti, fmaxf(t->knock - BODY_MIN, 0), t->knock - FIX_MIN, t->knock_at, t->knock_n);
            d->last = t->knock;
            d->cool = 0.3f;
        }
        if(t->scrape > SCRAPE_MIN) {
            float rate = fminf(0.3f * (t->scrape - SCRAPE_MIN), 1.5f) * dt;
            blow(ti, rate, 2.0f * rate, v3(0, 0, 0), t->scrape_n);
        }
        /* Over onto a side. The tyres stand further out than the body and
           take that landing in the physics, so the body's own knock says
           little of it: going over is a blow to that side by itself (the
           mirror never survives it), harder the faster the truck is
           moving, and sliding along on it wears it. */
        float lean = t->ax[0].y;
        int side = lean < -0.8f ? 1 : lean > 0.8f ? -1 : fabsf(lean) > 0.6f ? d->side : 0;
        if(side && !d->side && t->pos.y - ground_below(t->pos.x, t->pos.z, t->pos.y + 0.5f, NULL, NULL) > t->spec.hull_h[0] + 0.6f)
            side = 0;   /* (in the air) */
        if(side) {
            float sp = vlen(t->vel);
            V3 n = v3((float)-side, 0, 0), at = v3(side * t->spec.body_hi[0], 1.0f, 0);
            if(side != d->side) blow(ti, 1.5f + 0.5f * sp, 1.5f + 0.5f * sp, at, n);
            else if(sp > 2.0f) {
                float rate = fminf(0.3f * (sp - 2.0f), 1.5f) * dt;
                blow(ti, rate, 2.0f * rate, at, n);
            }
        }
        d->side = (int8_t)side;
    }
    for(int i = 0; i < DMG_DEBRIS; i++) {
        Loose *l = &loose[i];
        if(!l->d.on) continue;
        if(vlen(vsub(l->cpos, r_cam.pos)) > 250.0f) {
            l->d.on = false;
            continue;
        }
        loose_step(l, dt);
        /* Where the piece's own origin is, for drawing it as it was modelled. */
        l->d.pos = to_world(l->d.ax, l->cpos, vscale(l->c, -1.0f));
    }
}
