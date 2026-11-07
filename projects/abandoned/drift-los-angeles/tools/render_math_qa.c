/* Run on SH-4: validate the renderer's fast math against its scalar path.
   This deliberately does not link or initialize the game renderer. */
#include <kos.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "../render_math.h"

#ifdef DRIFT_LA_SCALAR_RENDER_MATH
#error "Render math QA must exercise the accelerated math implementation"
#endif

KOS_INIT_FLAGS(INIT_DEFAULT);

#define QA_NEAR .35f
#define QA_FAR 760.0f
#define QA_FOCAL 512.0f
#define QA_RELATIVE_LIMIT .00002f
#define QA_PIXEL_LIMIT .1f
#define QA_TRIG_LIMIT .0002f
#define QA_ARRAY_COUNT(array) (sizeof(array)/sizeof((array)[0]))

typedef struct { float x,y,z; } qa_vec3_t;
typedef struct { float sy,cy,sp,cp,sr,cr; } qa_rotation_t;
typedef struct {
    unsigned rotations,projections,reciprocals,normalizations,trig_pairs,failures;
    float rotation_relative,projection_pixels,reciprocal_relative;
    float normalization_relative,trig_absolute;
} qa_results_t;

static qa_results_t results;

/* Keep the original sequence as the independent reference. */
static qa_vec3_t scalar_rotate(qa_vec3_t v,qa_rotation_t r) {
    const float yaw_x=v.x*r.cy-v.z*r.sy;
    const float yaw_z=v.x*r.sy+v.z*r.cy;
    const float pitch_y=v.y*r.cp-yaw_z*r.sp;
    const float pitch_z=v.y*r.sp+yaw_z*r.cp;
    return (qa_vec3_t){yaw_x*r.cr-pitch_y*r.sr,
                      yaw_x*r.sr+pitch_y*r.cr,pitch_z};
}

/* Only used to generate probes near the visible frustum. Both tested paths
   receive the same resulting relative coordinates. */
static qa_vec3_t camera_probe_to_relative(qa_vec3_t v,qa_rotation_t r) {
    const float yaw_x=v.x*r.cr+v.y*r.sr;
    const float pitch_y=-v.x*r.sr+v.y*r.cr;
    const float relative_y=pitch_y*r.cp+v.z*r.sp;
    const float yaw_z=-pitch_y*r.sp+v.z*r.cp;
    return (qa_vec3_t){yaw_x*r.cy+yaw_z*r.sy,relative_y,
                      -yaw_x*r.sy+yaw_z*r.cy};
}

static void fail(const char *category,float error,float limit) {
    if(results.failures<8)
        printf("Render math QA mismatch: %s error=%.9g limit=%.9g.\n",
               category,(double)error,(double)limit);
    ++results.failures;
}

static void check_rotation(qa_vec3_t relative,qa_rotation_t rotation,
                           bool check_projection) {
    const qa_vec3_t scalar=scalar_rotate(relative,rotation);
    const shz_vec3_t fast=dla_rotate_camera(relative.x,relative.y,relative.z);
    const float scale=fmaxf(1.0f,fabsf(relative.x)+fabsf(relative.y)+fabsf(relative.z));
    const float error=fmaxf(fmaxf(fabsf(fast.x-scalar.x),fabsf(fast.y-scalar.y)),
                            fabsf(fast.z-scalar.z))/scale;
    ++results.rotations;
    results.rotation_relative=fmaxf(results.rotation_relative,error);
    if(!isfinite(fast.x)||!isfinite(fast.y)||!isfinite(fast.z)||
       !isfinite(error)||error>QA_RELATIVE_LIMIT)
        fail("rotation",error,QA_RELATIVE_LIMIT);

    /* Do not compare discontinuous clipping decisions right on a plane.
       Neither path performs a reciprocal before the positive-depth guard. */
    if(check_projection&&scalar.z>QA_NEAR+.0001f&&scalar.z<QA_FAR-.0001f) {
        float scalar_inv,fast_inv,pixels;
        ++results.projections;
        if(!isfinite(fast.z)||fast.z<QA_NEAR||fast.z>QA_FAR) {
            fail("visible depth",fast.z,scalar.z);
            return;
        }
        scalar_inv=1.0f/scalar.z;
        fast_inv=dla_depth_reciprocal(fast.z);
        pixels=QA_FOCAL*fmaxf(fabsf(fast.x*fast_inv-scalar.x*scalar_inv),
                             fabsf(fast.y*fast_inv-scalar.y*scalar_inv));
        results.projection_pixels=fmaxf(results.projection_pixels,pixels);
        if(!isfinite(pixels)||pixels>QA_PIXEL_LIMIT)
            fail("projection pixels",pixels,QA_PIXEL_LIMIT);
    }
}

static void check_camera_poses(void) {
    static const float yaws[]={0.0f,.37f,-.83f,1.5707963f,2.91f,-3.10f};
    static const float pitches[]={0.0f,-.13f,.24f};
    static const float rolls[]={0.0f,-.08f,.075f};
    static const qa_vec3_t asymmetric[]={
        {0,0,0},{1,0,0},{0,1,0},{0,0,1},
        {-1,0,0},{0,-1,0},{0,0,-1},{.17f,-2.3f,7.9f},
        {-513.25f,117.0f,729.125f},{.0001f,-.0003f,.0007f}
    };
    static const float depths[]={.351f,.7f,3.0f,27.0f,180.0f,759.0f};
    static const qa_vec3_t origins[]={
        {0,0,0},{100000.0f,51.0f,-100000.0f},{-32768.0f,-19.0f,65536.0f}
    };
    size_t yi,pi,ri,vi,di,oi;
    int sx,sy;
    for(yi=0;yi<QA_ARRAY_COUNT(yaws);++yi)
        for(pi=0;pi<QA_ARRAY_COUNT(pitches);++pi)
            for(ri=0;ri<QA_ARRAY_COUNT(rolls);++ri) {
                qa_rotation_t r;
                fsincosr(yaws[yi],&r.sy,&r.cy);
                fsincosr(pitches[pi],&r.sp,&r.cp);
                fsincosr(rolls[ri],&r.sr,&r.cr);
                dla_load_camera_rotation(r.sy,r.cy,r.sp,r.cp,r.sr,r.cr);
                for(vi=0;vi<QA_ARRAY_COUNT(asymmetric);++vi)
                    check_rotation(asymmetric[vi],r,false);
                for(di=0;di<QA_ARRAY_COUNT(depths);++di)
                    for(sx=-1;sx<=1;++sx)
                        for(sy=-1;sy<=1;++sy) {
                            const float depth=depths[di];
                            const qa_vec3_t view={(float)sx*depth*.56f,
                                                 (float)sy*depth*.40f,depth};
                            const qa_vec3_t local=camera_probe_to_relative(view,r);
                            for(oi=0;oi<QA_ARRAY_COUNT(origins);++oi) {
                                const qa_vec3_t origin=origins[oi];
                                const qa_vec3_t world={origin.x+local.x,
                                                      origin.y+local.y,
                                                      origin.z+local.z};
                                /* Translation is the caller's responsibility,
                                   matching world_to_camera in the game. */
                                const qa_vec3_t relative={world.x-origin.x,
                                                         world.y-origin.y,
                                                         world.z-origin.z};
                                check_rotation(relative,r,true);
                            }
                        }
            }
}

static void check_reciprocals(void) {
    enum { SAMPLES=512 };
    const float depth_step=expf(logf(QA_FAR/QA_NEAR)/(float)SAMPLES);
    const float length_step=expf(logf(1.0e10f)/(float)SAMPLES);
    float depth=QA_NEAR,length_squared=.0001f;
    unsigned i;
    for(i=0;i<=SAMPLES;++i) {
        float reference,fast,error;
        /* Set endpoints explicitly so geometric stepping cannot miss them. */
        if(i==SAMPLES) { depth=QA_FAR; length_squared=1.0e6f; }
        reference=1.0f/depth;
        fast=dla_depth_reciprocal(depth);
        error=fabsf(fast-reference)/reference;
        ++results.reciprocals;
        results.reciprocal_relative=fmaxf(results.reciprocal_relative,error);
        if(!isfinite(fast)||error>QA_RELATIVE_LIMIT)
            fail("depth reciprocal",error,QA_RELATIVE_LIMIT);
        reference=1.0f/sqrtf(length_squared);
        fast=dla_reflection_inv_length(length_squared);
        error=fabsf(fast-reference)/reference;
        ++results.normalizations;
        results.normalization_relative=fmaxf(results.normalization_relative,error);
        if(!isfinite(fast)||error>QA_RELATIVE_LIMIT)
            fail("reflection reciprocal sqrt",error,QA_RELATIVE_LIMIT);
        depth*=depth_step;
        length_squared*=length_step;
    }
    {
        /* Volatile inputs ensure these exercise the target instructions and
           protected floor rather than compile-time constant evaluation. */
        volatile float below_floor[]={0.0f,1.0e-8f,.00009999f};
        for(i=0;i<QA_ARRAY_COUNT(below_floor);++i) {
            const float fast=dla_reflection_inv_length(below_floor[i]);
            const float error=fabsf(fast-100.0f)/100.0f;
            ++results.normalizations;
            results.normalization_relative=fmaxf(results.normalization_relative,error);
            if(!isfinite(fast)||error>QA_RELATIVE_LIMIT)
                fail("reflection zero/floor protection",error,QA_RELATIVE_LIMIT);
        }
    }
}

static void check_context_retention(void) {
    qa_rotation_t r;
    fsincosr(.73f,&r.sy,&r.cy);
    fsincosr(-.13f,&r.sp,&r.cp);
    fsincosr(.08f,&r.sr,&r.cr);
    dla_load_camera_rotation(r.sy,r.cy,r.sp,r.cp,r.sr,r.cr);
    /* Force a scheduler/interrupt round trip with the camera in XMTRX. */
    thd_sleep(2);
    check_rotation((qa_vec3_t){1,0,0},r,false);
    check_rotation((qa_vec3_t){0,1,0},r,false);
    check_rotation((qa_vec3_t){0,0,1},r,false);
    check_rotation((qa_vec3_t){1.9f,-3.7f,11.3f},r,true);
}

static void check_trig_pair(float angle) {
    const shz_sincos_t fast=shz_sincosf(angle);
    const float error=fmaxf(fabsf(fast.sin-fsin(angle)),
                            fabsf(fast.cos-fcos(angle)));
    ++results.trig_pairs;
    results.trig_absolute=fmaxf(results.trig_absolute,error);
    if(!isfinite(fast.sin)||!isfinite(fast.cos)||!isfinite(error)||
       error>QA_TRIG_LIMIT)
        fail("sin/cos pair",error,QA_TRIG_LIMIT);
}

static void check_trig_pairs(void) {
    enum { SAMPLES=1021 };
    const float pi=3.14159265358979323846f;
    volatile float boundaries[]={0.0f,-.000001f,.000001f,
        -1.5707963268f,1.5707963268f,-3.1415926536f,3.1415926536f,
        -6.2831853072f,6.2831853072f};
    unsigned i;
    for(i=0;i<=SAMPLES;++i) {
        const float angle=-8.0f*pi+16.0f*pi*(float)i/(float)SAMPLES;
        check_trig_pair(angle);
        /* Exercise either side of FSCA phase bins as well as whole turns. */
        check_trig_pair(angle+.000048f);
    }
    for(i=0;i<QA_ARRAY_COUNT(boundaries);++i)
        check_trig_pair(boundaries[i]);
}

int main(int argc,char **argv) {
    const uint64_t start=timer_us_gettime64();
    (void)argc;
    (void)argv;
    check_camera_poses();
    check_reciprocals();
    check_context_retention();
    check_trig_pairs();
    printf("Render math QA: rotations=%u projections=%u reciprocals=%u "
           "normalizations=%u trig_pairs=%u.\n",
           results.rotations,results.projections,results.reciprocals,
           results.normalizations,results.trig_pairs);
    printf("Render math QA maxima: rotation_relative=%.9g projection_pixels=%.9g "
           "reciprocal_relative=%.9g normalization_relative=%.9g "
           "trig_absolute=%.9g elapsed_us=%llu.\n",
           (double)results.rotation_relative,(double)results.projection_pixels,
           (double)results.reciprocal_relative,(double)results.normalization_relative,
           (double)results.trig_absolute,
           (unsigned long long)(timer_us_gettime64()-start));
    printf("Render math QA: %s (%u failures).\n",
           results.failures?"FAIL":"PASS",results.failures);
    return results.failures?1:0;
}
