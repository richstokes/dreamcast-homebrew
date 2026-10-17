/* Kart and Chao rendering from the generated rigid-part model.

   Each part gets its own matrix (kart pose * pivot * part rotation). The sun
   is rotated into part space once, so per-vertex lighting is one dot
   product. Rival karts recolour paint slots: slot 1 is the body paint, slots
   3/4 the Chao's body and tip colours, blended per vertex. */
#include "models.h"
#include "render.h"
#include "world.h"
#include "models_data.h"
#include "texture_assets.h"

static uint32_t paint[MAX_KARTS][DK_MODEL_VERT_COUNT];
/* Per-kart vertex buffers: positions are fixed, lit colours are refreshed
   once per frame and shared by both split-screen views. */
static RV kart_rv[MAX_KARTS][DK_MODEL_VERT_COUNT] __attribute__((aligned(32)));
static uint32_t lit_frame[MAX_KARTS][DK_MODEL_MESH_COUNT];
static uint32_t frame_id = 1;
/* Kart part matrices, computed once per frame and shared by both views. */
static float part_mats[MAX_KARTS][DK_PART_COUNT][16];
static V3 part_light[MAX_KARTS][DK_PART_COUNT];
static uint32_t mats_frame[MAX_KARTS];
static const pvr_poly_hdr_t *h_model, *h_shadow;
static float ref_luma1 = 1.0f;
static int far_lod = 1;   /* lod 3 when the model has a far level */
static float normals[DK_MODEL_VERT_COUNT][3];
static V3 light_dir;

static float luma(uint32_t c) {
    return (0.30f * ((c >> 16) & 255) + 0.59f * ((c >> 8) & 255) + 0.11f * (c & 255)) / 255.0f;
}

void models_init(void) {
    h_model = r_header(TEX_NONE, HF_OP);
    h_shadow = r_header(TEX_SHADOW, HF_TR);
    /* Reference brightness of the default body paint. */
    float sum = 0;
    int n = 0;
    for(int i = 0; i < DK_MODEL_VERT_COUNT; i++)
        if(dk_model_verts[i].paint == 1) {
            sum += luma(dk_model_verts[i].argb);
            n++;
        }
    if(n) ref_luma1 = fmaxf(sum / n, 0.05f);
    for(int k = 0; k < MAX_KARTS; k++)
        for(int i = 0; i < DK_MODEL_VERT_COUNT; i++)
            kart_rv[k][i] = (RV){dk_model_verts[i].x, dk_model_verts[i].y, dk_model_verts[i].z, 0, 0, 0xffffffff};
    for(int i = 0; i < DK_MODEL_VERT_COUNT; i++) {
        normals[i][0] = dk_model_verts[i].nx * (1.0f / 127.0f);
        normals[i][1] = dk_model_verts[i].ny * (1.0f / 127.0f);
        normals[i][2] = dk_model_verts[i].nz * (1.0f / 127.0f);
    }
    /* Key light from above, slightly in front of the karts' usual view. */
    light_dir = vnorm(v3(0.35f, 0.85f, 0.40f));
    for(int i = 0; i < DK_MODEL_MESH_COUNT; i++)
        if(dk_model_meshes[i].lod == 3) far_lod = 3;
    int meshes_max = 0;
    for(int i = 0; i < DK_MODEL_MESH_COUNT; i++)
        if(dk_model_meshes[i].vert_count > meshes_max) meshes_max = dk_model_meshes[i].vert_count;
    printf("DreamKarts models: %d verts, %d tris, %d meshes (largest %d verts)\n",
           DK_MODEL_VERT_COUNT, DK_MODEL_INDEX_COUNT / 3, DK_MODEL_MESH_COUNT, meshes_max);
}

void models_paint_kart(int index) {
    const Kart *k = &karts[index];
    for(int i = 0; i < DK_MODEL_VERT_COUNT; i++) {
        const dk_mvert_t *v = &dk_model_verts[i];
        uint32_t c = v->argb;
        if(v->paint == 1 && k->paint1) {
            c = color_scale(k->paint1, luma(v->argb) / ref_luma1);
        }
        else if((v->paint == 3 || v->paint == 4) && k->chao) {
            c = color_lerp(k->chao, k->chao_tip, (float)v->tip / 255.0f);
        }
        paint[index][i] = c | 0xff000000u;
    }
}

static void part_matrix(float *out, const float *kart, int part, const Kart *k) {
    float local[16], rot[16];
    m4_identity(local);
    const float *pv = dk_part_pivot[part];
    switch(part) {
        case DK_PART_WHEEL_FL:
        case DK_PART_WHEEL_FR: {
            float steer[16];
            m4_rot_y(steer, k->steer_vis * 0.55f);
            m4_rot_x(rot, -k->wheel_spin);
            m4_mul(local, steer, rot);
            break;
        }
        case DK_PART_WHEEL_RL:
        case DK_PART_WHEEL_RR:
            m4_rot_x(local, -k->wheel_spin);
            break;
        case DK_PART_STEER:
            m4_rot_axis(local, v3(dk_steer_axis[0], dk_steer_axis[1], dk_steer_axis[2]), -k->steer_vis * 1.4f);
            break;
        case DK_PART_CHAO_BODY: {
            m4_rot_z(local, -k->head_tilt * 0.35f);
            local[13] += 0.02f * sinf(k->emote_bob * 2.0f);
            break;
        }
        case DK_PART_CHAO_HEAD: {
            float nod[16];
            m4_rot_z(local, -k->head_tilt);
            m4_rot_x(nod, 0.06f * sinf(k->emote_bob * 1.3f) - k->pitch * 1.5f);
            m4_mul(local, local, nod);
            break;
        }
        case DK_PART_EMOTE:
            m4_rot_y(local, k->emote_bob * 0.8f);
            local[13] += 0.05f * sinf(k->emote_bob * 2.2f);
            break;
        default:
            break;
    }
    local[12] += pv[0];
    local[13] += pv[1];
    local[14] += pv[2];
    m4_mul(out, kart, local);
}

static void kart_matrix(float *m, const Kart *k) {
    float sq = k->squash;
    V3 p = k->pos;
    m4_translate_rotate(m, p, k->heading + k->vis_yaw, k->pitch, k->roll);
    m4_scale_apply(m, 1.0f - sq * 0.45f, 1.0f + sq, 1.0f - sq * 0.45f);
}

static int emote_variant(const Kart *k) {
    if(k->finished && k->place == 1) return 3;
    if(k->boost_t > 0) return 2;
    return 1;
}

static void update_kart_mats(int index) {
    if(mats_frame[index] == frame_id) return;
    mats_frame[index] = frame_id;
    const Kart *k = &karts[index];
    float km[16];
    kart_matrix(km, k);
    for(int part = 0; part < DK_PART_COUNT; part++) {
        float *pm = part_mats[index][part];
        part_matrix(pm, km, part, k);
        /* Sun into part space (columns are the part axes). */
        V3 cx = v3(pm[0], pm[1], pm[2]), cy = v3(pm[4], pm[5], pm[6]), cz = v3(pm[8], pm[9], pm[10]);
        part_light[index][part] = v3(vdot(vnorm(cx), light_dir), vdot(vnorm(cy), light_dir), vdot(vnorm(cz), light_dir));
    }
}

/* lod 2 = split-screen "own kart": full-detail body and Chao, the simple
   wheels, and no steering wheel (hidden behind the Chao from the chase
   camera). */
static bool mesh_in_lod(const dk_mesh_t *m, int lod) {
    if(lod != 2) return m->lod == lod;
    switch(m->part) {
        case DK_PART_WHEEL_FL: case DK_PART_WHEEL_FR: case DK_PART_WHEEL_RL: case DK_PART_WHEEL_RR:
            return m->lod == 1;
        case DK_PART_STEER:
            return false;
        default:
            return m->lod == 0;
    }
}

static void draw_kart_lod(int index, int lod) {
    const Kart *k = &karts[index];
    update_kart_mats(index);
    int variant = emote_variant(k);
    int last_part = -1;
    V3 lp = light_dir;
    r_hdr(h_model);
    for(int mi = 0; mi < DK_MODEL_MESH_COUNT; mi++) {
        const dk_mesh_t *m = &dk_model_meshes[mi];
        if(!mesh_in_lod(m, lod)) continue;
        if(m->variant != 0 && m->variant != variant) continue;
        if(m->part != last_part) {
            r_load_model(part_mats[index][m->part]);
            lp = part_light[index][m->part];
            last_part = m->part;
        }
        int nv = m->vert_count;
        RV *out = &kart_rv[index][m->first_vert];
        if(lit_frame[index][mi] != frame_id) {
            lit_frame[index][mi] = frame_id;
            const uint32_t *col = &paint[index][m->first_vert];
            bool unlit = m->flags & 1;
            const float (*nrm)[3] = &normals[m->first_vert];
            /* Lighting in 8.8 fixed point: one float->int per vertex. */
            float lx = lp.x * 158.7f, ly = lp.y * 158.7f + 25.6f, lz = lp.z * 158.7f;
            for(int i = 0; i < nv; i++) {
                uint32_t c = col[i];
                if(unlit) {
                    out[i].c = c;
                    continue;
                }
                float d = nrm[i][0] * lx + nrm[i][1] * ly + nrm[i][2] * lz;
                int li = 133 + (d > 0 ? (int)d : 0);
                unsigned r = (((c >> 16) & 255) * li) >> 8, g = (((c >> 8) & 255) * li) >> 8, b = ((c & 255) * li) >> 8;
                out[i].c = 0xff000000u | ((r > 255 ? 255 : r) << 16) | ((g > 255 ? 255 : g) << 8) | (b > 255 ? 255 : b);
            }
        }
        r_mesh(out, nv, &dk_model_indices[m->first_index], m->index_count, true);
    }
}

void models_draw_kart(int index) {
    const Kart *k = &karts[index];
    V3 c = vadd(k->pos, v3(0, 0.8f, 0));
    if(!r_sphere_visible(c, 1.8f)) return;
    float d = vlen(vsub(c, r_cam.pos));
    draw_kart_lod(index, d > 38.0f ? 1 : 0);
}

/* Kart detail is chosen by distance from each view's camera, with
   hysteresis so karts don't flick between models at a threshold:
     lod 0  full detail
     lod 2  "own kart" in split screen: full body and Chao, simple wheels,
            no steering wheel (hidden behind the Chao from the chase camera)
     lod 1  330-triangle model built to match lod 0 from the chase camera
     lod 3  ~150-triangle far model
   Single view: lod 0 within ~16 m, lod 1 to ~42 m, lod 3 beyond. Split
   screen draws every kart twice: each view's own kart uses lod 2 and every
   other kart lod 3 (measured on hardware: 10 dropped frames a race, versus
   ~140 with lod 1 for nearby rivals). */
static const float lod_in[2][2] = {{16.0f, 42.0f}, {0.0f, 0.0f}};   /* [split][near/mid] */
static const float lod_out[2][2] = {{19.0f, 46.0f}, {0.0f, 0.0f}};
static uint8_t lod_state[MAX_PLAYERS][MAX_KARTS];   /* 0 near, 1 mid, 2 far */

void models_begin_frame(void) {
    frame_id++;
}

void models_draw_karts(int view_kart) {
    int view = 0;
    for(int p = 0; p < game.num_players; p++)
        if(game.pl[p].kart == view_kart) view = p;
    int sp = world_split ? 1 : 0;
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        V3 d = vsub(k->pos, r_cam.pos);
        float dist = sqrtf(vdot(d, d));
        uint8_t *st = &lod_state[view][i];
        /* Step out past the outer edge, step in past the inner edge. */
        while(*st < 2 && dist > lod_out[sp][*st]) (*st)++;
        while(*st > 0 && dist < lod_in[sp][*st - 1]) (*st)--;
        V3 c = vadd(k->pos, v3(0, 0.8f, 0));
        if(!r_sphere_visible(c, 1.8f)) continue;
        int lod = *st == 0 ? 0 : *st == 1 ? 1 : far_lod;
        if(i == view_kart) lod = sp ? 2 : 0;
        draw_kart_lod(i, lod);
    }
}

void models_draw_shadows(void) {
    r_load_vp();
    r_hdr(h_shadow);
    for(int i = 0; i < kart_count; i++) {
        const Kart *k = &karts[i];
        if(!r_sphere_visible(k->pos, 2.0f)) continue;
        /* Cast onto the road below: the floor under a jump's gap is y = 0. */
        float ground = track_height(k->tp.s);
        float h = fmaxf(k->pos.y - ground, 0.0f);
        if(h > 12.0f) continue;
        float a = clampf(0.62f - h * 0.35f, 0.15f, 0.62f);
        float s = 1.0f + h * 0.25f;
        V3 f = vscale(heading_fwd(k->heading + k->vis_yaw), 1.35f * s), r = vscale(heading_right(k->heading + k->vis_yaw), 0.95f * s);
        V3 c = v3(k->pos.x, ground + 0.04f, k->pos.z);
        uint32_t col = color_alpha(0xff000000, a);
        RV q[4] = {
            {c.x + f.x - r.x, c.y, c.z + f.z - r.z, 0, 0, col},
            {c.x + f.x + r.x, c.y, c.z + f.z + r.z, 1, 0, col},
            {c.x - f.x - r.x, c.y, c.z - f.z - r.z, 0, 1, col},
            {c.x - f.x + r.x, c.y, c.z - f.z + r.z, 1, 1, col},
        };
        r_strip(q, 4);
    }
}

static V3 kart_point(const Kart *k, V3 local) {
    float m[16];
    kart_matrix(m, k);
    return v3(m[0] * local.x + m[4] * local.y + m[8] * local.z + m[12],
              m[1] * local.x + m[5] * local.y + m[9] * local.z + m[13],
              m[2] * local.x + m[6] * local.y + m[10] * local.z + m[14]);
}

V3 models_exhaust_world(const Kart *k, int which) {
    const float *e = dk_exhaust_pos[which & 1];
    return kart_point(k, v3(e[0], e[1], e[2]));
}

V3 models_wheel_world(const Kart *k, int wheel) {
    const float *p = dk_part_pivot[DK_PART_WHEEL_FL + wheel];
    return kart_point(k, v3(p[0], 0.02f, p[2]));
}
