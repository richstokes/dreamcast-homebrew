#include "dolphins.h"

const DolphinPod dolphin_pods[DOLPHIN_PODS]={
    {0,{290,-.2f,115},   {.6f,0,-.8f},  3.0f,22.7f}, /* Resort beach */
    {0,{2860,-.2f,570},  {1,0,0},       9.2f,28.3f}, /* Ocean pier */
    {0,{520,-.2f,900},   {0,0,1},      14.0f,31.1f}, /* Crescent beach */
    {0,{5480,-.2f,780},  {1,0,0},       6.4f,26.9f}, /* Far sandbar */
    {1,{-20,539,-1730},  {.6f,0,-.8f},  4.8f,24.1f}, /* Blue lagoon */
    {1,{2500,-.2f,-1800},{0,0,1},      11.0f,32.3f}, /* Clifftop bay */
    {1,{4900,-.2f,-2460},{1,0,0},       7.1f,29.9f}, /* Outer cove */
    {1,{6750,-.2f,-1940},{.8f,0,.6f},  2.1f,21.3f}, /* Eastern shore */
};

static void splash(DolphinFrame *frame,Vec3 start,Vec3 direction,
                   float distance,float crossing,float age,float strength){
    if(age<0||age>=DOLPHIN_SPLASH_LIFE)return;
    frame->splashes[frame->splash_count++]=(DolphinSplash){
        add(start,mul(direction,distance*crossing)),age,strength};
}
void dolphins_sample(int act,float time,DolphinFrame *frame){
    frame->count=frame->splash_count=0;
    if(!isfinite(time)||time<0)return;
    for(unsigned i=0;i<DOLPHIN_PODS;i++){
        const DolphinPod *pod=&dolphin_pods[i];
        if(pod->act!=act||time<pod->first)continue;
        float elapsed=time-pod->first;
        unsigned cycle=(unsigned)floorf(elapsed/pod->period);
        /* Each pod has an independent clock. Change direction, height, speed,
           and the delay within its next cycle without using global RNG state. */
        unsigned seed=cycle*1664525u+i*1013904223u;
        float delay=cycle?((seed>>8)&7)*.37f:0;
        float clock=elapsed-cycle*pod->period-delay;
        float duration=2.55f+((seed>>4)&3)*.18f;
        float height=18+((seed>>12)&3)*2.1f,distance=90+((seed>>16)&3)*6;
        Vec3 direction=mul(pod->direction,cycle&1?-1:1);
        Vec3 right=v3(direction.z,0,-direction.x);
        for(unsigned member=0;member<2;member++){
            float age=clock-member*(.57f+(i%3)*.09f);
            if(age<0||age>duration+DOLPHIN_SPLASH_LIFE)continue;
            float scale=member?.78f:.92f,depth=12*scale;
            Vec3 start=add(pod->center,add(mul(direction,-distance*.5f),mul(right,member?11:-11)));
            if(age<=duration){
                float u=age/duration;
                Vec3 position=add(start,mul(direction,distance*u));
                position.y+=-depth+4*(height+depth)*u*(1-u);
                Vec3 forward=unit(add(mul(direction,distance),v3(0,4*(height+depth)*(1-2*u),0)));
                frame->dolphins[frame->count++]=(Dolphin){position,right,cross(forward,right),forward,
                    scale,fsin(age*9+i)*.65f,i};
            }
            /* Splash when the body crosses the surface, not at the submerged
               endpoints. Landing is stronger than the take-off wake. */
            float crossing=.5f*(1-sqrtf(height/(height+depth)));
            splash(frame,start,direction,distance,crossing,age-duration*crossing,.65f);
            splash(frame,start,direction,distance,1-crossing,age-duration*(1-crossing),1);
        }
    }
}
