/* A short visual tour: held apex poses for inspecting the original mesh, then
   real-time breaches and landings at every pod. Only the diagnostic ELF uses
   these cameras/times; the retail game keeps its normal exploration controls. */
#include "dolphin_checks.h"
static int run_dolphin_preview(void){
    unsigned failures=dolphin_checks();
    game.postcard=true;game.arrival=game.fade=0;
    for(unsigned i=0;i<DOLPHIN_PODS;i++){
        const DolphinPod *pod=&dolphin_pods[i];
        travel_to(pod->act?4:0);game.arrival=game.fade=0;
        Vec3 side=v3(pod->direction.z,0,-pod->direction.x);
        game.camera.position=add(pod->center,add(mul(side,90),v3(0,21,0)));
        game.camera.target=add(pod->center,v3(0,12,0));
        game.camera.forward=unit(sub(game.camera.target,game.camera.position));
        game.camera.right=unit(cross(v3(0,1,0),game.camera.forward));
        game.camera.up=cross(game.camera.forward,game.camera.right);
        /* Move the explorer out of the wildlife inspection camera. */
        game.player.position=sub(pod->center,v3(0,100,0));
        printf("[dolphins] pod %u, act %d: apex pose then two live cycles\n",i,pod->act);
        uint64_t start=timer_ms_gettime64(),last=start;unsigned frames=0;
        while(timer_ms_gettime64()-start<11000){
            uint64_t now=timer_ms_gettime64();float elapsed=(now-start)*.001f;
            float dt=(now-last)*.001f;last=now;
            game.time=pod->first+(elapsed<3?1.55f:fmodf(elapsed-3,4));
            audio_update(dt);renderer_draw();frames++;thd_pass();
        }
        printf("[dolphins] pod %u: %.1f fps\n",i,(double)frames/11);
    }
    printf("[dolphins] %s: eight-pod visual tour\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
