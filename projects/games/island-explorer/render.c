#include "engine.h"
#include "render_math.h"
#include "dolphins.h"
#include <sh4zam/shz_mem.h>
#include "assets/generated/asset_ids.h"
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <dc/matrix.h>

#define NEAR 0.8f
#define FOCAL 510.0f
#define FAR_CLIP 4200.0f
#define MAX_VERTICES 8192

typedef struct {unsigned width,height,pixel,layout,offset,size;} TextureInfo;
static pvr_ptr_t textures[TEXTURE_COUNT],font_texture;
static TextureInfo texture_info[TEXTURE_COUNT];
static pvr_poly_hdr_t solid,translucent,overlay,font_header,sky_headers[8],water_headers[10],shadow_header;
static pvr_poly_hdr_t *mesh_headers;
static Vertex *camera_vertices;
static pvr_vertex_t *projected;
static uint8_t clip_codes[MAX_VERTICES];
static World sky;
static const pvr_poly_hdr_t *active_header;
static unsigned mesh_list[10000];
static unsigned visible_count;
static bool initialized, drew_frame;
static DolphinFrame wildlife;
#ifdef IE_BENCHMARK
uint64_t renderer_cpu_us;
#endif

static void header(const pvr_poly_hdr_t *h){if(active_header!=h){pvr_prim(h,sizeof(*h));active_header=h;}}
static Vertex view(Vertex v){
#ifdef IE_REFERENCE_RENDER
    Vec3 p=sub(v.p,game.camera.position);
    v.p=v3(dot(p,game.camera.right),dot(p,game.camera.up),dot(p,game.camera.forward));return v;
#else
    v.p=render_transform(v.p,&game.camera);return v;
#endif
}
static Vertex lerp_vertex(Vertex a,Vertex b,float t){
    Vertex r={add(a.p,mul(sub(b.p,a.p),t)),mixf(a.u,b.u,t),mixf(a.v,b.v,t),0};
    for(int s=0;s<32;s+=8)r.color|=(uint32_t)mixf((a.color>>s)&255,(b.color>>s)&255,t)<<s;
    return r;
}
static void submit_triangle(const pvr_poly_hdr_t *h,Vertex a,Vertex b,Vertex c,bool background){
    if(game.triangles>=12000||a.p.z<NEAR||b.p.z<NEAR||c.p.z<NEAR)return;
    Vertex vv[3]={a,b,c};pvr_vertex_t out[3] __attribute__((aligned(32)));
    float minx=1e9f,miny=1e9f,maxx=-1e9f,maxy=-1e9f;
    for(int i=0;i<3;i++){
        float inv=render_depth_reciprocal(vv[i].p.z);
        float x=320+vv[i].p.x*FOCAL*inv,y=240-vv[i].p.y*FOCAL*inv;
        minx=fminf(x,minx);miny=fminf(y,miny);maxx=fmaxf(x,maxx);maxy=fmaxf(y,maxy);
        out[i]=(pvr_vertex_t){.flags=i==2?PVR_CMD_VERTEX_EOL:PVR_CMD_VERTEX,.x=x,.y=y,.z=background?.00011f:inv,.u=vv[i].u,.v=vv[i].v,.argb=vv[i].color,.oargb=0};
    }
    if(maxx<0||minx>640||maxy<0||miny>480)return;
    header(h);pvr_prim(out,sizeof(out));game.triangles++;
}
static void clip_triangle(const pvr_poly_hdr_t *h,Vertex a,Vertex b,Vertex c,bool bg){
    if(a.p.z>=NEAR&&b.p.z>=NEAR&&c.p.z>=NEAR){submit_triangle(h,a,b,c,bg);return;}
    Vertex in[3]={a,b,c},out[4];int count=0;
    for(int i=0;i<3;i++){
        Vertex start=in[i],end=in[(i+1)%3];bool ins=start.p.z>=NEAR,ine=end.p.z>=NEAR;
        if(ins)out[count++]=start;
        if(ins!=ine){Vertex v=lerp_vertex(start,end,(NEAR-start.p.z)/(end.p.z-start.p.z));v.p.z=NEAR;out[count++]=v;}
    }
    for(int i=1;i<count-1;i++)submit_triangle(h,out[0],out[i],out[i+1],bg);
}
static void world_triangle(const pvr_poly_hdr_t *h,Vec3 a,Vec3 b,Vec3 c,uint32_t color){
    clip_triangle(h,view((Vertex){a,0,0,color}),view((Vertex){b,0,0,color}),view((Vertex){c,0,0,color}),false);
}
static void world_quad(const pvr_poly_hdr_t *h,Vertex a,Vertex b,Vertex c,Vertex d){
    a=view(a);b=view(b);c=view(c);d=view(d);
    clip_triangle(h,a,b,c,false);clip_triangle(h,c,b,d,false);
}
static void rect(const pvr_poly_hdr_t *h,float x,float y,float w,float height,uint32_t color){
    pvr_vertex_t v[4] __attribute__((aligned(32)))={
        {.flags=PVR_CMD_VERTEX,.x=x,.y=y,.z=100,.u=0,.v=0,.argb=color},{.flags=PVR_CMD_VERTEX,.x=x+w,.y=y,.z=100,.u=1,.v=0,.argb=color},
        {.flags=PVR_CMD_VERTEX,.x=x,.y=y+height,.z=100,.u=0,.v=1,.argb=color},{.flags=PVR_CMD_VERTEX_EOL,.x=x+w,.y=y+height,.z=100,.u=1,.v=1,.argb=color}};
    header(h);pvr_prim(v,sizeof(v));
}
static void text(float x,float y,float size,const char *s,uint32_t color,float spacing){
    header(&font_header);
    while(*s){
        unsigned c=(unsigned char)*s++;
        if(c<32||c>127)c='?';
        unsigned i=c-32;float u=(i%16)/16.f,v=(i/16)/8.f;
        pvr_vertex_t p[4] __attribute__((aligned(32)))={
            {.flags=PVR_CMD_VERTEX,.x=x,.y=y,.z=110,.u=u,.v=v,.argb=color},{.flags=PVR_CMD_VERTEX,.x=x+size,.y=y,.z=110,.u=u+1/16.f,.v=v,.argb=color},
            {.flags=PVR_CMD_VERTEX,.x=x,.y=y+size,.z=110,.u=u,.v=v+1/8.f,.argb=color},{.flags=PVR_CMD_VERTEX_EOL,.x=x+size,.y=y+size,.z=110,.u=u+1/16.f,.v=v+1/8.f,.argb=color}};
        pvr_prim(p,sizeof(p));x+=size*.55f+spacing;
    }
}
static uint32_t texture_format(const TextureInfo *t){
    uint32_t f=t->pixel==0?PVR_TXRFMT_ARGB1555:(t->pixel==1?PVR_TXRFMT_RGB565:PVR_TXRFMT_ARGB4444);
    if(t->layout==3||t->layout==4)f|=PVR_TXRFMT_VQ_ENABLE;
    return f;
}
static void make_header(pvr_poly_hdr_t *h,int texture,uint32_t flags,pvr_list_t list,bool fog){
    pvr_poly_cxt_t c;
    if(texture>=0&&texture<TEXTURE_COUNT){
        const TextureInfo *t=&texture_info[texture];
        pvr_poly_cxt_txr(&c,list,texture_format(t),t->width,t->height,textures[texture],PVR_FILTER_BILINEAR);
        c.txr.mipmap=(t->layout==2||t->layout==4);
        c.txr.uv_flip=((flags&0x40000)?PVR_UVFLIP_U:0)|((flags&0x20000)?PVR_UVFLIP_V:0);
        c.txr.uv_clamp=((flags&0x10000)?PVR_UVCLAMP_U:0)|((flags&0x8000)?PVR_UVCLAMP_V:0);
        c.txr.env=PVR_TXRENV_MODULATEALPHA;
    }else pvr_poly_cxt_col(&c,list);
    c.gen.culling=PVR_CULLING_NONE;c.gen.fog_type=fog?PVR_FOG_TABLE:PVR_FOG_DISABLE;
    c.gen.alpha=(list!=PVR_LIST_OP_POLY);
    if(list==PVR_LIST_TR_POLY){c.blend.src=PVR_BLEND_SRCALPHA;c.blend.dst=PVR_BLEND_INVSRCALPHA;}
    pvr_poly_compile(h,&c);
}
static pvr_list_t mesh_pass(const Mesh *m){
    if(m->material_flags&0x100000)return PVR_LIST_TR_POLY;
    return PVR_LIST_OP_POLY;
}
void renderer_world_changed(void){
    free(mesh_headers);mesh_headers=memalign(32,game.world.mesh_count*sizeof(*mesh_headers));
    if(!mesh_headers){printf("[render] out of memory for polygon headers\n");return;}
    for(unsigned i=0;i<game.world.mesh_count;i++){
        const Mesh *m=&game.world.meshes[i];
        make_header(&mesh_headers[i],m->texture,m->material_flags,mesh_pass(m),true);
    }
}
bool renderer_init(void){
    pvr_init_params_t params={.opb_sizes={PVR_BINSIZE_16,0,PVR_BINSIZE_16,0,0},.vertex_buf_size=1280*1024,.dma_enabled=0,.fsaa_enabled=0,.autosort_disabled=0};
    if(pvr_init(&params)<0)return false;
    initialized=true;
    pvr_set_bg_color(.34f,.68f,.88f);
    pvr_fog_table_color(1,.57f,.79f,.86f);pvr_fog_table_linear(2300,5000);
    if(memcmp(textures_start,"IET1",4))return false;
    unsigned count=((const uint32_t *)textures_start)[1];
    if(count!=TEXTURE_COUNT)return false;
    memcpy(texture_info,textures_start+8,sizeof(texture_info));
    for(unsigned i=0;i<count;i++){
        TextureInfo *t=&texture_info[i];
        if(t->offset+t->size>(unsigned)(textures_end-textures_start))return false;
        unsigned size=(t->size+31)&~31u;
        textures[i]=pvr_mem_malloc(size);
        if(!textures[i]){printf("[render] VRAM allocation failed at texture %u\n",i);return false;}
        /* sq_cpy works in 32-byte blocks, so stage a padded final block. */
        unsigned full=t->size&~31u;
        if(full)pvr_txr_load(textures_start+t->offset,textures[i],full);
        if(t->size-full){uint8_t tail[32] __attribute__((aligned(32)))={0};memcpy(tail,textures_start+t->offset+full,t->size-full);pvr_txr_load(tail,(uint8_t *)textures[i]+full,32);}
    }
    make_header(&solid,-1,0,PVR_LIST_OP_POLY,true);
    make_header(&translucent,-1,0,PVR_LIST_TR_POLY,true);
    make_header(&overlay,-1,0,PVR_LIST_TR_POLY,false);
    make_header(&shadow_header,-1,0,PVR_LIST_TR_POLY,true);
    font_texture=pvr_mem_malloc(512*256*2);
    if(!font_texture)return false;
    pvr_txr_load(font_start,font_texture,512*256*2);
    pvr_poly_cxt_t font;
    pvr_poly_cxt_txr(&font,PVR_LIST_TR_POLY,PVR_TXRFMT_ARGB4444|PVR_TXRFMT_NONTWIDDLED,512,256,font_texture,PVR_FILTER_BILINEAR);
    font.gen.culling=PVR_CULLING_NONE;font.gen.fog_type=PVR_FOG_DISABLE;font.depth.write=false;
    pvr_poly_compile(&font_header,&font);
    if(!world_load(&sky,sky_start,(size_t)(sky_end-sky_start)))return false;
    for(unsigned i=0;i<sky.mesh_count&&i<8;i++)make_header(&sky_headers[i],sky.meshes[i].texture,0,PVR_LIST_OP_POLY,false);
    for(int i=0;i<10;i++)make_header(&water_headers[i],TEX_BEACH_SEA+i,0,PVR_LIST_OP_POLY,true);
    camera_vertices=memalign(32,MAX_VERTICES*sizeof(Vertex));
    projected=memalign(32,MAX_VERTICES*sizeof(pvr_vertex_t));
    if(!camera_vertices||!projected)return false;
    renderer_world_changed();
    printf("[render] %u original PVR textures; free VRAM %lu bytes\n",count,(unsigned long)pvr_mem_available());
    return mesh_headers!=NULL;
}
static void gather_visible(void){
    visible_count=0;
    for(unsigned i=0;i<game.world.mesh_count&&visible_count<10000;i++){
        const Mesh *m=&game.world.meshes[i];
        if(!(m->surface&0x80000000)||m->vertex_count>MAX_VERTICES)continue;
        Vec3 d=sub(m->center,game.camera.position);
        float z=dot(d,game.camera.forward),r=m->radius;
        if(z+r<NEAR||z-r>FAR_CLIP)continue;
        if(m->reserved && dot(d,d)>1000.f*1000.f && m->radius<150)continue;
        if(fabsf(dot(d,game.camera.right))>z*.68f+r*1.22f||fabsf(dot(d,game.camera.up))>z*.52f+r*1.14f)continue;
        mesh_list[visible_count++]=i;
    }
    game.visible_meshes=visible_count;
}
static void draw_stage(pvr_list_t pass){
    if(!mesh_headers)return;
    const World *w=&game.world;
    for(unsigned j=0;j<visible_count;j++){
        unsigned im=mesh_list[j];const Mesh *m=&w->meshes[im];
        if(mesh_pass(m)!=pass)continue;
        for(unsigned i=0;i<m->vertex_count;i++){
            const Vertex *v=&w->vertices[m->first_vertex+i];
            Vec3 camera=render_transform(v->p,&game.camera);
            float x=camera.x,y=camera.y,z=camera.z;
            camera_vertices[i]=*v;camera_vertices[i].p=v3(x,y,z);
            float inv=z>=NEAR?render_depth_reciprocal(z):0;
            pvr_vertex_t *p=&projected[i];
            p->flags=PVR_CMD_VERTEX;p->x=320+x*FOCAL*inv;p->y=240-y*FOCAL*inv;
            p->z=inv;p->u=v->u;p->v=v->v;p->argb=v->color;p->oargb=0;
            clip_codes[i]=(z<NEAR?16:0)|(p->x<0?1:0)|(p->x>640?2:0)|(p->y<0?4:0)|(p->y>480?8:0);
        }
        header(&mesh_headers[im]);
        for(unsigned i=0;i<m->index_count;i+=3){
            if(game.triangles>=12000)break;
            const uint16_t *idx=w->indices+m->first_index+i;
            unsigned a=idx[0],b=idx[1],d=idx[2];
            if(clip_codes[a]&clip_codes[b]&clip_codes[d])continue;
            if((clip_codes[a]|clip_codes[b]|clip_codes[d])&16){
                clip_triangle(&mesh_headers[im],camera_vertices[a],camera_vertices[b],camera_vertices[d],false);
            }else{
                if(!(m->material_flags&0x800000)){
                    const pvr_vertex_t *pa=&projected[a],*pb=&projected[b],*pd=&projected[d];
                    float facing=(pb->x-pa->x)*(pd->y-pa->y)-(pb->y-pa->y)*(pd->x-pa->x);
                    if(facing<=0)continue;
                }
#ifdef IE_REFERENCE_RENDER
                pvr_vertex_t triangle[3] __attribute__((aligned(32)));
                triangle[0]=projected[a];triangle[1]=projected[b];triangle[2]=projected[d];
                triangle[2].flags=PVR_CMD_VERTEX_EOL;
                pvr_prim(triangle,sizeof(triangle));
#else
                /* KOS owns/locks the Store Queues between list_begin/finish.
                   Copy aligned cached vertices straight to the TA, avoiding
                   three intermediate struct copies for every stage triangle.
                   Use the non-XMTRX variant: the camera must stay loaded. */
                shz_sq_memcpy32_1(pvr_dr_target(),&projected[a]);
                shz_sq_memcpy32_1(pvr_dr_target(),&projected[b]);
                projected[d].flags=PVR_CMD_VERTEX_EOL;
                shz_sq_memcpy32_1(pvr_dr_target(),&projected[d]);
                projected[d].flags=PVR_CMD_VERTEX;
#endif
                game.triangles++;
            }
        }
    }
}
static void draw_sky(void){
    for(unsigned im=0;im<sky.mesh_count;im++){
        const Mesh *m=&sky.meshes[im];
        for(unsigned i=0;i<m->vertex_count;i++){
            Vertex v=sky.vertices[m->first_vertex+i];
            v.p=add(v.p,game.camera.position);v.color=0xffffffff;
            camera_vertices[i]=view(v);
        }
        for(unsigned i=0;i<m->index_count;i+=3){const uint16_t *idx=sky.indices+m->first_index+i;
            clip_triangle(&sky_headers[im],camera_vertices[idx[0]],camera_vertices[idx[1]],camera_vertices[idx[2]],true);}
    }
}
static Vertex water_vertex(float x,float z,float y){
    float phase=game.time;
    float wave=fsin(x*.031f+z*.019f+phase*.85f)*.14f+fsin(z*.047f-x*.011f+phase*1.13f)*.09f;
    float light=.90f+.06f*fsin(x*.045f+z*.039f+phase*.95f);
    uint32_t color=0xff000000|((int)(150*light)<<16)|((int)(235*light)<<8)|(int)(244*light);
    return (Vertex){v3(x,y+wave,z),x/180.f+phase*.011f,z/180.f+phase*.006f,color};
}
#ifndef IE_REFERENCE_RENDER
/* Neighbouring tiles share corners. Cache the wave, lighting, UV and SH4ZAM
   camera transform once per visible corner, without changing tessellation.
   The 29x29 grid costs 21 KB of main RAM and no additional PVR memory. */
static Vertex water_grid[29*29];
static uint8_t water_valid[29*29];
static Vertex water_corner(unsigned x,unsigned z,float wx,float wz,float y){
    unsigned index=z*29+x;
    if(!water_valid[index]){
        water_grid[index]=view(water_vertex(wx,wz,y));
        water_valid[index]=1;
    }
    return water_grid[index];
}
static void water_tile(const pvr_poly_hdr_t *h,unsigned x,unsigned z,
                       float wx,float wz,float spacing,float y){
    Vertex a=water_corner(x,z,wx,wz,y);
    Vertex b=water_corner(x+1,z,wx+spacing,wz,y);
    Vertex c=water_corner(x,z+1,wx,wz+spacing,y);
    Vertex d=water_corner(x+1,z+1,wx+spacing,wz+spacing,y);
    clip_triangle(h,a,b,c,false);clip_triangle(h,c,b,d,false);
}
#endif
static void draw_water(void){
    int frame=(int)(game.time*7.5f)%10;
    float cx=floorf(game.camera.position.x/180)*180,cz=floorf(game.camera.position.z/180)*180;
#ifndef IE_REFERENCE_RENDER
    memset(water_valid,0,sizeof(water_valid));
#endif
    for(int z=-14;z<14;z++)for(int x=-14;x<14;x++){
        float wx=cx+x*180,wz=cz+z*180;
        Vec3 d=sub(v3(wx+90,0,wz+90),game.camera.position);
        float depth=dot(d,game.camera.forward);
        if(depth<-160||fabsf(dot(d,game.camera.right))>depth*.70f+250)continue;
#ifdef IE_REFERENCE_RENDER
        world_quad(&water_headers[frame],water_vertex(wx,wz,-.2f),water_vertex(wx+180,wz,-.2f),water_vertex(wx,wz+180,-.2f),water_vertex(wx+180,wz+180,-.2f));
#else
        water_tile(&water_headers[frame],x+14,z+14,wx,wz,180,-.2f);
#endif
    }
    /* Beyond the detailed patch, the same ocean reaches the horizon. */
    float a=cx-2520,b=cx+2520,c=cz-2520,d=cz+2520,e=18000;
    const float regions[4][4]={{-e,-e,a,e},{b,-e,e,e},{a,-e,b,c},{a,d,b,e}};
    for(int k=0;k<4;k++){const float *r=regions[k];world_quad(&water_headers[frame],water_vertex(r[0],r[1],-1),water_vertex(r[2],r[1],-1),water_vertex(r[0],r[3],-1),water_vertex(r[2],r[3],-1));}
    /* The elevated lagoon in the second area. */
#ifndef IE_REFERENCE_RENDER
    memset(water_valid,0,sizeof(water_valid));
#endif
    if(game.act==1)for(int z=0;z<12;z++)for(int x=0;x<12;x++){
        float wx=-630+x*128,wz=-2160+z*128;
#ifdef IE_REFERENCE_RENDER
        world_quad(&water_headers[frame],water_vertex(wx,wz,539),water_vertex(wx+128,wz,539),water_vertex(wx,wz+128,539),water_vertex(wx+128,wz+128,539));
#else
        water_tile(&water_headers[frame],x,z,wx,wz,128,539);
#endif
    }
}
static Vec3 body_point(Vec3 local){
    float s,c;render_sincos(game.player.heading,&s,&c);
    return add(game.player.position,v3(local.x*c+local.z*s,local.y,-local.x*s+local.z*c));
}
static void body_box(Vec3 center,Vec3 size,uint32_t color,float swing){
    Vec3 points[8];
    for(int i=0;i<8;i++){
        Vec3 p=v3((i&1?1:-1)*size.x,(i&2?1:-1)*size.y,(i&4?1:-1)*size.z);
        float s,c;render_sincos(swing,&s,&c);
        float y=p.y*c-p.z*s,z=p.y*s+p.z*c;
        points[i]=body_point(add(center,v3(p.x,y,z)));
    }
    const uint8_t faces[6][4]={{0,1,2,3},{5,4,7,6},{4,0,6,2},{1,5,3,7},{2,3,6,7},{4,5,0,1}};
    for(int f=0;f<6;f++){
        uint32_t shade=color;if(f==2||f==0)shade=(color&0xff000000)|(((color>>16&255)*85/100)<<16)|(((color>>8&255)*85/100)<<8)|((color&255)*85/100);
        const uint8_t *a=faces[f];
        world_triangle(&solid,points[a[0]],points[a[1]],points[a[2]],shade);
        world_triangle(&solid,points[a[2]],points[a[1]],points[a[3]],shade);
    }
}
static void draw_player(void){
    if(length(sub(game.camera.position,game.camera.target))<14)return;
    float stride=fsin(game.player.gait)*clampf(game.player.speed/60,0,1)*.62f;
    float bob=fabsf(fsin(game.player.gait))*clampf(game.player.speed/80,0,.28f);
    if(game.player.swimming)bob=fsin(game.time*2)*.2f;
    body_box(v3(-.92f,2.1f,0),v3(.6f,1.55f,.65f),0xffba9678,stride);
    body_box(v3(.92f,2.1f,0),v3(.6f,1.55f,.65f),0xffba9678,-stride);
    body_box(v3(-.92f,.65f,.35f+stride),v3(.65f,.4f,1),0xfff6e8d1,0);
    body_box(v3(.92f,.65f,.35f-stride),v3(.65f,.4f,1),0xfff6e8d1,0);
    body_box(v3(0,4.15f+bob,0),v3(1.6f,.9f,1),0xffe8d5a5,0);
    body_box(v3(0,6.15f+bob,0),v3(1.55f,1.35f,.94f),0xffd46742,0);
    body_box(v3(0,6.0f+bob,-1.35f),v3(1.12f,1.35f,.5f),0xff376661,0);
    body_box(v3(-2.05f,6.0f+bob,0),v3(.48f,1.45f,.5f),0xffbd9271,-stride);
    body_box(v3(2.05f,6.0f+bob,0),v3(.48f,1.45f,.5f),0xffbd9271,stride);
    body_box(v3(0,8.75f+bob,.05f),v3(1.05f,1.1f,1),0xffcaa17c,0);
    body_box(v3(0,9.5f+bob,-.18f),v3(1.13f,.38f,1.05f),0xff493d32,0);
    // Broad, octagonal sun hat: an unmistakable little holiday explorer.
    for(int i=0;i<8;i++){
        float a=i*6.2831853f/8,b=(i+1)*6.2831853f/8;
        Vec3 p=body_point(v3(fsin(a)*2,9.65f+bob,fcos(a)*2));
        Vec3 q=body_point(v3(fsin(b)*2,9.65f+bob,fcos(b)*2));
        world_triangle(&solid,body_point(v3(0,9.8f+bob,0)),p,q,0xfff5dfa0);
        Vec3 r=body_point(v3(fsin(a)*1.1f,10.7f+bob,fcos(a)*1.1f));
        Vec3 s=body_point(v3(fsin(b)*1.1f,10.7f+bob,fcos(b)*1.1f));
        world_triangle(&solid,p,q,r,0xffd7ba76);world_triangle(&solid,r,q,s,0xffd7ba76);
        world_triangle(&solid,body_point(v3(0,10.7f+bob,0)),r,s,0xfff5dfa0);
    }
}
static bool wildlife_visible(Vec3 position,float radius){
    Vec3 d=sub(position,game.camera.position);
    float z=dot(d,game.camera.forward);
    return dot(d,d)<1000*1000&&z+radius>NEAR&&
        fabsf(dot(d,game.camera.right))<z*.68f+radius*1.22f&&
        fabsf(dot(d,game.camera.up))<z*.52f+radius*1.14f;
}
static Vec3 dolphin_point(const Dolphin *d,Vec3 local){
    if(local.z<-6)local.y+=d->tail*(-local.z-6)/6;
    return add(d->position,mul(add(mul(d->right,local.x),
        add(mul(d->up,local.y),mul(d->forward,local.z))),d->scale));
}
static void dolphin_fin(const Dolphin *d,Vec3 a,Vec3 b,Vec3 c){
    /* A thin triangular wedge catches the sun from either side. */
    Vec3 normal=unit(cross(sub(b,a),sub(c,a)));
    Vec3 ridge=add(mul(add(a,c),.5f),mul(normal,.22f));
    world_triangle(&solid,dolphin_point(d,a),dolphin_point(d,b),dolphin_point(d,ridge),0xff4d7e90);
    world_triangle(&solid,dolphin_point(d,ridge),dolphin_point(d,b),dolphin_point(d,c),0xff315e75);
    world_triangle(&solid,dolphin_point(d,a),dolphin_point(d,c),dolphin_point(d,b),0xff8badb5);
}
static void draw_dolphins(void){
    /* Original, untextured mesh: tapered body, beak, dorsal fin, two flippers
       and horizontal tail flukes. Shared ring points are transformed once. */
    static const float rings[10][4]={
        {-9,.4f,.55f,-.3f},{-6,.85f,1.25f,-.1f},{-2,2.2f,2.5f,0},
        {2,2.35f,2.6f,0},{5.8f,1.7f,1.9f,-.05f},{7,1.6f,1.6f,-.05f},
        {7.7f,1.1f,1.1f,-.2f},{7.9f,.65f,.38f,-.9f},
        {10.8f,.42f,.3f,-.9f},{11.4f,.02f,.05f,-.9f}
    };
    static const float radial[8][2]={
        {0,1},{.7071f,.7071f},{1,0},{.7071f,-.7071f},
        {0,-1},{-.7071f,-.7071f},{-1,0},{-.7071f,.7071f}
    };
    static const uint32_t colors[8]={0xff345b73,0xff527e92,0xff86aeb7,0xffc3d9d7,
        0xffe2e9db,0xffb0cccf,0xff739aa7,0xff456e85};
    for(unsigned i=0;i<wildlife.count;i++){
        const Dolphin *d=&wildlife.dolphins[i];
        if(!wildlife_visible(d->position,16))continue;
        Vertex body[10][8];
        for(unsigned r=0;r<10;r++)for(unsigned s=0;s<8;s++){
            Vec3 local=v3(radial[s][0]*rings[r][1],radial[s][1]*rings[r][2]+rings[r][3],rings[r][0]);
            body[r][s]=view((Vertex){dolphin_point(d,local),0,0,colors[s]});
        }
        for(unsigned r=0;r<9;r++)for(unsigned s=0;s<8;s++){
            unsigned n=(s+1)&7;
            clip_triangle(&solid,body[r][s],body[r+1][s],body[r][n],false);
            clip_triangle(&solid,body[r][n],body[r+1][s],body[r+1][n],false);
        }
        dolphin_fin(d,v3(0,2.2f,.2f),v3(0,6,-3.5f),v3(0,1.6f,-4.5f));
        for(int side=-1;side<=1;side+=2){
            dolphin_fin(d,v3(side*1.8f,-.6f,2.4f),v3(side*5.3f,-2,-2.8f),v3(side*1.7f,-1.4f,-2.2f));
            dolphin_fin(d,v3(0,-.2f,-8.7f),v3(side*5.3f,0,-11.6f),v3(side*1.5f,-.35f,-12.7f));
            dolphin_fin(d,v3(0,-.2f,-8.7f),v3(side*1.5f,-.35f,-12.7f),v3(0,-.3f,-11.7f));
            world_triangle(&solid,dolphin_point(d,v3(side*1.57f,.3f,6.0f)),
                dolphin_point(d,v3(side*1.47f,.64f,6.25f)),dolphin_point(d,v3(side*1.40f,.23f,6.5f)),0xff163340);
        }
    }
}
static void draw_dolphin_splashes(void){
    for(unsigned i=0;i<wildlife.splash_count;i++){
        const DolphinSplash *s=&wildlife.splashes[i];
        if(!wildlife_visible(s->position,23))continue;
        float life=1-s->age/DOLPHIN_SPLASH_LIFE;
        float radius=2+s->age*10,width=(.7f+life)*s->strength;
        uint32_t alpha=(uint32_t)(life*life*170*s->strength)<<24;
        Vec3 center=add(s->position,v3(0,.45f,0));
        for(unsigned j=0;j<12;j++){
            float sa,ca,sb,cb;render_sincos(j*6.2831853f/12,&sa,&ca);
            render_sincos((j+1)*6.2831853f/12,&sb,&cb);
            Vertex a={add(center,v3(sa*radius,0,ca*radius)),0,0,alpha|0xe3fff4};
            Vertex b={add(center,v3(sb*radius,0,cb*radius)),0,0,alpha|0xe3fff4};
            Vertex c={add(center,v3(sa*(radius+width),0,ca*(radius+width))),0,0,0x00e3fff4};
            Vertex d={add(center,v3(sb*(radius+width),0,cb*(radius+width))),0,0,0x00e3fff4};
            world_quad(&translucent,a,b,c,d);
        }
        /* Six ballistic droplets. Camera-facing triangles cost no texture RAM. */
        if(s->age<.8f)for(unsigned j=0;j<6;j++){
            float angle=j*6.2831853f/6,spread=s->age*(6+j%3);
            float y=s->age*(19*s->strength+j%2*3)-22*s->age*s->age;
            if(y<0)continue;
            Vec3 p=add(center,v3(fsin(angle)*spread,y,fcos(angle)*spread));
            Vec3 right=mul(game.camera.right,.35f+life*.32f),up=mul(game.camera.up,.85f);
            world_triangle(&translucent,sub(p,right),add(p,right),add(p,up),alpha|0xf1fff9);
        }
    }
}
static void draw_shadow(void){
    float floor=world_floor(&game.world,game.player.position,1);
    if(floor<game.player.position.y-20)return;
    Vec3 center=v3(game.player.position.x,floor+.08f,game.player.position.z);
    float radius=3.2f+clampf((game.player.position.y-floor)*.06f,0,1.5f);
    for(int i=0;i<16;i++){
        float a=i*6.2831853f/16,b=(i+1)*6.2831853f/16;
        world_triangle(&shadow_header,center,add(center,v3(fsin(a)*radius,0,fcos(a)*radius*.65f)),add(center,v3(fsin(b)*radius,0,fcos(b)*radius*.65f)),0x36303d39);
    }
}
static void draw_foam(void){
    const uint8_t *data=game.act?foam1_start:foam0_start;
    unsigned count=*(const uint32_t *)data;
    const float *segments=(const float *)(data+4);
    for(unsigned i=0;i<count;i++){
        const float *s=segments+i*4;
        float x=(s[0]+s[2])*.5f,z=(s[1]+s[3])*.5f;
        Vec3 delta=sub(v3(x,0,z),game.camera.position);
        if(dot(delta,delta)>850*850||dot(delta,game.camera.forward)<-25)continue;
        float dx=s[2]-s[0],dz=s[3]-s[1],n=sqrtf(dx*dx+dz*dz);
        if(n<.01f)continue;
        float pulse=.5f+.5f*fsin(game.time*.85f+x*.007f+z*.004f);
        float width=.5f+pulse*1.6f;
        dx*=width/n;dz*=width/n;
        uint32_t alpha=(uint32_t)(32+55*pulse)<<24;
        Vertex a={v3(s[0]-dz,.28f,s[1]+dx),0,0,alpha|0xf0fff6};
        Vertex b={v3(s[2]-dz,.28f,s[3]+dx),0,0,alpha|0xf0fff6};
        Vertex c={v3(s[0]+dz,.28f,s[1]-dx),0,0,0x00f0fff6};
        Vertex d={v3(s[2]+dz,.28f,s[3]-dx),0,0,0x00f0fff6};
        world_quad(&translucent,a,b,c,d);
    }
}
static void draw_hud(void){
    if(game.postcard&&!game.paused&&!game.travel)return;
    rect(&overlay,22,22,3,39,0xcafbf1d4);
    text(34,18,22,"ISLAND EXPLORER",0xfffcf7e7,1.3f);
    text(35,43,13,"EMERALD COAST",0xddffffff,1.8f);
    if(game.arrival>0&&!game.paused&&!game.travel){
        float a=clampf(game.arrival,0,1);
        uint32_t alpha=(uint32_t)(a*255)<<24;
        text(26,371,26,game.location,alpha|0xfff4db,0);
        text(28,400,14,"Take your time. Follow the shoreline.",alpha|0xffffff,0);
    }
    if(game.time<18||game.paused){
        rect(&overlay,20,443,600,23,0x76324748);
        text(29,444,13,"STICK Walk   A Jump   B Jog   L/R Look   Y Travel",0xfff7f4e8,0);
    }
    if(game.muted)text(548,23,12,"MUTED",0xddffffff,0);
    if(game.debug){char line[96];snprintf(line,sizeof(line),"%.0f FPS  %u tris  %.0f %.0f %.0f",game.fps,game.triangles,(double)game.player.position.x,(double)game.player.position.y,(double)game.player.position.z);text(24,420,12,line,0xfffaffb3,0);}
    if(game.paused||game.travel){
        rect(&overlay,0,0,640,480,0xa216353c);
        text(68,64,15,"ISLAND EXPLORER / FIELD NOTES",0xffb9d9cc,1);
        text(65,92,36,game.travel?"Where to wander?":"A moment by the sea",0xffffefce,0);
        rect(&overlay,68,143,502,1,0x65c2e4dc);
        if(game.travel){
            for(unsigned i=0;i<destination_count;i++){
                float y=160+i*30;
                if((int)i==game.travel_selection)rect(&overlay,63,y-1,510,29,0x954c807e);
                text(76,y,18,destinations[i].name,i==(unsigned)game.travel_selection?0xffffedc4:0xffd2e3df,0);
            }
            text(69,420,14,"UP/DOWN Choose     A Visit     B Back",0xffc0dad3,0);
        }else{
            text(70,177,19,"START  Continue exploring",0xfff8efdc,0);
            text(70,216,19,"Y      Open the coastal travel journal",0xffe1ece6,0);
            text(70,255,19,"X      Toggle postcard view",0xffe1ece6,0);
            text(70,294,19,"B      Toggle ocean sound",0xffe1ece6,0);
            text(70,367,14,"No timer. No score. Just a little time away.",0xffb7d5cd,0);
        }
    }
    if(game.fade>0)rect(&overlay,0,0,640,480,((uint32_t)(clampf(game.fade,0,1)*255)<<24)|0xcde5df);
}
void renderer_draw(void){
#ifdef IE_BENCHMARK
    uint64_t begin=timer_us_gettime64();
#endif
    gather_visible();game.triangles=0;
    dolphins_sample(game.act,game.time,&wildlife);
#ifdef IE_BENCHMARK
    uint64_t wait_begin=timer_us_gettime64();
#endif
    pvr_wait_ready();
#ifdef IE_BENCHMARK
    uint64_t waited=timer_us_gettime64()-wait_begin;
#endif
    render_load_camera(&game.camera);
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);active_header=NULL;
    draw_sky();draw_water();draw_player();draw_dolphins();draw_stage(PVR_LIST_OP_POLY);pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);active_header=NULL;
    draw_stage(PVR_LIST_TR_POLY);draw_foam();draw_shadow();draw_dolphin_splashes();draw_hud();pvr_list_finish();pvr_scene_finish();
    drew_frame=true;
#ifdef IE_BENCHMARK
    renderer_cpu_us=timer_us_gettime64()-begin-waited;
#endif
}
void renderer_shutdown(void){
    if(!initialized)return;
    if(drew_frame)pvr_wait_ready();
    free(mesh_headers);free(camera_vertices);free(projected);
    for(int i=0;i<TEXTURE_COUNT;i++)if(textures[i])pvr_mem_free(textures[i]);
    if(font_texture)pvr_mem_free(font_texture);
    pvr_shutdown();
}
