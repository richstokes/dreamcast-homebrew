/* The city's life (life.h). Everything on wheels keeps to the road grid:
   a vehicle is on road `road` running along axis 0 (east-west) or 1
   (north-south), `s` metres along it. */
#include "life.h"
#include "cel.h"
#include "city.h"
#include "fx.h"
#include "score.h"
#include "audio.h"
#include "kaiju.h"

enum { K_CAR, K_BUS, K_TANK };
enum { V_DRIVE, V_FLAT, V_FLY, V_GONE, V_HELD };
/* Traffic and crowd for the city's size: the numbers are for nine blocks a side. */
/* By the city's width, not its area: a big city is busy where the
   kaiju is, and every one of them is moved every frame. */
#define CARS (96 * 11 * CITY_N / 81)
#define BUSES (14 * 11 * CITY_N / 81)
#define TANKS 4
#define VEHICLES (CARS + BUSES + TANKS)
#define PEOPLE (280 * 11 * CITY_N / 81)
#define HELIS 3
#define JETS 2
#define MISSILES 14
#define LANE 4.6f

typedef struct {
    uint8_t kind, state, axis, road;
    int8_t dir;
    bool thrown;      /* by the kaiju, not knocked flying: it hits harder */
    float s, speed, t, turn_cd, fire, aim;
    uint32_t col;
    V3 p, v;
    float yaw, ax, wx, wy;
} Vehicle;
typedef struct {
    uint8_t axis, road;
    int8_t side, dir;
    float s, away;
    uint32_t col;
    bool run;
} Person;
typedef struct {
    bool active, news, falling, held, thrown;
    float ang, radius, alt, fire, back, wait, yaw, rotor, wx;
    V3 p, v;
} Heli;
typedef struct { bool active, fired, falling; V3 p, v; float yaw; } Jet;
typedef struct { bool active, shell; V3 p, v; float life, puff; } Missile;

static Vehicle veh[VEHICLES];
static Person people[PEOPLE];
static Heli heli[HELIS];
static Jet jet[JETS];
static Missile missile[MISSILES];
/* People picked up: in the jaws, or in the air. They always land on their feet. */
#define TOSSED 4
static struct { bool on, held; V3 p, v; uint32_t col; float rot; } tossed[TOSSED];
static float jet_wait;
static int pancakes;
static const pvr_poly_hdr_t *h_person;

static const uint32_t car_cols[8] = {0xffe85048, 0xff48a0e8, 0xfff8d048, 0xfff4f4f4, 0xff58c878, 0xffa070d8, 0xffff9048, 0xff40c8c8};
static const uint32_t shirt[6] = {0xffff6058, 0xff58a8ff, 0xffffe060, 0xffffffff, 0xff70e090, 0xffffa0d0};
static const uint32_t value_k[3] = {30, 250, 4000};

static inline float rnd(float a, float b) { return a + (b - a) * frand(); }
static inline int rndi(int n) { return (int)(eng_rand() % (uint32_t)n); }
static inline float along(const V3 p, int axis) { return axis ? p.z : p.x; }

static V3 road_pos(int axis, int road, float s, float lane) {
    return axis ? v3(city_road(road) + lane, 0, s) : v3(s, 0, city_road(road) - lane);
}

/* A spot on a road of this axis within a few blocks of the kaiju, so the
   streets it is walking down stay busy however big the city is. */
static void road_near_kaiju(int axis, uint8_t *road, float *s) {
    float across = axis ? kj.pos.x : kj.pos.z;
    int r = (int)floorf((across + CITY_HALF - CITY_ROAD * 0.5f) / CITY_PITCH + 0.5f) + rndi(13) - 6;
    *road = (uint8_t)(r < 0 ? 0 : r >= CITY_ROADS ? CITY_ROADS - 1 : r);
    *s = clampf(along(kj.pos, axis) + rnd(-520, 520), -CITY_HALF + 6, CITY_HALF - 6);
}

static void place_vehicle(Vehicle *v, bool far) {
    for(int tries = 0; tries < 8; tries++) {
        v->axis = (uint8_t)rndi(2);
        v->road = (uint8_t)rndi(CITY_ROADS);
        v->dir = rndi(2) ? 1 : -1;
        v->s = rnd(-CITY_HALF, CITY_HALF);
        if(far) road_near_kaiju(v->axis, &v->road, &v->s);
        v->p = road_pos(v->axis, v->road, v->s, LANE * (float)v->dir);
        if(!far || dist2_xz(v->p, kj.pos) > 220.0f * 220.0f) break;
    }
    v->state = V_DRIVE;
    v->speed = v->kind == K_BUS ? rnd(9, 12) : rnd(12, 20);
    v->turn_cd = 0;
    v->t = 0;
}

void life_init(void) {
    h_person = r_header(TEX_PERSON, HF_PT | HF_CLAMP);
}

void life_reset(void) {
    memset(veh, 0, sizeof(veh));
    memset(heli, 0, sizeof(heli));
    memset(jet, 0, sizeof(jet));
    memset(missile, 0, sizeof(missile));
    memset(tossed, 0, sizeof(tossed));
    for(int i = 0; i < VEHICLES; i++) {
        Vehicle *v = &veh[i];
        v->kind = i < CARS ? K_CAR : i < CARS + BUSES ? K_BUS : K_TANK;
        v->col = v->kind == K_BUS ? (i & 1 ? 0xffffc040 : 0xffe85048) : car_cols[rndi(8)];
        if(v->kind == K_TANK) {
            v->state = V_GONE;
            v->t = 1e9f;
        }
        else place_vehicle(v, false);
    }
    for(int i = 0; i < PEOPLE; i++) {
        Person *p = &people[i];
        p->axis = (uint8_t)rndi(2);
        p->road = (uint8_t)rndi(CITY_ROADS);
        p->side = rndi(2) ? 1 : -1;
        p->dir = rndi(2) ? 1 : -1;
        p->s = rnd(-CITY_HALF, CITY_HALF);
        p->col = shirt[rndi(6)];
        p->away = 0;
    }
    /* The news is there from the start. */
    heli[0] = (Heli){.active = true, .news = true, .ang = 1.0f, .radius = 78, .alt = 44, .p = v3(200, 60, -300)};
    for(int i = 1; i < HELIS; i++) heli[i].wait = 1e9f;
    jet_wait = 14;
    pancakes = 0;
}

/* The army comes for the bill, not the body count: a street of houses is
   nothing, a tower is a lot. Thousands of dollars of damage done, before
   any multiplier, for each step up. */
int life_threat(void) {
    static const uint64_t steps[LIFE_THREAT_MAX] = {250000, 1500000, 4000000, 8000000};
    int t = 0;
    while(t < LIFE_THREAT_MAX && score.damage >= steps[t]) t++;
    return t;
}

/* ---- What the kaiju does to them ---- */
static void wreck(Vehicle *v, int src, bool trick) {
    score_hit(src, value_k[v->kind], v->p);
    score.vehicles++;
    if(trick && v->kind == K_TANK) score_trick(TR_TANK_YOU, 1);
}

int life_stomp(V3 p, float r) {
    int n = 0;
    for(int i = 0; i < VEHICLES; i++) {
        Vehicle *v = &veh[i];
        if(v->state != V_DRIVE || dist2_xz(v->p, p) > r * r) continue;
        v->state = V_FLAT;
        v->t = 7;
        n++;
        wreck(v, SRC_RAM, true);
        fx_boom(vadd(v->p, v3(0, 3, 0)), 6);
        audio_at(KL_SFX_SQUISH, v->p, 200);
        /* A trick for the first, then every third. */
        if(pancakes++ % 3 == 0) score_trick(TR_PANCAKE, 1);
    }
    for(int i = 0; i < PEOPLE; i++) {
        Person *q = &people[i];
        if(q->away > 0) continue;
        V3 pp = road_pos(q->axis, q->road, q->s, 12.5f * (float)q->side);
        if(dist2_xz(pp, p) < r * r) q->away = 5;   /* dived into a doorway */
    }
    return n;
}

int life_launch(V3 c, float r, float power) {
    int n = 0;
    for(int i = 0; i < VEHICLES; i++) {
        Vehicle *v = &veh[i];
        if(v->state != V_DRIVE || dist2_xz(v->p, c) > r * r) continue;
        if(v->kind == K_TANK && power < 1.0f) continue;
        V3 d = vsub(v->p, c);
        d.y = 0;
        float l = vlen(d);
        d = l > 0.5f ? vscale(d, 1.0f / l) : v3(frand_s(), 0, frand_s());
        float sp = (40.0f + frand() * 30.0f) * power;
        v->state = V_FLY;
        v->v = v3(d.x * sp, (38.0f + frand() * 22.0f) * power, d.z * sp);
        v->p.y = 2;
        v->wx = frand_s() * 9;
        v->wy = frand_s() * 9;
        v->ax = 0;
        n++;
        audio_at(KL_SFX_HORN, v->p, 120);
    }
    return n;
}

static void heli_down(Heli *h, V3 push) {
    h->falling = true;
    h->v = vadd(push, v3(0, 12, 0));
    h->wx = 5 + frand() * 4;
    fx_boom(h->p, 12);
    audio_at(KL_SFX_EXPLODE, h->p, 190);
}

int life_swat(V3 c, V3 fwd, float reach) {
    int n = 0;
    for(int i = 0; i < HELIS; i++) {
        Heli *h = &heli[i];
        if(!h->active || h->falling) continue;
        V3 d = vsub(h->p, c);
        float l = sqrtf(d.x * d.x + d.z * d.z);
        if(l > reach || h->p.y > 62 || (d.x * fwd.x + d.z * fwd.z) < l * 0.2f) continue;
        heli_down(h, vscale(fwd, 46));
        score_hit(SRC_CLAW, h->news ? 3000 : 6000, h->p);
        score_trick(TR_SWATTED, 1);
        score.vehicles++;
        n++;
    }
    return n;
}

int life_eat(V3 mouth, V3 ground, float r) {
    /* A helicopter at the head first, then whatever is in the road. */
    for(int i = 0; i < HELIS; i++) {
        Heli *h = &heli[i];
        if(!h->active || h->falling || vlen(vsub(h->p, mouth)) > r + 12) continue;
        h->active = false;
        h->wait = 14;
        score_hit(SRC_EAT, h->news ? 6000 : 12000, h->p);
        score_trick(h->news ? TR_BREAKING_NEWS : TR_AIR_MEAL, 1);
        score.eaten++;
        fx_boom(h->p, 9);
        return h->news ? LIFE_NEWS : LIFE_HELI;
    }
    int best = -1;
    float bd = r * r;
    for(int i = 0; i < VEHICLES; i++) {
        if(veh[i].state != V_DRIVE && veh[i].state != V_FLAT) continue;
        float d = dist2_xz(veh[i].p, ground);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    if(best < 0) return LIFE_NONE;
    Vehicle *v = &veh[best];
    v->state = V_GONE;
    v->t = v->kind == K_TANK ? 16 : 5;
    score_hit(SRC_EAT, value_k[v->kind] * 2, v->p);
    score_trick(v->kind == K_TANK ? TR_TANK_YOU : v->kind == K_BUS ? TR_BUS_LUNCH : TR_FAST_FOOD, 1);
    score.eaten++;
    fx_boom(vadd(v->p, v3(0, 4, 0)), 7);
    return v->kind == K_TANK ? LIFE_TANK : v->kind == K_BUS ? LIFE_BUS : LIFE_CAR;
}

#define HELI_HANDLE 1000
#define PERSON_HANDLE 2000
int life_grab(V3 mouth, V3 ground, float r) {
    for(int i = 0; i < HELIS; i++) {
        Heli *h = &heli[i];
        if(!h->active || h->falling || h->held || vlen(vsub(h->p, mouth)) > r + 12) continue;
        h->held = true;
        return HELI_HANDLE + i;
    }
    int best = -1;
    float bd = r * r;
    for(int i = 0; i < VEHICLES; i++) {
        if(veh[i].state != V_DRIVE && veh[i].state != V_FLAT) continue;
        float d = dist2_xz(veh[i].p, ground);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    if(best >= 0) {
        veh[best].state = V_HELD;
        return 1 + best;
    }
    /* Nothing on wheels: somebody off the pavement. */
    int slot = -1;
    for(int i = 0; i < TOSSED; i++)
        if(!tossed[i].on) slot = i;
    for(int i = 0; i < PEOPLE && slot >= 0; i++) {
        Person *q = &people[i];
        if(q->away > 0) continue;
        V3 pp = road_pos(q->axis, q->road, q->s, 12.5f * (float)q->side);
        float d = dist2_xz(pp, ground);
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    if(best < 0) return 0;
    people[best].away = 10;
    tossed[slot].on = tossed[slot].held = true;
    tossed[slot].col = people[best].col;
    tossed[slot].rot = 0;
    return PERSON_HANDLE + slot;
}

const char *life_handle_name(int handle) {
    if(handle >= PERSON_HANDLE) return "SOMEBODY";
    if(handle >= HELI_HANDLE) return "A HELICOPTER";
    int kind = veh[handle - 1].kind;
    return kind == K_TANK ? "A TANK" : kind == K_BUS ? "A BUS" : "A CAR";
}

void life_hold(int handle, V3 p, float yaw) {
    if(handle >= PERSON_HANDLE) {
        tossed[handle - PERSON_HANDLE].p = v3(p.x, p.y - 3.0f, p.z);
        return;
    }
    if(handle >= HELI_HANDLE) {
        Heli *h = &heli[handle - HELI_HANDLE];
        h->p = v3(p.x, p.y - 2.0f, p.z);
        h->yaw = yaw + 1.2f;
        return;
    }
    Vehicle *v = &veh[handle - 1];
    v->p = v3(p.x, p.y - 2.5f, p.z);
    v->yaw = yaw + ENG_PI * 0.5f;   /* crosswise in the jaws */
    v->ax = 0;
}

void life_throw(int handle, V3 vel) {
    if(handle >= PERSON_HANDLE) {
        tossed[handle - PERSON_HANDLE].held = false;
        tossed[handle - PERSON_HANDLE].v = vel;
        return;
    }
    if(handle >= HELI_HANDLE) {
        Heli *h = &heli[handle - HELI_HANDLE];
        h->held = false;
        h->falling = h->thrown = true;
        score_hit(SRC_THROWN, h->news ? 3000 : 6000, h->p);
        score.vehicles++;
        h->v = vel;
        h->wx = 7;
        return;
    }
    Vehicle *v = &veh[handle - 1];
    v->state = V_FLY;
    v->thrown = true;
    v->v = vel;
    v->wx = 8;
    v->wy = frand_s() * 4;
}

static float line_dist2(V3 p, V3 o, V3 d, float len) {
    V3 r = vsub(p, o);
    float t = clampf(vdot(r, d), 0, len);
    V3 q = vsub(r, vscale(d, t));
    return vdot(q, q);
}

int life_beam(V3 o, V3 d, float len) {
    int n = 0;
    for(int i = 0; i < HELIS; i++) {
        Heli *h = &heli[i];
        if(!h->active || h->falling || line_dist2(h->p, o, d, len) > 9 * 9) continue;
        heli_down(h, vscale(d, 30));
        score_hit(SRC_BEAM, h->news ? 3000 : 6000, h->p);
        score.vehicles++;
        n++;
    }
    for(int i = 0; i < JETS; i++) {
        Jet *j = &jet[i];
        if(!j->active || j->falling || line_dist2(j->p, o, d, len + 120) > 16 * 16) continue;
        j->falling = true;
        fx_boom(j->p, 18);
        audio_at(KL_SFX_EXPLODE, j->p, 220);
        score_hit(SRC_BEAM, 30000, j->p);
        score_trick(TR_TOP_GUN, 1);
        score.vehicles++;
        n++;
    }
    for(int i = 0; i < VEHICLES; i++) {
        Vehicle *v = &veh[i];
        if(v->state != V_DRIVE || line_dist2(vadd(v->p, v3(0, 2, 0)), o, d, len) > 6 * 6) continue;
        v->state = V_GONE;
        v->t = v->kind == K_TANK ? 16 : 5;
        fx_boom(vadd(v->p, v3(0, 3, 0)), 9);
        audio_at(KL_SFX_EXPLODE, v->p, 150);
        wreck(v, SRC_BEAM, false);
        n++;
    }
    return n;
}

void life_crush_line(V3 p, float dx, float dz, float len, float half_w) {
    for(int i = 0; i < VEHICLES; i++) {
        Vehicle *v = &veh[i];
        if(v->state != V_DRIVE) continue;
        float rx = v->p.x - p.x, rz = v->p.z - p.z, s = rx * dx + rz * dz;
        if(s < 0 || s > len || fabsf(rx * dz - rz * dx) > half_w + 2) continue;
        v->state = V_GONE;
        v->t = 8;
        wreck(v, SRC_DOMINO, false);
    }
    for(int i = 0; i < PEOPLE; i++) {
        Person *q = &people[i];
        V3 pp = road_pos(q->axis, q->road, q->s, 12.5f * (float)q->side);
        float rx = pp.x - p.x, rz = pp.z - p.z, s = rx * dx + rz * dz;
        if(s >= 0 && s <= len && fabsf(rx * dz - rz * dx) < half_w + 3) q->away = 6;
    }
}

#ifdef KL_FILM
#include "film.h"
void life_film_jam(V3 c, int n) {
    /* The nearest roads to c, nose to tail both ways. */
    int ri = (int)floorf((c.x + CITY_HALF - CITY_ROAD * 0.5f) / CITY_PITCH + 0.5f), rj = (int)floorf((c.z + CITY_HALF - CITY_ROAD * 0.5f) / CITY_PITCH + 0.5f);
    for(int i = 0; i < n && i < CARS + BUSES; i++) {
        Vehicle *v = &veh[i < n / 6 ? CARS + i : i];
        v->axis = (uint8_t)(i & 1);
        v->road = (uint8_t)(v->axis ? ri : rj);
        v->dir = (i & 2) ? 1 : -1;
        v->s = (v->axis ? c.z : c.x) + (float)((i >> 2) - n / 8) * 11.0f + 4.0f;
        v->state = V_DRIVE;
        v->speed = 3;
        v->p = road_pos(v->axis, v->road, v->s, LANE * (float)v->dir);
    }
}

void life_film_heli(V3 p) {
    heli[0].active = heli[0].news = true;
    heli[0].falling = false;
    heli[0].p = p;
    heli[0].radius = 12;
    heli[0].alt = p.y;
}
#endif

void life_scare(void) {
    for(int i = 0; i < HELIS; i++) heli[i].back = 3.5f;
}

/* ---- Running it ---- */
static void fire(V3 from, bool shell) {
    for(int i = 0; i < MISSILES; i++) {
        Missile *m = &missile[i];
        if(m->active) continue;
        V3 to = vadd(kj.pos, v3(0, 22 + kj.y, 0));
        *m = (Missile){true, shell, from, vscale(vnorm(vsub(to, from)), shell ? 120.0f : 62.0f), 6, 0};
        audio_at(KL_SFX_SHOT, from, shell ? 200 : 130);
        return;
    }
}

static void vehicle_drive(Vehicle *v, float dt) {
    float before = v->s;
    bool tank = v->kind == K_TANK;
    float kd = along(kj.pos, v->axis) - v->s, perp = (v->axis ? kj.pos.x : kj.pos.z) - city_road(v->road);
    float d2 = dist2_xz(v->p, kj.pos);
    float speed = v->speed;
    if(tank) {
        /* Close in along the grid, then stand off and shoot. */
        v->dir = kd >= 0 ? 1 : -1;
        if(d2 < 100.0f * 100.0f) {
            speed = 0;
            v->aim = atan2f(kj.pos.x - v->p.x, kj.pos.z - v->p.z);
            if((v->fire -= dt) <= 0) {
                v->fire = 3.0f + frand() * 1.5f;
                fire(vadd(v->p, v3(sinf(v->aim) * 9, 4, cosf(v->aim) * 9)), true);
            }
        }
        else v->aim = v->yaw;
    }
    else if(d2 < 75.0f * 75.0f && kd * (float)v->dir > 0 && fabsf(perp) < 40) {
        /* The kaiju is up the road: turn round and floor it. */
        v->dir = (int8_t)-v->dir;
        v->speed = 30;
        if(frand() < 0.3f) audio_at(KL_SFX_HORN, v->p, 110);
    }
    v->s += speed * (float)v->dir * dt;
    if(v->s > CITY_HALF - 6 || v->s < -CITY_HALF + 6) {
        v->s = clampf(v->s, -CITY_HALF + 6, CITY_HALF - 6);
        v->dir = (int8_t)-v->dir;
    }
    /* At a crossing: carry on or turn. */
    if(v->turn_cd > 0) v->turn_cd -= dt;
    int k0 = (int)floorf((before + CITY_HALF - CITY_ROAD * 0.5f) / CITY_PITCH + 0.5f);
    float cross = city_road(k0);
    if(v->turn_cd <= 0 && k0 >= 0 && k0 < CITY_ROADS && (before - cross) * (v->s - cross) <= 0 && speed > 0) {
        bool turn = tank ? fabsf(perp) > 45 : frand() < 0.35f;
        if(turn) {
            float here = city_road(v->road);
            v->axis ^= 1;
            v->road = (uint8_t)k0;
            v->s = here;
            v->dir = tank ? (perp >= 0 ? 1 : -1) : (rndi(2) ? 1 : -1);
            v->turn_cd = 1.0f;
        }
    }
    v->p = road_pos(v->axis, v->road, v->s, LANE * (float)v->dir);
    float want = v->axis ? (v->dir > 0 ? 0 : ENG_PI) : (v->dir > 0 ? ENG_PI * 0.5f : -ENG_PI * 0.5f);
    v->yaw += wrap_angle(want - v->yaw) * smooth_k(8, dt);
}

static void vehicle_fly(Vehicle *v, float dt) {
    v->v.y -= 62.0f * dt;
    v->p = vmad(v->p, v->v, dt);
    v->ax += v->wx * dt;
    v->yaw += v->wy * dt;
    int b = city_at(v->p.x, v->p.z);
    bool into = b >= 0 && v->p.y < city_b[b].h;
    if(v->thrown) {
        /* A thrown one takes a helicopter out of the air on its way. */
        for(int i = 0; i < HELIS; i++) {
            Heli *h = &heli[i];
            if(!h->active || h->falling || h->held || vlen(vsub(h->p, v->p)) > 13) continue;
            heli_down(h, vscale(v->v, 0.4f));
            score_hit(SRC_THROWN, h->news ? 3000 : 6000, h->p);
            score_trick(TR_AIR_MAIL, 1);
            score.vehicles++;
            into = true;
            b = -1;
        }
    }
    if(!into && v->p.y > 1) return;
    if(into && b >= 0) {
        static const float hit[3] = {60, 160, 240};
        city_damage_at(b, hit[v->kind] * (v->thrown ? 3.0f : 1.0f), v->v, SRC_THROWN, v->p);
        score_trick(TR_DELIVERY, 1);
    }
    fx_boom(vadd(v->p, v3(0, 3, 0)), v->kind == K_CAR ? 9 : 14);
    fx_puffs(v->p, 4, 3, 6);
    audio_at(KL_SFX_EXPLODE, v->p, 150);
    wreck(v, SRC_THROWN, false);
    v->thrown = false;
    v->state = V_GONE;
    v->t = v->kind == K_TANK ? 16 : 5;
}

void life_update(float dt) {
    int threat = life_threat();
    /* A tank a step; gunships from the second; jets at the last. */
    int tanks_wanted = threat < TANKS ? threat : TANKS, tanks = 0;
    for(int i = 0; i < VEHICLES; i++) {
        Vehicle *v = &veh[i];
        if(v->kind == K_TANK && v->state != V_GONE) tanks++;
    }
    for(int i = 0; i < VEHICLES; i++) {
        Vehicle *v = &veh[i];
        switch(v->state) {
            case V_DRIVE: vehicle_drive(v, dt); break;
            case V_FLY: vehicle_fly(v, dt); break;
            case V_FLAT:
                if((v->t -= dt) <= 0) {
                    v->state = V_GONE;
                    v->t = 3;
                }
                break;
            default:
                if(v->kind == K_TANK) {
                    /* The army sends another, in from the edge of town. */
                    if(v->t > 100 && tanks < tanks_wanted) v->t = 2 + frand() * 4;
                    if(v->t <= 100 && (v->t -= dt) <= 0 && tanks < tanks_wanted) {
                        v->axis = (uint8_t)rndi(2);
                        v->road = (uint8_t)rndi(CITY_ROADS);
                        v->s = rndi(2) ? CITY_HALF - 8 : -CITY_HALF + 8;
                        v->dir = v->s > 0 ? -1 : 1;
                        v->speed = 11;
                        v->state = V_DRIVE;
                        v->fire = 2;
                        v->turn_cd = 0;
                        v->p = road_pos(v->axis, v->road, v->s, LANE * (float)v->dir);
                        tanks++;
                    }
                    else if(v->t <= 0) v->t = 1e9f;
                }
                else if((v->t -= dt) <= 0) place_vehicle(v, true);
                break;
        }
    }

    for(int i = 0; i < TOSSED; i++) {
        typeof(tossed[0]) *q = &tossed[i];
        if(!q->on || q->held) continue;
        q->v.y -= 55.0f * dt;
        q->p = vmad(q->p, q->v, dt);
        q->rot += 9.0f * dt;
        int b = city_at(q->p.x, q->p.z);
        if(q->p.y > 0 && !(b >= 0 && q->p.y < city_b[b].h)) continue;
        /* Down, dusted off and gone. */
        q->on = false;
        fx_puff(q->p, v3(0, 5, 0), 3.5f, 0.5f, 0xffffffff);
    }
    /* The crowd. */
    int running = 0;
    for(int i = 0; i < PEOPLE; i++) {
        Person *q = &people[i];
        if(q->away > 0) {
            if((q->away -= dt) <= 0) road_near_kaiju(q->axis, &q->road, &q->s);
            continue;
        }
        V3 pp = road_pos(q->axis, q->road, q->s, 12.5f * (float)q->side);
        float d2 = dist2_xz(pp, kj.pos);
        q->run = d2 < 120.0f * 120.0f;
        if(q->run) {
            running++;
            q->dir = q->s - along(kj.pos, q->axis) >= 0 ? 1 : -1;
            if(d2 < 10.0f * 10.0f) q->away = 5;
        }
        q->s += (q->run ? 15.0f : 2.5f) * (float)q->dir * dt;
        if(q->s > CITY_HALF - 2 || q->s < -CITY_HALF + 2) {
            q->s = clampf(q->s, -CITY_HALF + 2, CITY_HALF - 2);
            q->dir = (int8_t)-q->dir;
        }
    }
    audio_crowd = clampf((float)running / 30.0f, 0, 1);

    /* Helicopters circle at head height. */
    int helis_wanted = 1 + (threat >= 2) + (threat >= 3);
    float near = 0;
    for(int i = 0; i < HELIS; i++) {
        Heli *h = &heli[i];
        if(!h->active) {
            if(h->wait > 100 && i < helis_wanted) h->wait = 3 + frand() * 3;
            if(h->wait <= 100 && (h->wait -= dt) <= 0) {
                float a = frand() * ENG_TAU;
                *h = (Heli){.active = true, .news = i == 0, .ang = a, .radius = i == 0 ? 78 : 60 + 8 * (float)i,
                            .alt = i == 0 ? 44 : 32 + 5 * (float)i, .fire = 3,
                            .p = vadd(kj.pos, v3(sinf(a) * 500, 90, cosf(a) * 500))};
            }
            continue;
        }
        h->rotor += dt * 30;
        if(h->falling) {
            h->v.y -= 45.0f * dt;
            h->p = vmad(h->p, h->v, dt);
            h->yaw += h->wx * dt;
            int b = city_at(h->p.x, h->p.z);
            bool into = b >= 0 && h->p.y < city_b[b].h;
            if(into || h->p.y < 2) {
                if(into) {
                    city_damage_at(b, h->thrown ? 480 : 220, h->v, SRC_THROWN, h->p);
                    score_trick(TR_DELIVERY, 1);
                }
                fx_boom(vadd(h->p, v3(0, 4, 0)), 20);
                fx_puffs(h->p, 6, 5, 8);
                fx_debris(h->p, 5, 30, 0xff708a54, 1.6f);
                audio_at(KL_SFX_EXPLODE, h->p, 230);
                h->active = false;
                h->wait = 12;
            }
            continue;
        }
        if(h->held) continue;
        if(h->back > 0) h->back -= dt;
        float radius = h->radius + (h->back > 0 ? 60 : 0);
        h->ang += (i & 1 ? -1.0f : 1.0f) * 18.0f / radius * dt;
        V3 want = vadd(kj.pos, v3(sinf(h->ang) * radius, h->alt + sinf(h->ang * 3) * 4, cosf(h->ang) * radius));
        V3 step = vsub(want, h->p);
        float l = vlen(step), sp = fminf(l * 1.6f, 60.0f);
        if(l > 0.01f) h->p = vmad(h->p, step, sp * dt / l);
        h->yaw += wrap_angle(atan2f(kj.pos.x - h->p.x, kj.pos.z - h->p.z) - h->yaw) * smooth_k(4, dt);
        float d = sqrtf(dist2_xz(h->p, kj.pos));
        if(d < 160) near = 1;
        if(!h->news && d < 110 && (h->fire -= dt) <= 0) {
            h->fire = 3.2f + frand() * 2;
            fire(vadd(h->p, v3(0, -3, 0)), false);
        }
    }
    audio_heli = near;
    audio_siren = threat > 0 ? 1 : 0;

    /* Jets, once it has all gone too far. */
    if(threat >= LIFE_THREAT_MAX && (jet_wait -= dt) <= 0) {
        jet_wait = 16 + frand() * 8;
        float a = frand() * ENG_TAU;
        V3 dir = v3(sinf(a), 0, cosf(a)), side = v3(dir.z, 0, -dir.x);
        for(int i = 0; i < JETS; i++) {
            jet[i] = (Jet){true, false, false, vadd(vmad(vmad(kj.pos, dir, -900), side, 30 * (float)i - 15), v3(0, 78, 0)),
                           vscale(dir, 190), a};
        }
    }
    for(int i = 0; i < JETS; i++) {
        Jet *j = &jet[i];
        if(!j->active) continue;
        if(j->falling) j->v.y -= 60 * dt;
        j->p = vmad(j->p, j->v, dt);
        float d2 = dist2_xz(j->p, kj.pos);
        if(!j->fired && !j->falling && d2 < 300.0f * 300.0f) {
            j->fired = true;
            fire(vadd(j->p, v3(0, -3, 0)), false);
        }
        if(j->p.y < 2) {
            fx_boom(j->p, 24);
            audio_at(KL_SFX_EXPLODE, j->p, 220);
            int b = city_at(j->p.x, j->p.z);
            if(b >= 0) city_damage(b, 300, j->v, SRC_THROWN);
            j->active = false;
        }
        else if(j->fired && d2 > 1000.0f * 1000.0f) j->active = false;
    }

    for(int i = 0; i < MISSILES; i++) {
        Missile *m = &missile[i];
        if(!m->active) continue;
        V3 to = vadd(kj.pos, v3(0, 22 + kj.y, 0)), d = vsub(to, m->p);
        float dist = vlen(d);
        if(!m->shell) {
            /* Missiles curve in. */
            float sp = vlen(m->v);
            m->v = vscale(vnorm(vmad(m->v, d, 2.2f * dt * sp / fmaxf(dist, 1))), sp);
        }
        m->p = vmad(m->p, m->v, dt);
        if((m->puff -= dt) <= 0) {
            m->puff = 0.05f;
            fx_puff(m->p, v3(0, 1, 0), m->shell ? 1.6f : 2.4f, 0.45f, 0xffe8e4f0);
        }
        if(dist < 10) {
            fx_boom(m->p, 11);
            audio_at(KL_SFX_EXPLODE, m->p, 150);
            kaiju_hurt();
            m->active = false;
        }
        else if((m->life -= dt) <= 0 || m->p.y < 0) {
            fx_boom(m->p, 7);
            m->active = false;
        }
    }
}

/* ---- Drawing ---- */
void life_draw_op(void) {
    V3 cp = r_cam.pos;
    for(int i = 0; i < VEHICLES; i++) {
        const Vehicle *v = &veh[i];
        if(v->state == V_GONE) continue;
        float d2 = dist2_xz(v->p, cp);
        if(d2 > 420.0f * 420.0f || !r_sphere_visible(v->p, 10)) continue;
        Xf x = xf_rot_y(v->yaw);
        if(v->state == V_FLY) {
            Xf rx = xf_rot_x(v->ax);
            x = xf_mul(&x, &rx);
        }
        x.p = v->p;
        int fm = v->kind == K_CAR ? FM_CAR : v->kind == K_BUS ? FM_BUS : FM_TANK;
        if(v->state == V_FLAT) {
            /* A pancake. */
            float m[16] __attribute__((aligned(32)));
            xf_m4(&x, m);
            m4_scale_apply(m, 1.25f, 0.1f, 1.15f);
            flat_mesh_draw_m(&cel_flat[fm], m, xf_local(&x, cel_sun), v->col, false);
            continue;
        }
        flat_mesh_draw(&cel_flat[fm], &x, v->col, d2 < 200.0f * 200.0f);
        if(v->kind == K_TANK && v->state == V_DRIVE) {
            Xf t = xf_rot_y(v->aim);
            t.p = vadd(v->p, v3(0, 2.7f, 0));
            flat_mesh_draw(&cel_flat[FM_TURRET], &t, 0, false);
        }
    }
    for(int i = 0; i < HELIS; i++) {
        const Heli *h = &heli[i];
        if(!h->active || !r_sphere_visible(h->p, 14)) continue;
        Xf x = xf_rot_y(h->yaw), tilt = xf_rot_x(h->falling ? 0.5f : 0.12f);
        x = xf_mul(&x, &tilt);
        x.p = h->p;
        flat_mesh_draw(&cel_flat[FM_HELI], &x, h->news ? 0xfff4f4f4 : 0xff708a54, true);
        Xf r = xf_rot_y(h->rotor), up = xf_trans(v3(0, 3.0f, 0.4f));
        up = xf_mul(&x, &up);
        r = xf_mul(&up, &r);
        flat_mesh_draw(&cel_flat[FM_ROTOR], &r, 0, false);
    }
    for(int i = 0; i < JETS; i++) {
        const Jet *j = &jet[i];
        if(!j->active || !r_sphere_visible(j->p, 16)) continue;
        Xf x = xf_rot_y(j->yaw);
        x.p = j->p;
        flat_mesh_draw(&cel_flat[FM_JET], &x, 0, true);
    }
    for(int i = 0; i < MISSILES; i++) {
        const Missile *m = &missile[i];
        if(!m->active || !r_sphere_visible(m->p, 4)) continue;
        V3 f = vnorm(m->v);
        Xf x;
        x.z = f;
        x.x = vnorm(vcross(v3(0, 1, 0), f));
        x.y = vcross(x.z, x.x);
        x.p = m->p;
        flat_mesh_draw(&cel_flat[FM_MISSILE], &x, 0, false);
    }
    r_load_vp();
}

void life_draw_pt(void) {
    V3 cp = r_cam.pos;
    r_hdr(h_person);
    for(int i = 0; i < PEOPLE; i++) {
        const Person *q = &people[i];
        if(q->away > 0) continue;
        V3 p = road_pos(q->axis, q->road, q->s, 12.5f * (float)q->side);
        if(dist2_xz(p, cp) > 300.0f * 300.0f || !r_sphere_visible(p, 4)) continue;
        p.y = q->run ? fabsf(sinf(game.time * 16.0f + (float)i)) * 0.9f : 0;
        r_billboard_y(p, 1.5f, 4.2f, q->dir > 0 ? 0 : 1, 0, q->dir > 0 ? 1 : 0, 1, q->col);
    }
    /* Whoever is in the jaws or in the air: drawn large enough to see up there. */
    for(int i = 0; i < TOSSED; i++)
        if(tossed[i].on) r_billboard(tossed[i].p, 2.2f, 4.4f, tossed[i].rot, 0, 0, 1, 1, tossed[i].col);
}
