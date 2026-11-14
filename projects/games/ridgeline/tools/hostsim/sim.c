/* Ridgeline physics on the host: drives a truck over the real level with
   scripted inputs and prints what it does, for tuning without Flycast.
   make -C tools/hostsim && tools/hostsim/sim <scenario> */
#include "rl.h"
#include "truck.h"
#include "terrain.h"
#include "ai.h"
#include "unlocks.h"
#include <stdlib.h>

extern Game game;
extern Camera r_cam;

static void report(Truck *t, float time) {
    printf("t=%5.2f pos %7.2f %6.2f %7.2f v %5.2f rpm %5.0f g %2d mode %d pitch %5.1f roll %5.1f | ", (double)time,
           (double)t->pos.x, (double)t->pos.y, (double)t->pos.z, (double)t->speed, (double)t->rpm, t->gear, t->mode,
           (double)t->pitch, (double)t->roll);
    for(int i = 0; i < 4; i++)
        printf("w%d c%.2f om%5.1f sl%4.1f fx%6.0f fy%6.0f ", i, (double)t->wh[i].comp, (double)t->wh[i].omega,
               (double)t->wh[i].slip, (double)t->wh[i].fx, (double)t->wh[i].fy);
    printf("ch %.0f\n", (double)t->dbg_chassis);
}

/* The CPU driver over a whole route, as fast as the host goes. Route 5 is
   the area's extreme trail, which it drives between the gates with the
   player's physics and nothing more (ai.c): a summit with no winch and no
   recover in the log is the proof that the trail can be driven. */
static int run_route(int route, int model, int quiet) {
    ai_helped = 0;
    game.ntrucks = 1;
    Truck *t = &trucks[0];
    t->model = model;
    t->cpu = true;
    truck_place(t, v3(lvl_spawns[1][0], 0, lvl_spawns[1][2]), lvl_spawns[1][3]);
    ai_start(0, route);
    float dt = 1.0f / 60.0f, best = t->pos.y;
    int recovers = 0;
    float last_y = t->pos.y;
    for(int f = 0; f < 60 * 1200; f++) {
        r_cam.pos = vsub(t->pos, vscale(t->ax[2], 8.0f));
        ai_drive(0, dt);
        truck_update(t, dt);
        if(isnan(t->pos.x)) {
            printf("route %d: NaN at %.0f s\n", route, f * dt);
            return 0;
        }
        if(t->pos.y > best) best = t->pos.y;
        if(t->pos.y < last_y - 15.0f) recovers++;
        last_y = t->pos.y;
        if(!quiet && f % (60 * 20) == 0)
            printf("  %4.0f s pos %6.1f %6.1f %6.1f v %5.1f g %d mode %d prog %.2f\n", f * dt, (double)t->pos.x,
                   (double)t->pos.y, (double)t->pos.z, (double)t->speed, t->gear, t->mode, (double)ai_progress(0));
        float dx = t->pos.x - LEVEL_SUMMIT_X, dz = t->pos.z - LEVEL_SUMMIT_Z;
        if(dx * dx + dz * dz < 18.0f * 18.0f) {
            if(quiet < 2) printf("route %d truck %d: SUMMIT in %.0f s, helped %d times\n", route, model, f * dt, ai_helped);
            return 1;
        }
    }
    if(quiet < 2)
        printf("route %d truck %d: no summit after 1200 s, best height %.0f of %.0f, prog %.2f, at %.0f %.0f %.0f\n", route,
               model, (double)best, (double)LEVEL_SUMMIT_Y, (double)ai_progress(0), (double)t->pos.x, (double)t->pos.y,
               (double)t->pos.z);
    return 0;
}

/* Can every truck be earned? unlocks.h says what each wants; this plays
   it through with the CPU's driver at the wheel, driving only trucks
   already earned, round after round until nothing more unlocks:

     a summit counts when one of them reaches it by any of the three ways
     an extreme trail counts when one of them drives it with no winch and
       no recover (between its gates the CPU has the player's physics and
       nothing more: ai.c)
     a viewpoint counts when it lies within reach of a route one of them
       has driven to the top (the game's own 16 m). There are more off
       the trails; these are the ones nobody has to go looking for

   Fails (exit 1) if a truck is left locked at the end. */
static int run_unlocks(void *grid) {
    static const char *const names[UNLOCK_TRUCKS] = {"CASCADE 84", "KESTREL", "BOBCAT", "FFR", "TACO", "GOOSE", "ONE TEN",
                                                    "EAGLE"};
    Done d = {{0}, {0}};
    bool have[UNLOCK_TRUCKS], driven[AREA_COUNT][LVL_ROUTE_SLOTS] = {{false}};
    int left = 0;
    for(int m = 0; m < UNLOCK_TRUCKS; m++) {
        have[m] = rl_unlocks[m].kind == UNLOCK_FREE;
        if(have[m]) printf("start: %s\n", names[m]);
    }
    for(int round = 1; round <= UNLOCK_TRUCKS; round++) {
        for(int area = 0; area < AREA_COUNT; area++) {
            game.area = area;
            if(!terrain_load(area, grid)) return 2;
            for(int route = 1; route <= LVL_ROUTE_EXTREME; route++) {
                if(route == 4 || driven[area][route] || (route == LVL_ROUTE_EXTREME && !LVL_HAS_EXTREME)) continue;
                for(int m = 0; m < UNLOCK_TRUCKS && !driven[area][route]; m++) {
                    if(!have[m] || !run_route(route, m, 2)) continue;
                    if(route == LVL_ROUTE_EXTREME && ai_helped) continue;
                    driven[area][route] = true;
                    printf("round %d: %s, %s to the top in the %s%s\n", round, lvl->name, lvl_routes[route].name, names[m],
                           route == LVL_ROUTE_EXTREME ? ", unaided" : "");
                    d.stamps[area] |= STAMP_SUMMIT;
                    if(route == LVL_ROUTE_EXTREME) d.stamps[area] |= STAMP_BIT(LOG_EXTREME, STAMP_TOP);
                    else d.stamps[area] |= STAMP_BIT(route - 1, STAMP_TOP);
                }
            }
            for(int v = 0; v < LVL_VIEWS; v++)
                for(int route = 0; route < LVL_ROUTES && !(d.views[area] & (1u << v)); route++) {
                    const lvl_route_t *r = &lvl_routes[route];
                    bool on = route == 0 || route == 4 ? (d.stamps[area] & STAMP_SUMMIT) != 0 : driven[area][route];
                    for(int k = r->own0; on && k <= r->own1; k++) {
                        float dx = r->pts[k].x - lvl_views[v].x, dz = r->pts[k].z - lvl_views[v].z;
                        if(dx * dx + dz * dz < 15.0f * 15.0f && fabsf(r->pts[k].y - lvl_views[v].y) < 9.0f) {
                            d.views[area] |= (uint16_t)(1u << v);
                            break;
                        }
                    }
                }
        }
        int fresh = 0;
        for(int m = 0; m < UNLOCK_TRUCKS; m++) {
            int got, want;
            unlock_progress(m, &d, &got, &want);
            if(!have[m] && got >= want) {
                have[m] = true;
                fresh++;
                printf("round %d: %s unlocked (%s)\n", round, names[m], rl_unlocks[m].need);
            }
        }
        if(!fresh) break;
    }
    printf("done: %d summits, %d extreme trails, %d viewpoints on the routes\n", done_summits(&d), done_extremes(&d),
           done_views(&d));
    for(int m = 0; m < UNLOCK_TRUCKS; m++)
        if(!have[m]) {
            int got, want;
            unlock_progress(m, &d, &got, &want);
            printf("LOCKED: %s wants \"%s\" (%d of %d)\n", names[m], rl_unlocks[m].need, got, want);
            left++;
        }
    printf(left ? "unlocks: FAIL, %d trucks cannot be earned\n" : "unlocks: PASS, every truck can be earned\n", left);
    return left ? 1 : 0;
}

/* The extreme trail a stretch at a time: xtrail [truck]. The truck is set
   down a little before each of the trail's named places in turn and has
   90 s to reach the next (the last: the trail's top gate), so one
   obstacle that beats it does not hide the ones after. For finding what
   is too hard while a trail is being built; the proof is `route <truck>
   5`, start to summit with no winch. */
static int run_xtrail(int model) {
    const lvl_route_t *r = lvl_extreme;
    if(!LVL_HAS_EXTREME) {
        printf("no extreme trail here\n");
        return 1;
    }
    game.ntrucks = 1;
    Truck *t = &trucks[0];
    t->model = model;
    t->cpu = true;
    float dt = 1.0f / 60.0f;
    int fails = 0;
    printf("truck %d %-10s", model, t->spec.name ? t->spec.name : "");
    int only = getenv("SIM_X_SPOT") ? atoi(getenv("SIM_X_SPOT")) : -1;   /* one stretch, traced */
    for(int s = 0; s < LVL_SPOTS; s++) {
        if(only >= 0 && s != only) continue;
        int from = lvl_spots[s].pt > r->own0 + 2 ? lvl_spots[s].pt - 2 : r->own0;
        int to = s + 1 < LVL_SPOTS ? lvl_spots[s + 1].pt : r->own1;
        float hd = atan2f(r->pts[from + 1].x - r->pts[from].x, r->pts[from + 1].z - r->pts[from].z);
        truck_place(t, v3(r->pts[from].x, 0, r->pts[from].z), hd);
        truck_set_mode(t, DRIVE_LOCK);
        ai_start_on(0, LVL_ROUTE_EXTREME, LVL_ROUTE_EXTREME, from, 0);
        int f, moved = 0;
        float low = 1e9f;
        V3 was = t->pos;
        for(f = 0; f < 60 * 90; f++) {
            r_cam.pos = vsub(t->pos, vscale(t->ax[2], 8.0f));
            ai_drive(0, dt);
            truck_update(t, dt);
            if(vlen(vsub(t->pos, was)) > 5.0f) moved++;   /* a winch or a recover: a jump */
            if(only >= 0 && f % 30 == 0)
                printf("\n  %4.1fs %6.1f %5.1f %6.1f v %4.1f thr %.2f brk %.2f st %+.2f pitch %+3.0f roll %+3.0f surf %d%d%d%d "
                       "c %d%d%d%d spin %+.1f ch %.0f", f * dt, (double)t->pos.x, (double)t->pos.y, (double)t->pos.z,
                       (double)t->speed, (double)t->in.throttle, (double)t->in.brake, (double)t->in.steer, (double)t->pitch,
                       (double)t->roll, t->wh[0].surf % 10, t->wh[1].surf % 10, t->wh[2].surf % 10, t->wh[3].surf % 10,
                       t->wh[0].contact, t->wh[1].contact, t->wh[2].contact, t->wh[3].contact,
                       (double)(t->wh[2].omega * t->spec.radius - t->speed), (double)t->dbg_chassis);
            was = t->pos;
            if(f > 120) low = fminf(low, t->speed);
            float dx = t->pos.x - r->pts[to].x, dz = t->pos.z - r->pts[to].z;
            if(dx * dx + dz * dz < 4.0f * 4.0f && fabsf(t->pos.y - r->pts[to].y) < 4.0f) break;
        }
        bool ok = f < 60 * 90 && !moved;
        if(!ok) fails++;
        printf(" | %s %s %2.0fs", lvl_spots[s].name, ok ? "ok" : moved ? "WINCH" : "FAIL", f * dt);
    }
    printf("\n");
    return fails;
}

/* CPU drivers going round and round: loop [minutes] [trucks]. Prints each
   arrival, stall, winch and recover (ai.c) and, at the end, how trucks
   meeting head on got past each other. SIM_TRACE_T=<truck> follows one,
   SIM_TRUCK=<model> is the first truck's model (the rest follow on). */
static int run_loop(int minutes, int n) {
    game.ntrucks = n;
    static const int slot[4] = {1, 2, 0, 3};
    int first = getenv("SIM_TRUCK") ? atoi(getenv("SIM_TRUCK")) : 0;
    for(int i = 0; i < n; i++) {
        Truck *t = &trucks[i];
        t->model = (first + i) % TRUCK_MODELS;
        t->cpu = true;
        truck_place(t, v3(lvl_spawns[slot[i]][0], 0, lvl_spawns[slot[i]][2]), lvl_spawns[slot[i]][3]);
        ai_start(i, 1 + i % 3);
    }
    float dt = 1.0f / 60.0f, dig = 0;
    int touching = 0, meets = 0, bumps = 0;
    int meeting[MAX_TRUCKS][MAX_TRUCKS] = {{0}};   /* 1 passing, 2 and touched */
    for(int f = 0; f < 60 * 60 * minutes; f++) {
        r_cam.pos = trucks[0].pos;
        for(int i = 0; i < n; i++) ai_drive(i, dt);
        for(int i = 0; i < n; i++) truck_update(&trucks[i], dt);
        for(int i = 0; i < n; i++) {
            if(isnan(trucks[i].pos.x)) {
                printf("truck %d: NaN at %.0f s\n", i, f * dt);
                return 1;
            }
            for(int j = i + 1; j < n; j++) {
                float o = truck_overlap(&trucks[i], &trucks[j]);
                if(o > 0.02f) touching++;
                dig = fmaxf(dig, o);
                float gap = vlen(vsub(trucks[i].pos, trucks[j].pos));
                if(!meeting[i][j] && gap < 12.0f && vdot(trucks[i].ax[2], trucks[j].ax[2]) < -0.5f) {
                    meeting[i][j] = 1;
                    meets++;
                }
                if(meeting[i][j] == 1 && o > 0.02f) {
                    meeting[i][j] = 2;
                    bumps++;
                }
                if(gap > 20.0f) meeting[i][j] = 0;
            }
        }
        if(f % (60 * 60) == 0) {
            printf("%3d min:", f / 3600);
            for(int i = 0; i < n; i++)
                printf("  %s %.0f,%.0f,%.0f v %.1f", ai_route_name(i), (double)trucks[i].pos.x, (double)trucks[i].pos.y,
                       (double)trucks[i].pos.z, (double)trucks[i].speed);
            printf("\n");
        }
        if(getenv("SIM_TRACE_T") && f % 30 == 0) {
            int w = atoi(getenv("SIM_TRACE_T"));
            Truck *t = &trucks[w];
            printf("  t=%.1f %s pos %.1f %.1f %.1f v %.2f hd %.0f steer %.2f thr %.2f brk %.2f g %d\n", f * dt,
                   ai_route_name(w), (double)t->pos.x, (double)t->pos.y, (double)t->pos.z, (double)t->speed,
                   (double)(truck_heading(t) * 57.3f), (double)t->in.steer, (double)t->in.throttle, (double)t->in.brake,
                   t->gear);
        }
    }
    printf("loop: %d min, trucks touching for %.0f s, deepest %.2f m, %d met head on, %d of those touched\n", minutes,
           touching * dt, (double)dig, meets, bumps);
    return 0;
}

/* Where a recover puts a truck that has drowned: recover [step]. A truck
   is stood in every stretch of water deep enough to stall it (every
   step'th vertex), recovered as the player's is and left alone for a few
   seconds. A recover is bad if the truck ends up

     WET     back in the water (it rolled or fell in, or never left)
     TUMBLE  on its side, or thrown: put down on ground too steep to stand on
     PERCH   on top of a wall, a boulder or a bridge
     ISLAND  on land with no dry way to a trail

   and far if it is put more than 150 m from where it went in. Fails
   (exit 1) on any bad one. SIM_RECOVER_LIST=1 prints each. */
static uint8_t *mainland;

static void mainland_fill(void) {
    static int *queue;
    if(!mainland) mainland = malloc(LEVEL_N * LEVEL_N);
    if(!queue) queue = malloc(sizeof(int) * LEVEL_N * LEVEL_N);
    memset(mainland, 0, LEVEL_N * LEVEL_N);
    int qn = 0;
    for(int r = 0; r < LVL_ROUTES; r++)
        for(int p = 0; p < lvl_routes[r].n; p++) {
            int i = lvl_clampi((int)((lvl_routes[r].pts[p].x - LEVEL_ORG) / LEVEL_D + 0.5f));
            int j = lvl_clampi((int)((lvl_routes[r].pts[p].z - LEVEL_ORG) / LEVEL_D + 0.5f));
            if(!mainland[j * LEVEL_N + i]) {
                mainland[j * LEVEL_N + i] = 1;
                queue[qn++] = j * LEVEL_N + i;
            }
        }
    for(int q = 0; q < qn; q++) {
        int i = queue[q] % LEVEL_N, j = queue[q] / LEVEL_N;
        for(int b = j - 1; b <= j + 1; b++)
            for(int a = i - 1; a <= i + 1; a++) {
                if(a < 0 || b < 0 || a >= LEVEL_N || b >= LEVEL_N || mainland[b * LEVEL_N + a]) continue;
                float w = lvl_water[b * LEVEL_N + a];
                if(w > lvl_h(a, b) + 0.02f) continue;                      /* any water: a causeway under it is no way back */
                float run = (a != i && b != j) ? LEVEL_D * 1.4142f : LEVEL_D;
                if(fabsf(lvl_h(a, b) - lvl_h(i, j)) > 0.7f * run) continue;   /* 35 degrees: no truck's way */
                mainland[b * LEVEL_N + a] = 1;
                queue[qn++] = b * LEVEL_N + a;
            }
    }
}

static int run_recover(int model, int step) {
    game.ntrucks = 1;
    Truck *t = &trucks[0];
    t->model = model;
    t->cpu = false;
    mainland_fill();
    float dt = 1.0f / 60.0f;
    int n = 0, bad[4] = {0}, far = 0, list = getenv("SIM_RECOVER_LIST") != NULL;
    float worst = 0, sum = 0;
    static const char *const what[4] = {"WET", "TUMBLE", "PERCH", "ISLAND"};
    for(int j = 0; j < LEVEL_N; j += step)
        for(int i = 0; i < LEVEL_N; i += step) {
            float w = lvl_water[j * LEVEL_N + i];
            if(w < lvl_h(i, j) + 1.0f) continue;
            float x = LEVEL_ORG + i * LEVEL_D, z = LEVEL_ORG + j * LEVEL_D;
            truck_place(t, v3(x, 0, z), (float)((i * 7 + j * 13) % 63) * 0.1f);
            truck_recover(t);
            V3 at = t->pos;
            int fail = -1;
            if(ground_at(at.x, at.z, NULL, NULL) > terrain_height(at.x, at.z) + 0.5f) fail = 2;
            int gi = lvl_clampi((int)((at.x - LEVEL_ORG) / LEVEL_D + 0.5f)), gj = lvl_clampi((int)((at.z - LEVEL_ORG) / LEVEL_D + 0.5f));
            if(fail < 0 && !mainland[gj * LEVEL_N + gi]) fail = 3;
            for(int f = 0; f < 60 * 5 && fail < 0; f++) {
                r_cam.pos = t->pos;
                truck_update(t, dt);
                if(isnan(t->pos.x)) fail = 1;
            }
            if(fail < 0) {
                float dx = t->pos.x - at.x, dz = t->pos.z - at.z;
                if(t->stalled || t->water > 0.6f) fail = 0;
                else if(t->ax[1].y < 0.6f || dx * dx + dz * dz > 8.0f * 8.0f) fail = 1;
            }
            float d = sqrtf((at.x - x) * (at.x - x) + (at.z - z) * (at.z - z));
            n++;
            sum += d;
            if(d > worst) worst = d;
            if(d > 150.0f) far++;
            if(fail >= 0) bad[fail]++;
            if(list && (fail >= 0 || d > 150.0f))
                printf("  %-6s in at %6.0f %6.0f, put at %6.1f %5.1f %6.1f (%3.0f m), after 5 s at %6.1f %5.1f %6.1f\n",
                       fail >= 0 ? what[fail] : "far", (double)x, (double)z, (double)at.x, (double)at.y, (double)at.z, (double)d,
                       (double)t->pos.x, (double)t->pos.y, (double)t->pos.z);
        }
    int bads = bad[0] + bad[1] + bad[2] + bad[3];
    printf("recover: %s, %d drownings: %d wet, %d tumbled, %d perched, %d on islands; put back %.0f m away on average, "
           "%.0f at most, %d over 150 m: %s\n", lvl->name, n, bad[0], bad[1], bad[2], bad[3], n ? (double)(sum / n) : 0.0,
           (double)worst, far, bads ? "FAIL" : "PASS");
    return bads ? 1 : 0;
}

/* Power on through a turn from rest: turn<mode> [truck] [steer] [handbrake 0/1].
   Prints the body's slide angle (beta), yaw rate and the rear tyres' spin. */
static float turn_throttle = 1.0f;
static bool turn_drift;

static int run_turn(int mode, int model, float steer, int hb) {
    game.ntrucks = 1;
    Truck *t = &trucks[0];
    t->model = model;
    truck_place(t, v3(0, 0, -240), ENG_PI / 2);
    t->mode = mode;
    t->tcs_off = getenv("SIM_TCS_OFF") != NULL;
    float dt = 1.0f / 60.0f, beta_max = 0, yaw_max = 0;
    for(int f = 0; f < 60 * 7; f++) {
        float tm = f * dt;
        t->in.throttle = turn_throttle;
        t->in.brake = 0;
        t->in.steer = tm > 1.5f ? steer : 0;
        if(turn_drift && tm > 2.2f) {
            /* A driver holding a slide: opposite lock against the slide
               angle beyond 25 degrees, easing the throttle as it grows. */
            float lat0 = vdot(t->vel, t->ax[0]), fwd0 = vdot(t->vel, t->ax[2]);
            float b = atan2f(lat0, fmaxf(fabsf(fwd0), 0.5f));
            float over = -b - 0.44f;   /* turning right, the tail out left: b < 0 */
            t->in.steer = clampf(steer - over * 3.0f, -1, 1);
            t->in.throttle = clampf(0.75f - over * 2.0f, 0.2f, 1.0f);
        }
        t->in.handbrake = hb && tm > 3.0f && tm < 3.6f;
        r_cam.pos = t->pos;
        truck_update(t, dt);
        float lat = vdot(t->vel, t->ax[0]), fwd = vdot(t->vel, t->ax[2]);
        float beta = atan2f(lat, fmaxf(fabsf(fwd), 0.5f)) * 57.3f;
        float yaw = vdot(t->w, t->ax[1]) * 57.3f;
        if(tm > 1.5f && fabsf(beta) > beta_max) beta_max = fabsf(beta);
        if(fabsf(yaw) > yaw_max) yaw_max = fabsf(yaw);
        if(f % 30 == 0)
            printf("  t=%.1f v %5.1f beta %6.1f yaw %6.1f g %d rpm %4.0f rear slip %4.1f %4.1f front %4.1f %4.1f tc %.2f\n",
                   (double)tm, (double)fwd, (double)beta, (double)yaw, t->gear, (double)t->rpm, (double)t->wh[2].slip,
                   (double)t->wh[3].slip, (double)t->wh[0].slip, (double)t->wh[1].slip, (double)t->tc);
    }
    printf("max slide angle %.1f deg, max yaw rate %.0f deg/s\n", (double)beta_max, (double)yaw_max);
    return 0;
}

/* Two trucks meeting on the meadow: crash<case> <model>. The player's
   truck drives at a parked one (0 rear end, 1 its side at 90 degrees, 2 a
   corner at 45, 3 head on), or 4 rubs alongside a moving one, steering
   into it. Prints how far each tipped, lifted and dug into the other. */
static int run_crash(int kind, int model) {
    game.ntrucks = 2;
    Truck *a = &trucks[0], *b = &trucks[1];
    a->model = model;
    b->model = (model + 1) % TRUCK_MODELS;
    a->cpu = false;
    b->cpu = true;
    static const float ang[5] = {0, 90, 45, 180, 0};
    truck_place(a, v3(-20, 0, -240), ENG_PI / 2);
    if(kind == 4) truck_place(b, v3(-20, 0, -237.6f), ENG_PI / 2);
    else truck_place(b, v3(0, 0, -240), ENG_PI / 2 + ang[kind] * (ENG_PI / 180.0f));
    a->mode = b->mode = DRIVE_4WD;
    float dt = 1.0f / 60.0f, roll[2] = {0}, pitch[2] = {0}, lift[2] = {0}, gap = 1e9f, vmax = 0, dig = 0;
    float base[2];
    for(int i = 0; i < 2; i++) base[i] = trucks[i].pos.y - terrain_height(trucks[i].pos.x, trucks[i].pos.z);
    for(int f = 0; f < 60 * 8; f++) {
        float tm = f * dt;
        a->in.throttle = tm < 5.0f ? 1.0f : 0.0f;
        a->in.brake = tm < 5.0f ? 0.0f : 0.6f;
        a->in.steer = kind == 4 && tm > 1.0f ? -0.35f : 0.0f;
        b->in.throttle = kind == 4 && tm < 5.0f ? 1.0f : 0.0f;
        b->in.brake = kind == 4 ? (tm < 5.0f ? 0.0f : 0.6f) : 0.4f;
        b->in.steer = 0;
        r_cam.pos = a->pos;
        truck_update(a, dt);
        truck_update(b, dt);
        if(fabsf(a->speed) > vmax) vmax = fabsf(a->speed);
        for(int i = 0; i < 2; i++) {
            Truck *t = &trucks[i];
            roll[i] = fmaxf(roll[i], fabsf(t->roll));
            pitch[i] = fmaxf(pitch[i], fabsf(t->pitch));
            lift[i] = fmaxf(lift[i], t->pos.y - terrain_height(t->pos.x, t->pos.z) - base[i]);
        }
        V3 d = vsub(b->pos, a->pos);
        d.y = 0;
        gap = fminf(gap, vlen(d));
        dig = fmaxf(dig, truck_overlap(a, b));
        if(getenv("SIM_TRACE") && f % 15 == 0)
            printf("  t=%.2f a %6.2f %6.2f %7.2f v %5.2f roll %5.1f pitch %5.1f | b %6.2f %6.2f %7.2f roll %5.1f pitch %5.1f\n",
                   (double)tm, (double)a->pos.x, (double)a->pos.y, (double)a->pos.z, (double)a->speed, (double)a->roll,
                   (double)a->pitch, (double)b->pos.x, (double)b->pos.y, (double)b->pos.z, (double)b->roll, (double)b->pitch);
    }
    printf("crash%d: hit at %.1f m/s, closest centres %.2f m, dug in %.2f m | player roll %.0f pitch %.0f lift %.2f m | other roll %.0f pitch %.0f lift %.2f m\n",
           kind, (double)vmax, (double)gap, (double)dig, (double)roll[0], (double)pitch[0], (double)lift[0], (double)roll[1], (double)pitch[1],
           (double)lift[1]);
    return 0;
}

int main(int argc, char **argv) {
    const char *scen = argc > 1 ? argv[1] : "climb";
    int model = argc > 2 ? atoi(argv[2]) : 1;
    /* SIM_AREA=1 for the desert, 2 for the dunes, 3 for the Highlands. */
    game.area = getenv("SIM_AREA") ? atoi(getenv("SIM_AREA")) : 0;
    void *grid = aligned_alloc(32, (LEVEL_GRID_BYTES + 31) & ~31);
    if(!terrain_load(game.area, grid)) return 2;
    if(!strcmp(scen, "unlocks")) return run_unlocks(grid);
    if(getenv("SIM_THROTTLE")) turn_throttle = atof(getenv("SIM_THROTTLE"));
    turn_drift = getenv("SIM_DRIFT") != NULL;
    if(!strncmp(scen, "turn", 4))
        return run_turn(scen[4] - '0', model, argc > 3 ? atof(argv[3]) : 0.5f, argc > 4 ? atoi(argv[4]) : 0);
    if(!strncmp(scen, "crash", 5)) return run_crash(scen[5] - '0', model);
    if(!strcmp(scen, "xtrail")) {
        truck_spec(&trucks[0].spec, model);
        return run_xtrail(model);
    }
    if(!strcmp(scen, "recover")) return run_recover(getenv("SIM_TRUCK") ? atoi(getenv("SIM_TRUCK")) : 0, argc > 2 ? atoi(argv[2]) : 2);
    if(!strcmp(scen, "loop")) return run_loop(argc > 2 ? atoi(argv[2]) : 60, argc > 3 ? atoi(argv[3]) : 4);
    if(!strcmp(scen, "route")) {
        int route = argc > 3 ? atoi(argv[3]) : 1;
        return !run_route(route, model, getenv("SIM_QUIET") != NULL);
    }
    game.ntrucks = 1;
    Truck *t = &trucks[0];
    t->model = model;
    float x = -16, z = -150, h = atan2f(-6.8f + 16, -144.3f + 150);
    int mode = DRIVE_LOCK;
    float throttle = 1, brake = 0, secs = 8;
    if(!strcmp(scen, "flat")) { x = 40; z = -280; h = 0; mode = DRIVE_4WD; }
    if(!strcmp(scen, "park")) { throttle = 0; }
    if(!strcmp(scen, "spawn")) {
        /* Hands off at the drive's start: the truck should sit still. */
        x = lvl_spawns[1][0];
        z = lvl_spawns[1][2];
        h = lvl_spawns[1][3];
        mode = getenv("SIM_MODE") ? atoi(getenv("SIM_MODE")) : DRIVE_2WD;
        throttle = 0;
        secs = 20;
    }
    if(!strcmp(scen, "climb2wd")) mode = DRIVE_2WD;
    if(!strcmp(scen, "climb4wd")) mode = DRIVE_4WD;
    if(!strncmp(scen, "speed", 5)) {
        /* Flat out east across the meadow: speed<mode> */
        x = 0;
        z = -240;
        h = ENG_PI / 2;
        mode = scen[5] - '0';
        secs = 14;
    }
    if(!strcmp(scen, "at") && argc >= 7) {
        x = atof(argv[3]);
        z = atof(argv[4]);
        h = atan2f(atof(argv[5]) - x, atof(argv[6]) - z);
        secs = 6;
    }
    truck_place(t, v3(x, 0, z), h);
    t->mode = mode;
    t->tcs_off = getenv("SIM_TCS_OFF") != NULL;
    r_cam.pos = t->pos;
    float dt = 1.0f / 60.0f;
    if(getenv("SIM_TRACE")) {
        /* Step by step for the first frames: what pushes. */
        for(int f = 0; f < 4; f++) {
            t->in.throttle = 0;
            truck_update(t, dt);
            printf("trace f%d pos %.2f %.2f %.2f vel %.2f %.2f %.2f w %.2f %.2f %.2f chassis %.0f comp %.2f %.2f %.2f %.2f\n", f,
                   (double)t->pos.x, (double)t->pos.y, (double)t->pos.z, (double)t->vel.x, (double)t->vel.y,
                   (double)t->vel.z, (double)t->w.x, (double)t->w.y, (double)t->w.z, (double)t->dbg_chassis,
                   (double)t->wh[0].comp, (double)t->wh[1].comp, (double)t->wh[2].comp, (double)t->wh[3].comp);
        }
        return 0;
    }
    for(int f = 0; f < (int)(secs * 60); f++) {
        t->in.throttle = throttle;
        t->in.brake = brake;
        t->in.steer = 0;
        truck_update(t, dt);
        r_cam.pos = t->pos;
        if(f % 15 == 0) report(t, f * dt);
        if(f == 0 || f == 120) {
            /* What the ground ahead looks like. */
            printf("  ground ahead:");
            for(int k = -2; k <= 8; k++) {
                V3 q = vmad(t->pos, t->ax[2], k * 0.5f);
                V3 n;
                int sf;
                printf(" %.2f(%d)", (double)ground_at(q.x, q.z, &n, &sf), sf);
            }
            printf("\n");
        }
    }
    return 0;
}
