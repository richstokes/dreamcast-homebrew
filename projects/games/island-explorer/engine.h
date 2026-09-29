#ifndef ISLAND_ENGINE_H
#define ISLAND_ENGINE_H
#include <kos.h>
#include <dc/pvr.h>
#include <dc/fmath.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct { float x,y,z; } Vec3;
static inline Vec3 v3(float x,float y,float z){return (Vec3){x,y,z};}
static inline Vec3 add(Vec3 a,Vec3 b){return v3(a.x+b.x,a.y+b.y,a.z+b.z);}
static inline Vec3 sub(Vec3 a,Vec3 b){return v3(a.x-b.x,a.y-b.y,a.z-b.z);}
static inline Vec3 mul(Vec3 a,float s){return v3(a.x*s,a.y*s,a.z*s);}
static inline float dot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static inline Vec3 cross(Vec3 a,Vec3 b){return v3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
static inline float length(Vec3 a){return sqrtf(dot(a,a));}
static inline Vec3 unit(Vec3 a){float n=length(a);return n>0.00001f?mul(a,1/n):v3(0,1,0);}
static inline float clampf(float a,float lo,float hi){return fminf(hi,fmaxf(lo,a));}
static inline float mixf(float a,float b,float t){return a+(b-a)*t;}

typedef struct {Vec3 p;float u,v;uint32_t color;} Vertex;
typedef struct {
    Vec3 center; float radius;
    uint32_t first_vertex,vertex_count,first_index,index_count;
    int32_t texture; uint32_t material_flags,surface,reserved;
} Mesh;
typedef struct {
    uint32_t mesh_count,vertex_count,index_count;
    const Mesh *meshes; const Vertex *vertices; const uint16_t *indices;
} World;
typedef struct {
    Vec3 position,velocity; float heading,gait,speed; bool grounded,swimming;
} Player;
typedef struct {
    Vec3 position,right,up,forward,target;
    float yaw,pitch,distance;
} Camera;
typedef struct {
    World world; Player player; Camera camera;
    int act,travel_selection; bool paused,travel,postcard,muted,debug;
    float time,arrival,fade,fps; unsigned visible_meshes,triangles;
    const char *location;
} Game;
typedef struct {const char *name; int act; Vec3 position;float yaw;} Destination;
extern const Destination destinations[];
extern const unsigned destination_count;
extern Game game;
bool world_load(World *world,const uint8_t *data,size_t size);
float world_water_level(Vec3 p);
float world_floor(const World *world,Vec3 p,float max_rise);
void world_slide(const World *world,Vec3 *position);
float world_raycast(const World *world,Vec3 from,Vec3 to);
void travel_to(unsigned index);
bool renderer_init(void);
void renderer_world_changed(void);
void renderer_draw(void);
void renderer_shutdown(void);
bool audio_init(void);
void audio_update(float dt);
void audio_footstep(void);
void audio_shutdown(void);
extern const uint8_t coast0_start[],coast0_end[],coast1_start[],coast1_end[];
extern const uint8_t textures_start[],textures_end[],sky_start[],sky_end[];
extern const uint8_t foam0_start[],foam1_start[];
extern const uint8_t ambience_start[],ambience_end[],font_start[];
#endif
