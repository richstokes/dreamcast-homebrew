#ifndef DLA_RENDER_VISIBILITY_H
#define DLA_RENDER_VISIBILITY_H

#include <math.h>
#include <stdbool.h>

typedef struct {
    float x,y,z;
    float sin_yaw,cos_yaw,sin_pitch,cos_pitch,sin_roll,cos_roll;
    float focal,center_x,center_y,width,height,near_z,far_z,margin;
} dla_frustum_camera_t;

typedef struct {
    float x,y,z,abs_x,abs_y,abs_z,offset;
} dla_frustum_plane_t;

typedef struct {
    float camera_x,camera_y,camera_z;
    dla_frustum_plane_t planes[6];
} dla_frustum_t;

static inline dla_frustum_plane_t dla_frustum_plane(float x,float y,float z,
                                                    float offset) {
    return (dla_frustum_plane_t){x,y,z,fabsf(x),fabsf(y),fabsf(z),offset};
}

/* Build camera-relative world-axis planes once per frame. These rows match
   dla_load_camera_rotation; retaining world axes makes AABB support exact. */
static inline void dla_frustum_setup(dla_frustum_t *out,
                                      const dla_frustum_camera_t *camera) {
    const float sy=camera->sin_yaw,cy=camera->cos_yaw;
    const float sp=camera->sin_pitch,cp=camera->cos_pitch;
    const float sr=camera->sin_roll,cr=camera->cos_roll;
    const float xx=cr*cy+sr*sp*sy,xy=-sr*cp,xz=-cr*sy+sr*sp*cy;
    const float yx=sr*cy-cr*sp*sy,yy=cr*cp,yz=-sr*sy-cr*sp*cy;
    const float zx=cp*sy,zy=sp,zz=cp*cy;
    const float left=(camera->center_x+camera->margin)/camera->focal;
    const float right=(camera->width-camera->center_x+camera->margin)/camera->focal;
    const float top=(camera->center_y+camera->margin)/camera->focal;
    const float bottom=(camera->height-camera->center_y+camera->margin)/camera->focal;
    out->camera_x=camera->x;
    out->camera_y=camera->y;
    out->camera_z=camera->z;
    out->planes[0]=dla_frustum_plane(xx+left*zx,xy+left*zy,xz+left*zz,0.0f);
    out->planes[1]=dla_frustum_plane(-xx+right*zx,-xy+right*zy,-xz+right*zz,0.0f);
    out->planes[2]=dla_frustum_plane(-yx+top*zx,-yy+top*zy,-yz+top*zz,0.0f);
    out->planes[3]=dla_frustum_plane(yx+bottom*zx,yy+bottom*zy,yz+bottom*zz,0.0f);
    out->planes[4]=dla_frustum_plane(zx,zy,zz,-camera->near_z);
    out->planes[5]=dla_frustum_plane(-zx,-zy,-zz,camera->far_z);
}

/* Cull only when every point in this box lies outside the same plane.
   The caller supplies bounds for all geometry gated by the result, including
   rooftop caps, side wings, signs and street furniture. Small numerical slack
   avoids a boundary flip from float rounding; it is not a geometric margin. */
static inline bool dla_frustum_aabb_visible(const dla_frustum_t *frustum,
                                            float cx,float cy,float cz,
                                            float hx,float hy,float hz) {
    const float dx=cx-frustum->camera_x,dy=cy-frustum->camera_y;
    const float dz=cz-frustum->camera_z;
    int i;
    for(i=0;i<6;++i) {
        const dla_frustum_plane_t *plane=&frustum->planes[i];
        const float distance=plane->x*dx+plane->y*dy+plane->z*dz+plane->offset;
        const float support=plane->abs_x*hx+plane->abs_y*hy+plane->abs_z*hz;
        if(distance+support<-.01f) return false;
    }
    return true;
}

/* Road centre lines lie on cell multiples; blocks occupy the intervals.
   Halfway ties select the greater cell, including on the negative axes. */
static inline int dla_nearest_road_cell(float coordinate,float cell_size) {
    return (int)floorf(coordinate/cell_size+.5f);
}

#endif
