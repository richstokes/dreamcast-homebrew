#include "../dolphins.h"

/* Run against the real stage collision on SH-4. In particular, the lagoon is
   539 units above the ocean and its banks must not become breach locations. */
static unsigned dolphin_checks(void){
    unsigned failures=0,seen=0,quiet=0,paired=0,splashes=0,probes=0;
    int saved_act=game.act;
    for(int act=0;act<2;act++){
        World world;
        const uint8_t *begin=act?coast1_start:coast0_start,*end=act?coast1_end:coast0_end;
        if(!world_load(&world,begin,(size_t)(end-begin)))return 1;
        game.act=act;
        for(unsigned tick=0;tick<480;tick++){
            DolphinFrame frame;dolphins_sample(act,tick*.25f,&frame);
            if(frame.count>DOLPHIN_CAPACITY||frame.splash_count>DOLPHIN_CAPACITY*2){failures++;break;}
            unsigned members[DOLPHIN_PODS]={0};
            for(unsigned i=0;i<frame.count;i++){
                const Dolphin *d=&frame.dolphins[i];
                if(d->pod>=DOLPHIN_PODS){failures++;continue;}
                const DolphinPod *pod=&dolphin_pods[d->pod];
                seen|=1u<<d->pod;members[d->pod]++;
                if(pod->act!=act||!isfinite(d->position.y)||
                   fabsf(length(d->forward)-1)>.001f||fabsf(dot(d->up,d->forward))>.001f||
                   fabsf(world_water_level(pod->center)-pod->center.y)>.01f)failures++;
                /* Check the entire route's width, including submerged entry
                   and exit. Sparse samples avoid turning QA into a long soak. */
                if(tick%4==0)for(int offset=-1;offset<=1;offset++){
                    Vec3 p=add(d->position,mul(d->right,offset*8));p.y=2000;
                    float ground=world_floor(&world,p,0);probes++;
                    if(ground>pod->center.y-8){
                        printf("[test] FAIL dolphin pod %u overlaps land at %.1f %.1f (floor %.1f water %.1f)\n",
                            d->pod,(double)p.x,(double)p.z,(double)ground,(double)pod->center.y);failures++;
                    }
                }
            }
            for(unsigned i=0;i<DOLPHIN_PODS;i++)if(dolphin_pods[i].act==act){
                if(!members[i])quiet++;
                if(members[i]==2)paired++;
            }
            for(unsigned i=0;i<frame.splash_count;i++){
                DolphinSplash *s=&frame.splashes[i];splashes++;
                if(s->age<0||s->age>=DOLPHIN_SPLASH_LIFE||
                   fabsf(world_water_level(s->position)-s->position.y)>.01f)failures++;
            }
        }
    }
    game.act=saved_act;
    if(seen!=(1u<<DOLPHIN_PODS)-1||quiet<3000||!paired||!splashes)failures++;
    printf("[test] dolphin paths / timing / pairs / splashes: %s (%u pods, %u clearance probes, %u failures)\n",
        failures?"FAIL":"PASS",DOLPHIN_PODS,probes,failures);
    return failures;
}
