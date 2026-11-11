/* Ridgeline CPU drivers: each follows the forest road to the fork, then
   its own route up to the lake and the summit trail, with pure-pursuit
   steering, a speed that drops for bends, grade and rough routes, low
   range on the hard parts, and backing off and trying again when stuck.

   At the summit a driver stops for a while, turns round and comes back
   down by another of the three routes to the foot of the forest road,
   then turns again and climbs a different way, for as long as the drive
   lasts. A trip is one way up or down: three legs, the routes followed
   backwards on the way down. Trucks meeting on a trail keep right. */
#include "ai.h"
#include "truck.h"
#include "terrain.h"
#include "world.h"
#include <stdlib.h>

typedef struct {
    int mid, leg;           /* the route between the fork and the lake this trip; 0 to 2 along it */
    int dir;                /* 1 climbing, -1 coming down */
    int trips;              /* ends reached: the summit first */
    int route, pt;
    float stuck_t, reverse_t, flip_t;
    int retries;
    float start_t, delay;   /* CPUs set off one after another */
    float wait_t, mode_t, push, lost_t, progress_t;
    int best_at;
    float shift;            /* metres right of the route's line: passing, pulling over */
    int turn, turn_side;    /* turning round: TURN_*, and the lock it noses in on (0: not chosen) */
    float turn_t, turn_all, block_t;
} Ai;

enum { TURN_NONE, TURN_NOSE_IN, TURN_BACK_OUT };

static Ai ai[MAX_TRUCKS];

/* Test builds can give the player's CPU driver the same guide. */
static inline bool getenv_assist(void) {
#if defined(RL_AUTOTEST) || defined(RL_AI_TRACE) || defined(RL_FILM)
    return true;
#else
    return false;
#endif
}

/* Route indices in level.h's order. */
enum { R_FOREST, R_LOGGING, R_RIDGE, R_CREEK, R_SUMMIT };

/* The route of the leg a driver is on. */
static int leg_route(const Ai *a) {
    if(a->leg == 1) return a->mid;
    return (a->leg == 0) == (a->dir > 0) ? R_FOREST : R_SUMMIT;
}

/* Points still to go on this leg, and how far the driver has come since
   the drive began, for noticing one that has stopped getting anywhere. */
static int pts_left(const Ai *a, const lvl_route_t *r) {
    return a->dir > 0 ? r->n - 1 - a->pt : a->pt;
}

static int progress_at(const Ai *a, const lvl_route_t *r) {
    return a->trips * 16384 + a->leg * 4096 + (r->n - 1 - pts_left(a, r));
}

/* 0 to n - 1. Stirred with where the truck is, which no two drives repeat. */
static int pick(const Truck *t, int n) {
    static uint32_t seed = 0x2545f491;
    union { float f; uint32_t u; } x = {.f = t->pos.x}, z = {.f = t->pos.z};
    seed = (seed ^ x.u ^ (z.u << 7)) * 1664525u + 1013904223u;
    return (int)((seed >> 16) % (uint32_t)n);
}

void ai_start(int ti, int route) {
    Ai *a = &ai[ti];
    memset(a, 0, sizeof(*a));
    a->mid = route;
    a->dir = 1;
    a->leg = 0;
    a->route = R_FOREST;
    a->pt = 0;
    a->delay = ti * 4.0f;
}

#ifdef RL_FILM
void ai_start_on(int ti, int route, int on, int pt, float delay) {
    ai_start(ti, route);
    Ai *a = &ai[ti];
    a->leg = on == R_FOREST ? 0 : on == R_SUMMIT ? 2 : 1;
    a->mid = on == R_FOREST || on == R_SUMMIT ? route : on;
    a->route = on;
    a->pt = pt;
    a->best_at = progress_at(a, &lvl_routes[on]);
    a->delay = delay;
}
#endif

static int nearest_pt(const lvl_route_t *r, V3 p, int from, int to) {
    if(from < 0) from = 0;
    int best = from;
    float bd = 1e18f;
    for(int i = from; i <= to && i < r->n; i++) {
        float dx = r->pts[i].x - p.x, dz = r->pts[i].z - p.z, d = dx * dx + dz * dz;
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* The point dist metres on from point i, the way dir goes. */
static V3 along(const lvl_route_t *r, int i, int dir, float dist, int *end_i) {
    V3 p = v3(r->pts[i].x, r->pts[i].y, r->pts[i].z);
    while(i + dir >= 0 && i + dir < r->n) {
        V3 q = v3(r->pts[i + dir].x, r->pts[i + dir].y, r->pts[i + dir].z);
        float seg = vlen(vsub(q, p));
        if(seg >= dist) {
            p = vlerp(p, q, dist / fmaxf(seg, 1e-3f));
            break;
        }
        dist -= seg;
        p = q;
        i += dir;
    }
    if(end_i) *end_i = i;
    return p;
}

/* Which way the route runs at point i for a truck going dir, flat. */
static V3 way_at(const lvl_route_t *r, int i, int dir) {
    int j = i + dir;
    if(j < 0 || j >= r->n) {
        j = i;
        i -= dir;
    }
    V3 d = v3(r->pts[j].x - r->pts[i].x, 0, r->pts[j].z - r->pts[i].z);
    float l = vlen(d);
    return l > 1e-3f ? vscale(d, 1.0f / l) : v3(0, 0, 1);
}

/* To the right of flat direction d. */
static inline V3 right_of(V3 d) {
    return v3(d.z, 0, -d.x);
}

static inline V3 flat(V3 d) {
    d.y = 0;
    float l = vlen(d);
    return l > 1e-3f ? vscale(d, 1.0f / l) : v3(0, 0, 1);
}

/* How far right of the line at point i a truck going dir can pull over,
   up to want: not where the ground within its width falls away (a shelf
   road's edge) or climbs into a bank, and not into a boulder, a building
   or the lookout's legs. */
static float room_right(const lvl_route_t *r, int i, int dir, float want) {
    V3 w = way_at(r, i, dir), side = right_of(w);
    float x = r->pts[i].x, z = r->pts[i].z, y = terrain_height(x, z), room = 0;
    for(float s = 0.5f; room < want; s += 0.5f) {
        float to = fminf(s, want), d = to + 1.0f;
        float dy = terrain_height(x + side.x * d, z + side.z * d) - y;
        if(dy < -0.3f * d || dy > 0.45f * d) break;
        room = to;
    }
    /* What stands within a truck's length of the point, on or right of
       the line: clear it by the truck's half width and a little. */
    #define BESIDE(ox, oz, orad) do { \
        float dx = (ox) - x, dz = (oz) - z, lat = dx * side.x + dz * side.z; \
        if(fabsf(dx * w.x + dz * w.z) < 4.0f + (orad) && lat > -1.0f) \
            room = fminf(room, fmaxf(lat - (orad) - 1.3f, 0)); \
    } while(0)
    for(int k = 0; k < LVL_SPHERES && room > 0; k++) BESIDE(lvl_spheres[k].x, lvl_spheres[k].z, lvl_spheres[k].r);
    for(int k = 0; k < LVL_POSTS && room > 0; k++) BESIDE(lvl_posts[k].x0, lvl_posts[k].z0, lvl_posts[k].r);
    for(int k = LVL_ROCK_BOXES; k < LVL_BOXES && room > 0; k++)
        BESIDE(lvl_boxes[k].x, lvl_boxes[k].z, fmaxf(lvl_boxes[k].hx, lvl_boxes[k].hz));
    #undef BESIDE
    return room;
}

/* Back on the route after a recover or the winch, facing the way to go. */
static void face_route(Ai *a, Truck *t, const lvl_route_t *r) {
    a->pt = nearest_pt(r, t->pos, 0, r->n - 1);
    if(a->dir > 0) return;   /* a recover faces up the route */
    V3 w = way_at(r, a->pt, a->dir);
    truck_place(t, v3(t->pos.x, 0, t->pos.z), atan2f(w.x, w.z));
}

/* The end of a trip, at the summit or back at the foot: rest a while,
   then turn round and take another route. */
static void arrive(Ai *a, const Truck *t, const lvl_route_t *r) {
    a->trips++;
    a->dir = -a->dir;
    a->leg = 0;
    a->mid = 1 + (a->mid - 1 + 1 + pick(t, 2)) % 3;
    a->start_t = 0;
    a->delay = (a->dir < 0 ? 8.0f : 5.0f) + (float)pick(t, 10);
    a->turn = TURN_NOSE_IN;
    a->turn_side = 0;
    a->turn_t = a->turn_all = a->block_t = 0;
    a->shift = -a->shift;   /* the same side of the trail, now the left */
    a->retries = 0;
    a->stuck_t = a->reverse_t = a->wait_t = a->push = a->progress_t = 0;
    a->best_at = progress_at(a, r);
#if (defined(RL_AUTOTEST) && !defined(RL_HW)) || defined(RL_AI_TRACE)
    printf("RL AI %d at the %s, next by route %d\n", (int)(a - ai), a->dir < 0 ? "summit" : "foot", a->mid);
#endif
}

bool ai_done(int ti) {
    return ai[ti].trips > 0;
}

/* Turning round on the trail, or on to one that doubles back from a
   junction: nose in on full lock (toward the higher side, if either
   would do), back out on the other, and again until facing the way to
   go, never straying far off the line. True while it has the controls. */
static bool turn_round(Ai *a, Truck *t, const lvl_route_t *r, float dt) {
    TruckInput *in = &t->in;
    V3 w = way_at(r, a->pt, a->dir);
    if(vdot(flat(t->ax[2]), w) > 0.5f) {
        a->turn = TURN_NONE;
        return false;
    }
    V3 fw = flat(t->ax[2]), rt = flat(t->ax[0]);
    float x = t->pos.x, z = t->pos.z, ground = terrain_height(x, z);
    if(a->turn_side == 0) {
        float hr = terrain_height(x + rt.x * 4.0f, z + rt.z * 4.0f) + terrain_height(x + rt.x * 8.0f, z + rt.z * 8.0f);
        float hl = terrain_height(x - rt.x * 4.0f, z - rt.z * 4.0f) + terrain_height(x - rt.x * 8.0f, z - rt.z * 8.0f);
        a->turn_side = hr >= hl ? 1 : -1;
    }
    a->turn_all += dt;
    a->turn_t += dt;
    if(a->turn_all > 45.0f) {
        /* No room, or something in the way: the winch swings it round. */
        truck_place(t, v3(r->pts[a->pt].x, 0, r->pts[a->pt].z), atan2f(w.x, w.z));
        a->turn = TURN_NONE;
#if (defined(RL_AUTOTEST) && !defined(RL_HW)) || defined(RL_AI_TRACE)
        printf("RL AI %d winched round on route %d pt %d\n", (int)(a - ai), a->route, a->pt);
#endif
        return false;
    }
    /* A move is over when it has used the room there is beside the line,
       the ground the way it is going falls away, something stops it, or
       it has gone on long enough. */
    bool nose = a->turn == TURN_NOSE_IN;
    float speed = t->speed, going = nose ? speed : -speed;
    V3 side = right_of(w);
    V3 from = v3(x - r->pts[a->pt].x, 0, z - r->pts[a->pt].z);
    float off = vdot(from, side) + vdot(t->vel, side) * 0.4f;
    float room = r->difficulty == 0 ? 9.0f : r->half + 1.0f;
    float reach = nose ? 4.0f : -4.0f;
    bool edge = terrain_height(x + fw.x * reach, z + fw.z * reach) < ground - 1.5f;
    if(fabsf(speed) < 0.25f) a->block_t += dt;
    else a->block_t = 0;
    bool over = going > 0.2f && ((fabsf(off) > room && vdot(t->vel, side) * off > 0) || edge);
    if(over || a->block_t > 1.2f || a->turn_t > (nose ? 5.0f : 3.0f)) {
        a->turn = nose ? TURN_BACK_OUT : TURN_NOSE_IN;
        a->turn_t = a->block_t = 0;
        nose = !nose;
        going = -going;
    }
    /* Walking pace either way; the brake at a standstill is reverse, and
       in reverse the pedals swap (truck.c). */
    float pedal = clampf((1.8f - going) * 0.6f + 0.15f + a->block_t * 0.4f, 0, 1);
    float other = going > 2.4f ? 0.6f : 0;
    in->handbrake = false;
    in->steer = (float)(nose ? a->turn_side : -a->turn_side);
    if(nose == (t->gear > 0)) {
        /* In the right gear: drive, or slow down. */
        in->throttle = nose ? pedal : other;
        in->brake = nose ? other : pedal;
    }
    else {
        /* Still rolling the other way: stop, then the gear changes. */
        in->throttle = nose ? 0.6f : 0;
        in->brake = nose ? 0 : 0.8f;
    }
    return true;
}

/* Dipped beams on as dusk falls and off once it is light again, each
   driver at a slightly different moment, with a gap between the two so
   they never flicker at the edge. */
static void cpu_lights(int ti, Truck *t) {
    float on = 0.45f + 0.1f * (float)(ti % 3);
    if(t->lights == LIGHTS_OFF && pal->dark > on) t->lights = LIGHTS_DIPPED;
    else if(t->lights != LIGHTS_OFF && pal->dark < on - 0.25f) t->lights = LIGHTS_OFF;
}

void ai_drive(int ti, float dt) {
    Truck *t = &trucks[ti];
    Ai *a = &ai[ti];
    TruckInput *in = &t->in;
    const lvl_route_t *r = &lvl_routes[a->route];
    if(t->cpu) cpu_lights(ti, t);
    /* Never across a hairpin to its other leg. */
    if(a->dir > 0) a->pt = nearest_pt(r, t->pos, a->pt, a->pt + 4);
    else a->pt = nearest_pt(r, t->pos, a->pt - 4, a->pt);
    if(a->leg < 2 && pts_left(a, r) <= 1) {
        a->leg++;
        a->route = leg_route(a);
        r = &lvl_routes[a->route];
        if(a->dir > 0) a->pt = nearest_pt(r, t->pos, 0, r->n - 1);
        else {
            a->pt = nearest_pt(r, t->pos, r->n - 16, r->n - 1);
            /* Coming down, the next route can double back from the
               junction: too tight to swing round in one. */
            V3 w = way_at(r, a->pt, a->dir);
            if(vdot(flat(t->ax[2]), w) < 0.1f) {
                a->turn = TURN_NOSE_IN;
                float to_right = vdot(flat(t->ax[0]), w);
                a->turn_side = to_right > 0.5f ? 1 : to_right < -0.5f ? -1 : 0;
                a->turn_t = a->turn_all = a->block_t = 0;
            }
        }
    }
    else if(a->leg == 2 && pts_left(a, r) == 0) arrive(a, t, r);
    /* Off the route (slid off a bend, fell down a bank): find it again,
       and if it is well above us, recover back on to it. */
    {
        const lvl_pt_t *q = &r->pts[a->pt];
        float dx = q->x - t->pos.x, dz = q->z - t->pos.z;
        a->lost_t = fmaxf(0, a->lost_t - dt);
        bool started = a->trips > 0 || a->leg > 0 || a->pt > 2;
        if(started && a->lost_t <= 0 && dx * dx + dz * dz > 14.0f * 14.0f) {
            a->lost_t = 4.0f;
            a->pt = nearest_pt(r, t->pos, 0, r->n - 1);
            q = &r->pts[a->pt];
            dx = q->x - t->pos.x;
            dz = q->z - t->pos.z;
            if(dx * dx + dz * dz > 14.0f * 14.0f || q->y > t->pos.y + 5.0f) {
#if (defined(RL_AUTOTEST) && !defined(RL_HW)) || defined(RL_AI_TRACE)
                printf("RL AI %d off route %d at %.1f,%.1f,%.1f, recovered\n", ti, a->route, (double)t->pos.x,
                       (double)t->pos.y, (double)t->pos.z);
#endif
                truck_recover_on(t, a->route);
                face_route(a, t, r);
            }
        }
    }
    /* A guide on the trail, as racing games give their drivers: a gentle
       pull back toward the line when drifting past the edge, and help
       against a roll. Companions only; the player drives unaided. */
    if(ti != 0 || getenv_assist()) {
        int j = a->pt + a->dir >= 0 && a->pt + a->dir < r->n ? a->pt + a->dir : a->pt;
        V3 p0 = v3(r->pts[a->pt].x, 0, r->pts[a->pt].z), p1 = v3(r->pts[j].x, 0, r->pts[j].z);
        V3 seg = vsub(p1, p0);
        float sl = vlen(seg);
        if(sl > 0.1f && !a->turn) {
            V3 sd = vscale(seg, 1.0f / sl);
            V3 side = right_of(sd);
            float off = vdot(vsub(v3(t->pos.x, 0, t->pos.z), p0), side) - a->shift;
            float edge = r->half * 0.6f;
            if(fabsf(off) > edge && fabsf(off) < 12.0f) {
                float pull = clampf((fabsf(off) - edge) * 1.5f, 0, 4.0f) * signf(off);
                t->vel = vmad(t->vel, side, -pull * dt);
            }
        }
        if(fabsf(t->roll) > 25.0f) t->w = vmad(t->w, t->ax[2], -vdot(t->w, t->ax[2]) * fminf(dt * 4.0f, 1.0f));
    }
    float speed = t->speed;
    float look = 5.5f + fabsf(speed) * 0.7f;
    int e;
    V3 target = along(r, a->pt, a->dir, look, &e);
    target = vmad(target, right_of(way_at(r, e, a->dir)), a->shift);
    V3 d = vsub(target, t->pos);
    float lx = vdot(d, t->ax[0]), lz = vdot(d, t->ax[2]);
    const TruckSpec *s = &t->spec;
    float wb = s->wheel[WFL].z - s->wheel[WRL].z;
    float L2 = lx * lx + lz * lz;
    float curv = 2.0f * lx / fmaxf(L2, 1.0f);
    float steer = atanf(wb * curv) / s->max_steer;
    /* A target behind: full lock toward it (pure pursuit gives none). */
    if(lz < 0.5f) steer = lx >= 0 ? 1.0f : -1.0f;
    /* How sharp the next stretch is, and how steep. */
    V3 p20 = along(r, a->pt, a->dir, 22.0f, NULL);
    V3 p10 = along(r, a->pt, a->dir, 11.0f, NULL);
    V3 here = v3(r->pts[a->pt].x, r->pts[a->pt].y, r->pts[a->pt].z);
    V3 d1 = vnorm(vsub(p10, here)), d2 = vnorm(vsub(p20, p10));
    float bend = 1.0f - clampf(vdot(d1, d2), -1, 1);
    float grade = fabsf(p20.y - here.y) / 22.0f;
    float base = r->difficulty == 0 ? 9.0f : r->difficulty == 1 ? 6.5f : r->difficulty == 2 ? 4.5f : 3.6f;
    float want = base / (1.0f + bend * 6.0f) / (1.0f + grade * 2.5f);
    want = fmaxf(want, 1.6f);
    /* Coming down, a rock ledge that stands proud of the trail wants a run
       at it: at a crawl the front wheels just stop against its face. (The
       climb is left as it was tuned: there the winch gets them over.) */
    for(int i = 0; i < LVL_ROCK_BOXES && a->dir < 0; i++) {
        const lvl_box_t *b = &lvl_boxes[i];
        V3 db = v3(b->x - t->pos.x, 0, b->z - t->pos.z);
        float bx = vdot(db, t->ax[0]), bz = vdot(db, t->ax[2]);
        if(bz > 0 && bz < 14.0f && fabsf(bx) < 3.0f && fabsf(b->y - t->pos.y) < 3.0f) {
            want = fmaxf(want, 6.0f);
            break;
        }
    }
    /* Keep a gap to a truck ahead on the trail, and keep right for one
       coming the other way: over far enough to clear it wherever it is on
       the trail, slowly, the one coming down stopping for the one climbing
       if they would still touch. */
    bool queued = false;
    float shift = a->leg == 2 && pts_left(a, r) < 6 ? r->half * 0.45f : 0;   /* pulling over to stop */
    for(int o = 0; o < game.ntrucks; o++) {
        const Truck *u = &trucks[o];
        if(u->absent) continue;
        if(u == t) continue;
        V3 du = vsub(u->pos, t->pos);
        float ux = vdot(du, t->ax[0]), uz = vdot(du, t->ax[2]);
        if(uz > 0 && uz < 34.0f && fabsf(ux) < 34.0f && fabsf(du.y) < 6.0f) {
            int j = a->dir > 0 ? nearest_pt(r, u->pos, a->pt, a->pt + 9) : nearest_pt(r, u->pos, a->pt - 9, a->pt);
            V3 w = way_at(r, j, a->dir);
            V3 uo = v3(u->pos.x - r->pts[j].x, 0, u->pos.z - r->pts[j].z);
            if(vdot(uo, uo) < 6.0f * 6.0f && vdot(flat(u->ax[2]), w) < -0.3f) {
                shift = fmaxf(shift, clampf(vdot(uo, right_of(w)) + 2.5f, 0, r->half - 0.4f));
                want = fminf(want, 2.5f);
                if(a->dir < 0 && uz < 11.0f && fabsf(ux) < 2.2f && fabsf(u->speed) > 0.5f) want = 0;
                continue;
            }
        }
        if(uz > 0 && uz < 16.0f && fabsf(ux) < 3.5f) {
            want = fminf(want, (uz - 7.0f) * 0.6f);
            if(want <= 0) queued = true;
        }
    }
    if(shift > 0) shift = fminf(room_right(r, a->pt, a->dir, shift), room_right(r, e, a->dir, shift));
    a->shift += clampf(shift - a->shift, -1.5f * dt, 1.5f * dt);
    a->start_t += dt;
    /* The trip's end with a truck already stopped there: that will do. */
    if(queued && a->leg == 2 && pts_left(a, r) < 6 && a->start_t >= a->delay) {
        arrive(a, t, r);
        r = &lvl_routes[a->route];
    }
    bool resting = a->start_t < a->delay;
    if(resting) want = 0;
    else if(a->turn && turn_round(a, t, r, dt)) return;
    /* Don't wait on a truck ahead for ever: it may be stuck itself. */
    if(want <= 0 && a->start_t >= a->delay) {
        a->wait_t += dt;
        if(a->wait_t > 6.0f) want = 1.6f;
        if(a->wait_t > 12.0f) a->wait_t = 0;
    }
    else a->wait_t = 0;
    /* Low range where it's rough or steep. */
    /* Low range where it's rough or steep, with hysteresis and a hold:
       each change is a shift with no drive for a moment. */
    bool low = r->difficulty >= 2 || a->retries > 0 || grade > (t->mode == DRIVE_LOCK ? 0.10f : 0.17f);
    int mode = low ? DRIVE_LOCK : DRIVE_4WD;
    a->mode_t += dt;
    if(t->mode != mode && fabsf(speed) < LOW_RANGE_SHIFT_MPS * 0.8f && a->mode_t > 3.0f) {
        truck_set_mode(t, mode);
        a->mode_t = 0;
    }
    if(want <= 0) {
        /* Waiting: hold still rather than creep into the truck ahead. */
        in->steer = resting && a->turn ? 0 : clampf(steer, -1, 1);
        in->throttle = 0;
        in->brake = fabsf(speed) > 0.3f ? 0.6f : 0.0f;
        in->handbrake = false;
        a->stuck_t = 0;
        return;
    }
    float err = want - speed;
    /* Held up (a ledge, a log, mud): feed in more throttle the longer it
       lasts, as a driver would. */
    if(speed < want * 0.5f) a->push = fminf(a->push + dt * 0.5f, 1.0f);
    else a->push = fmaxf(a->push - dt * 1.0f, 0.0f);
    float throttle = clampf(err * 0.5f + 0.15f + a->push, 0, 1);
    /* Nose rearing up: ease off before it goes over backwards. */
    if(t->pitch > 30.0f) throttle *= clampf(1.0f - (t->pitch - 30.0f) / 12.0f, 0.15f, 1.0f);
    float brake = clampf(-err * 0.35f, 0, 1);
    /* Stuck: back off a little with the wheel the other way, then go again. */
    if(a->reverse_t > 0) {
        a->reverse_t -= dt;
        in->steer = -clampf(steer, -1, 1) * 0.8f;
        in->throttle = 0;
        in->brake = 0.8f;
        in->handbrake = false;
        if(a->reverse_t <= 0) a->stuck_t = 0;
        return;
    }
    if(want - speed > 1.0f && fabsf(speed) < 0.5f) a->stuck_t += dt;
    else a->stuck_t = fmaxf(0, a->stuck_t - dt * 2.0f);
    if(t->ax[1].y < 0.35f) a->flip_t += dt;
    else a->flip_t = 0;
    /* No progress along the route for a long while, whatever the reason:
       the winch, so a companion always gets on up the mountain. */
    int at = progress_at(a, r);
    if(at > a->best_at) {
        a->best_at = at;
        a->progress_t = 0;
    }
    else a->progress_t += dt;
    if(a->progress_t > 25.0f) {
        a->progress_t = 0;
        a->retries = 4;
        a->stuck_t = 3.0f;
    }
    if(a->stuck_t > 2.5f) {
#if (defined(RL_AUTOTEST) && !defined(RL_HW)) || defined(RL_AI_TRACE)
        printf("RL AI %d stuck at %.1f,%.1f,%.1f route %d pt %d/%d target %.1f,%.1f steer %.2f mode %d rpm %.0f g %d "
               "contacts %d%d%d%d slip %.1f %.1f %.1f %.1f\n", ti, (double)t->pos.x, (double)t->pos.y, (double)t->pos.z,
               a->route, a->pt, r->n, (double)target.x, (double)target.z, (double)steer, t->mode, (double)t->rpm, t->gear,
               t->wh[0].contact, t->wh[1].contact, t->wh[2].contact, t->wh[3].contact, (double)t->wh[0].slip,
               (double)t->wh[1].slip, (double)t->wh[2].slip, (double)t->wh[3].slip);
        printf("RL AI %d   comp %.2f %.2f %.2f %.2f surf %d %d %d %d omega %.1f %.1f %.1f %.1f chassis %.0f N up %.2f "
               "fwd %.2f,%.2f,%.2f\n", ti, (double)t->wh[0].comp, (double)t->wh[1].comp, (double)t->wh[2].comp,
               (double)t->wh[3].comp, t->wh[0].surf, t->wh[1].surf, t->wh[2].surf, t->wh[3].surf, (double)t->wh[0].omega,
               (double)t->wh[1].omega, (double)t->wh[2].omega, (double)t->wh[3].omega, (double)t->dbg_chassis,
               (double)t->ax[1].y, (double)t->ax[2].x, (double)t->ax[2].y, (double)t->ax[2].z);
#endif
        a->retries++;
        a->reverse_t = 1.6f;
        a->stuck_t = 0;
        if(a->retries > 3) {
            /* Beaten by this one: the winch, past it along the route to
               the first point clear of other trucks. With none (the last
               of the trail is full of them), try again in a while. */
            int to = a->pt + 6 * a->dir, end = a->dir > 0 ? r->n - 1 : 0;
            bool clear = false;
            for(int tries = 0; tries < 6 && !clear; tries++) {
                if((to - end) * a->dir > 0) to = end;
                clear = true;
                for(int o = 0; o < game.ntrucks; o++) {
                    const Truck *u = &trucks[o];
                    float ox = u->pos.x - r->pts[to].x, oz = u->pos.z - r->pts[to].z;
                    if(u != t && !u->absent && ox * ox + oz * oz < 6.0f * 6.0f) clear = false;
                }
                if(!clear && to == end) break;
                if(!clear) to += a->dir;
            }
            if(clear) {
                V3 w = way_at(r, to, a->dir);
                truck_place(t, v3(r->pts[to].x, 0, r->pts[to].z), atan2f(w.x, w.z));
                a->pt = to;
                a->retries = 1;
#if (defined(RL_AUTOTEST) && !defined(RL_HW)) || defined(RL_AI_TRACE)
                printf("RL AI %d winched to route %d pt %d\n", ti, a->route, to);
#endif
            }
            else a->retries = 3;
        }
    }
    if(a->flip_t > 2.5f) {
        truck_recover_on(t, a->route);
        face_route(a, t, r);
        a->flip_t = 0;
    }
    if(fabsf(speed) > 2.0f && a->retries > 0 && a->stuck_t == 0) a->retries = a->retries > 1 ? a->retries - 1 : a->retries;
    in->steer = clampf(steer, -1, 1);
    in->throttle = throttle;
    in->brake = brake;
    in->handbrake = false;
#ifdef RL_AI_TRACE
    static int trace_n;
    if(getenv("SIM_TRACE_AI") && (trace_n++ % 60) == 0)
        printf("ai %d pos %.1f %.1f %.1f v %.2f want %.2f thr %.2f brk %.2f steer %.2f route %d pt %d tgt %.1f %.1f bend %.2f "
               "grade %.2f mode %d gear %d rpm %.0f retries %d stuck %.1f\n", ti, (double)t->pos.x, (double)t->pos.y,
               (double)t->pos.z, (double)speed, (double)want, (double)throttle, (double)brake, (double)steer, a->route, a->pt,
               (double)target.x, (double)target.z, (double)bend, (double)grade, t->mode, t->gear, (double)t->rpm,
               a->retries, (double)a->stuck_t);
#endif
}

const char *ai_route_name(int ti) {
    return lvl_routes[ai[ti].route].name;
}

float ai_progress(int ti) {
    const Ai *a = &ai[ti];
    if(a->trips > 0) return 3.0f;
    return a->leg + (float)a->pt / (float)lvl_routes[a->route].n;
}
