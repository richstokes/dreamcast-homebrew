/* The comic-book look (cel.h). */
#include "cel.h"
#define KL_MODELS_DATA
#include "models.h"

const V3 cel_sun = {0.34f, 0.74f, -0.58f};
FlatMesh cel_flat[FM_COUNT];

static pvr_poly_hdr_t ink_hdr __attribute__((aligned(32)));
static const pvr_poly_hdr_t *h_flat, *h_ramp;
static const uint32_t ink_col[1] = {INK_COL};

void cel_init(void) {
    pvr_poly_cxt_t c;
    pvr_poly_cxt_col(&c, PVR_LIST_OP_POLY);
    c.gen.culling = PVR_CULLING_CW;
    c.gen.clip_mode = PVR_USERCLIP_INSIDE;
    c.gen.fog_type = PVR_FOG_TABLE;
    pvr_poly_compile(&ink_hdr, &c);
    h_flat = r_header(TEX_NONE, HF_OP);
    h_ramp = r_header(TEX_RAMP, HF_OP | HF_CLAMP);
    static const int16_t km[FM_COUNT][2] = {
        {KM_CAR, -1}, {KM_BUS, -1}, {KM_TANK, KM_TANK_INK}, {KM_TURRET, -1}, {KM_HELI, KM_HELI_INK}, {KM_ROTOR, -1},
        {KM_JET, KM_JET_INK}, {KM_CUBE, -1}, {KM_DONUT, KM_DONUT_INK}, {KM_WTANK, KM_WTANK_INK}, {KM_MISSILE, -1}};
    for(int i = 0; i < FM_COUNT; i++)
        if(!flat_mesh_init(&cel_flat[i], km[i][0], km[i][1])) printf("KL cel: no memory for model %d\n", i);
}

const pvr_poly_hdr_t *cel_ink_hdr(void) { return &ink_hdr; }
const pvr_poly_hdr_t *cel_flat_hdr(void) { return h_flat; }

uint32_t cel_band(uint32_t tint, float d) {
    if(d > 0.5f) return tint;
    int r = (tint >> 16) & 255, g = (tint >> 8) & 255, b = tint & 255;
    if(d > 0.05f) return (tint & 0xff000000u) | ((uint32_t)(r * 222 >> 8) << 16) | ((uint32_t)(g * 220 >> 8) << 8) | (uint32_t)(b * 228 >> 8);
    return (tint & 0xff000000u) | ((uint32_t)(r * 150 >> 8) << 16) | ((uint32_t)(g * 146 >> 8) << 8) | (uint32_t)(b * 196 >> 8);
}

bool cel_mesh_init(CelMesh *c, int km, int km_ink) {
    const kl_mesh_desc_t *d = &kl_meshes[km], *di = &kl_meshes[km_ink];
    c->src = kl_mverts + d->v0;
    c->idx = kl_midx + d->i0;
    c->nv = d->nv;
    c->ni = d->ni;
    c->v = memalign(32, sizeof(RV) * (size_t)c->nv);
    if(!c->v) return false;
    for(int i = 0; i < c->nv; i++) c->v[i] = (RV){c->src[i].x, c->src[i].y, c->src[i].z, 0.9f, 0.5f, c->src[i].argb};
    return eng_mesh_build(&c->ink, kl_mverts + di->v0, di->nv, kl_midx + di->i0, di->ni, NULL);
}

void cel_mesh_draw(const CelMesh *c, const Xf *xf, uint32_t plate, float flash) {
    float m[16] __attribute__((aligned(32)));
    xf_m4(xf, m);
    V3 l = vscale(xf_local(xf, cel_sun), 0.5f / 127.0f);
    RV *v = c->v;
    const eng_mvert_t *s = c->src;
    for(int i = 0; i < c->nv; i++, v++, s++) {
        float u = 0.5f + s->nx * l.x + s->ny * l.y + s->nz * l.z;
        v->u = u < 0.04f ? 0.04f : u > 0.96f ? 0.96f : u;
        uint32_t col = s->paint == 2 ? plate : s->argb;
        v->c = flash > 0.01f ? color_lerp(col, 0xffffffffu, flash) : col;
    }
    r_hdr(h_ramp);
    r_load_model(m);
    r_mesh(c->v, c->nv, c->idx, c->ni, true);
    r_hdr(h_flat);
    r_mesh_kv(&c->ink.km, ink_col);
}

bool flat_mesh_init(FlatMesh *f, int km, int km_ink) {
    const kl_mesh_desc_t *d = &kl_meshes[km];
    if(!eng_mesh_build(&f->m, kl_mverts + d->v0, d->nv, kl_midx + d->i0, d->ni, NULL)) return false;
    f->has_ink = km_ink >= 0;
    if(f->has_ink) {
        d = &kl_meshes[km_ink];
        if(!eng_mesh_build(&f->ink, kl_mverts + d->v0, d->nv, kl_midx + d->i0, d->ni, NULL)) return false;
    }
    return true;
}

#define FLAT_GROUPS 512
void flat_mesh_draw_m(const FlatMesh *f, const float *m, V3 l, uint32_t own, bool ink) {
    static uint32_t col[FLAT_GROUPS];
    const EngMesh *e = &f->m;
    int n = e->ngroups < FLAT_GROUPS ? e->ngroups : FLAT_GROUPS;
    for(int g = 0; g < n; g++) {
        float d = e->gnrm[g][0] * l.x + e->gnrm[g][1] * l.y + e->gnrm[g][2] * l.z;
        col[g] = cel_band(e->gpaint[g] == 1 ? own : e->gargb[g], d);
    }
    r_hdr(h_flat);
    r_load_model(m);
    r_mesh_kv(&e->km, col);
    if(ink && f->has_ink) r_mesh_kv(&f->ink.km, ink_col);
}

void flat_mesh_draw(const FlatMesh *f, const Xf *xf, uint32_t own, bool ink) {
    float m[16] __attribute__((aligned(32)));
    xf_m4(xf, m);
    flat_mesh_draw_m(f, m, xf_local(xf, cel_sun), own, ink);
}
