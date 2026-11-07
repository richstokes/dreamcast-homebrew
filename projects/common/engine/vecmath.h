/* Engine: vectors, scalar helpers, packed colours and the camera.

   World convention: +X east, +Y up, +Z north, metres. A heading of 0 faces
   +Z and a positive heading change turns right (forward = (sin h, 0, cos h)).
   Everything here is inline, so a game pays nothing for using it. */
#ifndef ENG_VECMATH_H
#define ENG_VECMATH_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define ENG_PI  3.14159265f
#define ENG_TAU 6.28318531f

typedef struct { float x, y, z; } V3;

static inline V3 v3(float x, float y, float z) { return (V3){x, y, z}; }
static inline V3 vadd(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V3 vsub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V3 vscale(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static inline V3 vmad(V3 a, V3 b, float s) { return v3(a.x + b.x * s, a.y + b.y * s, a.z + b.z * s); }
static inline float vdot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V3 vcross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float vlen(V3 a) { return sqrtf(vdot(a, a)); }
static inline V3 vnorm(V3 a) {
    float l = vlen(a);
    return l > 1e-6f ? vscale(a, 1.0f / l) : v3(0, 0, 1);
}
static inline V3 vlerp(V3 a, V3 b, float t) { return vmad(a, vsub(b, a), t); }
static inline float clampf(float a, float lo, float hi) { return a < lo ? lo : (a > hi ? hi : a); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float signf(float a) { return a < 0 ? -1.0f : 1.0f; }
static inline float wrap_angle(float a) {
    while(a > ENG_PI) a -= ENG_TAU;
    while(a < -ENG_PI) a += ENG_TAU;
    return a;
}
/* Move a toward b by at most step. */
static inline float approach(float a, float b, float step) {
    if(a < b) return a + step > b ? b : a + step;
    return a - step < b ? b : a - step;
}
/* Frame-rate independent exponential smoothing factor. */
static inline float smooth_k(float rate, float dt) {
    float k = rate * dt;
    return k > 1.0f ? 1.0f : k;
}
static inline V3 heading_fwd(float h) { return v3(sinf(h), 0, cosf(h)); }
static inline V3 heading_right(float h) { return v3(cosf(h), 0, -sinf(h)); }

static inline uint32_t argb(int a, int r, int g, int b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static inline uint32_t color_scale(uint32_t c, float s) {
    int r = (int)(((c >> 16) & 255) * s), g = (int)(((c >> 8) & 255) * s), b = (int)((c & 255) * s);
    if(r > 255) r = 255;
    if(g > 255) g = 255;
    if(b > 255) b = 255;
    return (c & 0xff000000u) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static inline uint32_t color_lerp(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for(int s = 0; s < 32; s += 8) {
        float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
        r |= (uint32_t)(ca + (cb - ca) * t) << s;
    }
    return r;
}
static inline uint32_t color_alpha(uint32_t c, float a) {
    int ai = (int)(clampf(a, 0, 1) * 255.0f);
    return (c & 0x00ffffffu) | ((uint32_t)ai << 24);
}

typedef struct {
    V3 pos, fwd, right, up;
    float fov;           /* horizontal field of view, radians */
    float shake;
} Camera;

/* Point the camera at look from pos (world up +Y); fov and shake are left
   as they are. */
static inline void cam_look_at(Camera *c, V3 pos, V3 look) {
    c->pos = pos;
    c->fwd = vnorm(vsub(look, pos));
    c->right = vnorm(vcross(v3(0, 1, 0), c->fwd));
    c->up = vcross(c->fwd, c->right);
}
/* Nudge the view: fwd leans by k along axis (the camera's right or up),
   and right and up follow. A tilt down or a shift that frames a subject
   off centre. */
static inline void cam_lean(Camera *c, V3 axis, float k) {
    c->fwd = vnorm(vmad(c->fwd, axis, k));
    c->right = vnorm(vcross(v3(0, 1, 0), c->fwd));
    c->up = vcross(c->fwd, c->right);
}
/* A point circling target: angle a (radians, 0 at +Z), radius r, h above
   it. Orbit cameras and flyovers. */
static inline V3 orbit_point(V3 target, float a, float r, float h) {
    return vadd(target, v3(sinf(a) * r, h, cosf(a) * r));
}

#endif
