/* DreamKarts renderer.

   The camera matrix folds the perspective and the 640x480 viewport into one
   4x4 so a single SH4ZAM FTRV per vertex yields (x*w, y*w, -, w). Vertices
   in front of the near plane are divided with FSRRA and written straight to
   the Tile Accelerator through the store queues (pvr_dr). Triangles that
   cross the near plane are clipped in that homogeneous space, which keeps
   UVs, colours and depth linear. */
#include "render.h"
#include "texture_assets.h"
#include <malloc.h>
#include <sh4zam/shz_sh4zam.h>

#define NEAR_W   0.25f
#define MAX_STRIP 512
#define MAX_MESH_VERTS 1024

extern const uint8_t dk_textures_pak[];

RenderStats rstats;
Camera r_cam;
pvr_ptr_t r_textures[TEX_COUNT];

typedef struct { float x, y, w, u, v; uint32_t c; } CV;

static shz_mat4x4_t vp __attribute__((aligned(32)));
static float cam_f, cam_tx, cam_ty;
static const pvr_poly_hdr_t *cur_hdr, *sent_hdr;
static CV cvbuf[MAX_STRIP] __attribute__((aligned(32)));
static CV mesh_cv[MAX_MESH_VERTS] __attribute__((aligned(32)));
static pvr_vertex_t mesh_pv[MAX_MESH_VERTS] __attribute__((aligned(32)));
static uint8_t mesh_code[MAX_MESH_VERTS];

typedef struct { int key; pvr_poly_hdr_t hdr __attribute__((aligned(32))); } HeaderSlot;
#define MAX_HEADERS 160
static HeaderSlot *headers;
static int header_count;

/* ------------------------------------------------------------------ */
/* Setup                                                               */

static void load_textures(void) {
    size_t used = 0;
    for(int i = 0; i < TEX_COUNT; i++) {
        const dk_tex_asset_t *t = &dk_tex_assets[i];
        size_t size = (t->bytes + 31) & ~31u;
        r_textures[i] = pvr_mem_malloc(size);
        if(!r_textures[i]) {
            printf("DreamKarts render: VRAM exhausted at texture %d\n", i);
            continue;
        }
        pvr_txr_load(dk_textures_pak + t->offset, r_textures[i], size);
        used += size;
    }
    printf("DreamKarts render: %d textures, %lu KiB VRAM, %lu KiB free\n", TEX_COUNT,
           (unsigned long)(used / 1024), (unsigned long)(pvr_mem_available() / 1024));
}

void render_init(void) {
    pvr_init_params_t params = {
        .opb_sizes = {PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16},
        .vertex_buf_size = 768 * 1024,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 0,
        .opb_overflow_count = 3
    };
    if(pvr_init(&params) < 0) {
        printf("DreamKarts render: pvr_init failed\n");
        return;
    }
    pvr_set_bg_color(0.55f, 0.82f, 0.98f);
    PVR_SET(PVR_PT_ALPHA_REF, 0x60);
    pvr_fog_table_color(1.0f, 0.74f, 0.89f, 0.98f);
    pvr_fog_table_linear(160.0f, 820.0f);
    headers = memalign(32, sizeof(HeaderSlot) * MAX_HEADERS);
    load_textures();
}

void render_shutdown(void) {
    for(int i = 0; i < TEX_COUNT; i++)
        if(r_textures[i]) pvr_mem_free(r_textures[i]);
    free(headers);
    pvr_shutdown();
}

const pvr_poly_hdr_t *r_header(int tex, int flags) {
    int key = ((tex + 1) << 8) | flags;
    for(int i = 0; i < header_count; i++)
        if(headers[i].key == key) return &headers[i].hdr;
    if(header_count >= MAX_HEADERS) {
        printf("DreamKarts render: header cache full\n");
        return &headers[0].hdr;
    }
    pvr_list_t list = (flags & HF_LIST_MASK) == HF_PT ? PVR_LIST_PT_POLY :
                      (flags & HF_LIST_MASK) == HF_TR ? PVR_LIST_TR_POLY : PVR_LIST_OP_POLY;
    pvr_poly_cxt_t c;
    if(tex >= 0 && tex < TEX_COUNT && r_textures[tex]) {
        const dk_tex_asset_t *t = &dk_tex_assets[tex];
        int fmt = (t->fmt == DK_TEXFMT_ARGB4444 ? PVR_TXRFMT_ARGB4444 : PVR_TXRFMT_RGB565) |
                  PVR_TXRFMT_TWIDDLED;
        pvr_poly_cxt_txr(&c, list, fmt, t->w, t->h, r_textures[tex],
                         (flags & HF_POINT) ? PVR_FILTER_NEAREST : PVR_FILTER_BILINEAR);
        c.txr.mipmap = t->mip ? true : false;
        c.txr.env = PVR_TXRENV_MODULATEALPHA;
        c.txr.uv_clamp = (flags & HF_CLAMP) ? PVR_UVCLAMP_UV : PVR_UVCLAMP_NONE;
    }
    else {
        pvr_poly_cxt_col(&c, list);
    }
    c.gen.culling = PVR_CULLING_NONE;
    c.gen.fog_type = (flags & HF_NOFOG) ? PVR_FOG_DISABLE : PVR_FOG_TABLE;
    if(list == PVR_LIST_TR_POLY) {
        c.gen.alpha = true;
        c.blend.src = PVR_BLEND_SRCALPHA;
        c.blend.dst = (flags & HF_ADD) ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
        c.depth.write = false;
    }
    if(list == PVR_LIST_PT_POLY) c.gen.alpha = true;
    if(flags & HF_NOZWRITE) c.depth.write = false;
    if(flags & HF_NOZTEST) c.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    HeaderSlot *s = &headers[header_count++];
    s->key = key;
    pvr_poly_compile(&s->hdr, &c);
    return &s->hdr;
}

/* The caller paces frames with pvr_wait_ready() before updating. */
void r_frame_begin(void) {
    pvr_scene_begin();
    rstats.tris = rstats.verts = 0;
}

void r_list(pvr_list_t list) {
    pvr_list_begin(list);
    sent_hdr = NULL;
    cur_hdr = NULL;
}

void r_frame_end(void) {
    pvr_scene_finish();
}

/* ------------------------------------------------------------------ */
/* Matrices                                                            */

void m4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

void m4_mul(float *out, const float *a, const float *b) {
    float r[16];
    for(int c = 0; c < 4; c++)
        for(int row = 0; row < 4; row++)
            r[c * 4 + row] = a[0 * 4 + row] * b[c * 4 + 0] + a[1 * 4 + row] * b[c * 4 + 1] +
                             a[2 * 4 + row] * b[c * 4 + 2] + a[3 * 4 + row] * b[c * 4 + 3];
    memcpy(out, r, sizeof(r));
}

void m4_rot_y(float *m, float a) {
    float s = sinf(a), c = cosf(a);
    m4_identity(m);
    m[0] = c; m[2] = -s;      /* image of +X */
    m[8] = s; m[10] = c;      /* image of +Z */
}

void m4_rot_x(float *m, float a) {
    /* Positive pitches the nose (+Z) up. */
    float s = sinf(a), c = cosf(a);
    m4_identity(m);
    m[5] = c; m[6] = -s;
    m[9] = s; m[10] = c;
}

void m4_rot_z(float *m, float a) {
    /* Positive rolls the right side (+X) up. */
    float s = sinf(a), c = cosf(a);
    m4_identity(m);
    m[0] = c; m[1] = s;
    m[4] = -s; m[5] = c;
}

void m4_rot_axis(float *m, V3 n, float a) {
    float s = sinf(a), c = cosf(a), t = 1.0f - c;
    m4_identity(m);
    m[0] = t * n.x * n.x + c;       m[1] = t * n.x * n.y + s * n.z; m[2] = t * n.x * n.z - s * n.y;
    m[4] = t * n.x * n.y - s * n.z; m[5] = t * n.y * n.y + c;       m[6] = t * n.y * n.z + s * n.x;
    m[8] = t * n.x * n.z + s * n.y; m[9] = t * n.y * n.z - s * n.x; m[10] = t * n.z * n.z + c;
}

void m4_translate_rotate(float *m, V3 t, float yaw, float pitch, float roll) {
    float ry[16], rx[16], rz[16];
    m4_rot_y(ry, yaw);
    m4_rot_x(rx, pitch);
    m4_rot_z(rz, roll);
    m4_mul(m, ry, rx);
    m4_mul(m, m, rz);
    m[12] = t.x; m[13] = t.y; m[14] = t.z;
}

void m4_scale_apply(float *m, float sx, float sy, float sz) {
    for(int i = 0; i < 3; i++) {
        m[0 + i] *= sx;
        m[4 + i] *= sy;
        m[8 + i] *= sz;
    }
}

void r_set_camera(const Camera *c) {
    r_cam = *c;
    cam_f = 320.0f / tanf(c->fov * 0.5f);
    cam_tx = 320.0f / cam_f;
    cam_ty = 240.0f / cam_f;
    V3 r = c->right, u = c->up, f = c->fwd, e = c->pos;
    float re = vdot(r, e), ue = vdot(u, e), fe = vdot(f, e);
    float F = cam_f;
    /* Rows of P*V, stored column-major. */
    float row[4][4] = {
        {F * r.x + 320 * f.x, F * r.y + 320 * f.y, F * r.z + 320 * f.z, -(F * re + 320 * fe)},
        {-F * u.x + 240 * f.x, -F * u.y + 240 * f.y, -F * u.z + 240 * f.z, -(-F * ue + 240 * fe)},
        {f.x, f.y, f.z, -fe},
        {f.x, f.y, f.z, -fe}
    };
    for(int col = 0; col < 4; col++)
        for(int rr = 0; rr < 4; rr++)
            vp.elem[col * 4 + rr] = row[rr][col];
}

void r_load_vp(void) {
    shz_xmtrx_load_4x4(&vp);
}

void r_load_model(const float *m) {
    shz_mat4x4_t full __attribute__((aligned(32)));
    m4_mul(full.elem, vp.elem, m);
    shz_xmtrx_load_4x4(&full);
}

bool r_sphere_visible(V3 c, float radius) {
    V3 d = vsub(c, r_cam.pos);
    float z = vdot(d, r_cam.fwd);
    if(z + radius < NEAR_W) return false;
    float x = vdot(d, r_cam.right), y = vdot(d, r_cam.up);
    if(fabsf(x) > z * cam_tx + radius * 1.6f) return false;
    if(fabsf(y) > z * cam_ty + radius * 1.4f) return false;
    return true;
}

float r_view_depth(V3 p) {
    return vdot(vsub(p, r_cam.pos), r_cam.fwd);
}

bool r_project(V3 p, float *sx, float *sy, float *inv_w) {
    V3 d = vsub(p, r_cam.pos);
    float z = vdot(d, r_cam.fwd);
    if(z < NEAR_W) return false;
    float inv = 1.0f / z;
    *sx = 320.0f + vdot(d, r_cam.right) * cam_f * inv;
    *sy = 240.0f - vdot(d, r_cam.up) * cam_f * inv;
    *inv_w = inv;
    return true;
}

/* ------------------------------------------------------------------ */
/* Submission                                                          */

void r_hdr(const pvr_poly_hdr_t *h) {
    cur_hdr = h;
}

static inline void flush_hdr(void) {
    if(cur_hdr != sent_hdr) {
        pvr_prim(cur_hdr, sizeof(pvr_poly_hdr_t));
        sent_hdr = cur_hdr;
    }
}

static inline void xform(const RV *in, CV *o) {
    shz_vec4_t r = shz_xmtrx_transform_vec4(shz_vec4_init(in->x, in->y, in->z, 1.0f));
    o->x = r.x;
    o->y = r.y;
    o->w = r.w;
    o->u = in->u;
    o->v = in->v;
    o->c = in->c;
}

static inline unsigned outcode(const CV *o) {
    unsigned code = o->w < NEAR_W ? 16u : 0u;
    if(o->x < 0) code |= 1;
    if(o->x > 640.0f * o->w) code |= 2;
    if(o->y < 0) code |= 4;
    if(o->y > 480.0f * o->w) code |= 8;
    return code;
}

static inline void emit(const CV *c, uint32_t flags) {
    float inv = shz_invf_fsrra(c->w);
    pvr_vertex_t *d = pvr_dr_target();
    d->flags = flags;
    d->x = c->x * inv;
    d->y = c->y * inv;
    d->z = inv;
    d->u = c->u;
    d->v = c->v;
    d->argb = c->c;
    d->oargb = 0;
    pvr_dr_commit(d);
}

static inline CV cv_lerp(const CV *a, const CV *b, float t) {
    CV r;
    r.x = a->x + (b->x - a->x) * t;
    r.y = a->y + (b->y - a->y) * t;
    r.w = NEAR_W;
    r.u = a->u + (b->u - a->u) * t;
    r.v = a->v + (b->v - a->v) * t;
    r.c = color_lerp(a->c, b->c, t);
    return r;
}

static void clip_tri(const CV *a, const CV *b, const CV *c) {
    const CV *in[3] = {a, b, c};
    CV out[4];
    int n = 0;
    for(int i = 0; i < 3; i++) {
        const CV *s = in[i], *e = in[(i + 1) % 3];
        bool si = s->w >= NEAR_W, ei = e->w >= NEAR_W;
        if(si) out[n++] = *s;
        if(si != ei) out[n++] = cv_lerp(s, e, (NEAR_W - s->w) / (e->w - s->w));
    }
    if(n < 3) return;
    flush_hdr();
    emit(&out[0], PVR_CMD_VERTEX);
    emit(&out[1], PVR_CMD_VERTEX);
    if(n == 4) {
        emit(&out[3], PVR_CMD_VERTEX);
        emit(&out[2], PVR_CMD_VERTEX_EOL);
        rstats.tris += 2;
    }
    else {
        emit(&out[2], PVR_CMD_VERTEX_EOL);
        rstats.tris++;
    }
}

void r_strip(const RV *v, int n) {
    if(n < 3 || n > MAX_STRIP) return;
    unsigned and_code = 31, or_code = 0;
    for(int i = 0; i < n; i++) {
        xform(&v[i], &cvbuf[i]);
        unsigned code = outcode(&cvbuf[i]);
        and_code &= code;
        or_code |= code;
    }
    if(and_code & 16) return;
    if(!(or_code & 16) && (and_code & 15)) return;
    if(or_code & 16) {
        for(int i = 0; i + 2 < n; i++)
            clip_tri(&cvbuf[i], &cvbuf[i + 1], &cvbuf[i + 2]);
        return;
    }
    flush_hdr();
    for(int i = 0; i < n - 1; i++)
        emit(&cvbuf[i], PVR_CMD_VERTEX);
    emit(&cvbuf[n - 1], PVR_CMD_VERTEX_EOL);
    rstats.tris += n - 2;
    rstats.verts += n;
}

void r_quad(const RV *a, const RV *b, const RV *c, const RV *d) {
    RV q[4] = {*a, *b, *c, *d};
    r_strip(q, 4);
}

void r_mesh(const RV *v, int nv, const uint16_t *idx, int ni, bool cull) {
    if(nv > MAX_MESH_VERTS) nv = MAX_MESH_VERTS;
    for(int i = 0; i < nv; i++) {
        CV *c = &mesh_cv[i];
        xform(&v[i], c);
        mesh_code[i] = (uint8_t)outcode(c);
        if(c->w >= NEAR_W) {
            float inv = shz_invf_fsrra(c->w);
            pvr_vertex_t *p = &mesh_pv[i];
            p->flags = PVR_CMD_VERTEX;
            p->x = c->x * inv;
            p->y = c->y * inv;
            p->z = inv;
            p->u = c->u;
            p->v = c->v;
            p->argb = c->c;
            p->oargb = 0;
        }
    }
    rstats.verts += nv;
    flush_hdr();
    for(int i = 0; i + 2 < ni; i += 3) {
        unsigned a = idx[i], b = idx[i + 1], c = idx[i + 2];
        unsigned ca = mesh_code[a], cb = mesh_code[b], cc = mesh_code[c];
        if(ca & cb & cc) continue;
        if((ca | cb | cc) & 16) {
            clip_tri(&mesh_cv[a], &mesh_cv[b], &mesh_cv[c]);
            continue;
        }
        const pvr_vertex_t *pa = &mesh_pv[a], *pb = &mesh_pv[b], *pc = &mesh_pv[c];
        if(cull) {
            float area = (pb->x - pa->x) * (pc->y - pa->y) - (pb->y - pa->y) * (pc->x - pa->x);
            if(area <= 0) continue;
        }
        pvr_vertex_t *d = pvr_dr_target();
        *d = *pa;
        pvr_dr_commit(d);
        d = pvr_dr_target();
        *d = *pb;
        pvr_dr_commit(d);
        d = pvr_dr_target();
        *d = *pc;
        d->flags = PVR_CMD_VERTEX_EOL;
        pvr_dr_commit(d);
        rstats.tris++;
    }
}

void r_billboard(V3 c, float w, float h, float rot, float u0, float v0, float u1, float v1, uint32_t col) {
    float s = 0, co = 1;
    if(rot != 0) {
        shz_sincos_t sc = shz_sincosf(rot);
        s = sc.sin;
        co = sc.cos;
    }
    V3 rx = vadd(vscale(r_cam.right, co * w), vscale(r_cam.up, s * w));
    V3 ry = vadd(vscale(r_cam.right, -s * h), vscale(r_cam.up, co * h));
    RV q[4];
    V3 p0 = vsub(vsub(c, rx), ry), p1 = vadd(vsub(c, rx), ry);
    V3 p2 = vsub(vadd(c, rx), ry), p3 = vadd(vadd(c, rx), ry);
    q[0] = (RV){p1.x, p1.y, p1.z, u0, v0, col};
    q[1] = (RV){p3.x, p3.y, p3.z, u1, v0, col};
    q[2] = (RV){p0.x, p0.y, p0.z, u0, v1, col};
    q[3] = (RV){p2.x, p2.y, p2.z, u1, v1, col};
    r_strip(q, 4);
}

void r_billboard_y(V3 b, float w, float h, float u0, float v0, float u1, float v1, uint32_t col) {
    V3 rt = vnorm(v3(r_cam.right.x, 0, r_cam.right.z));
    V3 l = vmad(b, rt, -w), r = vmad(b, rt, w);
    RV q[4] = {
        {l.x, l.y + h, l.z, u0, v0, col}, {r.x, r.y + h, r.z, u1, v0, col},
        {l.x, l.y, l.z, u0, v1, col}, {r.x, r.y, r.z, u1, v1, col}
    };
    r_strip(q, 4);
}

void r_streak_ex(V3 a, V3 b, float width_a, float width_b, uint32_t col_a, uint32_t col_b,
                 float v_a, float v_b) {
    float ax, ay, aw, bx, by, bw;
    if(!r_project(a, &ax, &ay, &aw) || !r_project(b, &bx, &by, &bw)) return;
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    if(len < 0.5f) {
        dx = 1;
        dy = 0;
        len = 1;
    }
    float wa = clampf(width_a * cam_f * aw, 0.8f, 40.0f), wb = clampf(width_b * cam_f * bw, 0.6f, 40.0f);
    float nx = -dy / len, ny = dx / len;
    flush_hdr();
    const float xs[4] = {ax + nx * wa, ax - nx * wa, bx + nx * wb, bx - nx * wb};
    const float ys[4] = {ay + ny * wa, ay - ny * wa, by + ny * wb, by - ny * wb};
    const float zs[4] = {aw, aw, bw, bw};
    const float us[4] = {0, 1, 0, 1}, vs[4] = {v_a, v_a, v_b, v_b};
    for(int i = 0; i < 4; i++) {
        pvr_vertex_t *d = pvr_dr_target();
        d->flags = i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
        d->x = xs[i];
        d->y = ys[i];
        d->z = zs[i];
        d->u = us[i];
        d->v = vs[i];
        d->argb = i < 2 ? col_a : col_b;
        d->oargb = 0;
        pvr_dr_commit(d);
    }
    rstats.tris += 2;
}

void r_streak(V3 a, V3 b, float width, uint32_t col_a, uint32_t col_b) {
    r_streak_ex(a, b, width, width, col_a, col_b, 0.5f, 0.5f);
}

static inline void emit2d(float x, float y, float z, float u, float v, uint32_t c, uint32_t flags) {
    pvr_vertex_t *d = pvr_dr_target();
    d->flags = flags;
    d->x = x;
    d->y = y;
    d->z = z;
    d->u = u;
    d->v = v;
    d->argb = c;
    d->oargb = 0;
    pvr_dr_commit(d);
}

void r_quad2d(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
              uint32_t col, float z) {
    flush_hdr();
    emit2d(x0, y0, z, u0, v0, col, PVR_CMD_VERTEX);
    emit2d(x1, y0, z, u1, v0, col, PVR_CMD_VERTEX);
    emit2d(x0, y1, z, u0, v1, col, PVR_CMD_VERTEX);
    emit2d(x1, y1, z, u1, v1, col, PVR_CMD_VERTEX_EOL);
    rstats.tris += 2;
}

void r_quad2d_grad(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom, float z) {
    flush_hdr();
    emit2d(x0, y0, z, 0, 0, top, PVR_CMD_VERTEX);
    emit2d(x1, y0, z, 1, 0, top, PVR_CMD_VERTEX);
    emit2d(x0, y1, z, 0, 1, bottom, PVR_CMD_VERTEX);
    emit2d(x1, y1, z, 1, 1, bottom, PVR_CMD_VERTEX_EOL);
    rstats.tris += 2;
}

void r_tri2d(float x0, float y0, float x1, float y1, float x2, float y2,
             uint32_t c0, uint32_t c1, uint32_t c2, float z) {
    flush_hdr();
    emit2d(x0, y0, z, 0, 0, c0, PVR_CMD_VERTEX);
    emit2d(x1, y1, z, 1, 0, c1, PVR_CMD_VERTEX);
    emit2d(x2, y2, z, 0, 1, c2, PVR_CMD_VERTEX_EOL);
    rstats.tris++;
}

void r_quad2d_pts(const float *xy, const float *uv, const uint32_t *col, float z) {
    flush_hdr();
    for(int i = 0; i < 4; i++)
        emit2d(xy[i * 2], xy[i * 2 + 1], z, uv[i * 2], uv[i * 2 + 1], col[i],
               i == 3 ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX);
    rstats.tris += 2;
}
