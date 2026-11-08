/* Ridgeline: drawing the trucks.

   Each model part (body, wheels, axle) at each level of detail is an
   engine mesh, lit once a frame per lighting group with the sun turned
   into the part's own space. The livery repaints the body's paint slots;
   lenses light up with the lights. Bodies too big for the renderer's mesh
   limit are split by triangles into several meshes at boot. */
#include "truckdraw.h"
#include "truck.h"
#include "terrain.h"
#include "world.h"
#include "engine/mesh.h"
#include "engine/prof.h"
#if __has_include("trucks.h")
#include "trucks.h"
#define HAVE_TRUCK_MODELS 1
#endif

#define MAX_PARTS_SPLIT 4
#define LODS 3

typedef struct {
    EngMesh m[MAX_PARTS_SPLIT];
    int n;
} PartMeshes;

#ifdef HAVE_TRUCK_MODELS
static PartMeshes parts[RL_TRUCK_COUNT][RL_PART_COUNT][LODS];
static uint32_t ref_col[RL_TRUCK_COUNT][RL_PAINT_COUNT];
#endif
static uint32_t lit_buf[2048] __attribute__((aligned(32)));
static const pvr_poly_hdr_t *h_model, *h_shadow, *h_glow, *h_flat;

#ifdef HAVE_TRUCK_MODELS
/* Split a mesh's triangles into pieces of at most ~1000 vertices. */
static void build_part(PartMeshes *pm, const rl_mesh_t *src) {
    const rl_mvert_t *v = &rl_truck_verts[src->first_vert];
    const uint16_t *idx = &rl_truck_indices[src->first_index];
    int ntri = src->index_count / 3;
    static uint16_t remap[8192];
    static eng_mvert_t vb[1024];
    static uint16_t ib[6144];
    int t = 0;
    while(t < ntri && pm->n < MAX_PARTS_SPLIT) {
        memset(remap, 0xff, sizeof(uint16_t) * src->vert_count);
        int nv = 0, ni = 0;
        while(t < ntri) {
            int need = 0;
            for(int k = 0; k < 3; k++)
                if(remap[idx[t * 3 + k]] == 0xffff) need++;
            if(nv + need > 1000 || ni + 3 > 6144) break;
            for(int k = 0; k < 3; k++) {
                int o = idx[t * 3 + k];
                if(remap[o] == 0xffff) {
                    remap[o] = (uint16_t)nv;
                    memcpy(&vb[nv], &v[o], sizeof(eng_mvert_t));
                    nv++;
                }
                ib[ni++] = remap[o];
            }
            t++;
        }
        if(!eng_mesh_build(&pm->m[pm->n], vb, nv, ib, ni, NULL)) break;
        pm->n++;
    }
}
#endif

void truckdraw_init(void) {
    h_model = r_header(TEX_NONE, HF_OP);
    h_flat = h_model;
    h_shadow = r_header(TEX_SHADOW, HF_TR);
    h_glow = r_header(TEX_GLOW, HF_TR | HF_ADD);
#ifdef HAVE_TRUCK_MODELS
    _Static_assert(sizeof(rl_mvert_t) == sizeof(eng_mvert_t), "model vertex layout");
    int groups = 0, meshes = 0;
    for(int i = 0; i < RL_TRUCK_MESH_COUNT; i++) {
        const rl_mesh_t *m = &rl_truck_meshes[i];
        if(m->lod >= LODS) continue;
        PartMeshes *pm = &parts[m->truck][m->part][m->lod];
        build_part(pm, m);
        for(int k = 0; k < pm->n; k++) groups += pm->m[k].ngroups;
        meshes += pm->n;
    }
    for(int tr = 0; tr < RL_TRUCK_COUNT; tr++)
        for(int s = 0; s < RL_PAINT_COUNT; s++) {
            /* The slot's reference colour: its first vertex's in the body. */
            ref_col[tr][s] = 0;
            for(int i = 0; i < RL_TRUCK_MESH_COUNT && !ref_col[tr][s]; i++) {
                const rl_mesh_t *m = &rl_truck_meshes[i];
                if(m->truck != tr || m->lod != 0) continue;
                for(int v = m->first_vert; v < m->first_vert + m->vert_count; v++)
                    if(rl_truck_verts[v].paint == s) {
                        ref_col[tr][s] = rl_truck_verts[v].argb;
                        break;
                    }
            }
        }
    printf("Ridgeline trucks: %d meshes, %d lighting groups\n", meshes, groups);
#endif
}

/* Column-major model matrix from axes and an origin. */
static void mat_axes(float *m, V3 x, V3 y, V3 z, V3 o) {
    m[0] = x.x; m[1] = x.y; m[2] = x.z; m[3] = 0;
    m[4] = y.x; m[5] = y.y; m[6] = y.z; m[7] = 0;
    m[8] = z.x; m[9] = z.y; m[10] = z.z; m[11] = 0;
    m[12] = o.x; m[13] = o.y; m[14] = o.z; m[15] = 1;
}

#ifdef HAVE_TRUCK_MODELS
/* Light a part's meshes into buf (consecutive group colours per mesh);
   returns the words used. */
static int light_part(const PartMeshes *pm, const EngLight *l, uint32_t *buf) {
    int o = 0;
    for(int k = 0; k < pm->n; k++) {
        const EngMesh *m = &pm->m[k];
        if(o + m->ngroups > 2048) break;
        eng_mesh_light(m, l, buf + o);
        o += m->ngroups;
    }
    return o;
}

static void draw_part(const PartMeshes *pm, const float *mat, const uint32_t *lit) {
    r_load_model(mat);
    r_hdr(h_model);
    int o = 0;
    for(int k = 0; k < pm->n; k++) {
        r_mesh_kv(&pm->m[k].km, lit + o);
        o += pm->m[k].ngroups;
    }
}
#endif

/* A soft shadow on the ground: a 3 x 3 grid following the terrain. */
static void ground_quad(V3 c, V3 ax, V3 az, float lift, uint32_t col) {
    RV v[3][3];
    for(int j = 0; j < 3; j++)
        for(int i = 0; i < 3; i++) {
            V3 p = vadd(c, vadd(vscale(ax, (i - 1.0f)), vscale(az, (j - 1.0f))));
            p.y = ground_at(p.x, p.z, NULL, NULL) + lift;
            v[j][i] = (RV){p.x, p.y, p.z, i * 0.5f, j * 0.5f, col};
        }
    for(int j = 0; j < 2; j++) {
        RV s[6] = {v[j + 1][0], v[j][0], v[j + 1][1], v[j][1], v[j + 1][2], v[j][2]};
        r_strip(s, 6);
    }
}

void truckdraw_draw_op(void) {
    PROF_BEGIN(pt);
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        V3 d = vsub(t->pos, r_cam.pos);
        float dist = vlen(d);
        if(!r_sphere_visible(t->pos, 3.5f)) continue;
        /* Full detail for the player's truck and anything very close. */
        int lod = (ti == 0 && dist < 18.0f) || dist < 9.0f ? 0 : dist < 50.0f ? 1 : 2;
        V3 R = t->ax[0], U = t->ax[1], F = t->ax[2];
        float mat[16];
        V3 origin = vsub(vsub(t->pos, vscale(U, t->spec.cog_y)), vscale(F, t->spec.cog_z));
        mat_axes(mat, R, U, F, origin);
#ifdef HAVE_TRUCK_MODELS
        EngLight l;
        l.L = v3(vdot(sun_dir, R), vdot(sun_dir, U), vdot(sun_dir, F));
        for(int i = 0; i < 3; i++) {
            l.sun[i] = pal->sun_k[i] * 1.1f;
            l.sky[i] = pal->sky_k[i];
            l.ambient[i] = pal->amb_k[i] * 1.2f;
        }
        uint32_t paint[RL_PAINT_COUNT] = {0};
        const rl_truck_info_t *info = &rl_truck_info[t->model];
        int lv = t->livery % RL_LIVERIES;
        /* Livery 0 is the model's own colours (the stripes' three tones). */
        if(lv) {
            paint[RL_PAINT_BODY] = info->livery[lv][0];
            paint[RL_PAINT_ACCENT] = info->livery[lv][1];
            paint[RL_PAINT_TRIM] = info->livery[lv][2];
        }
        uint32_t unlit = 0;
        if(t->lights) {
            paint[RL_PAINT_HEADLIGHT] = 0xfffff4d8;
            paint[RL_PAINT_AUX] = 0xfffff8e8;
            paint[RL_PAINT_TAILLIGHT] = t->brake > 0.2f ? 0xffff3020 : 0xffd81818;
            unlit = (1u << RL_PAINT_HEADLIGHT) | (1u << RL_PAINT_AUX) | (1u << RL_PAINT_TAILLIGHT);
        }
        else if(t->brake > 0.2f) {
            paint[RL_PAINT_TAILLIGHT] = 0xffff3020;
            unlit = 1u << RL_PAINT_TAILLIGHT;
        }
        l.paint = paint;
        l.ref = ref_col[t->model];
        l.unlit_mask = unlit;
        int used = light_part(&parts[t->model][RL_PART_BODY][lod], &l, lit_buf);
        draw_part(&parts[t->model][RL_PART_BODY][lod], mat, lit_buf);
        /* Wheels: steered, spinning, where the suspension puts them. Lit
           once for all four with the body's sun: a tyre is dark and round,
           and nobody sees its light turn with the spin. */
        EngLight lw = l;
        lw.unlit_mask = 0;
        uint32_t *lit_l = lit_buf + used;
        int used_l = light_part(&parts[t->model][RL_PART_WHEEL_L][lod], &lw, lit_l);
        uint32_t *lit_r = lit_l + used_l;
        int used_r = light_part(&parts[t->model][RL_PART_WHEEL_R][lod], &lw, lit_r);
        uint32_t *lit_a = lit_r + used_r;
        light_part(&parts[t->model][RL_PART_AXLE][lod], &lw, lit_a);
        for(int i = 0; i < 4; i++) {
            const Wheel *w = &t->wh[i];
            float cs = cosf(w->steer), sn = sinf(w->steer);
            V3 f1 = vadd(vscale(F, cs), vscale(R, sn)), r1 = vsub(vscale(R, cs), vscale(F, sn));
            float cp = cosf(w->spin), sp = sinf(w->spin);
            V3 u2 = vadd(vscale(U, cp), vscale(f1, sp)), f2 = vsub(vscale(f1, cp), vscale(U, sp));
            mat_axes(mat, r1, u2, f2, w->centre);
            draw_part(&parts[t->model][(i & 1) ? RL_PART_WHEEL_R : RL_PART_WHEEL_L][lod], mat, (i & 1) ? lit_r : lit_l);
        }
        /* Axles: a beam between each pair of wheel centres. */
        if(lod < 2) {
            for(int a = (t->model == 0 ? 0 : 1); a < 2; a++) {
                V3 cl = t->wh[a * 2].centre, cr = t->wh[a * 2 + 1].centre;
                V3 x = vsub(cr, cl);
                V3 xn = vnorm(x);
                V3 z = vnorm(vcross(xn, U));
                V3 y = vcross(z, xn);
                mat_axes(mat, x, y, z, vscale(vadd(cl, cr), 0.5f));
                draw_part(&parts[t->model][RL_PART_AXLE][lod], mat, lit_a);
            }
        }
#else
        /* No models yet: a box for the body. */
        (void)lod;
        r_hdr(h_flat);
        const TruckSpec *s = &t->spec;
        V3 c[8];
        for(int k = 0; k < 8; k++)
            c[k] = truck_to_world(t, v3((k & 1) ? s->body_hi[0] : s->body_lo[0], (k & 2) ? s->body_hi[1] : s->body_lo[1],
                                        (k & 4) ? s->body_hi[2] : s->body_lo[2]));
        static const int faces[6][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {0, 1, 4, 5}, {2, 3, 6, 7}, {0, 2, 4, 6}, {1, 3, 5, 7}};
        for(int f = 0; f < 6; f++) {
            uint32_t col = color_scale(t->model ? 0xffe0601c : 0xffc8202a, 0.6f + 0.08f * f);
            RV q[4];
            for(int k = 0; k < 4; k++) q[k] = (RV){c[faces[f][k]].x, c[faces[f][k]].y, c[faces[f][k]].z, 0, 0, col};
            r_strip(q, 4);
        }
        for(int i = 0; i < 4; i++) {
            V3 wc = t->wh[i].centre;
            r_billboard(wc, s->radius, s->radius, 0, 0, 0, 1, 1, 0xff202020);
        }
#endif
    }
    PROF_LAP(PS_TRUCKS, pt);
}

void truckdraw_draw_tr(void) {
    for(int ti = 0; ti < game.ntrucks; ti++) {
        Truck *t = &trucks[ti];
        if(vlen(vsub(t->pos, r_cam.pos)) > 120.0f) continue;
        const TruckSpec *s = &t->spec;
        V3 F = t->ax[2];
        V3 fh = vnorm(v3(F.x, 0, F.z)), rh = v3(fh.z, 0, -fh.x);
        /* The shadow, darker the closer the body sits to the ground. */
        V3 c = t->pos;
        float g = ground_at(c.x, c.z, NULL, NULL);
        float k = clampf(1.0f - (c.y - g - s->cog_y) / 3.0f, 0, 1);
        if(k > 0) {
            r_hdr(h_shadow);
            float hx = (s->body_hi[0] - s->body_lo[0]) * 0.62f, hz = (s->body_hi[2] - s->body_lo[2]) * 0.56f;
            V3 sc = vadd(v3(c.x, 0, c.z), vscale(v3(-sun_dir.x, 0, -sun_dir.z), 0.6f));
            ground_quad(sc, vscale(rh, hx), vscale(fh, hz), 0.06f, color_alpha(0xff000000, 0.55f * k));
        }
        if(!t->lights) continue;
        r_hdr(h_glow);
#ifdef HAVE_TRUCK_MODELS
        const rl_truck_info_t *info = &rl_truck_info[t->model];
        V3 to_cam = vnorm(vsub(r_cam.pos, t->pos));
        float facing = clampf(vdot(to_cam, F), 0, 1);
        for(int i = 0; i < 2; i++) {
            V3 p = truck_to_world(t, v3(info->headlight[i][0], info->headlight[i][1], info->headlight[i][2]));
            /* Brighter and bigger in the dark. */
            float sz = info->headlight_r * (2.5f + 6.0f * facing * facing) * (1.0f + 0.5f * pal->dark);
            r_billboard(vmad(p, F, 0.05f), sz, sz, 0, 0, 0, 1, 1,
                        color_alpha(0xfffff0c8, (0.5f + 0.5f * facing) * (1.0f + 0.6f * pal->dark)));
            V3 tl = truck_to_world(t, v3(info->taillight[i][0], info->taillight[i][1], info->taillight[i][2]));
            r_billboard(tl, 0.35f, 0.35f, 0, 0, 0, 1, 1, color_alpha(0xffff2010, 0.6f));
        }
        for(int i = 0; i < info->n_aux && i < 4; i++) {
            V3 p = truck_to_world(t, v3(info->aux_light[i][0], info->aux_light[i][1], info->aux_light[i][2]));
            float sz = 0.25f + 0.9f * facing * facing;
            r_billboard(vmad(p, F, 0.05f), sz, sz, 0, 0, 0, 1, 1, color_alpha(0xffffffff, 0.4f + 0.5f * facing));
        }
#endif
        /* The pool of light on the ground ahead. */
        V3 ahead = vmad(v3(c.x, 0, c.z), fh, 9.0f);
        ground_quad(ahead, vscale(rh, 4.5f), vscale(fh, 6.5f), 0.08f, color_alpha(0xffffe8b0, 0.28f + 0.45f * pal->dark));
    }
}
