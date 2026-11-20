/* The kaiju (kaiju.h). */
#include "kaiju.h"
#include "cel.h"
#include "city.h"
#include "fx.h"
#include "score.h"
#include "audio.h"
#include "life.h"
#include "models.h"
#include "aim.h"

Kaiju kj;

enum { P_TORSO, P_HEAD, P_JAW, P_UPPER, P_FORE, P_THIGH, P_SHIN, P_FOOT, P_TAIL0, P_COUNT = P_TAIL0 + KJ_TAIL_SEGS };
static CelMesh part[P_COUNT];
static const pvr_poly_hdr_t *h_shadow;

#define BODY_R 9.5f
#define WALK_SPEED 27.0f
#define STRIDE 6.5f
#define DASH_SPEED 50.0f
#define SEA_REACH 105.0f     /* how far out it can wade */
#define SEA_DEPTH 18.0f
#define TREE_HANDLE (-100)   /* kj.held at or under this: a tree (over it and negative: a chunk) */

/* The sea bed under p: it shelves from the water's edge. */
static float sea_depth(V3 p) {
    float out = fmaxf(fabsf(p.x), fabsf(p.z)) - CITY_SHORE;
    return clampf(out / 70.0f, 0, 1) * SEA_DEPTH;
}

bool kaiju_init(void) {
    static const int16_t km[P_COUNT][2] = {
        {KM_TORSO, KM_TORSO_INK}, {KM_HEAD, KM_HEAD_INK}, {KM_JAW, KM_JAW_INK}, {KM_UPPER_ARM, KM_UPPER_ARM_INK},
        {KM_FOREARM, KM_FOREARM_INK}, {KM_THIGH, KM_THIGH_INK}, {KM_SHIN, KM_SHIN_INK}, {KM_FOOT, KM_FOOT_INK},
        {KM_TAIL0, KM_TAIL0_INK}, {KM_TAIL1, KM_TAIL1_INK}, {KM_TAIL2, KM_TAIL2_INK}, {KM_TAIL3, KM_TAIL3_INK},
        {KM_TAIL4, KM_TAIL4_INK}};
    for(int i = 0; i < P_COUNT; i++)
        if(!cel_mesh_init(&part[i], km[i][0], km[i][1])) return false;
    h_shadow = r_header(TEX_BLOBSHADOW, HF_PT | HF_CLAMP);
    return true;
}

static V3 foot_home(int i) {
    V3 r = heading_right(kj.heading), f = heading_fwd(kj.heading);
    return vadd(vadd(kj.pos, vscale(r, i ? KJ_HIP_X : -KJ_HIP_X)), vscale(f, kj.speed * 0.1f));
}

void kaiju_reset(void) {
    memset(&kj, 0, sizeof(kj));
    /* Chest deep off the beach, facing town. */
    kj.pos = v3(0, 0, -CITY_SHORE - 62.0f);
    kj.sink = sea_depth(kj.pos);
    kj.energy = 0.5f;
    aim_off();
    for(int i = 0; i < 2; i++) {
        kj.foot[i].p = foot_home(i);
        kj.foot[i].planted = true;
        kj.foot[i].t = 1;
    }
}

const char *kaiju_carrying(void) {
    if(!kj.held) return NULL;
    return kj.held > 0 ? life_handle_name(kj.held) : kj.held <= TREE_HANDLE ? "A TREE" : "A CHUNK OF BUILDING";
}

void kaiju_hurt(void) {
    kj.flash = 1.0f;
    score_hurt();
    fx_shake(0.45f);
    audio_sfx(KL_SFX_HURT, 170);
}

static void start(int action) {
    kj.action = action;
    kj.at = 0;
    kj.hit_done = false;
    kj.queued = false;
    switch(action) {
        case ACT_SWIPE: audio_sfx(KL_SFX_SWIPE, 190); break;
        case ACT_TAIL: audio_sfx(KL_SFX_TAIL, 210); break;
        case ACT_JUMP: audio_sfx(KL_SFX_JUMP, 170); break;
        case ACT_CHOMP: audio_sfx(KL_SFX_CHOMP, 210); break;
        default: break;
    }
}

static void footfall(V3 p, float weight) {
    if(kj.sink > 1.5f) {
        /* In the sea: a ring on the water, and nothing underfoot. */
        fx_ring(p, 9.0f);
        audio_at(KL_SFX_STEP, p, (int)(60 * weight));
        return;
    }
    audio_at(KL_SFX_STEP, p, (int)(150 * weight));
    fx_puffs(p, 3.5f, 2, 4.0f * weight);
    fx_shake(0.1f * weight);
    life_stomp(p, 6.5f);
    city_trample(p, 7.5f);
}

static void feet(float dt) {
    if(kj.action == ACT_JUMP) {
        for(int i = 0; i < 2; i++) {
            kj.foot[i].p = foot_home(i);
            kj.foot[i].planted = true;
            kj.foot[i].lift = 0;
        }
        return;
    }
    for(int i = 0; i < 2; i++) {
        V3 home = foot_home(i);
        if(kj.foot[i].planted) {
            const typeof(kj.foot[0]) *o = &kj.foot[1 - i];
            float lag = sqrtf(dist2_xz(kj.foot[i].p, home));
            float olag = o->planted ? sqrtf(dist2_xz(o->p, foot_home(1 - i))) : 0;
            /* The foot furthest behind goes, once the other is down (or nearly). */
            if(lag > STRIDE && (o->planted ? lag >= olag : o->t > 0.7f)) {
                kj.foot[i].planted = false;
                kj.foot[i].from = kj.foot[i].p;
                kj.foot[i].t = 0;
                kj.foot[i].dur = clampf(0.3f - kj.speed * 0.004f, 0.17f, 0.3f);
            }
            continue;
        }
        kj.foot[i].t += dt / kj.foot[i].dur;
        float t = kj.foot[i].t > 1 ? 1 : kj.foot[i].t;
        /* It lands ahead of where the hip will be. */
        V3 to = vmad(home, heading_fwd(kj.heading), kj.speed * kj.foot[i].dur * 0.9f + (kj.speed > 2 ? 2.5f : 0));
        kj.foot[i].p = vlerp(kj.foot[i].from, to, ease_io(t));
        kj.foot[i].lift = sinf(t * ENG_PI) * 5.0f;
        if(kj.foot[i].t >= 1) {
            kj.foot[i].planted = true;
            kj.foot[i].lift = 0;
            footfall(kj.foot[i].p, clampf(0.6f + kj.speed * 0.02f, 0.6f, 1.2f));
        }
    }
}

static void breathe(float dt) {
    V3 f = heading_fwd(kj.heading);
    V3 d = vnorm(vsub(vmad(v3(kj.pos.x, 3.0f, kj.pos.z), f, 150.0f), kj.mouth));
    float first;
    int before = city_standing();
    city_damage_beam(kj.mouth, d, 175.0f, 300.0f * dt, SRC_BEAM, &first);
    kj.breath_kills += before - city_standing();
    kj.breath_kills += life_beam(kj.mouth, d, 175.0f);
    if(kj.breath_kills >= 3 && !kj.sliced) {
        kj.sliced = true;
        score_trick(TR_SLICER, 1);
    }
    float ground = d.y < -0.01f ? kj.mouth.y / -d.y : 175.0f;
    V3 end = vmad(kj.mouth, d, fminf(ground, 175.0f));
    fx_beam(kj.mouth, end);
    if(ground < 175.0f && (eng_rand() & 3) == 0) fx_puff(end, v3(frand_s() * 8, 9, frand_s() * 8), 6, 0.5f, 0xffd8f8ff);
    kj.mouth_dir = d;
    kj.energy -= 0.2f * dt;
    audio_beam = 1;
}

void kaiju_update(const Ctl *c, float dt) {
    kj.clock += dt;
    kj.energy = clampf(kj.energy + score.energy_gain, 0, 1);
    score.energy_gain = 0;
    kj.flash = approach(kj.flash, 0, dt * 4.0f);
    kj.squash = approach(kj.squash, 0, dt * 5.0f);
    if(kj.roar_cd > 0) kj.roar_cd -= dt;

    if(kj.stagger > 0) kj.stagger -= dt;
    kj.toss = approach(kj.toss, 0, dt * 4.0f);
    kj.want_grab = c->grab;
    if(kj.action == ACT_NONE && kj.stagger <= 0) {
        if(c->jump) start(ACT_JUMP);
        else if(c->tail) start(ACT_TAIL);
        else if(c->claw) start(ACT_SWIPE);
        else if(c->chomp && !kj.held) start(ACT_CHOMP);
        else if(c->roar && kj.roar_cd <= 0) start(ACT_ROAR);
    }
    else if(kj.action == ACT_SWIPE && c->claw) kj.queued = true;

    bool was = kj.breathing;
    kj.breathing = kj.action == ACT_NONE && !kj.held && c->breath && kj.energy > (was ? 0.0f : 0.08f);
    if(kj.breathing && !was) {
        kj.breath_kills = 0;
        kj.sliced = false;
    }

    /* Walking. */
    float ml = sqrtf(c->mx * c->mx + c->mz * c->mz);
    if(ml > 1) ml = 1;
    /* The charge: fast, hard to steer, and only on dry land. */
    bool dash_was = kj.dashing;
    kj.dashing = c->dash && kj.action == ACT_NONE && !kj.breathing && kj.stagger <= 0 && ml > 0.15f && kj.sink < 5.0f;
    if(kj.dashing && !dash_was) kj.dash_from = city_standing();
    if(!kj.dashing && dash_was && kj.dash_from - city_standing() >= 4) score_trick(TR_BULLDOZER, 1);
    float turn = kj.dashing ? 1.5f : kj.breathing ? 1.2f : kj.action == ACT_TAIL || kj.action == ACT_ROAR ? 0 : 3.6f, dh = 0;
    if(ml > 0.15f) {
        dh = wrap_angle(atan2f(c->mx, c->mz) - kj.heading);
        float step = clampf(dh, -turn * dt, turn * dt);
        kj.heading = wrap_angle(kj.heading + step);
        kj.turn_rate = lerpf(kj.turn_rate, step / dt, smooth_k(8, dt));
    }
    else kj.turn_rate = lerpf(kj.turn_rate, 0, smooth_k(8, dt));
    float want = ml > 0.15f ? (kj.dashing ? DASH_SPEED : WALK_SPEED * ml) * fmaxf(0, cosf(dh)) : 0;
    if(kj.stagger > 0) want = 0;
    want *= 1.0f - 0.45f * kj.sink / SEA_DEPTH;      /* wading is slow going */
    switch(kj.action) {
        case ACT_TAIL: case ACT_CHOMP: case ACT_ROAR: want = 0; break;
        case ACT_SWIPE: want *= 0.5f; break;
        case ACT_JUMP: want = 36.0f * fmaxf(ml, 0.25f); break;
        default: if(kj.breathing) want *= 0.35f; break;
    }
    kj.speed = approach(kj.speed, want, 80.0f * dt);
    V3 f = heading_fwd(kj.heading);
    kj.pos = vmad(kj.pos, f, kj.speed * dt);
    bool hit = city_collide(&kj.pos, BODY_R, kj.y, f, kj.speed > 4 ? (70.0f + kj.speed * 5.0f) * dt : 0, kj.dashing ? 26.0f : 0);
    if(hit && kj.dashing && kj.speed > 30.0f) {
        /* Head first into something too big to go through: it loses a
           bite, and the kaiju its footing. */
        V3 at = vmad(kj.pos, f, 12.0f);
        at.y = 16.0f;
        city_damage_circle(at, 13.0f, 260.0f, SRC_RAM);
        kj.pos = vmad(kj.pos, f, -3.0f);
        kj.speed = 0;
        kj.stagger = 0.5f;
        kj.dashing = false;
        if(kj.dash_from - city_standing() >= 4) score_trick(TR_BULLDOZER, 1);
        fx_shake(1.0f);
        fx_hitstop(0.05f);
        fx_puffs(at, 8.0f, 6, 8.0f);
        audio_sfx(KL_SFX_CRASH_M, 230);
    }
    else if(hit && kj.action != ACT_JUMP) kj.speed = fminf(kj.speed, 9.0f);
    if(kj.dashing) {
        life_launch(vmad(kj.pos, f, 9.0f), 11.0f, 0.9f);
        if((kj.dust -= dt) <= 0) {
            kj.dust = 0.07f;
            fx_puff(vadd(kj.pos, v3(frand_s() * 6, 3, frand_s() * 6)), vscale(f, -10.0f), 7.0f, 0.5f, 0xfff4f0e8);
        }
    }
    float lim = CITY_SHORE + SEA_REACH;
    kj.pos.x = clampf(kj.pos.x, -lim, lim);
    kj.pos.z = clampf(kj.pos.z, -lim, lim);
    city_trample(kj.pos, BODY_R);
    kj.sink = lerpf(kj.sink, sea_depth(kj.pos), smooth_k(6, dt));
    if(kj.sink > 1.0f && (kj.speed > 3.0f || kj.action == ACT_TAIL) && (kj.splash -= dt) <= 0) {
        /* White water where it wades. */
        kj.splash = 0.09f;
        float a = frand() * ENG_TAU;
        fx_puff(vadd(kj.pos, v3(sinf(a) * 9, 1.5f, cosf(a) * 9)), v3(sinf(a) * 7, 5, cosf(a) * 7), 4.5f + frand() * 3, 0.5f, 0xffe4f6ff);
    }
    /* A wave on its way in. */
    if(kj.wave_r > 0) {
        float r0 = kj.wave_r;
        kj.wave_r += 85.0f * dt;
        city_damage_ring(kj.wave_c, r0 - 6.0f, kj.wave_r, 110.0f, SRC_STOMP);
        for(int k = 0; k < 5; k++) {
            float a = frand() * ENG_TAU;
            V3 p = vadd(kj.wave_c, v3(sinf(a) * kj.wave_r, 2, cosf(a) * kj.wave_r));
            /* (the front shows where it has come ashore, and a little way out) */
            if(fmaxf(fabsf(p.x), fabsf(p.z)) < CITY_SHORE + 30.0f)
                fx_puff(p, v3(sinf(a) * 14, 4, cosf(a) * 14), 8.0f, 0.6f, 0xffe4f6ff);
        }
        if(kj.wave_r > 200.0f) {
            if(kj.wave_from - city_standing() >= 4) score_trick(TR_TSUNAMI, 1);
            kj.wave_r = 0;
        }
    }
    /* What is in the jaws goes where they go, and flies when they open. */
    if(kj.grab_pop > 0) kj.grab_pop -= dt;
    if(kj.held) {
        V3 at = vmad(kj.mouth, f, 3.0f);
        if(kj.held > 0) life_hold(kj.held, at, kj.heading);
        else if(kj.held <= TREE_HANDLE) city_tree_hold(TREE_HANDLE - kj.held, at);
        else city_chunk_hold(-1 - kj.held, at);
        aim_update(at, f, kj.held > 0 ? life_handle_gravity(kj.held) : 55.0f, dt);
        if((kj.hold_t += dt) > 0.25f && (!c->grab || kj.hold_t > 6.0f)) {
            V3 v = aim_velocity();
            if(aim_locked()) fx_shake(0.25f);
            aim_off();
            if(kj.held > 0) life_throw(kj.held, v);
            else if(kj.held <= TREE_HANDLE) city_tree_throw(TREE_HANDLE - kj.held, v);
            else city_chunk_throw(-1 - kj.held, v);
            kj.held = 0;
            kj.toss = 1;
            audio_sfx(KL_SFX_SWIPE, 220);
        }
    }

    /* What the kaiju is doing. */
    kj.at += dt;
    switch(kj.action) {
        case ACT_SWIPE:
            if(!kj.hit_done && kj.at >= 0.2f) {
                kj.hit_done = true;
                V3 at = vmad(kj.pos, f, 17.0f);
                at.y = 27.0f;   /* claw height */
                city_damage_circle(at, 17.0f, 115.0f, SRC_CLAW);
                life_swat(kj.pos, f, 36.0f);
                life_launch(at, 15.0f, 0.7f);
            }
            if(kj.at >= 0.44f) {
                kj.arm ^= 1;
                if(kj.queued) start(ACT_SWIPE);
                else kj.action = ACT_NONE;
            }
            break;
        case ACT_TAIL:
            if(!kj.hit_done && kj.at >= 0.28f) {
                kj.hit_done = true;
                city_damage_circle(v3(kj.pos.x, 8.0f, kj.pos.z), 34.0f, 95.0f, SRC_TAIL);
                if(life_launch(kj.pos, 38.0f, 1.2f) >= 3) score_trick(TR_HOME_RUN, 1);
                fx_ring(kj.pos, 38.0f);
            }
            if(kj.at >= 0.62f) kj.action = ACT_NONE;
            break;
        case ACT_JUMP: {
            float u = kj.at / 0.9f;
            kj.y = u < 1 ? 104.0f * u * (1 - u) : 0;
            if(u > 0.5f) {
                /* Whatever is under the belly on the way down. */
                int under = city_at(kj.pos.x, kj.pos.z);
                if(under >= 0 && city_b[under].h <= 96.0f && kj.y < city_b[under].h + 4.0f)
                    city_damage(under, 9999, v3(0, 0, 0), SRC_FLOP);
            }
            if(u >= 1) {
                kj.action = ACT_NONE;
                kj.y = 0;
                kj.squash = 1;
                /* What it lands among comes down; the shock beyond that
                   only cracks things (a house two doors along is left
                   standing, and burning). */
                int n = city_damage_circle(v3(kj.pos.x, 4.0f, kj.pos.z), 18.0f, 170.0f, SRC_STOMP);
                n += city_damage_circle(v3(kj.pos.x, 4.0f, kj.pos.z), 30.0f, 45.0f, SRC_STOMP);
                if(n >= 3) score_trick(TR_SHOCKWAVE, 1);
                if(kj.sink > 5.0f) {
                    /* In the sea: the splash goes ashore as a wave. */
                    kj.wave_c = kj.pos;
                    kj.wave_r = 12.0f;
                    kj.wave_from = city_standing();
                    for(int k = 0; k < 14; k++) {
                        float a = (float)k * (ENG_TAU / 14.0f);
                        fx_puff(vadd(kj.pos, v3(sinf(a) * 12, 3, cosf(a) * 12)), v3(sinf(a) * 20, 22, cosf(a) * 20), 10.0f, 0.9f, 0xffe4f6ff);
                    }
                }
                life_stomp(kj.pos, 13.0f);
                life_launch(kj.pos, 30.0f, 1.0f);
                fx_ring(kj.pos, 32.0f);
                fx_puffs(kj.pos, 14.0f, 10, 9.0f);
                fx_shake(1.3f);
                fx_hitstop(0.06f);
                audio_sfx(KL_SFX_CRASH_M, 220);
                audio_sfx(KL_SFX_STEP, 255);
                city_trample(kj.pos, 20.0f);
            }
            break;
        }
        case ACT_CHOMP:
            if(!kj.hit_done && kj.at >= 0.22f) {
                kj.hit_done = true;
                V3 ground = vmad(kj.pos, f, 19.0f);
                if(kj.want_grab) {
                    /* The button is still down: carry it. A vehicle or a
                       helicopter first, else a chunk out of a building. */
                    int h = life_grab(kj.mouth, ground, 18.0f);
                    if(!h) {
                        int tree = city_grab_tree(ground, 18.0f);
                        if(tree >= 0) h = TREE_HANDLE - tree;
                    }
                    if(!h) {
                        int chunk = city_rip(v3(ground.x, 26.0f, ground.z), 15.0f);
                        if(chunk >= 0) h = -1 - chunk;
                    }
                    if(h) {
                        /* Got it: a jolt, a puff, the HUD's shout. */
                        kj.held = h;
                        kj.hold_t = 0;
                        kj.grab_pop = 1.0f;
                        fx_ring(ground, 16.0f);
                        fx_puffs(ground, 8.0f, 5, 7.0f);
                        fx_shake(0.35f);
                        audio_sfx(KL_SFX_TRICK, 210);
                    }
                }
                if(kj.held) audio_sfx(KL_SFX_GULP, 120);
                else if(life_eat(kj.mouth, ground, 16.0f) != LIFE_NONE || city_eat(ground, 30.0f)) {
                    kj.energy = clampf(kj.energy + 0.22f, 0, 1);
                    audio_sfx(KL_SFX_GULP, 200);
                }
            }
            if(kj.at >= 0.5f) kj.action = ACT_NONE;
            break;
        case ACT_ROAR:
            if(!kj.hit_done && kj.at >= 0.1f) {
                kj.hit_done = true;
                audio_sfx(KL_SFX_ROAR, 255);
                score_roar();
                life_scare();
                fx_shake(0.9f);
                kj.roar_cd = 6.0f;
            }
            if(kj.at >= 1.3f) kj.action = ACT_NONE;
            break;
        default: break;
    }
    feet(dt);
    if(kj.breathing) breathe(dt);
}

/* ---- The pose ---- */
static inline float tri_wave(float u, float peak) {   /* 0 at 0 and 1, 1 at peak */
    return u <= 0 || u >= 1 ? 0 : u < peak ? u / peak : (1 - u) / (1 - peak);
}

static void leg(const Xf *root, int side, uint32_t plate) {
    const float A = KJ_THIGH_LEN, B = KJ_SHIN_LEN;
    V3 hip = xf_point(root, v3(side ? KJ_HIP_X : -KJ_HIP_X, 0, 0));
    V3 target = vadd(kj.foot[side].p, v3(0, KJ_ANKLE_H + kj.foot[side].lift + kj.y - kj.sink, 0));
    V3 d = vsub(target, hip);
    float dist = clampf(vlen(d), fabsf(A - B) + 0.2f, A + B - 0.05f);
    V3 dir = vnorm(d);
    /* The knee bends forward. */
    V3 fw = vnorm(vsub(root->z, vscale(dir, vdot(root->z, dir))));
    float a = (A * A - B * B + dist * dist) / (2 * dist), hgt = sqrtf(fmaxf(A * A - a * a, 0));
    V3 knee = vadd(vmad(hip, dir, a), vscale(fw, hgt)), ankle = vmad(hip, dir, dist);
    Xf x = xf_hang(hip, knee, root->z);
    cel_mesh_draw(&part[P_THIGH], &x, plate, kj.flash);
    x = xf_hang(knee, ankle, root->z);
    cel_mesh_draw(&part[P_SHIN], &x, plate, kj.flash);
    Xf yaw = xf_rot_y(kj.heading), pitch = xf_rot_x(kj.foot[side].planted ? 0 : sinf(kj.foot[side].t * ENG_PI) * 0.45f);
    x = xf_mul(&yaw, &pitch);
    x.p = ankle;
    cel_mesh_draw(&part[P_FOOT], &x, plate, kj.flash);
}

void kaiju_draw(void) {
    if(!r_sphere_visible(vadd(kj.pos, v3(0, 20 + kj.y, 0)), 50)) return;
    float u = 0;
    /* What the action adds to the pose. */
    float lean = 0.1f + kj.speed * 0.006f, nod = 0, twist = 0, spin = 0, jaw = 0, splay = 0.25f;
    float arm_u[2] = {-0.55f, -0.55f}, arm_f[2] = {-0.95f, -0.95f};
    float gait = (kj.foot[1].lift - kj.foot[0].lift) * 0.06f;
    arm_u[0] += gait;
    arm_u[1] -= gait;
    switch(kj.action) {
        case ACT_SWIPE: {
            u = kj.at / 0.44f;
            int s = kj.arm;
            float up = tri_wave(u, 0.38f);
            arm_u[s] = u < 0.38f ? lerpf(-0.55f, -3.0f, ease_out(u / 0.38f)) : lerpf(-3.0f, -0.3f, ease_out((u - 0.38f) / 0.62f));
            arm_f[s] = lerpf(-0.95f, -0.1f, up);
            twist = (s ? -1.0f : 1.0f) * (u < 0.38f ? -0.35f * u / 0.38f : lerpf(-0.35f, 0.5f, ease_out((u - 0.38f) / 0.3f)) * (1 - u));
            lean += 0.3f * sinf(u * ENG_PI);
            jaw = 0.4f * up;
            break;
        }
        case ACT_TAIL:
            u = kj.at / 0.62f;
            spin = ENG_TAU * ease_io(u);
            lean += 0.15f * sinf(u * ENG_PI);
            break;
        case ACT_JUMP:
            u = kj.at / 0.9f;
            arm_u[0] = arm_u[1] = lerpf(-0.55f, -2.6f, sinf(u * ENG_PI));
            lean += 0.25f * (u - 0.5f);
            jaw = 0.5f;
            break;
        case ACT_CHOMP:
            u = kj.at / 0.5f;
            lean += 0.75f * tri_wave(u, 0.44f);
            nod = 0.5f * tri_wave(u, 0.44f);
            jaw = u < 0.44f ? u / 0.44f : 0;
            arm_u[0] = arm_u[1] = 0.3f;
            break;
        case ACT_ROAR:
            u = kj.at / 1.3f;
            lean -= 0.3f * tri_wave(u, 0.2f);
            nod = -0.55f * tri_wave(u, 0.2f) + sinf(kj.at * 40.0f) * 0.03f;
            jaw = tri_wave(u, 0.15f) * 1.2f;
            splay = 0.25f + 0.8f * tri_wave(u, 0.2f);
            arm_u[0] = arm_u[1] = -0.9f;
            break;
        default: break;
    }
    if(kj.dashing) {
        /* Head down, arms back. */
        lean += 0.18f;
        nod = 0.3f;
        arm_u[0] = arm_u[1] = 0.7f;
    }
    if(kj.stagger > 0) lean -= 0.5f * kj.stagger;
    if(kj.held) jaw = 0.55f;
    if(kj.toss > 0) {
        nod -= 0.5f * kj.toss;
        jaw = 1;
    }
    if(kj.breathing) {
        jaw = 1;
        nod = 0.2f;
        lean += 0.12f;
    }
    kj.jaw = lerpf(kj.jaw, jaw, 0.4f);
    kj.look = lerpf(kj.look, clampf(kj.turn_rate * 0.16f, -0.5f, 0.5f), 0.15f);
    float breath = sinf(kj.clock * 2.0f) * 0.25f;
    float hips = KJ_HIP_Y - 1.3f - (kj.foot[0].lift + kj.foot[1].lift) * 0.1f - kj.squash * 3.0f + breath + kj.y - kj.sink;
    float sway = (kj.foot[1].lift - kj.foot[0].lift) * 0.014f;

    Xf root = xf_rot_y(kj.heading + spin), t = xf_rot_x(lean), r2 = xf_rot_z(sway);
    root = xf_mul(&root, &t);
    root = xf_mul(&root, &r2);
    root.p = v3(kj.pos.x, hips, kj.pos.z);
    /* The plates glow as the breath charges, and blaze while it fires. */
    uint32_t plate = kj.breathing ? ((int)(kj.clock * 30) & 1 ? 0xffffffff : 0xff90f0ff)
                                  : kj.energy < 0.6f ? color_lerp(0xffff8c3c, 0xffffd850, kj.energy / 0.6f)
                                                     : color_lerp(0xffffd850, 0xff70f0ff, (kj.energy - 0.6f) / 0.4f);

    Xf tw = xf_rot_y(twist), torso = xf_mul(&root, &tw);
    cel_mesh_draw(&part[P_TORSO], &torso, plate, kj.flash);

    Xf nt = xf_trans(v3(KJ_NECK_X, KJ_NECK_Y, KJ_NECK_Z)), ny = xf_rot_y(kj.look), nx = xf_rot_x(nod);
    Xf head = xf_mul(&torso, &nt);
    head = xf_mul(&head, &ny);
    head = xf_mul(&head, &nx);
    cel_mesh_draw(&part[P_HEAD], &head, plate, kj.flash);
    Xf jt = xf_trans(v3(KJ_JAW_X, KJ_JAW_Y, KJ_JAW_Z)), jr = xf_rot_x(kj.jaw * 0.6f), jw = xf_mul(&head, &jt);
    jw = xf_mul(&jw, &jr);
    cel_mesh_draw(&part[P_JAW], &jw, plate, kj.flash);
    kj.head = head.p;
    kj.mouth = xf_point(&head, v3(KJ_MOUTH_X, KJ_MOUTH_Y, KJ_MOUTH_Z));

    for(int s = 0; s < 2; s++) {
        float sx = s ? 1.0f : -1.0f;
        Xf st = xf_trans(v3(sx * KJ_SHOULDER_X, KJ_SHOULDER_Y, KJ_SHOULDER_Z)), sz = xf_rot_z(sx * splay), su = xf_rot_x(arm_u[s]);
        Xf a = xf_mul(&torso, &st);
        a = xf_mul(&a, &sz);
        a = xf_mul(&a, &su);
        cel_mesh_draw(&part[P_UPPER], &a, plate, kj.flash);
        Xf et = xf_trans(v3(0, -KJ_UPPER_ARM_LEN, 0)), ef = xf_rot_x(arm_f[s]);
        a = xf_mul(&a, &et);
        a = xf_mul(&a, &ef);
        cel_mesh_draw(&part[P_FORE], &a, plate, kj.flash);
        leg(&root, s, plate);
    }

    /* The tail: each segment lags the one before it into a turn. */
    static const float droop[KJ_TAIL_SEGS] = {-0.42f, -0.14f, 0.10f, 0.18f, 0.16f};
    Xf seg = xf_trans(v3(KJ_TAIL_ROOT_X, KJ_TAIL_ROOT_Y, KJ_TAIL_ROOT_Z));
    seg = xf_mul(&root, &seg);
    for(int i = 0; i < KJ_TAIL_SEGS; i++) {
        float want = kj.action == ACT_TAIL ? 0.22f : clampf(-kj.turn_rate * 0.11f, -0.42f, 0.42f) + sinf(kj.clock * 1.8f - (float)i * 0.9f) * 0.07f;
        kj.tail_a[i] = lerpf(kj.tail_a[i], want, 0.22f - 0.03f * (float)i);
        float pitch = kj.action == ACT_TAIL ? droop[i] * 0.2f + 0.05f : droop[i] + (kj.y > 0 ? 0.12f : 0);
        Xf ya = xf_rot_y(kj.tail_a[i]), pa = xf_rot_x(pitch);
        seg = xf_mul(&seg, &ya);
        seg = xf_mul(&seg, &pa);
        cel_mesh_draw(&part[P_TAIL0 + i], &seg, plate, kj.flash);
        Xf next = xf_trans(v3(0, 0, -kj_tail_len[i]));
        seg = xf_mul(&seg, &next);
    }
    r_load_vp();
}

void kaiju_draw_pt(void) {
    if(kj.sink > 1.0f) return;   /* no shadow on the sea */
    float r = 17.0f * (1.0f - fminf(kj.y, 30.0f) / 60.0f);
    RV v[4] = {{kj.pos.x - r, 0.14f, kj.pos.z - r, 0, 0, 0xffffffff}, {kj.pos.x + r, 0.14f, kj.pos.z - r, 1, 0, 0xffffffff},
               {kj.pos.x - r, 0.14f, kj.pos.z + r, 0, 1, 0xffffffff}, {kj.pos.x + r, 0.14f, kj.pos.z + r, 1, 1, 0xffffffff}};
    r_hdr(h_shadow);
    r_strip(v, 4);
}
