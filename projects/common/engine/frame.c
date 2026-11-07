/* Engine: frame pacing (frame.h). */
#include "frame.h"
#include "vecmath.h"
#include <kos.h>

EngFrame eng_frame;
static uint64_t last, fps_t0;
static int fps_frames;
static float owed;   /* clock time not yet stepped: timer jitter, normally */

void frame_init(void) {
    last = fps_t0 = timer_us_gettime64();
    fps_frames = 0;
    owed = 0;
    eng_frame.vbl_dt = ENG_DT;
}

void frame_begin(void) {
    pvr_wait_ready();
    uint64_t now = timer_us_gettime64();
    float dt_real = (float)(now - last) * 1e-6f;
    last = now;
    if(dt_real > 0.1f) dt_real = 0.1f;
    owed += dt_real;
    float vbl_dt = eng_frame.vbl_dt;
    int steps = 1;
    if(dt_real < 1.5f * vbl_dt) {
        vbl_dt = clampf(vbl_dt + (dt_real - vbl_dt) * 0.01f, 0.8f * ENG_DT, 1.25f * ENG_DT);
    }
    else {
        steps = (int)(dt_real / vbl_dt + 0.5f);
        if(steps > 4) {   /* a stall: forget the time rather than replay it */
            steps = 4;
            owed = steps * vbl_dt;
        }
    }
    float dt = vbl_dt + clampf((owed / steps - vbl_dt) * 0.1f, -0.01f * vbl_dt, 0.01f * vbl_dt);
    owed = clampf(owed - steps * dt, -vbl_dt, vbl_dt);
    eng_frame.t0 = now;
    eng_frame.dt_real = dt_real;
    eng_frame.dt = dt;
    eng_frame.steps = steps;
    eng_frame.vbl_dt = vbl_dt;
}

bool frame_end(void) {
    uint64_t end = timer_us_gettime64();
    eng_frame.cpu_ms = (float)(end - eng_frame.t0) * 0.001f;
    fps_frames++;
    if(end - fps_t0 < 1000000) return false;
    eng_frame.fps = fps_frames * 1e6f / (float)(end - fps_t0);
    fps_frames = 0;
    fps_t0 = end;
    return true;
}
