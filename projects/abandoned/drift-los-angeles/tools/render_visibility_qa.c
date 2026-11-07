/* Standalone SH-4 regression tests for streamed-city visibility. */
#include <kos.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "../render_visibility.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

typedef struct { float x,y,z; } qa_point_t;
static unsigned checks,failures,reference_boxes;

static void check(bool condition,const char *name) {
    ++checks;
    if(condition) return;
    if(failures<8) printf("Render visibility QA mismatch: %s.\n",name);
    ++failures;
}

static dla_frustum_camera_t camera_at(float x,float y,float z,
                                      float yaw,float pitch,float roll) {
    return (dla_frustum_camera_t){
        .x=x,.y=y,.z=z,
        .sin_yaw=fsin(yaw),.cos_yaw=fcos(yaw),
        .sin_pitch=fsin(pitch),.cos_pitch=fcos(pitch),
        .sin_roll=fsin(roll),.cos_roll=fcos(roll),
        .focal=500.0f,.center_x=320.0f,.center_y=240.0f,
        .width=640.0f,.height=480.0f,.near_z=.35f,.far_z=760.0f,.margin=72.0f
    };
}

/* Independent original scalar sequence, not the precomposed plane rows. */
static qa_point_t camera_point(const dla_frustum_camera_t *c,qa_point_t p) {
    const float dx=p.x-c->x,dy=p.y-c->y,dz=p.z-c->z;
    const float x=dx*c->cos_yaw-dz*c->sin_yaw;
    const float z=dx*c->sin_yaw+dz*c->cos_yaw;
    const float y=dy*c->cos_pitch-z*c->sin_pitch;
    return (qa_point_t){x*c->cos_roll-y*c->sin_roll,
                       x*c->sin_roll+y*c->cos_roll,
                       dy*c->sin_pitch+z*c->cos_pitch};
}

static void production_false_cull(void) {
    /* Actual generated Downtown block (-1,0), while the player drifts along
       x=-10,z=80. Its corner is near screen (193,225), not near a screen edge. */
    const dla_frustum_camera_t camera=camera_at(-8.13648666f,2.3f,75.1453818f,
                                                -.366519143f,-.13f,.08f);
    const qa_point_t block=camera_point(&camera,(qa_point_t){-60.0f,12.0f,60.0f});
    const qa_point_t corner=camera_point(&camera,
        (qa_point_t){-24.3101244f,.1f,98.1586481f});
    const float old_half=392.0f*fmaxf(block.z,.35f)/500.0f+51.0f;
    const float sx=320.0f+corner.x*500.0f/corner.z;
    const float sy=240.0f-corner.y*500.0f/corner.z;
    dla_frustum_t frustum;
    dla_frustum_setup(&frustum,&camera);
    check(fabsf(block.x)>old_half,"reproduction rejects old block bound");
    check(sx>190.0f && sx<196.0f && sy>222.0f && sy<229.0f,
          "reproduction corner is visibly inside the screen");
    check(dla_frustum_aabb_visible(&frustum,-60.0f,42.8063409f,60.0f,
                                   35.6898756f,42.7063409f,38.1586481f),
          "production building survives plane support test");
    printf("Render visibility QA production corner: x=%.4f y=%.4f.\n",
           (double)sx,(double)sy);
}

static void compare_box(const dla_frustum_camera_t *camera,
                         float cx,float cy,float cz,float hx,float hy,float hz) {
    float maximum[6]={-1e30f,-1e30f,-1e30f,-1e30f,-1e30f,-1e30f};
    dla_frustum_t frustum;
    bool safely_visible=true,clearly_hidden=false;
    int corner,i;
    dla_frustum_setup(&frustum,camera);
    for(corner=0;corner<8;++corner) {
        const qa_point_t p=camera_point(camera,(qa_point_t){
            cx+((corner&1)?hx:-hx),cy+((corner&2)?hy:-hy),
            cz+((corner&4)?hz:-hz)});
        const float values[6]={
            p.x+(camera->center_x+camera->margin)*p.z/camera->focal,
            -p.x+(camera->width-camera->center_x+camera->margin)*p.z/camera->focal,
            -p.y+(camera->center_y+camera->margin)*p.z/camera->focal,
            p.y+(camera->height-camera->center_y+camera->margin)*p.z/camera->focal,
            p.z-camera->near_z,camera->far_z-p.z};
        for(i=0;i<6;++i) maximum[i]=fmaxf(maximum[i],values[i]);
    }
    for(i=0;i<6;++i) {
        if(maximum[i]<.03f) safely_visible=false;
        if(maximum[i]<-.04f) clearly_hidden=true;
    }
    if(safely_visible)
        check(dla_frustum_aabb_visible(&frustum,cx,cy,cz,hx,hy,hz),
              "no false rejection against eight-corner oracle");
    if(clearly_hidden)
        check(!dla_frustum_aabb_visible(&frustum,cx,cy,cz,hx,hy,hz),
              "fully hidden AABB still rejected");
    ++reference_boxes;
}

static void camera_sweep(void) {
    static const float origins[3][2]={{0,0},{1037,-543},{-99500,100000}};
    unsigned origin;
    int yaw,roll,x,z,height;
    for(origin=0;origin<3;++origin)
        for(yaw=0;yaw<24;++yaw)
            for(roll=-1;roll<=1;++roll) {
                const dla_frustum_camera_t camera=camera_at(
                    origins[origin][0],2.3f,origins[origin][1],
                    (float)yaw*.261799388f,-.13f,(float)roll*.08f);
                for(x=-2;x<=2;++x)
                    for(z=-2;z<=2;++z)
                        for(height=0;height<3;++height)
                            compare_box(&camera,
                                camera.x+(float)x*120.0f+60.0f,
                                6.0f+(float)height*38.0f,
                                camera.z+(float)z*120.0f+60.0f,
                                44.5f,6.0f+(float)height*38.0f,44.5f);
            }
}

static void plane_boundaries(void) {
    dla_frustum_camera_t camera=camera_at(0,0,0,0,0,0);
    dla_frustum_t frustum;
    camera.margin=0;
    dla_frustum_setup(&frustum,&camera);
    check(dla_frustum_aabb_visible(&frustum,6.4f,0,10,0,0,0),"right tangent retained");
    check(dla_frustum_aabb_visible(&frustum,-6.4f,0,10,0,0,0),"left tangent retained");
    check(dla_frustum_aabb_visible(&frustum,0,4.8f,10,0,0,0),"top tangent retained");
    check(dla_frustum_aabb_visible(&frustum,0,-4.8f,10,0,0,0),"bottom tangent retained");
    check(dla_frustum_aabb_visible(&frustum,0,0,.35f,0,0,0),"near tangent retained");
    check(dla_frustum_aabb_visible(&frustum,0,0,760,0,0,0),"far tangent retained");
    check(dla_frustum_aabb_visible(&frustum,0,0,0,50,50,50),"camera inside box retained");
    check(!dla_frustum_aabb_visible(&frustum,20,0,10,1,1,1),"box wholly right rejected");
    check(!dla_frustum_aabb_visible(&frustum,0,0,-5,1,1,1),"box wholly behind rejected");
    check(!dla_frustum_aabb_visible(&frustum,0,0,800,1,1,1),"box beyond far rejected");
}

static void road_boundaries(void) {
    static const int cells[]={-80,-4,-1,0,1,4,80};
    unsigned i;
    for(i=0;i<sizeof(cells)/sizeof(cells[0]);++i) {
        const int cell=cells[i];
        const float center=(float)cell*120.0f;
        check(dla_nearest_road_cell(center-.01f,120)==cell,"road left of centre unchanged");
        check(dla_nearest_road_cell(center,120)==cell,"road at centre unchanged");
        check(dla_nearest_road_cell(center+.01f,120)==cell,"road right of centre unchanged");
        check(dla_nearest_road_cell(center+59.99f,120)==cell,"road before halfway tie");
        check(dla_nearest_road_cell(center+60.0f,120)==cell+1,"road halfway selects greater cell");
        check(dla_nearest_road_cell(center+60.01f,120)==cell+1,"road after halfway tie");
    }
    check(dla_nearest_road_cell(-.000001f,120)==0,"negative tiny offset remains road zero");
    check(dla_nearest_road_cell(.000001f,120)==0,"positive tiny offset remains road zero");
}

int main(int argc,char **argv) {
    (void)argc;
    (void)argv;
    production_false_cull();
    camera_sweep();
    plane_boundaries();
    road_boundaries();
    printf("Render visibility QA: checks=%u reference_boxes=%u failures=%u.\n",
           checks,reference_boxes,failures);
    printf("Render visibility QA: %s.\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
