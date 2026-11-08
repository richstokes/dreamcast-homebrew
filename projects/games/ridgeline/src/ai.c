/* Ridgeline CPU drivers: each follows the forest road to the fork, then
   its own route up to the lake and the summit trail, with pure-pursuit
   steering, a speed that drops for bends, grade and rough routes, low
   range on the hard parts, and backing off and trying again when stuck. */
#include "ai.h"
#include "truck.h"
#include "terrain.h"
#include "world.h"
#include <stdlib.h>

typedef struct {
    int plan[3], leg;
    int route, pt;
    float stuck_t, reverse_t, flip_t;
    int retries;
    float steer_bias;
    float start_t, delay;   /* CPUs set off one after another */
    float wait_t, mode_t, push, lost_t, progress_t;
    int best_at;
} Ai;

static Ai ai[MAX_TRUCKS];

/* Test builds can give the player's CPU driver the same guide. */
static inline bool getenv_assist(void) {
#if defined(RL_AUTOTEST) || defined(RL_AI_TRACE)
    return true;
#else
    return false;
#endif
}

/* Route indices in level.h's order. */
enum { R_FOREST, R_LOGGING, R_RIDGE, R_CREEK, R_SUMMIT };

void ai_start(int ti, int route) {
    Ai *a = &ai[ti];
    memset(a, 0, sizeof(*a));
    a->plan[0] = R_FOREST;
    a->plan[1] = route;
    a->plan[2] = R_SUMMIT;
    a->leg = 0;
    a->route = R_FOREST;
    a->pt = 0;
    a->delay = ti * 4.0f;
}

static int nearest_pt(const lvl_route_t *r, V3 p, int from, int to) {
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

/* The point dist metres along the route from point i. */
static V3 along(const lvl_route_t *r, int i, float dist, int *end_i) {
    V3 p = v3(r->pts[i].x, r->pts[i].y, r->pts[i].z);
    while(i + 1 < r->n) {
        V3 q = v3(r->pts[i + 1].x, r->pts[i + 1].y, r->pts[i + 1].z);
        float seg = vlen(vsub(q, p));
        if(seg >= dist) {
            p = vlerp(p, q, dist / fmaxf(seg, 1e-3f));
            break;
        }
        dist -= seg;
        p = q;
        i++;
    }
    if(end_i) *end_i = i;
    return p;
}

bool ai_done(int ti) {
    const Ai *a = &ai[ti];
    return a->leg >= 2 && a->pt >= lvl_routes[R_SUMMIT].n - 2;
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
    a->pt = nearest_pt(r, t->pos, a->pt, a->pt + 4);   /* never across a hairpin to its other leg */
    if(a->pt >= r->n - 2 && a->leg < 2) {
        a->leg++;
        a->route = a->plan[a->leg];
        r = &lvl_routes[a->route];
        a->pt = nearest_pt(r, t->pos, 0, r->n - 1);
    }
    /* Off the route (slid off a bend, fell down a bank): find it again,
       and if it is well above us, recover back on to it. */
    {
        const lvl_pt_t *q = &r->pts[a->pt];
        float dx = q->x - t->pos.x, dz = q->z - t->pos.z;
        a->lost_t = fmaxf(0, a->lost_t - dt);
        bool started = a->leg > 0 || a->pt > 2;
        if(started && a->lost_t <= 0 && dx * dx + dz * dz > 14.0f * 14.0f) {
            a->lost_t = 4.0f;
            a->pt = nearest_pt(r, t->pos, 0, r->n - 1);
            q = &r->pts[a->pt];
            dx = q->x - t->pos.x;
            dz = q->z - t->pos.z;
            if(dx * dx + dz * dz > 14.0f * 14.0f || q->y > t->pos.y + 5.0f) {
                truck_recover_on(t, a->route);
                a->pt = nearest_pt(r, t->pos, 0, r->n - 1);
            }
        }
    }
    /* A guide on the trail, as racing games give their drivers: a gentle
       pull back toward the line when drifting past the edge, and help
       against a roll. Companions only; the player drives unaided. */
    if(ti != 0 || getenv_assist()) {
        int j = a->pt + 1 < r->n ? a->pt + 1 : a->pt;
        V3 p0 = v3(r->pts[a->pt].x, 0, r->pts[a->pt].z), p1 = v3(r->pts[j].x, 0, r->pts[j].z);
        V3 seg = vsub(p1, p0);
        float sl = vlen(seg);
        if(sl > 0.1f) {
            V3 sd = vscale(seg, 1.0f / sl);
            V3 side = v3(sd.z, 0, -sd.x);
            float off = vdot(vsub(v3(t->pos.x, 0, t->pos.z), p0), side);
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
    V3 target = along(r, a->pt, look, NULL);
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
    int e;
    V3 p20 = along(r, a->pt, 22.0f, &e);
    V3 p10 = along(r, a->pt, 11.0f, NULL);
    V3 here = v3(r->pts[a->pt].x, r->pts[a->pt].y, r->pts[a->pt].z);
    V3 d1 = vnorm(vsub(p10, here)), d2 = vnorm(vsub(p20, p10));
    float bend = 1.0f - clampf(vdot(d1, d2), -1, 1);
    float grade = fabsf(p20.y - here.y) / 22.0f;
    float base = r->difficulty == 0 ? 9.0f : r->difficulty == 1 ? 6.5f : r->difficulty == 2 ? 4.5f : 3.6f;
    float want = base / (1.0f + bend * 6.0f) / (1.0f + grade * 2.5f);
    want = fmaxf(want, 1.6f);
    /* Keep a gap to a truck ahead on the trail. */
    for(int o = 0; o < game.ntrucks; o++) {
        const Truck *u = &trucks[o];
        if(u->absent) continue;
        if(u == t) continue;
        V3 du = vsub(u->pos, t->pos);
        float ux = vdot(du, t->ax[0]), uz = vdot(du, t->ax[2]);
        if(uz > 0 && uz < 16.0f && fabsf(ux) < 3.5f) want = fminf(want, (uz - 7.0f) * 0.6f);
    }
    a->start_t += dt;
    if(a->start_t < a->delay) want = 0;
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
        in->steer = clampf(steer, -1, 1);
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
    int at = a->leg * 4096 + a->pt;
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
            /* Beaten by this one: the winch, past it along the route. */
            int to = a->pt + 6 < r->n ? a->pt + 6 : r->n - 1;
            int to2 = to + 1 < r->n ? to + 1 : to;
            float hd = atan2f(r->pts[to2].x - r->pts[to].x, r->pts[to2].z - r->pts[to].z);
            if(to2 == to) hd = truck_heading(t);
            truck_place(t, v3(r->pts[to].x, 0, r->pts[to].z), hd);
            a->pt = to;
            a->retries = 1;
#if (defined(RL_AUTOTEST) && !defined(RL_HW)) || defined(RL_AI_TRACE)
            printf("RL AI %d winched to route %d pt %d\n", ti, a->route, to);
#endif
        }
    }
    if(a->flip_t > 2.5f) {
        truck_recover_on(t, a->route);
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
    return a->leg + (float)a->pt / (float)lvl_routes[a->route].n;
}
