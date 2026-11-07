/* Bay City Hills' cable car.

   It runs up and down one of Hyde Street's two tracks the whole race:
   it waits at the foot by turn 1, climbs the three blocks at a steady
   pace, waits at the summit and comes back down. Which track it runs on,
   and how far through its cycle it is at the green light, are drawn at
   random for each race offline, so it can turn up anywhere on the hill.
   Online and in a time trial they are fixed (the first track, the
   eastern one on the left going up, and the car coming down to meet the
   pack mid-hill on lap 1), so every console sees the same car and
   records stay comparable.

   It runs on the race clock, not world_time: online that is the
   server's green-light clock, so every console has it in the same place,
   and its cycle (~63 s) is longer than a lap (~43-50 s), so a kart meets
   it somewhere else each time round. Driving into its side bounces a kart
   off as a wall does; running into either end spins it out. It is drawn
   with the scenery (world_sf.c). */
#include "cablecar.h"
#include "track.h"

#define CC_HALF_L   4.3f    /* half its length along the street, with the bumpers */
#define CC_HALF_W   1.3f    /* half its width */
#define CC_HEIGHT   3.6f
#define CC_RAIL     1.75f   /* each track's offset from the centreline */
#define CC_BOTTOM   12.0f   /* where it waits, metres into Hyde Street from turn 1 */
#define CC_TOP      205.0f
#define CC_SPEED    7.0f    /* m/s, quicker than the real thing */
#define CC_WAIT     2.0f    /* seconds at each end */
#define CC_EASE     2.0f    /* seconds to get up to speed, and to stop */
#define CC_PHASE    30.0f   /* online and time trials: seconds into its cycle at the green light, so
                               lap 1 meets it mid-hill at every class (just over the top, coming down) */
#define CC_SPIN     9.0f    /* m/s into an end that spins a kart out */

static int rail;      /* 0 the first track (d -CC_RAIL), 1 the second */
static float phase;   /* seconds into its cycle at the green light */

static float cc_run(void) {
    return (CC_TOP - CC_BOTTOM) / CC_SPEED + CC_EASE;
}

static float cc_cycle(void) {
    return 2.0f * (cc_run() + CC_WAIT);
}

bool cablecar_on(void) {
    return track.id == TRACK_SF;
}

void cablecar_reset(void) {
    if(!cablecar_on()) return;
    bool fixed = game.online || game.mode == MODE_TT;
#ifdef DK_RECORD
    fixed = true;   /* the demo lobby's bots replay these races online, where the car is fixed */
#endif
    rail = fixed ? 0 : (int)(eng_rand() & 1u);
    phase = fixed ? CC_PHASE : (float)(eng_rand() % 1024u) / 1024.0f * cc_cycle();
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    printf("DK CABLECAR on track %d, %.1f s into its %.1f s cycle at the green light\n", rail + 1, (double)phase,
           (double)cc_cycle());
#endif
}

static float cc_along(float x, float run, float len) {
    float vm = len / (run - CC_EASE);
    if(x < CC_EASE) return vm * x * x / (2.0f * CC_EASE);
    if(x > run - CC_EASE) return len - vm * (run - x) * (run - x) / (2.0f * CC_EASE);
    return vm * (x - CC_EASE * 0.5f);
}

void cablecar_pose(float *s, float *d, int *dir) {
    float len = CC_TOP - CC_BOTTOM, run = cc_run();
    float t = fmodf(game.race_time + phase, cc_cycle()), at;
    /* Each run gets up to speed over CC_EASE, cruises, and slows to a stop
       over CC_EASE: x seconds into a run, at metres along it. */
    if(t < CC_WAIT) {
        at = 0.0f;
        *dir = 0;
    }
    else if(t < CC_WAIT + run) {
        at = cc_along(t - CC_WAIT, run, len);
        *dir = 1;
    }
    else if(t < 2.0f * CC_WAIT + run) {
        at = len;
        *dir = 0;
    }
    else {
        at = len - cc_along(t - 2.0f * CC_WAIT - run, run, len);
        *dir = -1;
    }
    *s = track_wrap(track_seg_s(SF_HYDE_SEG) + CC_BOTTOM + at);
    *d = rail ? CC_RAIL : -CC_RAIL;
}

void cablecar_collide(Kart *k) {
    if(!cablecar_on()) return;
    float cs, cd;
    int dir;
    cablecar_pose(&cs, &cd, &dir);
    float ds = track_delta(cs, k->tp.s), dd = k->tp.d - cd;
    float ls = CC_HALF_L + KART_RADIUS, ld = CC_HALF_W + KART_RADIUS;
    if(fabsf(ds) >= ls || fabsf(dd) >= ld) return;
    if(k->pos.y > track_road_y(cs, cd) + CC_HEIGHT) return;   /* over it, in the air */
    float over_s = ls - fabsf(ds), over_d = ld - fabsf(dd);
    V3 n;   /* out of the car, toward the kart */
    if(over_s < over_d) {
        /* Into an end: pushed back out along the street. */
        n = vscale(k->tp.tangent, ds < 0 ? -1.0f : 1.0f);
        k->pos = vmad(k->pos, n, over_s);
        float vn = k->vel.x * n.x + k->vel.z * n.z;
        float car_v = (float)dir * CC_SPEED * (ds < 0 ? -1.0f : 1.0f);   /* the car's speed toward the kart */
        float closing = car_v - vn;
        if(closing > 0) {
            k->vel.x += n.x * closing * 1.3f;
            k->vel.z += n.z * closing * 1.3f;
            k->speed *= 0.25f;
            if(closing > CC_SPIN && k->spin_t <= 0) kart_spin_out(k, dd < 0 ? -1 : 1);
#if defined(DK_AUTOTEST) && !defined(DK_HW)
            if(k->bump_t <= 0)
                printf("DK CABLECAR kart %d hit its %s at %.1f m/s%s\n", (int)(k - karts), ds < 0 ? "back" : "front",
                       (double)closing, closing > CC_SPIN ? ", spun out" : "");
#endif
            if(k->bump_t <= 0) {
                k->events |= EV_WALL;
                k->wall_v = closing;
                k->bump_t = 0.35f;
            }
        }
    }
    else {
        /* Along a side: off it as off a barrier. */
        n = vscale(k->tp.right, dd < 0 ? -1.0f : 1.0f);
        k->pos = vmad(k->pos, n, over_d);
        float vn = -(k->vel.x * n.x + k->vel.z * n.z);
        if(vn > 0) {
            k->vel.x += n.x * vn * 1.35f;
            k->vel.z += n.z * vn * 1.35f;
            float spd = fabsf(k->speed), frac = spd > 0.1f ? clampf(vn / spd, 0, 1) : 0;
            k->speed *= 1.0f - 0.75f * frac;
            if(vn > 2.0f && k->bump_t <= 0) {
#if defined(DK_AUTOTEST) && !defined(DK_HW)
                printf("DK CABLECAR kart %d scraped its side at %.1f m/s\n", (int)(k - karts), (double)vn);
#endif
                k->events |= EV_WALL;
                k->wall_v = vn;
                k->bump_t = 0.35f;
            }
        }
    }
    track_locate(k->pos, k->tp.idx, &k->tp);
}

float cablecar_avoid(const Kart *k, float s, float td) {
    if(!cablecar_on()) return td;
    float cs, cd;
    int dir;
    cablecar_pose(&cs, &cd, &dir);
    /* In the way: ahead within the reach of the kart's look and its own
       length (it may be coming down at the kart), or alongside. */
    float ahead = track_delta(k->tp.s, cs);
    if(ahead < -CC_HALF_L - 2.0f || ahead > track_delta(k->tp.s, s) + CC_HALF_L + 25.0f) return td;
    float clear = CC_HALF_W + KART_RADIUS + 1.2f;
    if(fabsf(td - cd) >= clear) return td;
    /* Round whichever side the kart is nearer, if the road has room. */
    float edge = track_half_at(&k->tp) - 0.9f;
    float lo = cd - clear, hi = cd + clear;
    bool go_hi = k->tp.d > cd ? hi <= edge : lo < -edge;
    return go_hi ? hi : lo;
}
