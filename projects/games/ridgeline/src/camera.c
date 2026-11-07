/* Ridgeline cameras: a chase camera that swings round behind the truck and
   keeps clear of the ground, a far chase, a bumper view, and the title's
   slow flyover. */
#include "camera.h"
#include "truck.h"
#include "terrain.h"

Camera cam;
static float cam_yaw, cam_pitch_k;
static V3 cam_pos, cam_look;
static bool cam_init;

static void look_at(Camera *c, V3 pos, V3 target, float fov) {
    cam_look_at(c, pos, target);
    c->fov = fov;
}

void camera_reset(void) {
    cam_init = false;
}

void camera_follow(const Truck *t, float dt) {
    float heading = truck_heading(t);
    float speed = t->speed;
    if(!cam_init) {
        cam_yaw = heading;
        cam_pos = vsub(t->pos, vscale(heading_fwd(heading), 8.0f));
        cam_look = t->pos;
        cam_init = true;
    }
    /* Swing round behind the truck's heading, slower when crawling so the
       view doesn't wag with every correction, and stay put backing up. */
    float rate = 1.6f + fminf(fabsf(speed), 15.0f) * 0.12f;
    if(speed > -1.0f) cam_yaw += wrap_angle(heading - cam_yaw) * smooth_k(rate, dt);
    int mode = game.cam_mode;
    float dist = mode == 1 ? 13.5f : 8.2f, height = mode == 1 ? 4.6f : 2.9f;
    V3 back = heading_fwd(cam_yaw);
    V3 target = vadd(t->pos, v3(0, 1.1f, 0));
    if(mode == 2) {
        /* On the bumper, looking where the truck points. */
        V3 eye = truck_to_world(t, v3(0, t->spec.body_hi[1] - 0.1f, t->spec.body_hi[2] * 0.25f));
        V3 ahead = vmad(eye, t->ax[2], 10.0f);
        ahead = vmad(ahead, t->ax[1], -1.2f);
        look_at(&cam, eye, ahead, 1.35f);
        cam_pos = eye;
        cam_look = ahead;
        return;
    }
    V3 want = vadd(vsub(target, vscale(back, dist)), v3(0, height, 0));
    /* Up the slope behind, rise over the ground. */
    float g = terrain_height(want.x, want.z);
    if(want.y < g + 1.4f) want.y = g + 1.4f;
    cam_pos = vlerp(cam_pos, want, smooth_k(6.0f, dt));
    float g2 = terrain_height(cam_pos.x, cam_pos.z);
    if(cam_pos.y < g2 + 1.2f) cam_pos.y = g2 + 1.2f;
    cam_look = vlerp(cam_look, vadd(target, vscale(back, 3.0f)), smooth_k(10.0f, dt));
    look_at(&cam, cam_pos, cam_look, 1.25f);
    (void)cam_pitch_k;
}

void camera_flyover(float t) {
    /* A slow orbit round the mountain at the height of its shoulders. */
    float a = t * 0.045f + 3.6f;
    V3 p = orbit_point(v3(LEVEL_SUMMIT_X, 0, LEVEL_SUMMIT_Z - 120.0f), a, 300.0f, 0);
    p.y = fmaxf(terrain_height(p.x, p.z) + 30.0f, 70.0f + 25.0f * sinf(t * 0.07f));
    V3 look = v3(LEVEL_SUMMIT_X, LEVEL_SUMMIT_Y * 0.55f, LEVEL_SUMMIT_Z - 60.0f);
    look_at(&cam, p, look, 1.2f);
}

void camera_set(V3 pos, V3 target, float fov) {
    look_at(&cam, pos, target, fov);
    cam_pos = pos;
    cam_look = target;
}

/* ---- Photo mode ---- */

static float ph_yaw, ph_pitch, ph_dist;

static V3 photo_target(const Truck *t) {
    return vadd(t->pos, v3(0, 0.4f, 0));
}

void camera_photo_begin(const Truck *t) {
    V3 d = vsub(cam.pos, photo_target(t));
    ph_dist = clampf(vlen(d), 4.0f, 30.0f);
    ph_yaw = atan2f(d.x, d.z);
    ph_pitch = asinf(clampf(d.y / fmaxf(vlen(d), 0.01f), -0.2f, 1.3f));
}

void camera_photo(const Truck *t, float dt) {
    /* Stick: round and over the truck; R closer, L further. */
    ph_yaw += pad.stick_x * 1.8f * dt;
    ph_pitch = clampf(ph_pitch - pad.stick_y * 1.2f * dt, -0.15f, 1.35f);
    ph_dist = clampf(ph_dist * (1.0f + (pad.ltrig - pad.rtrig) * 1.2f * dt), 3.5f, 35.0f);
    V3 c = photo_target(t);
    float cp = cosf(ph_pitch);
    V3 p = vadd(c, vscale(v3(sinf(ph_yaw) * cp, sinf(ph_pitch), cosf(ph_yaw) * cp), ph_dist));
    float g = terrain_height(p.x, p.z) + 0.6f;
    if(p.y < g) p.y = g;
    look_at(&cam, p, c, 1.15f);
    cam_pos = p;
    cam_look = c;
}
