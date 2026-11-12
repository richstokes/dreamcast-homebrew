/* Ridgeline demo mode (demo.h). Four CPU trucks, one of each route plus a
   second on the logging road, set off from the meadow as a normal run
   does; the camera cuts every few seconds to another truck that is on the
   move, rotating through a chase, a helicopter orbit, a trackside camera
   the truck drives past, and the far chase. */
#include "demo.h"
#include "truck.h"
#include "ai.h"
#include "camera.h"
#include "hud.h"
#include "audio.h"
#include "fx.h"
#include "menu.h"
#include "terrain.h"
#include "sound_assets.h"
#include "tod.h"
#include "world.h"

enum { SHOT_CHASE, SHOT_HELI, SHOT_TRACKSIDE, SHOT_FAR, SHOT_COUNT };
static const float shot_len[SHOT_COUNT] = {6.0f, 9.0f, 6.0f, 6.0f};
#define DEMO_SECONDS 180.0f

static struct {
    int shot, cut;
    float t;              /* seconds into this shot */
    int saved_cam_mode;
    /* Helicopter: angle round the truck and the smoothed eye. */
    float heli_a;
    V3 eye;
    bool eye_set;
    /* Trackside: where the camera stands. */
    V3 post;
} dm;

static void cut(void);

void demo_start(void) {
    /* A different area from the last demo. */
    static int next_area = AREA_MOAB;
    game_load_area(next_area);
    next_area = (next_area + 1) % AREA_COUNT;
    game.demo = true;
    game.ntrucks = MAX_TRUCKS;
    static const int slot[4] = {1, 2, 0, 3};
    for(int i = 0; i < game.ntrucks; i++) {
        Truck *t = &trucks[i];
        t->model = (game.truck_model + i) % TRUCK_MODELS;
        t->livery = (game.livery + i) % 4;
        t->cpu = true;
        t->tcs_off = false;
        const float *s = lvl_spawns[slot[i]];
        truck_place(t, v3(s[0], 0, s[2]), s[3]);
        ai_start(i, 1 + i % 3);
    }
    game.best_y = trucks[0].pos.y;
    game.summit_frac = 0;
    game.views_found = 0;
    game.last_view = -1;
    game.summit_reached = false;
    game.clock = 0;
    game.paused = false;
    game.photo = false;
    dm.saved_cam_mode = game.cam_mode;
    dm.cut = -1;
    dm.shot = SHOT_FAR;   /* so the first cut is a chase */
    game.demo_truck = MAX_TRUCKS - 1;
    fx_clear();
    audio_drive_start();
    game_set_state(ST_DRIVE);
    /* Any hour of the day, a different one each time; the CPU drivers
       put their lights on in the dark (ai.c). */
    tod_set_hour(tod_hours[(int)(timer_us_gettime64() / 1000) % TOD_COUNT]);
    for(int i = 0; i < game.ntrucks; i++) trucks[i].lights = LIGHTS_OFF;
    cut();
    printf("RL demo: start\n");
}

void demo_stop(void) {
    audio_drive_stop();
    game.cam_mode = dm.saved_cam_mode;
    game.demo = false;
    game.paused = false;
    game.photo = false;
    game_set_state(ST_TITLE);
    printf("RL demo: stop at %.0f s\n", (double)game.clock);
}

/* The next truck round that is actually driving (the CPUs set off one
   after another, and one may be backing off a ledge); any of them if none
   is. */
static int pick_truck(void) {
    for(int k = 1; k <= game.ntrucks; k++) {
        int i = (game.demo_truck + k) % game.ntrucks;
        if(trucks[i].speed > 1.0f) return i;
    }
    return (game.demo_truck + 1) % game.ntrucks;
}

static void cut(void) {
    dm.cut++;
    dm.t = 0;
    game.demo_truck = pick_truck();
    dm.shot = (dm.shot + 1) % SHOT_COUNT;
    const Truck *t = &trucks[game.demo_truck];
    float heading = truck_heading(t);
    audio_listen(game.demo_truck);
    camera_reset();
    dm.eye_set = false;
    switch(dm.shot) {
        case SHOT_CHASE: game.cam_mode = 0; break;
        case SHOT_FAR: game.cam_mode = 1; break;
        case SHOT_HELI:
            /* Start off its shoulder, behind, and circle forward past it. */
            dm.heli_a = heading + ENG_PI * (dm.cut & 1 ? 0.7f : -0.7f);
            break;
        case SHOT_TRACKSIDE: {
            /* Up the trail ahead of the truck, off to one side on the
               camera's tripod, so it comes toward us and goes by. */
            V3 fwd = heading_fwd(heading);
            V3 side = v3(fwd.z, 0, -fwd.x);
            float s = dm.cut & 2 ? 5.0f : -5.0f;
            dm.post = vadd(vmad(t->pos, fwd, 16.0f), vscale(side, s));
            dm.post.y = fmaxf(terrain_height(dm.post.x, dm.post.z), t->pos.y - 1.0f) + 2.2f;
            break;
        }
    }
}

static void demo_camera(float dt) {
    const Truck *t = &trucks[game.demo_truck];
    V3 focus = vadd(t->pos, v3(0, 0.8f, 0));
    switch(dm.shot) {
        case SHOT_CHASE:
        case SHOT_FAR: camera_follow(t, dt); break;
        case SHOT_HELI: {
            /* A slow circle round the truck from above, drifting down as
               it goes, as a helicopter shot does. */
            dm.heli_a += dt * (dm.cut & 1 ? 0.26f : -0.26f);
            float h = 8.0f - 2.5f * clampf(dm.t / shot_len[SHOT_HELI], 0, 1);
            V3 want = vadd(t->pos, v3(sinf(dm.heli_a) * 21.0f, h, cosf(dm.heli_a) * 21.0f));
            float g = terrain_height(want.x, want.z) + 3.0f;
            if(want.y < g) want.y = g;
            if(!dm.eye_set) {
                dm.eye = want;
                dm.eye_set = true;
            }
            dm.eye = vlerp(dm.eye, want, smooth_k(4.0f, dt));
            /* Framed a little ahead of the truck, so the trail and the
               horizon are in shot rather than the ground under it. */
            V3 aim = vmad(focus, heading_fwd(truck_heading(t)), 4.0f);
            camera_set(dm.eye, vadd(aim, v3(0, 0.6f, 0)), 1.15f);
            break;
        }
        case SHOT_TRACKSIDE: {
            /* The tripod stays put and pans; the lens tightens with distance. */
            float d = vlen(vsub(focus, dm.post));
            camera_set(dm.post, focus, clampf(1.5f - d * 0.03f, 0.75f, 1.3f));
            break;
        }
    }
}

void demo_update(float dt) {
    if(pad.pressed || pad2.pressed || fabsf(pad.stick_x) > 0.6f || fabsf(pad.stick_y) > 0.6f) {
        audio_sfx(RL_SFX_MENU_BACK, 200);
        demo_stop();
        return;
    }
    for(int i = 0; i < game.ntrucks; i++) ai_drive(i, dt);
    for(int i = 0; i < game.ntrucks; i++) truck_update(&trucks[i], dt);
    game.clock += dt;
    fx_update(dt);
    dm.t += dt;
    const Truck *t = &trucks[game.demo_truck];
    bool gone = dm.shot == SHOT_TRACKSIDE && dm.t > 2.0f && vlen(vsub(t->pos, dm.post)) > 30.0f;
    bool dull = dm.t > 2.5f && fabsf(t->speed) < 0.3f;   /* stopped behind another, or stuck */
    if(dm.t >= shot_len[dm.shot] || gone || dull) cut();
    demo_camera(dt);
    bool all_done = true;
    for(int i = 0; i < game.ntrucks; i++)
        if(!ai_done(i)) all_done = false;
    if(game.clock > DEMO_SECONDS || all_done) demo_stop();
}

void demo_draw(void) {
    int map_mode = game.map_mode;
    game.map_mode = 0;   /* the corner map, whatever the player last chose */
    hud_draw_map();
    game.map_mode = map_mode;
    /* Who we are watching, in for the shot's first moments. */
    float a = clampf(dm.t / 0.3f, 0, 1) * clampf((3.5f - dm.t) / 0.5f, 0, 1);
    if(a > 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s  -  %s", menu_truck_name(trucks[game.demo_truck].model),
                 ai_route_name(game.demo_truck));
        hud_panel(20, 408, 330, 440, color_alpha(0xa0182028, a));
        hud_text_c(30, 424, 0.6f, ALIGN_LEFT, color_alpha(0xfff8f0dc, a), buf);
    }
    hud_text_c(624, 462, 0.42f, ALIGN_RIGHT, 0xa0ffffff, "DEMO");
    if(fmodf(game.clock, 1.2f) < 0.8f) hud_text_c(320, 456, 0.8f, ALIGN_CENTER, 0xffffffff, "PRESS START");
}
