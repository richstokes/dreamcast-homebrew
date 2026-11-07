/* Run on SH-4: compare accelerated projection against independent scalar
   calculations, including near-plane depths and the far end of the level. */
#include "../render_math.h"
#include <stdio.h>

KOS_INIT_FLAGS(INIT_DEFAULT);
static unsigned failures,probes,reciprocals,trig_pairs;
static float max_pixels,max_relative,max_trig;

static void check(bool good,const char *kind,float error) {
    if(!good) {
        if(failures<8)printf("[math] FAIL %s error=%.9g\n",kind,(double)error);
        failures++;
    }
}

int main(int argc,char **argv) {
    (void)argc;(void)argv;
    const Vec3 origins[]={{0,0,0},{7000,1000,-3000},{-2000,0,2800}};
    const float yaws[]={0,.37f,1.57f,2.9f,-2.1f};
    const float pitches[]={-.2f,.2f,1.12f};
    const float depths[]={.81f,1.2f,17,250,4200,18000};
    for(unsigned o=0;o<3;o++)for(unsigned y=0;y<5;y++)for(unsigned p=0;p<3;p++) {
        Camera c={.position=origins[o]};
        float sy=sinf(yaws[y]),cy=cosf(yaws[y]);
        float sp=sinf(pitches[p]),cp=cosf(pitches[p]);
        c.right=v3(cy,0,-sy);
        c.forward=v3(sy*cp,-sp,cy*cp);
        c.up=cross(c.forward,c.right);
        render_load_camera(&c);
        for(unsigned d=0;d<6;d++)for(int x=-1;x<=1;x++)for(int y=-1;y<=1;y++) {
            float z=depths[d];
            Vec3 world=add(c.position,add(mul(c.forward,z),
                         add(mul(c.right,z*x*.60f),mul(c.up,z*y*.42f))));
            Vec3 relative=sub(world,c.position);
            Vec3 expected=v3(dot(relative,c.right),dot(relative,c.up),dot(relative,c.forward));
            Vec3 actual=render_transform(world,&c);
            float scale=fmaxf(1,length(relative));
            float error=length(sub(actual,expected))/scale;
            max_relative=fmaxf(max_relative,error);probes++;
            check(isfinite(error)&&error<.00002f,"camera transform",error);
            if(actual.z>=.8f&&expected.z>=.8f) {
                float inv=render_depth_reciprocal(actual.z);
                float pixels=510*fmaxf(fabsf(actual.x*inv-expected.x/expected.z),
                                      fabsf(actual.y*inv-expected.y/expected.z));
                max_pixels=fmaxf(max_pixels,pixels);
                check(isfinite(pixels)&&pixels<.1f,"projection pixels",pixels);
            }
        }
    }
    for(unsigned i=0;i<=10000;i++) {
        float z=.8f+i*(40000.f-.8f)/10000;
        float ref=1/z,fast=render_depth_reciprocal(z);
        float error=fabsf(fast-ref)/ref;
        check(isfinite(fast)&&fast>0&&error<.000002f,"positive reciprocal",error);
        reciprocals++;
    }
    for(int i=-720;i<=720;i++) {
        float angle=i*.01f,s,c;render_sincos(angle,&s,&c);
        float error=fmaxf(fabsf(s-sinf(angle)),fabsf(c-cosf(angle)));
        max_trig=fmaxf(max_trig,error);trig_pairs++;
        check(isfinite(error)&&error<.0002f,"sine/cosine",error);
    }
    printf("[math] %s probes=%u reciprocals=%u trig=%u max_relative=%.9g max_pixels=%.9g max_trig=%.9g failures=%u\n",
           failures?"FAIL":"PASS",probes,reciprocals,trig_pairs,(double)max_relative,
           (double)max_pixels,(double)max_trig,failures);
    printf("[shutdown] Clean exit\n");
    return failures?1:0;
}
