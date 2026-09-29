#include "engine.h"
#include <string.h>
#include <float.h>

_Static_assert(sizeof(Vertex)==24,"Asset vertex layout");
_Static_assert(sizeof(Mesh)==48,"Asset mesh layout");
bool world_load(World *w,const uint8_t *data,size_t size){
    if(size<16||memcmp(data,"IEW1",4))return false;
    const uint32_t *h=(const uint32_t *)data;
    uint64_t needed=16ull+h[1]*48ull+h[2]*24ull+h[3]*2ull;
    if(needed!=size||h[1]>20000||h[2]>500000||h[3]>1000000)return false;
    *w=(World){h[1],h[2],h[3],(const Mesh *)(data+16),(const Vertex *)(data+16+h[1]*48),
        (const uint16_t *)(data+16+h[1]*48+h[2]*24)};
    for(unsigned i=0;i<w->mesh_count;i++){
        const Mesh *m=&w->meshes[i];
        if(m->first_vertex+m->vertex_count>w->vertex_count||m->first_index+m->index_count>w->index_count||m->index_count%3)return false;
        for(unsigned j=0;j<m->index_count;j++)if(w->indices[m->first_index+j]>=m->vertex_count)return false;
    }
    return true;
}
float world_water_level(Vec3 p){
    if(game.act==1&&p.x>=-630&&p.x<=1098&&p.z>=-2160&&p.z<=-688&&p.y>523)return 539.0f;
    return -.2f;
}
static bool close_xz(const Mesh *m,Vec3 p,float radius){
    float x=m->center.x-p.x,z=m->center.z-p.z,r=m->radius+radius;
    return x*x+z*z<r*r;
}
static Vec3 vertex(const World *w,const Mesh *m,unsigned i){return w->vertices[m->first_vertex+w->indices[m->first_index+i]].p;}
float world_floor(const World *w,Vec3 p,float rise){
    float best=-FLT_MAX;
    for(unsigned im=0;im<w->mesh_count;im++){
        const Mesh *m=&w->meshes[im];
        if(!(m->surface&1)||!close_xz(m,p,4)||m->center.y-m->radius>p.y+rise)continue;
        for(unsigned i=0;i<m->index_count;i+=3){
            Vec3 a=vertex(w,m,i),b=vertex(w,m,i+1),c=vertex(w,m,i+2);
            float den=(b.z-c.z)*(a.x-c.x)+(c.x-b.x)*(a.z-c.z);
            if(fabsf(den)<0.0001f)continue;
            float u=((b.z-c.z)*(p.x-c.x)+(c.x-b.x)*(p.z-c.z))/den;
            float v=((c.z-a.z)*(p.x-c.x)+(a.x-c.x)*(p.z-c.z))/den;
            if(u<-.0002f||v<-.0002f||u+v>1.0002f)continue;
            Vec3 n=cross(sub(b,a),sub(c,a));
            if(n.y*n.y<dot(n,n)*0.30f)continue;
            float y=u*a.y+v*b.y+(1-u-v)*c.y;
            if(y<=p.y+rise&&y>best)best=y;
        }
    }
    return best;
}
static Vec3 closest_triangle(Vec3 p,Vec3 a,Vec3 b,Vec3 c){
    Vec3 ab=sub(b,a),ac=sub(c,a),ap=sub(p,a);
    float d1=dot(ab,ap),d2=dot(ac,ap);
    if(d1<=0&&d2<=0)return a;
    Vec3 bp=sub(p,b);float d3=dot(ab,bp),d4=dot(ac,bp);
    if(d3>=0&&d4<=d3)return b;
    float vc=d1*d4-d3*d2;
    if(vc<=0&&d1>=0&&d3<=0)return add(a,mul(ab,d1/(d1-d3)));
    Vec3 cp=sub(p,c);float d5=dot(ab,cp),d6=dot(ac,cp);
    if(d6>=0&&d5<=d6)return c;
    float vb=d5*d2-d1*d6;
    if(vb<=0&&d2>=0&&d6<=0)return add(a,mul(ac,d2/(d2-d6)));
    float va=d3*d6-d5*d4;
    if(va<=0&&d4-d3>=0&&d5-d6>=0)return add(b,mul(sub(c,b),(d4-d3)/(d4-d3+d5-d6)));
    float den=va+vb+vc;
    if(fabsf(den)<1e-9f)return a;
    return add(a,add(mul(ab,vb/den),mul(ac,vc/den)));
}
void world_slide(const World *w,Vec3 *p){
    const float radius=2.2f;
    for(int iteration=0;iteration<2;iteration++){
        for(unsigned im=0;im<w->mesh_count;im++){
            const Mesh *m=&w->meshes[im];
            if(!(m->surface&1)||!close_xz(m,*p,10)||fabsf(m->center.y-p->y)>m->radius+12)continue;
            for(unsigned i=0;i<m->index_count;i+=3){
                Vec3 a=vertex(w,m,i),b=vertex(w,m,i+1),c=vertex(w,m,i+2);
                Vec3 normal=cross(sub(b,a),sub(c,a));
                if(normal.y*normal.y>dot(normal,normal)*0.30f)continue;
                for(int s=0;s<2;s++){
                    Vec3 center=add(*p,v3(0,s?7.1f:3.0f,0));
                    Vec3 q=closest_triangle(center,a,b,c),delta=sub(center,q);
                    float d=length(delta);
                    if(d<radius&&d>0.001f){
                        float k=(radius-d)/d;
                        p->x+=delta.x*k;p->z+=delta.z*k;
                    }
                }
            }
        }
    }
}
float world_raycast(const World *w,Vec3 from,Vec3 to){
    Vec3 dir=sub(to,from);float hit=1;
    Vec3 midpoint=mul(add(from,to),.5f);float radius=length(dir)*.5f;
    for(unsigned im=0;im<w->mesh_count;im++){
        const Mesh *m=&w->meshes[im];
        if(!(m->surface&1)||length(sub(m->center,midpoint))>m->radius+radius)continue;
        for(unsigned i=0;i<m->index_count;i+=3){
            Vec3 a=vertex(w,m,i),e1=sub(vertex(w,m,i+1),a),e2=sub(vertex(w,m,i+2),a);
            Vec3 p=cross(dir,e2);float det=dot(e1,p);
            if(fabsf(det)<1e-5f)continue;
            Vec3 t=sub(from,a);float u=dot(t,p)/det;
            if(u<0||u>1)continue;
            Vec3 q=cross(t,e1);float v=dot(dir,q)/det;
            if(v<0||u+v>1)continue;
            float f=dot(e2,q)/det;
            if(f>0.01f&&f<hit)hit=f;
        }
    }
    return hit;
}
