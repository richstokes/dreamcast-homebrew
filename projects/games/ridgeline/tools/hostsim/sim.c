/* Ridgeline physics on the host: drives a truck over the real level with
   scripted inputs and prints what it does, for tuning without Flycast.
   make -C tools/hostsim && tools/hostsim/sim <scenario> */
#include "rl.h"
#include "truck.h"
#include "terrain.h"
#include "ai.h"
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

/* The CPU driver over a whole route, as fast as the host goes. */
static int run_route(int route, int model, int quiet) {
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
            printf("route %d truck %d: SUMMIT in %.0f s\n", route, model, f * dt);
            return 1;
        }
    }
    printf("route %d truck %d: no summit after 1200 s, best height %.0f of %.0f, prog %.2f, at %.0f %.0f %.0f\n", route,
           model, (double)best, (double)LEVEL_SUMMIT_Y, (double)ai_progress(0), (double)t->pos.x, (double)t->pos.y,
           (double)t->pos.z);
    return 0;
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
    b->model = model ^ 1;
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
    terrain_init();
    if(getenv("SIM_THROTTLE")) turn_throttle = atof(getenv("SIM_THROTTLE"));
    turn_drift = getenv("SIM_DRIFT") != NULL;
    if(!strncmp(scen, "turn", 4))
        return run_turn(scen[4] - '0', model, argc > 3 ? atof(argv[3]) : 0.5f, argc > 4 ? atoi(argv[4]) : 0);
    if(!strncmp(scen, "crash", 5)) return run_crash(scen[5] - '0', model);
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
