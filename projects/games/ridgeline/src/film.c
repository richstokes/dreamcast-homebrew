/* Ridgeline film builds (film.h). */
#include "film.h"
#ifdef RL_FILM
#include "truck.h"
#include "ai.h"
#include "camera.h"
#include "audio.h"
#include "fx.h"
#include "terrain.h"
#include "tod.h"
#include "world.h"
#include "deer.h"
#include "birds.h"
#include "engine/frame.h"

/* Camera shots. Their numbers, a[]:
   SH_HELI    radius, height, turn rate (rad/s, signed), start angle from
              the truck's heading
   SH_POST    a tripod by the trail: metres up the route from where the
              truck is placed, metres to the side, height
   SH_STATIC  eye x, height above the ground, z; look x, height, z (all 0:
              follow the truck); dolly to x, height, z over the shot
   SH_LEAD    ahead of the truck looking back: metres ahead, to the side,
              height
   SH_SIDE    beside the truck, keeping pace: metres to the side (- left),
              height, metres ahead */
enum { SH_CHASE, SH_FAR, SH_BUMPER, SH_HELI, SH_POST, SH_STATIC, SH_LEAD, SH_SIDE };

typedef struct {
    int8_t model, livery;
    /* The route it is on (level.h order: 0 forest road, 1 logging road,
       2 ridge line, 3 creek bed, 4 summit trail), or -1 for the scene's
       path. */
    int8_t route;
    int8_t lights;      /* LIGHTS_*, or -1: the CPU driver's call */
    float x, z;         /* placed at the route's point nearest here... */
    float back;         /* ...this many metres back down it */
    float delay;        /* seconds before it sets off */
} FilmTruck;

typedef struct {
    const char *name;
    float hour, rate;   /* the start hour; hours a second for a time-lapse */
    float pre, secs;    /* run-up before "go", then the shot */
    int shot, watch;    /* the camera, and the truck it is on (and we hear) */
    float fov;          /* 0: 1.2 */
    float a[9];
    int n;
    FilmTruck t[MAX_TRUCKS];
    /* A path off the routes for trucks with route -1 (x, z pairs), its
       speed (m/s) and drive mode. */
    const float (*path)[2];
    int npath;
    float path_speed;
    int path_mode;
} Scene;

/* ---- Paths off the routes ---- */
static const float p_mud[][2] = {{20, -279}, {44, -270}, {62, -262}, {82, -255}, {110, -246}, {140, -238}};
static const float p_ford[][2] = {{-30, -216}, {-48, -216.5f}, {-61, -217}, {-80, -219}, {-95, -221}, {-115, -223}, {-140, -226}};
static const float p_boulders[][2] = {{-100, -266}, {-114, -260}, {-126, -255}, {-138, -247}, {-150, -238}, {-170, -226}};
static const float p_deer[][2] = {{100, -232}, {112, -224}, {130, -210}, {150, -198}, {170, -192}, {200, -188}, {230, -184}};
static const float p_pond[][2] = {{-34, -287}, {-40, -286}, {-56, -283}, {-76, -280}, {-92, -276}, {-110, -270}};
static const float p_slide[][2] = {{-20, -262}, {0, -252}, {22, -244}, {34, -232}, {32, -216}, {18, -208}, {0, -212}, {-12, -226}, {-6, -244}, {12, -250}};
#define PATH(p) p, (int)(sizeof(p) / sizeof(p[0]))

#define T_CASCADE 0
#define T_KESTREL 1
#define T_BOBCAT 2

static const Scene scenes[] = {
    /* Dawn mist over the meadow, the clock running fast. */
    {"meadow-timelapse", 5.0f, 0.22f, 2.0f, 12.0f, SH_STATIC, 0, 1.05f,
     {10, 6, -300, 20, 40, 60}, 0, {{0}}, NULL, 0, 0, 0},
    /* First light: all four set off from the grid, lights on. */
    {"grid-dawn", 6.0f, 0, 4.0f, 12.0f, SH_LEAD, 0, 1.1f,
     {16, -3, 2.0f},
     4,
     {{T_KESTREL, 0, 1, LIGHTS_DIPPED, -3, -279, 0, 0.3f},
      {T_CASCADE, 1, 2, LIGHTS_DIPPED, 3, -279, 0, 1.0f},
      {T_BOBCAT, 2, 3, LIGHTS_DIPPED, -9, -281, 0, 1.7f},
      {T_KESTREL, 3, 1, LIGHTS_DIPPED, 9, -281, 0, 2.4f}},
     NULL, 0, 0, 0},
    /* The old pickup in 2WD through the mud wallow. */
    {"mud-wallow", 8.3f, 0, 3.0f, 10.0f, SH_STATIC, 0, 1.0f,
     {70, 1.0f, -270, 0, 0, 0},
     1, {{T_CASCADE, 2, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_mud), 10.0f, DRIVE_2WD},
    {"mud-side", 8.3f, 0, 3.0f, 9.0f, SH_SIDE, 0, 1.15f,
     {-6, 0.9f, 1.0f},
     1, {{T_CASCADE, 2, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_mud), 10.0f, DRIVE_2WD},
    /* The Kestrel through the deep ford and under the stone arch. */
    {"ford-arch", 9.6f, 0, 3.0f, 14.0f, SH_STATIC, 0, 1.0f,
     {-50, 2.2f, -209, 0, 0, 0},
     1, {{T_KESTREL, 1, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_ford), 6.0f, DRIVE_4WD},
    {"ford-bumper", 9.6f, 0, 3.0f, 9.0f, SH_BUMPER, 0, 0,
     {0},
     1, {{T_KESTREL, 1, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_ford), 6.0f, DRIVE_4WD},
    {"arch-front", 9.6f, 0, 8.0f, 8.0f, SH_STATIC, 0, 1.0f,
     {-114, 1.5f, -226, 0, 0, 0},
     1, {{T_KESTREL, 1, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_ford), 6.0f, DRIVE_4WD},
    /* Along the pond's shore. */
    {"pond-side", 10.5f, 0, 3.0f, 9.0f, SH_SIDE, 0, 1.1f,
     {-8, 1.0f, 2},
     1, {{T_BOBCAT, 0, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_pond), 8.0f, DRIVE_4WD},
    /* The Bobcat crawling the boulder field. */
    {"boulders", 11.0f, 0, 3.0f, 14.0f, SH_HELI, 0, 1.15f,
     {13, 4.5f, 0.22f, 2.2f},
     1, {{T_BOBCAT, 1, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_boulders), 2.6f, DRIVE_LOCK},
    /* Deer in the east meadow. */
    {"deer", 12.0f, 0, 3.0f, 12.0f, SH_STATIC, 0, 0.95f,
     {164, 1.4f, -185, 0, 0, 0},
     1, {{T_KESTREL, 2, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_deer), 9.0f, DRIVE_4WD},
    /* Up the logging road's east flank, a convoy. */
    {"logging-heli", 14.0f, 0, 4.0f, 14.0f, SH_HELI, 0, 1.15f,
     {16, 6, -0.18f, -2.4f},
     3,
     {{T_CASCADE, 0, 1, -1, 160, -70, 0, 0},
      {T_KESTREL, 2, 1, -1, 160, -70, 14, 0},
      {T_BOBCAT, 3, 1, -1, 160, -70, 28, 0}},
     NULL, 0, 0, 0},
    /* The washout. */
    {"washout", 15.0f, 0, 4.0f, 12.0f, SH_POST, 0, 0,
     {40, 6, 3.0f},
     2,
     {{T_KESTREL, 0, 1, -1, 238, 31, 40, 0},
      {T_CASCADE, 3, 1, -1, 238, 31, 54, 0}},
     NULL, 0, 0, 0},
    /* The log bridge and the ford below it. */
    {"log-bridge", 16.0f, 0, 4.0f, 12.0f, SH_STATIC, 0, 1.0f,
     {197, 3.5f, 84, 0, 0, 0},
     2,
     {{T_BOBCAT, 1, 1, -1, 192, 101, 34, 0},
      {T_KESTREL, 2, 1, -1, 192, 101, 48, 0}},
     NULL, 0, 0, 0},
    /* Rock on the ridge line. */
    {"ridge-rock", 16.5f, 0, 3.0f, 12.0f, SH_POST, 0, 0,
     {28, 4, 1.0f},
     1, {{T_BOBCAT, 2, 2, -1, 16, -75, 12, 0}}, NULL, 0, 0, 0},
    {"knife-edge", 17.2f, 0, 3.0f, 14.0f, SH_HELI, 0, 1.15f,
     {18, 7, 0.16f, 2.6f},
     1, {{T_KESTREL, 3, 2, -1, 72, 12, 18, 0}}, NULL, 0, 0, 0},
    /* Wading the creek below Cedar Falls. */
    {"creek-falls", 17.6f, 0, 3.0f, 12.0f, SH_POST, 0, 0,
     {26, -7, 3.5f},
     2,
     {{T_CASCADE, 1, 3, -1, -72, 43, 20, 0},
      {T_KESTREL, 0, 3, -1, -72, 43, 34, 0}},
     NULL, 0, 0, 0},
    /* Golden hour on the summit trail past the lake. */
    {"lake-side", 18.1f, 0, 3.0f, 12.0f, SH_STATIC, 0, 1.0f,
     {22, 1.4f, 62, 0, 0, 0},
     1, {{T_KESTREL, 2, 4, -1, 2, 44, 0, 0}}, NULL, 0, 0, 0},
    {"summit-sunset", 18.4f, 0, 3.0f, 15.0f, SH_HELI, 0, 1.2f,
     {22, 9, 0.12f, 2.4f},
     1, {{T_BOBCAT, 3, 4, -1, 30, 140, 40, 0}}, NULL, 0, 0, 0},
    /* Dusk, lights on, coming down past the high camp. */
    {"camp-dusk", 19.1f, 0, 3.0f, 12.0f, SH_LEAD, 0, 1.15f,
     {9, -1.5f, 1.8f},
     1, {{T_CASCADE, 0, 4, LIGHTS_DIPPED, -3, 85, 30, 0}}, NULL, 0, 0, 0},
    /* Night: full beam through the forest. */
    {"night-far", 22.5f, 0, 3.0f, 12.0f, SH_FAR, 0, 0,
     {0},
     1, {{T_BOBCAT, 1, 1, LIGHTS_FULL, 266, 100, 0, 0}}, NULL, 0, 0, 0},
    {"night-lead", 22.5f, 0, 3.0f, 12.0f, SH_LEAD, 0, 1.1f,
     {15, 4, 2.5f},
     1, {{T_CASCADE, 3, 1, LIGHTS_DIPPED, 266, 100, 0, 0}}, NULL, 0, 0, 0},
    {"night-convoy", 23.0f, 0, 4.0f, 12.0f, SH_POST, 0, 0,
     {40, 5, 2.4f},
     3,
     {{T_BOBCAT, 0, 1, LIGHTS_FULL, 254, 64, 30, 0},
      {T_CASCADE, 2, 1, LIGHTS_DIPPED, 254, 64, 44, 0},
      {T_KESTREL, 3, 1, LIGHTS_FULL, 254, 64, 58, 0}},
     NULL, 0, 0, 0},
    /* The view from the top, the day going. */
    {"summit-timelapse", 17.5f, 0.3f, 2.0f, 14.0f, SH_STATIC, 0, 1.1f,
     {60, 30, 120, -100, 20, -300}, 0, {{0}}, NULL, 0, 0, 0},
    /* The old pickup in 2WD, the tail stepping out on the meadow grass. */
    {"meadow-slide", 9.0f, 0, 3.0f, 12.0f, SH_HELI, 0, 1.15f,
     {15, 5, 0.2f, 1.8f},
     1, {{T_CASCADE, 0, -1, LIGHTS_OFF, 0, 0, 0, 0}}, PATH(p_slide), 13.0f, DRIVE_2WD},
    /* Sunrise mist on the forest road. */
    {"forest-mist", 6.5f, 0, 4.0f, 12.0f, SH_HELI, 0, 1.15f,
     {18, 7, 0.15f, 2.4f},
     3,
     {{T_BOBCAT, 1, 0, LIGHTS_DIPPED, 8, -160, 0, 0},
      {T_CASCADE, 0, 0, LIGHTS_DIPPED, 8, -160, 12, 0},
      {T_KESTREL, 3, 0, LIGHTS_DIPPED, 8, -160, 24, 0}},
     NULL, 0, 0, 0},
    /* Arriving at the lookout as the sun goes. */
    {"summit-arrive", 18.3f, 0, 3.0f, 12.0f, SH_POST, 0, 0,
     {38, 6, 2.5f},
     1, {{T_KESTREL, 0, 4, LIGHTS_DIPPED, 30, 140, 50, 0}}, NULL, 0, 0, 0},
};
#define NSCENES (int)(sizeof(scenes) / sizeof(scenes[0]))

static struct {
    int scene;
    float t;            /* seconds since the scene started (go at its pre) */
    bool go;
    int late;           /* frames that ran long during the shot */
    const Scene *s;
    int pk[MAX_TRUCKS]; /* path trucks: the point each is heading for */
    V3 eye, post;
    bool eye_set;
    float yaw, heli_a;
} fm;

/* ---- Placing trucks ---- */

static int nearest_pt(const lvl_route_t *r, float x, float z) {
    int best = 0;
    float bd = 1e18f;
    for(int i = 0; i < r->n; i++) {
        float dx = r->pts[i].x - x, dz = r->pts[i].z - z, d = dx * dx + dz * dz;
        if(d < bd) {
            bd = d;
            best = i;
        }
    }
    return best;
}

/* The point d metres along the route from point i (back down it for a
   negative d), and which point that is. */
static V3 route_walk(const lvl_route_t *r, int i, float d, int *at) {
    int step = d < 0 ? -1 : 1;
    d = fabsf(d);
    V3 p = v3(r->pts[i].x, r->pts[i].y, r->pts[i].z);
    while(i + step >= 0 && i + step < r->n) {
        V3 q = v3(r->pts[i + step].x, r->pts[i + step].y, r->pts[i + step].z);
        float seg = vlen(vsub(q, p));
        if(seg >= d) break;
        d -= seg;
        p = q;
        i += step;
    }
    if(at) *at = i;
    return p;
}

static float route_heading(const lvl_route_t *r, int i) {
    int j = i + 1 < r->n ? i + 1 : i, k = j == i ? i - 1 : i;
    return atan2f(r->pts[j].x - r->pts[k].x, r->pts[j].z - r->pts[k].z);
}

static void scene_begin(int n) {
    const Scene *s = &scenes[n];
    fm.scene = n;
    fm.s = s;
    fm.t = 0;
    fm.go = false;
    fm.late = 0;
    fm.eye_set = false;
    game.ntrucks = s->n;
    for(int i = 0; i < MAX_TRUCKS; i++) trucks[i].remote = trucks[i].absent = false;
    for(int i = 0; i < s->n; i++) {
        const FilmTruck *f = &s->t[i];
        Truck *t = &trucks[i];
        t->model = f->model;
        t->livery = f->livery;
        t->tcs_off = false;
        t->horn = false;
        if(f->route < 0) {
            truck_place(t, v3(s->path[0][0], 0, s->path[0][1]),
                        atan2f(s->path[1][0] - s->path[0][0], s->path[1][1] - s->path[0][1]));
            truck_set_mode(t, s->path_mode);
            fm.pk[i] = 1;
        }
        else {
            const lvl_route_t *r = &lvl_routes[f->route];
            int at;
            route_walk(r, nearest_pt(r, f->x, f->z), -f->back, &at);
            if(f->x == 0 && f->z == 0) at = 0;
            truck_place(t, v3(r->pts[at].x, 0, r->pts[at].z), route_heading(r, at));
            int plan = f->route >= 1 && f->route <= 3 ? f->route : 1 + i % 3;
            ai_start_on(i, plan, f->route, at, f->delay);
            if(i == 0 && s->shot == SH_POST) {
                int pa;
                V3 p = route_walk(r, at, s->a[0], &pa);
                float h = route_heading(r, pa);
                V3 side = v3(cosf(h), 0, -sinf(h));
                fm.post = vmad(p, side, s->a[1]);
            }
        }
        t->cpu = f->lights < 0;
        t->lights = f->lights < 0 ? LIGHTS_OFF : (uint8_t)f->lights;
    }
    if(s->shot == SH_POST) fm.post.y = terrain_height(fm.post.x, fm.post.z) + s->a[2];
    if(s->n > 0) fm.yaw = truck_heading(&trucks[s->watch]);
    fm.heli_a = fm.yaw + s->a[3];
    tod_set_hour(s->hour);
    tod_relight_all();
    fx_clear();
    deer_reset();
    birds_reset();
    camera_reset();
    if(s->n > 0) {
        audio_drive_start();
        if(s->watch) audio_listen(s->watch);
    }
    else audio_drive_stop();
    printf("RL FILM scene %d %s\n", n, s->name);
}

/* -DRL_FILM_PICK=3,7,8 films only those scenes, in that order. */
#ifdef RL_FILM_PICK
static const int pick[] = {RL_FILM_PICK};
#define NPICK (int)(sizeof(pick) / sizeof(pick[0]))
#endif
static int pick_i;

static int scene_at(int i) {
#ifdef RL_FILM_PICK
    return i < NPICK ? pick[i] : NSCENES;
#else
    return i;
#endif
}

void film_start(void) {
    game.paused = game.photo = false;
    game.cam_mode = 0;
    game_set_state(ST_DRIVE);
    pick_i = 0;
    scene_begin(scene_at(0));
}

/* ---- Driving a path ---- */

static void path_drive(int i, float dt) {
    const Scene *s = fm.s;
    Truck *t = &trucks[i];
    TruckInput *in = &t->in;
    int *k = &fm.pk[i];
    for(;;) {
        if(*k >= s->npath) break;
        float dx = s->path[*k][0] - t->pos.x, dz = s->path[*k][1] - t->pos.z;
        /* Past it, or near: on to the next. */
        V3 seg = *k > 0 ? v3(s->path[*k][0] - s->path[*k - 1][0], 0, s->path[*k][1] - s->path[*k - 1][1]) : v3(0, 0, 1);
        if(dx * dx + dz * dz < 5.0f * 5.0f || vdot(seg, v3(dx, 0, dz)) < 0) (*k)++;
        else break;
    }
    if(*k >= s->npath) {
        in->steer = 0;
        in->throttle = 0;
        in->brake = 0.7f;
        in->handbrake = false;
        return;
    }
    /* Aim a little past the point, along the path, for a smooth line. */
    V3 tgt = v3(s->path[*k][0], 0, s->path[*k][1]);
    V3 d = vsub(tgt, t->pos);
    float lx = vdot(d, t->ax[0]), lz = vdot(d, t->ax[2]);
    const TruckSpec *sp = &t->spec;
    float wb = sp->wheel[WFL].z - sp->wheel[WRL].z;
    float curv = 2.0f * lx / fmaxf(lx * lx + lz * lz, 1.0f);
    float steer = atanf(wb * curv) / sp->max_steer;
    if(lz < 0.5f) steer = lx >= 0 ? 1.0f : -1.0f;
    float err = s->path_speed - t->speed;
    in->steer = clampf(steer, -1, 1);
    in->throttle = clampf(err * 0.45f + 0.2f, 0, 1);
    in->brake = clampf(-err * 0.3f, 0, 1);
    in->handbrake = false;
    (void)dt;
}

/* ---- The camera ---- */

static void eye_to(V3 want, float k, float dt) {
    float g = terrain_height(want.x, want.z) + 0.6f;
    if(want.y < g) want.y = g;
    if(!fm.eye_set) {
        fm.eye = want;
        fm.eye_set = true;
    }
    fm.eye = vlerp(fm.eye, want, smooth_k(k, dt));
    g = terrain_height(fm.eye.x, fm.eye.z) + 0.5f;
    if(fm.eye.y < g) fm.eye.y = g;
}

static V3 agl(float x, float h, float z) {
    return v3(x, terrain_height(x, z) + h, z);
}

static void film_camera(float dt) {
    const Scene *s = fm.s;
    const float *a = s->a;
    float fov = s->fov > 0 ? s->fov : 1.2f;
    const Truck *t = s->n > 0 ? &trucks[s->watch] : NULL;
    V3 focus = t ? vadd(t->pos, v3(0, 0.8f, 0)) : v3(0, 0, 0);
    if(t) fm.yaw += wrap_angle(truck_heading(t) - fm.yaw) * smooth_k(2.0f, dt);
    V3 fwd = heading_fwd(fm.yaw), side = v3(fwd.z, 0, -fwd.x);
    switch(s->shot) {
        case SH_CHASE:
        case SH_FAR:
        case SH_BUMPER:
            game.cam_mode = s->shot == SH_CHASE ? 0 : s->shot == SH_FAR ? 1 : 2;
            camera_follow(t, dt);
            break;
        case SH_HELI: {
            fm.heli_a += dt * a[2];
            float k = clampf((fm.t - s->pre) / s->secs, 0, 1);
            float h = a[1] * (1.0f - 0.3f * k);
            eye_to(vadd(t->pos, v3(sinf(fm.heli_a) * a[0], h, cosf(fm.heli_a) * a[0])), 4.0f, dt);
            camera_set(fm.eye, vadd(vmad(focus, fwd, 3.0f), v3(0, 0.4f, 0)), fov);
            break;
        }
        case SH_POST: {
            float d = vlen(vsub(focus, fm.post));
            camera_set(fm.post, focus, clampf(1.45f - d * 0.02f, 0.7f, 1.3f));
            break;
        }
        case SH_STATIC: {
            V3 eye = agl(a[0], a[1], a[2]);
            if(a[6] != 0 || a[8] != 0) {
                float k = clampf((fm.t - s->pre) / s->secs, 0, 1);
                k = k * k * (3.0f - 2.0f * k);
                eye = vlerp(eye, agl(a[6], a[7], a[8]), k);
            }
            V3 look;
            if(a[3] == 0 && a[5] == 0 && t) {
                if(!fm.eye_set) {
                    fm.post = focus;
                    fm.eye_set = true;
                }
                fm.post = vlerp(fm.post, focus, smooth_k(5.0f, dt));
                look = fm.post;
            }
            else look = agl(a[3], a[4], a[5]);
            camera_set(eye, look, fov);
            break;
        }
        case SH_LEAD:
            eye_to(vadd(vadd(vmad(t->pos, fwd, a[0]), vscale(side, a[1])), v3(0, a[2], 0)), 3.0f, dt);
            camera_set(fm.eye, vadd(focus, v3(0, 0.2f, 0)), fov);
            break;
        case SH_SIDE:
            eye_to(vadd(vadd(vmad(t->pos, side, a[0]), vscale(fwd, a[2])), v3(0, a[1], 0)), 3.0f, dt);
            camera_set(fm.eye, vmad(focus, fwd, 2.5f), fov);
            break;
    }
}

void film_update(float dt) {
    const Scene *s = fm.s;
    if(fm.scene >= NSCENES) return;
    for(int i = 0; i < game.ntrucks; i++) {
        if(s->t[i].route < 0) path_drive(i, dt);
        else ai_drive(i, dt);
    }
    for(int i = 0; i < game.ntrucks; i++) truck_update(&trucks[i], dt);
    game.clock += dt;
    fx_update(dt);
    /* A time-lapse runs the clock on (tod_update relights as it can). */
    if(s->rate > 0 && fm.go) tod.hour = fmodf(tod.hour + s->rate * dt, 24.0f);
    fm.t += dt;
    film_camera(dt);
    if(!fm.go && fm.t >= s->pre) {
        fm.go = true;
        printf("RL FILM go %d %s\n", fm.scene, s->name);
    }
    if(fm.go && eng_frame.steps > 1) fm.late++;
    if(fm.t >= s->pre + s->secs) {
        printf("RL FILM cut %d %s late %d\n", fm.scene, s->name, fm.late);
        int next = scene_at(++pick_i);
        if(next >= NSCENES) {
            printf("RL FILM done\n");
            fm.scene = NSCENES;
            return;
        }
        scene_begin(next);
    }
}
#endif
