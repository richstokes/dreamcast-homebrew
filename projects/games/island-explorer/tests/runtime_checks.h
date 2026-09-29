/* These checks run on the SH-4 in the diagnostic ELF. */
#include "dolphin_checks.h"
static unsigned runtime_checks(void) {
    const Vertex vertices[]={
        {{-20,0,-20},0,0,0},{{20,0,-20},0,0,0},{{-20,0,20},0,0,0},{{20,0,20},0,0,0},
        {{10,0,-10},0,0,0},{{10,10,-10},0,0,0},{{10,0,10},0,0,0},{{10,10,10},0,0,0}
    };
    const uint16_t indices[]={0,2,1,1,2,3,4,5,6,6,5,7};
    const Mesh mesh={.center={0,5,0},.radius=35,.vertex_count=8,.index_count=12,.surface=1};
    const World world={1,8,12,&mesh,vertices,indices};
    unsigned failures=dolphin_checks();
    float floor=world_floor(&world,v3(0,5,0),0);
    if(fabsf(floor)>0.001f){printf("[test] FAIL floor query\n");failures++;}
    Vec3 body=v3(9,0,0);world_slide(&world,&body);
    if(body.x>7.81f||body.x<7.79f){printf("[test] FAIL wall collision: %.3f\n",(double)body.x);failures++;}
    float hit=world_raycast(&world,v3(0,5,0),v3(20,5,0));
    if(fabsf(hit-.5f)>.001f){printf("[test] FAIL camera obstruction: %.3f\n",(double)hit);failures++;}
    printf("[test] SH-4 floor / wall / camera checks: %s\n",failures?"FAIL":"PASS");
    unsigned previous_failures=failures;
    handle_buttons(CONT_START,CONT_START);
    if(!game.paused||game.travel)failures++;
    handle_buttons(CONT_B,CONT_B);
    if(!game.muted)failures++;
    handle_buttons(CONT_X,CONT_X);
    if(!game.postcard)failures++;
    handle_buttons(CONT_Y,CONT_Y);
    if(!game.travel||game.paused)failures++;
    handle_buttons(CONT_DPAD_UP,CONT_DPAD_UP);
    if(game.travel_selection!=(int)destination_count-1)failures++;
    handle_buttons(CONT_DPAD_DOWN,CONT_DPAD_DOWN);
    if(game.travel_selection!=0)failures++;
    handle_buttons(CONT_A,CONT_A);
    if(game.travel||game.paused||game.act!=0)failures++;
    uint32_t quit=CONT_START|CONT_A|CONT_B|CONT_X|CONT_Y;
    if(handle_buttons(quit,quit))failures++;
    game.muted=false;game.postcard=false;
    printf("[test] pause / mute / postcard / travel / exit checks: %s\n",failures==previous_failures?"PASS":"FAIL");
    return failures;
}
