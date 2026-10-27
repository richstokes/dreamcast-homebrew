/* DreamKarts renderer.

   The camera matrix folds the perspective and the 640x480 viewport into one
   4x4 so a single SH4ZAM FTRV per vertex yields (x*w, y*w, -, w). Vertices
   in front of the near plane are divided with FSRRA and written straight to
   the Tile Accelerator through the store queues (pvr_dr). Triangles that
   cross the near plane are clipped in that homogeneous space, which keeps
   UVs, colours and depth linear.

   The per-vertex loops (transform, outcodes, projection, store-queue
   writes, triangle tests) are SH-4 assembly in src/xform.S; this file keeps
   the decisions around them and the rare paths (near-plane clipping).
   Build with -DDK_XF_CHECK to run the C each routine replaces beside it and
   count any difference (DK XFCHECK lines in the autotest log). */
#include "render.h"
#include "prof.h"
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
/* Viewport: full screen, or one half of a two-player split. Views are
   whole 32-pixel tile rows so the PVR's user tile clip trims them for free;
   every polygon header opts in to that clip. */
static float view_y0 = 0, view_y1 = 480, view_cx = 320, view_cy = 240;
static unsigned clip_mask = 16;
static const pvr_poly_hdr_t *cur_hdr, *sent_hdr;
static bool list_open;
/* Projected vertices and their clip outcodes, staged in the operand cache
   by dk_xf_stage (src/xform.S) for r_strip and r_mesh. */
static pvr_vertex_t stage_pv[MAX_MESH_VERTS] __attribute__((aligned(32)));
static uint8_t stage_code[MAX_MESH_VERTS];
/* A kart mesh's projected positions {X/W, Y/W, 1/W, -}, from dk_xf_pos. */
static float stage_pos[MAX_MESH_VERTS][4] __attribute__((aligned(32)));

/* src/xform.S */
typedef struct { float near_w, xmax, y0, y1; uint8_t *codes; const uint32_t *col; } XfParams;
typedef struct { const pvr_vertex_t *pv; const uint8_t *codes; uint32_t clip_mask, sent, back; } MeshSubmit;
uint32_t dk_xf_stage(const RV *v, int n, pvr_vertex_t *out, const XfParams *p);
uint32_t dk_xf_stage_kv(const KV *v, int n, pvr_vertex_t *out, const XfParams *p);
void dk_sq_run(const pvr_vertex_t *src, int n);
void dk_xf_emit(const RV *v, int n);
int dk_sq_tris(MeshSubmit *ms, const uint16_t *idx, int ntri, int cull);
typedef struct { const float (*pos)[4]; const uint32_t *col; uint32_t sent, back; } StripSubmit;
uint32_t dk_xf_pos(const float (*pos)[4], int n, float (*out)[4], const XfParams *p);
void dk_sq_strips(StripSubmit *ss, const uint16_t *strips);
static XfParams xf = {NEAR_W, 640.0f, 0.0f, 480.0f, stage_code, NULL};

typedef struct { int key; pvr_poly_hdr_t hdr __attribute__((aligned(32))); } HeaderSlot;
#define MAX_HEADERS 160
static HeaderSlot *headers;
static int header_count;

/* A texture's mip D-adjust (render.h DK_MIP_BIAS): the surfaces driven on
   apart from everything beside them, the alpha textures at the hardware's own. */
static int tex_mip_bias(int tex) {
    if(dk_tex_assets[tex].fmt == DK_TEXFMT_ARGB4444) return PVR_MIPBIAS_NORMAL;
    switch(tex) {
        case TEX_ROAD: case TEX_TKROAD: case TEX_CANYONROAD: case TEX_BOARDWALK: case TEX_DASH: case TEX_CHECKER:
            return DK_MIP_ROAD_BIAS;
        default:
            return tex >= TEX_SKYROAD_0 && tex <= TEX_SKYROAD_15 ? DK_MIP_ROAD_BIAS : DK_MIP_BIAS;
    }
}

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
        .vertex_buf_size = 1536 * 1024,   /* Tsudoken Drift's start straight in a split screen passed 1 MB and stalled the TA */
        /* Measured: the CPU waits on the TA only ~0.1 ms a frame, so DMA
           to it is not worth having. */
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        /* Presort: translucent polygons draw in submission order. The
           hardware autosort peels overlapping layers per tile and cost up to
           7 ms a frame on the console; callers submit back to front. */
        .autosort_disabled = 1,
        .opb_overflow_count = 3
    };
    if(pvr_init(&params) < 0) {
        printf("DreamKarts render: pvr_init failed\n");
        return;
    }
    pvr_set_bg_color(0.55f, 0.82f, 0.98f);
    PVR_SET(PVR_PT_ALPHA_REF, 0x60);
    /* FPU_CULL_VAL: the area under which PVR_CULLING_SMALL drops a
       triangle. KOS leaves it as the boot left it; this only has to catch
       the zero-area joins between strips, not real slivers. */
    union { float f; uint32_t u; } cull_val = {0.01f};
    PVR_SET(PVR_OBJECT_CLIP, cull_val.u);
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
    int key = ((tex + 1) << 10) | flags;
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
        c.txr.mipmap_bias = tex_mip_bias(tex);
        c.txr.env = PVR_TXRENV_MODULATEALPHA;
        c.txr.uv_clamp = (flags & HF_CLAMP) ? PVR_UVCLAMP_UV : PVR_UVCLAMP_NONE;
    }
    else {
        pvr_poly_cxt_col(&c, list);
    }
    /* Small, not none: strips are joined with zero-area triangles, which
       Flycast drops but the console rasterizes as dark streaks out to the
       tile edge (Tsudoken's lanterns and roof edges against the sky). It
       never culls by winding. */
    c.gen.culling = PVR_CULLING_SMALL;
    c.gen.clip_mode = PVR_USERCLIP_INSIDE;
    c.gen.fog_type = (flags & HF_NOFOG) ? PVR_FOG_DISABLE : PVR_FOG_TABLE;
    if(list == PVR_LIST_TR_POLY) {
        c.gen.alpha = true;
        c.blend.src = PVR_BLEND_SRCALPHA;
        c.blend.dst = (flags & HF_ADD) ? PVR_BLEND_ONE : PVR_BLEND_INVSRCALPHA;
        c.depth.write = PVR_DEPTHWRITE_DISABLE;
    }
    if(list == PVR_LIST_PT_POLY) c.gen.alpha = true;
    /* depth.write is the hardware's Z-write-disable bit despite its name
       (PVR_DEPTHWRITE_ENABLE is 0), so "false" here used to turn writes on:
       a translucent quad then hid anything translucent drawn after it and
       further away, even through its clear texels (one cloud cutting a
       straight edge through another). */
    if(flags & HF_NOZWRITE) c.depth.write = PVR_DEPTHWRITE_DISABLE;
    if(flags & HF_ZWRITE) c.depth.write = PVR_DEPTHWRITE_ENABLE;
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

#if defined(DK_HW) && defined(DK_CAPTURE)
/* Tile Accelerator back-pressure probe (PROBE_NOTA): direct-render writes
   go to a cached scratch line instead of the store queues, so the CPU time
   of a frame without the TA can be set against the real one. Headers still
   go to the TA through pvr_prim; the frame shows no geometry. */
static uint8_t ta_sink[64] __attribute__((aligned(64)));
static uint32_t ta_saved_addr;
static bool ta_sunk;

static void ta_sink_begin(void) {
    if(!(dk_probe_off & PROBE_NOTA) || ta_sunk) return;
    ta_saved_addr = pvr_dr_addr;
    pvr_dr_addr = (uint32_t)ta_sink;
    ta_sunk = true;
}

static void ta_sink_end(void) {
    if(!ta_sunk) return;
    pvr_dr_addr = ta_saved_addr;
    ta_sunk = false;
}
#else
static inline void ta_sink_begin(void) {}
static inline void ta_sink_end(void) {}
#endif

void r_list(pvr_list_t list) {
    pvr_list_begin(list);
    ta_sink_begin();
    sent_hdr = NULL;
    cur_hdr = NULL;
    list_open = true;
    r_set_viewport(view_y0, view_y1);
}

void r_frame_end(void) {
    ta_sink_end();
    list_open = false;
    pvr_scene_finish();
}

/* ------------------------------------------------------------------ */
/* Matrices                                                            */

void m4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

/* Saves and restores XMTRX (see render.h). */
void m4_mul(float *out, const float *a, const float *b) {
    shz_mat4x4_t saved;
    shz_xmtrx_store_4x4(&saved);
    shz_xmtrx_load_unaligned_4x4(a);
    shz_xmtrx_apply_unaligned_4x4(b);
    shz_xmtrx_store_unaligned_4x4(out);
    shz_xmtrx_load_4x4(&saved);
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
    /* Positive pitch raises the nose (+Z); SH4ZAM's positive X rotation lowers it. */
    shz_mat4x4_t saved;
    shz_xmtrx_store_4x4(&saved);
    shz_xmtrx_init_translation(t.x, t.y, t.z);
    shz_xmtrx_rotate_y(yaw);
    shz_xmtrx_rotate_x(-pitch);
    shz_xmtrx_rotate_z(roll);
    shz_xmtrx_store_unaligned_4x4(m);
    shz_xmtrx_load_4x4(&saved);
}

void m4_scale_apply(float *m, float sx, float sy, float sz) {
    for(int i = 0; i < 3; i++) {
        m[0 + i] *= sx;
        m[4 + i] *= sy;
        m[8 + i] *= sz;
    }
}

void r_set_viewport(float y0, float y1) {
    view_y0 = y0;
    view_y1 = y1;
    view_cy = (y0 + y1) * 0.5f;
    xf.y0 = y0;
    xf.y1 = y1;
    clip_mask = 16u;
    /* Tell the TA (only valid inside an open list). */
    if(list_open) {
        uint32_t *d = pvr_dr_target();
        d[0] = PVR_CMD_USERCLIP;
        d[1] = d[2] = d[3] = 0;
        d[4] = 0;                                 /* x min tile */
        d[5] = (uint32_t)(y0 / 32.0f);            /* y min tile */
        d[6] = 19;                                /* x max tile */
        d[7] = (uint32_t)((y1 - 1.0f) / 32.0f);   /* y max tile */
        pvr_dr_commit(d);
        sent_hdr = NULL;                          /* resend the header after it */
    }
}

/* Lengths of the clip-plane gradients in view space, for r_strip_bounded:
   left, right, top, bottom (set by r_set_camera). */
static float plane_g[4];

void r_set_camera(const Camera *c) {
    r_cam = *c;
    cam_f = 320.0f / tanf(c->fov * 0.5f);
    cam_tx = 320.0f / cam_f;
    cam_ty = (view_y1 - view_y0) * 0.5f / cam_f;
    V3 r = c->right, u = c->up, f = c->fwd, e = c->pos;
    float re = vdot(r, e), ue = vdot(u, e), fe = vdot(f, e);
    float F = cam_f, CX = view_cx, CY = view_cy;
    /* Rows of P*V, stored column-major. */
    float row[4][4] = {
        {F * r.x + CX * f.x, F * r.y + CX * f.y, F * r.z + CX * f.z, -(F * re + CX * fe)},
        {-F * u.x + CY * f.x, -F * u.y + CY * f.y, -F * u.z + CY * f.z, -(-F * ue + CY * fe)},
        {f.x, f.y, f.z, -fe},
        {f.x, f.y, f.z, -fe}
    };
    for(int col = 0; col < 4; col++)
        for(int rr = 0; rr < 4; rr++)
            vp.elem[col * 4 + rr] = row[rr][col];
    plane_g[0] = sqrtf(F * F + CX * CX);
    plane_g[1] = sqrtf(F * F + (640.0f - CX) * (640.0f - CX));
    plane_g[2] = sqrtf(F * F + (CY - view_y0) * (CY - view_y0));
    plane_g[3] = sqrtf(F * F + (view_y1 - CY) * (view_y1 - CY));
}

void r_load_vp(void) {
    shz_xmtrx_load_4x4(&vp);
}

void r_load_model(const float *m) {
    /* XMTRX = view-projection * model, multiplied by the FTRV unit. */
    shz_xmtrx_load_4x4(&vp);
    shz_xmtrx_apply_unaligned_4x4(m);
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
    *sx = view_cx + vdot(d, r_cam.right) * cam_f * inv;
    *sy = view_cy - vdot(d, r_cam.up) * cam_f * inv;
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
        PROF_COUNT(PN_HDRS, 1);
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
    if(o->y < view_y0 * o->w) code |= 4;
    if(o->y > view_y1 * o->w) code |= 8;
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
    r.w = a->w + (b->w - a->w) * t;
    r.u = a->u + (b->u - a->u) * t;
    r.v = a->v + (b->v - a->v) * t;
    r.c = color_lerp(a->c, b->c, t);
    return r;
}

/* Signed distance to a clip plane in homogeneous space (>= 0 is inside). */
static inline float plane_dist(const CV *v, int plane) {
    switch(plane) {
        case 0: return v->w - NEAR_W;
        case 1: return v->y - view_y0 * v->w;
        default: return view_y1 * v->w - v->y;
    }
}

static inline void emit_tri(const CV *a, const CV *b, const CV *c) {
    flush_hdr();
    emit(a, PVR_CMD_VERTEX);
    emit(b, PVR_CMD_VERTEX);
    emit(c, PVR_CMD_VERTEX_EOL);
    rstats.tris++;
}

/* Clip against the near plane and, in a split screen, the viewport's top
   and bottom, then emit the polygon as a fan. */
static void clip_tri(const CV *a, const CV *b, const CV *c) {
    CV buf[2][9];
    int n = 3, cur = 0;
    buf[0][0] = *a;
    buf[0][1] = *b;
    buf[0][2] = *c;
    for(int plane = 0; plane < 3; plane++) {
        if(plane > 0 && !(clip_mask & 4)) break;
        const CV *in = buf[cur];
        CV *out = buf[cur ^ 1];
        int m = 0;
        for(int i = 0; i < n; i++) {
            const CV *s = &in[i], *e = &in[(i + 1) % n];
            float ds = plane_dist(s, plane), de = plane_dist(e, plane);
            if(ds >= 0) out[m++] = *s;
            if((ds >= 0) != (de >= 0) && m < 9) {
                out[m] = cv_lerp(s, e, ds / (ds - de));
                if(plane == 0) out[m].w = NEAR_W;
                m++;
            }
        }
        n = m;
        cur ^= 1;
        if(n < 3) return;
    }
    const CV *p = buf[cur];
    for(int i = 1; i + 1 < n; i++)
        emit_tri(&p[0], &p[i], &p[i + 1]);
}

#ifdef DK_XF_CHECK
/* Checks the assembly against the C it replaced: every staged vertex and
   code, and every triangle decision (src/xform.S must match bit for bit). */
uint32_t xf_checked, xf_mismatch;

static void check_stage(const RV *v, int n, uint32_t codes) {
    unsigned and_code = 31, or_code = 0;
    for(int i = 0; i < n; i++) {
        CV c;
        xform(&v[i], &c);
        unsigned code = outcode(&c);
        and_code &= code;
        or_code |= code;
        xf_checked++;
        if(stage_code[i] != code) {
            xf_mismatch++;
            continue;
        }
        if(c.w < NEAR_W) continue;
        float inv = shz_invf_fsrra(c.w);
        pvr_vertex_t e;
        e.flags = PVR_CMD_VERTEX;
        e.x = c.x * inv;
        e.y = c.y * inv;
        e.z = inv;
        e.u = c.u;
        e.v = c.v;
        e.argb = c.c;
        e.oargb = 0;
        if(memcmp(&e, &stage_pv[i], sizeof(e))) xf_mismatch++;
    }
    if(codes != (and_code | or_code << 8)) xf_mismatch++;
}
#endif

void r_strip(const RV *v, int n) {
    if(n < 3 || n > MAX_STRIP) return;
    uint32_t codes = dk_xf_stage(v, n, stage_pv, &xf);
#ifdef DK_XF_CHECK
    check_stage(v, n, codes);
#endif
    unsigned and_code = codes & 0xff, or_code = codes >> 8;
    PROF_COUNT(PN_SVERTS, n);
    if(and_code & 16) { PROF_COUNT(PN_SCULL, 1); return; }
    if(!(or_code & 16) && (and_code & 15)) { PROF_COUNT(PN_SCULL, 1); return; }
    if(or_code & clip_mask) {
        PROF_COUNT(PN_SCLIP, 1);
        /* Triangle by triangle; only those crossing the near plane go back
           to C, re-transformed from the source vertices. */
        for(int i = 0; i + 2 < n; i++) {
            unsigned ca = stage_code[i], cb = stage_code[i + 1], cc = stage_code[i + 2];
            if(ca & cb & cc) continue;
            if((ca | cb | cc) & clip_mask) {
                CV ta, tb, tc;
                xform(&v[i], &ta);
                xform(&v[i + 1], &tb);
                xform(&v[i + 2], &tc);
                clip_tri(&ta, &tb, &tc);
            }
            else {
                flush_hdr();
                dk_sq_run(&stage_pv[i], 3);
                rstats.tris++;
            }
        }
        return;
    }
    flush_hdr();
    dk_sq_run(stage_pv, n);
    rstats.tris += n - 2;
    rstats.verts += n;
    PROF_COUNT(PN_SEMIT, n);
}

/* Where a sphere lies against the clip planes r_strip tests each vertex
   against: SPHERE_OUT if wholly outside one of them, SPHERE_IN if wholly
   inside all five, else SPHERE_CROSS. The planes are those of the
   outcodes in view space (x right, y up, z forward):
     X >= 0           F x + CX z
     X <= 640 W       (640 - CX) z - F x
     Y >= y0 W        -F y + (CY - y0) z
     Y <= y1 W        F y + (y1 - CY) z
     W >= near        z - near
   Each margin is far wider than the rounding of FTRV or of this test, so
   SPHERE_OUT strips are ones r_strip would cull (or skip triangle by
   triangle) and SPHERE_IN strips ones it would send whole. */
enum { SPHERE_IN, SPHERE_OUT, SPHERE_CROSS };

static int sphere_class(V3 c, float radius) {
    V3 d = vsub(c, r_cam.pos);
    float x = vdot(d, r_cam.right), y = vdot(d, r_cam.up), z = vdot(d, r_cam.fwd);
    float F = cam_f;
    float mag = fabsf(x) + fabsf(y) + fabsf(z) + radius;
    float v[5] = {F * x + view_cx * z, (640.0f - view_cx) * z - F * x, -F * y + (view_cy - view_y0) * z,
                  F * y + (view_y1 - view_cy) * z, z - NEAR_W};
    bool inside = true;
    for(int p = 0; p < 5; p++) {
        float g = p < 4 ? plane_g[p] : 1.0f;
        float m = g * (radius * 1.01f + mag * 1e-4f + 0.01f);
        if(v[p] < -m) return SPHERE_OUT;
        if(v[p] < m) inside = false;
    }
    return inside ? SPHERE_IN : SPHERE_CROSS;
}

void r_strip_bounded(const RV *v, int n, V3 c, float radius) {
    if(n < 3 || n > MAX_STRIP) return;
    PROF_COUNT(PN_STRIPS, 1);
    int cls = sphere_class(c, radius);
#ifdef DK_XF_CHECK
    if(cls != SPHERE_CROSS) {
        /* The vertex outcodes must agree with the sphere. */
        uint32_t codes = dk_xf_stage(v, n, stage_pv, &xf);
        unsigned and_code = codes & 0xff, or_code = codes >> 8;
        extern uint32_t xf_checked, xf_mismatch;
        xf_checked++;
        if(cls == SPHERE_IN && or_code != 0) xf_mismatch++;
        if(cls == SPHERE_OUT && and_code == 0) {
            /* No plane has every vertex outside: every triangle must
               still share one. */
            for(int i = 0; i + 2 < n; i++)
                if(!(stage_code[i] & stage_code[i + 1] & stage_code[i + 2])) {
                    xf_mismatch++;
                    break;
                }
        }
    }
#endif
    if(cls == SPHERE_OUT) {
        PROF_COUNT(PN_SOUT, 1);
        return;
    }
    if(cls == SPHERE_CROSS) {
        r_strip(v, n);
        return;
    }
    PROF_COUNT(PN_SIN, 1);
    PROF_COUNT(PN_SVERTS, n);
    flush_hdr();
    dk_xf_emit(v, n);
    rstats.tris += n - 2;
    rstats.verts += n;
    PROF_COUNT(PN_SEMIT, n);
}

void r_quad(const RV *a, const RV *b, const RV *c, const RV *d) {
    RV q[4] = {*a, *b, *c, *d};
    r_strip(q, 4);
}

#ifdef DK_XF_CHECK
/* The C triangle decisions dk_sq_tris must reproduce: sent, back faces. */
static void check_tris(const uint16_t *idx, int ntri, bool cull, uint32_t sent, uint32_t back) {
    uint32_t s = 0, b = 0;
    for(int t = 0; t < ntri; t++) {
        unsigned a = idx[t * 3], bb = idx[t * 3 + 1], c = idx[t * 3 + 2];
        unsigned ca = stage_code[a], cb = stage_code[bb], cc = stage_code[c];
        if(ca & cb & cc) continue;
        if((ca | cb | cc) & clip_mask) break;
        const pvr_vertex_t *pa = &stage_pv[a], *pb = &stage_pv[bb], *pc = &stage_pv[c];
        if(cull) {
            float area = (pb->x - pa->x) * (pc->y - pa->y) - (pb->y - pa->y) * (pc->x - pa->x);
            if(area <= 0) { b++; continue; }
        }
        s++;
    }
    if(s != sent || b != back) xf_mismatch++;
}

/* The C dk_xf_pos must reproduce: each position's projection, bit for
   bit, and the AND and OR of the outcodes. */
static void check_pos(const float (*pos)[4], int n, uint32_t codes) {
    unsigned and_code = 31, or_code = 0;
    for(int i = 0; i < n; i++) {
        CV c;
        xform(&(RV){pos[i][0], pos[i][1], pos[i][2], 0, 0, 0}, &c);
        unsigned code = outcode(&c);
        and_code &= code;
        or_code |= code;
        xf_checked++;
        float inv = shz_invf_fsrra(c.w), e[3] = {c.x * inv, c.y * inv, inv};
        if(c.w >= NEAR_W && memcmp(e, stage_pos[i], sizeof(e))) xf_mismatch++;
    }
    if(codes != (and_code | or_code << 8)) xf_mismatch++;
}

/* The C dk_sq_strips must reproduce: each strip triangle is a front
   face if its area is positive (counted in sent), else a back face. */
static void check_strips(const uint16_t *s, uint32_t sent, uint32_t back) {
    uint32_t sn = 0, bk = 0;
    for(int n; (n = *s++) != 0; s += n * 2) {
        for(int k = 0; k + 2 < n; k++) {
            const float *pa = stage_pos[s[(k + (k & 1)) * 2] / 16], *pb = stage_pos[s[(k + 1 - (k & 1)) * 2] / 16];
            const float *pc = stage_pos[s[(k + 2) * 2] / 16];
            float area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
            if(area > 0) sn++;
            else bk++;
        }
    }
    if(sn != sent || bk != back) xf_mismatch++;
}
#endif

/* A mesh's vertex i as an RV: from the RV array, or a KV and its colour
   with u = v = 0. */
static inline RV mesh_vertex(const RV *v, const KV *kv, const uint32_t *col, int i) {
    if(v) return v[i];
    return (RV){kv[i].x, kv[i].y, kv[i].z, 0, 0, col[kv[i].group]};
}

static void mesh_draw(const RV *v, const KV *kv, const uint32_t *col, int nv, const uint16_t *idx, int ni,
                      bool cull) {
    if(nv > MAX_MESH_VERTS) nv = MAX_MESH_VERTS;
    if(nv < 1) return;
    PROF_BEGIN(pm);
    /* Only the projected vertex is kept; the rare triangles that need
       near-plane clipping re-transform their corners (XMTRX still holds
       this mesh's matrix). */
    uint32_t codes;
    if(v) codes = dk_xf_stage(v, nv, stage_pv, &xf);
    else {
        xf.col = col;
        codes = dk_xf_stage_kv(kv, nv, stage_pv, &xf);
    }
    (void)codes;   /* the triangle tests read stage_code; the check build reads codes */
#ifdef DK_XF_CHECK
    {
        static RV tmp[MAX_MESH_VERTS];
        for(int i = 0; i < nv; i++) tmp[i] = mesh_vertex(v, kv, col, i);
        check_stage(tmp, nv, codes);
    }
#endif
    rstats.verts += nv;
    PROF_COUNT(PN_MVERTS, nv);
    PROF_LAP(PS_MXFORM, pm);
    flush_hdr();
    MeshSubmit ms = {stage_pv, stage_code, clip_mask, 0, 0};
    int ntri = ni / 3;
    for(int t = 0; t < ntri;) {
#ifdef DK_XF_CHECK
        uint32_t sent0 = ms.sent, back0 = ms.back;
#endif
        int done = dk_sq_tris(&ms, idx + t * 3, ntri - t, cull);
#ifdef DK_XF_CHECK
        check_tris(idx + t * 3, ntri - t, cull, ms.sent - sent0, ms.back - back0);
#endif
        t += done;
        if(t >= ntri) break;
        /* Triangle t crosses the near plane. */
        RV a = mesh_vertex(v, kv, col, idx[t * 3]), b = mesh_vertex(v, kv, col, idx[t * 3 + 1]),
           c = mesh_vertex(v, kv, col, idx[t * 3 + 2]);
        CV ta, tb, tc;
        xform(&a, &ta);
        xform(&b, &tb);
        xform(&c, &tc);
        clip_tri(&ta, &tb, &tc);
        t++;
    }
    rstats.tris += ms.sent;
    PROF_COUNT(PN_MTRIS, ms.sent);
    PROF_COUNT(PN_MBACK, ms.back);
    PROF_LAP(PS_MSUBMIT, pm);
}

void r_mesh(const RV *v, int nv, const uint16_t *idx, int ni, bool cull) {
    mesh_draw(v, NULL, NULL, nv, idx, ni, cull);
}

void r_mesh_kv(const KMesh *m, const uint32_t *col) {
    if(m->npos > MAX_MESH_VERTS) return;
    PROF_BEGIN(pm);
    uint32_t codes = dk_xf_pos(m->pos, m->npos, stage_pos, &xf);
#ifdef DK_XF_CHECK
    check_pos(m->pos, m->npos, codes);
#endif
    PROF_LAP(PS_MXFORM, pm);
    if(codes >> 8) {
        /* A vertex outside a clip plane: triangle by triangle, each one
           skipped, clipped or sent. */
        mesh_draw(NULL, m->v, col, m->nv, m->idx, m->ni, true);
        return;
    }
    PROF_COUNT(PN_MVERTS, m->npos);
    flush_hdr();
    StripSubmit ss = {stage_pos, col, 0, 0};
    dk_sq_strips(&ss, m->strips);
#ifdef DK_XF_CHECK
    check_strips(m->strips, ss.sent, ss.back);
#endif
    rstats.verts += m->npos;
    rstats.tris += ss.sent;
    PROF_COUNT(PN_MTRIS, ss.sent);
    PROF_COUNT(PN_MBACK, ss.back);
    PROF_LAP(PS_MSUBMIT, pm);
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
    if(ay < view_y0 + 2 || ay > view_y1 - 2 || by < view_y0 + 2 || by > view_y1 - 2) return;
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
