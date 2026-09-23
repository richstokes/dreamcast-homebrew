#ifndef DLA_RENDER_MATH_H
#define DLA_RENDER_MATH_H

#include "third_party/sh4zam/include/sh4zam/shz_matrix.h"

/* The render pass owns XMTRX. Load once after camera/HUD setup; do not use
   another matrix API inside the pass without restoring this rotation.
   Translate relative to the camera before FTRV to preserve precision as the
   streamed city moves farther from the origin. Physics keeps its scalar math. */
static inline void dla_load_camera_rotation(float sy, float cy,
                                            float sp, float cp,
                                            float sr, float cr) {
    const shz_mat4x4_t rotation = {.elem = {
        cr*cy+sr*sp*sy, sr*cy-cr*sp*sy, cp*sy, 0.0f,
        -sr*cp, cr*cp, sp, 0.0f,
        -cr*sy+sr*sp*cy, -sr*sy-cr*sp*cy, cp*cy, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f
    }};
    shz_xmtrx_load_4x4(&rotation);
}

static inline shz_vec3_t dla_rotate_camera(float x, float y, float z) {
    return shz_xmtrx_transform_vec3(shz_vec3_init(x,y,z));
}

/* FSRRA(x*x) returns 1/abs(x). Call only after the positive near/far test;
   the renderer's depth interval also keeps x*x normal and finite. */
static inline float dla_depth_reciprocal(float positive_depth) {
#ifdef DRIFT_LA_SCALAR_RENDER_MATH
    return 1.0f/positive_depth;
#else
    return shz_invf_fsrra(positive_depth);
#endif
}

static inline float dla_reflection_inv_length(float squared_length) {
    const float safe_length=fmaxf(squared_length,.0001f);
#ifdef DRIFT_LA_SCALAR_RENDER_MATH
    return 1.0f/sqrtf(safe_length);
#else
    return shz_inv_sqrtf_fsrra(safe_length);
#endif
}

#endif
