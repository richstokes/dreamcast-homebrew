#include "engine.h"
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
KOS_INIT_FLAGS(INIT_DEFAULT);
Game game;
static bool rendering;
static bool handle_buttons(uint32_t buttons,uint32_t pressed);
#ifdef IE_AUTOTEST
#include "tests/runtime_checks.h"
#endif
const Destination destinations[]={
    {"The resort beach",0,{-9,16,4},1.12f},
    {"Across the ocean pier",0,{2820,15,365},1.5708f},
    {"The far sandbar",0,{5650,12,1100},1.12f},
    {"Above the hidden cove",1,{-986.5f,1145,-2856},0.0f},
    {"The blue lagoon",1,{-150,548,-1850},1.4f},
    {"The eastern shore",1,{7000,12,-2300},-1.2f},
};
const unsigned destination_count=sizeof(destinations)/sizeof(destinations[0]);
void travel_to(unsigned index){
    if(index>=destination_count)return;
    const Destination *d=&destinations[index];
    if(!game.world.meshes||game.act!=d->act){
        game.act=d->act;
        const uint8_t *start=d->act?coast1_start:coast0_start,*end=d->act?coast1_end:coast0_end;
        if(!world_load(&game.world,start,(size_t)(end-start))){printf("[world] invalid scene asset\n");arch_exit();}
        if(rendering)renderer_world_changed();
    }
    game.player=(Player){.position=d->position,.heading=d->yaw};
    float ground=world_floor(&game.world,d->position,0);
    float water=world_water_level(d->position)-4.3f;
    game.player.position.y=fmaxf(ground,water);
    game.player.grounded=true;game.player.swimming=ground<water;
    game.camera.yaw=d->yaw;game.camera.pitch=.20f;game.camera.distance=42;
    game.camera.target=add(game.player.position,v3(0,6.7f,0));
    game.location=d->name;game.arrival=7;game.fade=1;game.travel=false;game.paused=false;
    printf("[travel] %s: act %d, at %.2f %.2f %.2f; %lu meshes\n",d->name,d->act+1,(double)game.player.position.x,(double)game.player.position.y,(double)game.player.position.z,(unsigned long)game.world.mesh_count);
}
static void update_player(float dt,float input_x,float input_y,bool run,bool jump){
    Player *p=&game.player;
    float magnitude=sqrtf(input_x*input_x+input_y*input_y);
    if(magnitude>.13f){
        magnitude=clampf((magnitude-.13f)/.87f,0,1);
        float heading=game.camera.yaw+atan2f(input_x,-input_y);
        float delta=remainderf(heading-p->heading,6.2831853f);
        p->heading+=delta*clampf(dt*14,0,1);
        float speed=(p->swimming?38:(run?122:63))*magnitude;
        float blend=clampf(dt*11,0,1);
        p->velocity.x=mixf(p->velocity.x,fsin(heading)*speed,blend);
        p->velocity.z=mixf(p->velocity.z,fcos(heading)*speed,blend);
    }else{
        float blend=clampf(dt*13,0,1);
        p->velocity.x=mixf(p->velocity.x,0,blend);p->velocity.z=mixf(p->velocity.z,0,blend);
    }
    p->speed=sqrtf(p->velocity.x*p->velocity.x+p->velocity.z*p->velocity.z);
    if(jump&&(p->grounded||p->swimming)){p->velocity.y=72;p->grounded=false;printf("[player] jump\n");}
    if(p->grounded&&p->speed<.05f&&p->velocity.y==0)return;
    float old_y=p->position.y;
    int steps=(int)ceilf(dt/(1.f/90));
    float step_dt=dt/steps;
    for(int step=0;step<steps;step++){
        Vec3 next=add(p->position,mul(p->velocity,step_dt));
        world_slide(&game.world,&next);
        Vec3 floor_probe=next;floor_probe.y=fmaxf(p->position.y,next.y);
        float ground=world_floor(&game.world,floor_probe,p->grounded?3.4f:0.1f);
        float water_height=world_water_level(next)-4.3f;
        bool water=ground<water_height;
        float support=water?water_height:ground;
        if(p->velocity.y<=0&&next.y<=support+(p->grounded?3.4f:0)){
            next.y=support;p->velocity.y=0;p->grounded=true;p->swimming=water;
        }else{
            p->grounded=false;p->swimming=false;p->velocity.y-=185*step_dt;
        }
        p->position=next;
    }
    float old_gait=p->gait;
    p->gait+=dt*p->speed*.17f;
    if(p->grounded&&(int)(old_gait/3.14159265f)!=(int)(p->gait/3.14159265f))audio_footstep();
    if(p->position.y<old_y-1000||!isfinite(p->position.y))travel_to(game.act?3:0);
}
static void update_camera(float dt,float turn,float tilt,bool recenter){
    Camera *c=&game.camera;
    if(recenter)c->yaw=game.player.heading;
    c->yaw+=turn*dt*1.65f;c->pitch=clampf(c->pitch+tilt*dt*.7f,-.2f,1.12f);
    Vec3 target=add(game.player.position,v3(0,6.7f,0));
    c->target=add(c->target,mul(sub(target,c->target),clampf(dt*10,0,1)));
    Vec3 backwards=v3(-fsin(c->yaw)*fcos(c->pitch),fsin(c->pitch),-fcos(c->yaw)*fcos(c->pitch));
    Vec3 desired=add(c->target,mul(backwards,c->distance));
    float fraction=world_raycast(&game.world,c->target,desired);
    /* Rise above a beach bank before pulling the lens into the character. */
    float angle=c->pitch;
    for(int attempt=0;attempt<3&&fraction*c->distance<22;attempt++){
        angle=fminf(1.2f,angle+.24f);
        backwards=v3(-fsin(c->yaw)*fcos(angle),fsin(angle),-fcos(c->yaw)*fcos(angle));
        desired=add(c->target,mul(backwards,c->distance));
        fraction=world_raycast(&game.world,c->target,desired);
    }
    float distance=clampf(c->distance*fraction-1.2f,1.6f,c->distance);
    c->position=add(c->target,mul(backwards,distance));
    c->forward=unit(sub(c->target,c->position));
    c->right=unit(cross(v3(0,1,0),c->forward));c->up=cross(c->forward,c->right);
}
static bool key(kbd_state_t *k,kbd_key_t code){return k&&k->key_states[code].is_down;}
static bool handle_buttons(uint32_t buttons,uint32_t pressed){
    uint32_t quit=CONT_START|CONT_A|CONT_B|CONT_X|CONT_Y;
    if((buttons&quit)==quit)return false;
    if(pressed&CONT_START){game.paused=!game.paused;game.travel=false;}
    if(pressed&CONT_Y){game.travel=!game.travel;game.paused=false;}
    if(game.travel){
        if(pressed&CONT_DPAD_UP)game.travel_selection=(game.travel_selection+destination_count-1)%destination_count;
        if(pressed&CONT_DPAD_DOWN)game.travel_selection=(game.travel_selection+1)%destination_count;
        if(pressed&CONT_A)travel_to(game.travel_selection);
        if(pressed&CONT_B)game.travel=false;
    }else if(game.paused){
        if(pressed&CONT_B)game.muted=!game.muted;
        if(pressed&CONT_X)game.postcard=!game.postcard;
    }else if(pressed&CONT_X)game.postcard=!game.postcard;
    return true;
}
#ifdef IE_BENCHMARK
#include "tests/render_benchmark.h"
#endif
#ifdef IE_DOLPHIN_PREVIEW
#include "tests/dolphin_preview.h"
#endif
int main(int argc,char **argv){
    (void)argc;(void)argv;
    printf("\nIsland Explorer / Emerald Coast / KallistiOS\n");
    vid_set_mode(DM_640x480,PM_RGB565);
    travel_to(0);
    if(!renderer_init()){printf("[fatal] renderer initialization failed\n");renderer_shutdown();return 1;}
    rendering=true;
    if(!audio_init())printf("[audio] unavailable, exploration continues\n");
    update_camera(1,0,0,false);
#ifdef IE_DOLPHIN_PREVIEW
    int result=run_dolphin_preview();
    audio_shutdown();renderer_shutdown();
    printf("[shutdown] Clean exit\n");return result;
#endif
#ifdef IE_BENCHMARK
    int result=run_render_benchmark();
    audio_shutdown();renderer_shutdown();
    printf("[shutdown] Clean exit\n");return result;
#endif
    uint64_t last=timer_ms_gettime64(),report=last;
    uint32_t previous=0;unsigned frames=0;bool saw_controller=false;
    bool running=true;
    #ifdef IE_AUTOTEST
    unsigned auto_phase=0;float phase_time=0;game.debug=true;
    unsigned test_failures=runtime_checks();Vec3 phase_start=game.player.position;
    bool jumped=false,airborne=false;float best_motion=0;
    float resort_floor=world_floor(&game.world,v3(-9,16,4),0);
    if(fabsf(resort_floor-4)>0.1f){printf("[test] FAIL: resort floor %.3f\n",(double)resort_floor);test_failures++;}
    World invalid_world;
    if(world_load(&invalid_world,coast0_start,15)){printf("[test] FAIL: truncated asset accepted\n");test_failures++;}
    #endif
    while(running){
        uint64_t now=timer_ms_gettime64();float dt=clampf((now-last)*.001f,.001f,.05f);last=now;
        uint32_t buttons=0;float ix=0,iy=0,turn=0,tilt=0;
        maple_device_t *dev=maple_enum_type(0,MAPLE_FUNC_CONTROLLER);
        cont_state_t *pad=dev?maple_dev_status(dev):NULL;
        if(pad){
            if(!saw_controller){printf("[input] Dreamcast controller connected\n");saw_controller=true;}
            buttons=pad->buttons;ix=pad->joyx/127.f;iy=pad->joyy/127.f;
            turn=(pad->rtrig-pad->ltrig)/255.f;
            if(buttons&CONT_DPAD_LEFT)turn-=1;
            if(buttons&CONT_DPAD_RIGHT)turn+=1;
            if(buttons&CONT_DPAD_UP)tilt-=1;
            if(buttons&CONT_DPAD_DOWN)tilt+=1;
        }else if(saw_controller){saw_controller=false;printf("[input] controller disconnected\n");}
        maple_device_t *kd=maple_enum_type(0,MAPLE_FUNC_KEYBOARD);
        kbd_state_t *k=kd?kbd_get_state(kd):NULL;
        if(k){
            ix+=key(k,KBD_KEY_D)-key(k,KBD_KEY_A);iy+=key(k,KBD_KEY_S)-key(k,KBD_KEY_W);
            turn+=key(k,KBD_KEY_RIGHT)-key(k,KBD_KEY_LEFT);tilt+=key(k,KBD_KEY_DOWN)-key(k,KBD_KEY_UP);
            if(key(k,KBD_KEY_SPACE))buttons|=CONT_A;
            if(key(k,KBD_KEY_E))buttons|=CONT_B;
            if(key(k,KBD_KEY_P))buttons|=CONT_START;
            if(key(k,KBD_KEY_TAB))buttons|=CONT_Y;
            if(key(k,KBD_KEY_C))buttons|=CONT_X;
            if(key(k,KBD_KEY_UP))buttons|=CONT_DPAD_UP;
            if(key(k,KBD_KEY_DOWN))buttons|=CONT_DPAD_DOWN;
            if(key(k,KBD_KEY_F3))game.debug=true;
        }
        uint32_t pressed=buttons&~previous;previous=buttons;
        bool menu_was_open=game.travel||game.paused;
        if(!handle_buttons(buttons,pressed))running=false;
        if(!menu_was_open&&!game.travel&&!game.paused){
            #ifdef IE_AUTOTEST
            phase_time+=dt;
            if(phase_time>12){
                if(best_motion<12||!jumped||!airborne){
                    printf("[test] FAIL: phase %u motion %.1f jump %d airborne %d\n",auto_phase,(double)best_motion,jumped,airborne);test_failures++;
                }
                printf("[test] phase %u: travelled %.1f units, jump %d, airborne %d\n",auto_phase,(double)best_motion,jumped,airborne);
                phase_time=0;auto_phase++;
                if(auto_phase>=destination_count){printf("[test] %s: traversal complete, %u failures\n",test_failures?"FAIL":"PASS",test_failures);running=false;}
                else {travel_to(auto_phase);phase_start=game.player.position;best_motion=0;jumped=false;airborne=false;}
            }
            iy=(phase_time>3&&phase_time<8)?-1:0;
            ix=(phase_time>6&&phase_time<8)?.4f:0;
            turn=(phase_time>8)?.5f:0;
            if(phase_time>1.2f&&phase_time<1.2f+dt*1.5f){pressed|=CONT_A;jumped=true;}
            #endif
            update_player(dt,ix,iy,buttons&CONT_B,pressed&CONT_A);
            #ifdef IE_AUTOTEST
            Vec3 motion=sub(game.player.position,phase_start);motion.y=0;
            best_motion=fmaxf(best_motion,length(motion));airborne|=!game.player.grounded;
            if(!isfinite(game.player.position.x)||!isfinite(game.player.position.y)||!isfinite(game.player.position.z)){test_failures++;running=false;}
            #endif
            update_camera(dt,turn,tilt,pad&&pad->ltrig>200&&pad->rtrig>200);
            game.arrival=fmaxf(0,game.arrival-dt);
        }
        game.time+=dt;game.fade=fmaxf(0,game.fade-dt*1.5f);
        audio_update(dt);renderer_draw();frames++;
        if(now-report>3000){
            game.fps=frames*1000.f/(now-report);frames=0;report=now;
            printf("[frame] %.1f fps | %u batches %u triangles | p %.1f %.1f %.1f | %s\n",(double)game.fps,game.visible_meshes,game.triangles,(double)game.player.position.x,(double)game.player.position.y,(double)game.player.position.z,game.player.swimming?"swimming":game.player.grounded?"grounded":"airborne");
        }
        thd_pass();
    }
    audio_shutdown();renderer_shutdown();printf("[shutdown] Clean exit\n");return 0;
}
