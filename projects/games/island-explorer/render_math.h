#ifndef ISLAND_RENDER_MATH_H
#define ISLAND_RENDER_MATH_H

#include "engine.h"
#include <sh4zam/shz_matrix.h>
#include <dc/matrix.h>

/* The render thread owns XMTRX from this load through scene submission.
   Do not call matrix-mutating helpers inside that interval. Physics retains
   its existing scalar arithmetic and signed division. */
static inline void render_load_camera(const Camera *c) {
    Vec3 translation={0};
#ifdef IE_REFERENCE_RENDER
    translation=v3(-dot(c->position,c->right),-dot(c->position,c->up),
                   -dot(c->position,c->forward));
#endif
    const shz_mat4x4_t matrix = {.elem = {
        c->right.x, c->up.x, c->forward.x, 0,
        c->right.y, c->up.y, c->forward.y, 0,
        c->right.z, c->up.z, c->forward.z, 0,
        translation.x, translation.y, translation.z, 1
    }};
    shz_xmtrx_load_4x4(&matrix);
}

static inline Vec3 render_transform(Vec3 p,const Camera *c) {
#ifdef IE_REFERENCE_RENDER
    (void)c;
    float x=p.x,y=p.y,z=p.z;
    mat_trans_single3_nodiv(x,y,z);
    return v3(x,y,z);
#else
    /* Subtract before rotation: the distant shore is 7,000 units from the
       origin, but a near-clipped vertex may be less than one unit away. */
    p=sub(p,c->position);
    shz_vec3_t out=shz_xmtrx_transform_vec3(shz_vec3_init(p.x,p.y,p.z));
    return v3(out.x,out.y,out.z);
#endif
}

/* FSRRA(x*x) is 1/abs(x), not a signed reciprocal. The two projection
   callers must first clip to z >= 0.8; their finite depths cannot overflow
   x*x. Keep the signed near-plane intersection division separate. */
static inline float render_depth_reciprocal(float positive_depth) {
#ifdef IE_REFERENCE_RENDER
    return 1.0f/positive_depth;
#else
    return shz_invf_fsrra(positive_depth);
#endif
}

static inline void render_sincos(float radians,float *s,float *c) {
#ifdef IE_REFERENCE_RENDER
    *s=fsin(radians);*c=fcos(radians);
#else
    shz_sincos_t pair=shz_sincosf(radians);
    *s=pair.sin;*c=pair.cos;
#endif
}
#endif
