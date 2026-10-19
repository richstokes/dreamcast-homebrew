/* DreamKarts renderer: PowerVR lists, SH4ZAM transforms, near-plane
   clipping and direct store-queue submission. */
#ifndef DK_RENDER_H
#define DK_RENDER_H

#include "dk.h"
#include <dc/pvr.h>

/* World-space vertex for static and dynamic geometry. */
typedef struct {
    float x, y, z, u, v;
    uint32_t c;
} RV;

/* Header flags for r_header(). Exactly one list flag. */
enum {
    HF_OP = 0, HF_PT = 1, HF_TR = 2, HF_LIST_MASK = 3,
    HF_ADD = 1 << 2,      /* additive (src alpha, one) */
    HF_NOFOG = 1 << 3,
    HF_CLAMP = 1 << 4,
    HF_NOZWRITE = 1 << 5,
    HF_NOZTEST = 1 << 6,
    HF_POINT = 1 << 7
};

#define TEX_NONE -1

void render_init(void);
void render_shutdown(void);
const pvr_poly_hdr_t *r_header(int tex, int flags);

void r_frame_begin(void);
void r_list(pvr_list_t list);
void r_frame_end(void);

/* Screen rows [y0, y1) for the next r_set_camera (full screen: 0, 480). */
void r_set_viewport(float y0, float y1);
void r_set_camera(const Camera *cam);
void r_load_vp(void);
/* Load view-projection * m (column-major 4x4) into XMTRX. */
void r_load_model(const float *m);
void r_hdr(const pvr_poly_hdr_t *h);

/* Transform with XMTRX, clip against the near plane and submit. */
void r_strip(const RV *v, int n);
/* r_strip for a strip whose vertices all lie within the sphere (c, radius):
   the sphere decides whole strips, and only those it cannot are tested
   vertex by vertex. */
void r_strip_bounded(const RV *v, int n, V3 c, float radius);
void r_quad(const RV *a, const RV *b, const RV *c, const RV *d);  /* strip order */
/* Lit/coloured indexed triangles (already filled RV array). */
void r_mesh(const RV *v, int nv, const uint16_t *idx, int ni, bool cull);
/* Kart vertex: position and the colour (lighting group) it takes. Karts
   are untextured (u = v = 0); a textured part would draw through r_mesh. */
typedef struct {
    float x, y, z;
    uint16_t group, pad;
} KV;
/* r_mesh for KV vertices, with u = v = 0 and vertex i coloured
   col[v[i].group]; off holds idx times 32 (byte offsets of the staged
   vertices), for the fast path taken when no vertex is outside a clip
   plane. */
void r_mesh_kv(const KV *v, const uint32_t *col, int nv, const uint16_t *idx, const uint16_t *off, int ni,
               bool cull);

/* Camera-facing quad in world space (XMTRX must hold the view-projection). */
void r_billboard(V3 c, float w, float h, float rot, float u0, float v0, float u1, float v1, uint32_t col);
/* Upright billboard that only turns about Y (trees, signs). */
void r_billboard_y(V3 base, float w, float h, float u0, float v0, float u1, float v1, uint32_t col);
/* Screen-aligned streak from a to b (sparks). */
void r_streak(V3 a, V3 b, float width, uint32_t col_a, uint32_t col_b);
/* Streak with per-end widths and V coordinates (flames). */
void r_streak_ex(V3 a, V3 b, float width_a, float width_b, uint32_t col_a, uint32_t col_b,
                 float v_a, float v_b);

/* 2D overlay helpers; z larger than any world depth. */
void r_quad2d(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
              uint32_t col, float z);
void r_quad2d_grad(float x0, float y0, float x1, float y1, uint32_t top, uint32_t bottom, float z);
void r_tri2d(float x0, float y0, float x1, float y1, float x2, float y2,
             uint32_t c0, uint32_t c1, uint32_t c2, float z);
void r_quad2d_pts(const float *xy, const float *uv, const uint32_t *col, float z);

bool r_sphere_visible(V3 c, float radius);
bool r_project(V3 p, float *sx, float *sy, float *inv_w);
float r_view_depth(V3 p);

/* Column-major 4x4 helpers. */
void m4_identity(float *m);
void m4_mul(float *out, const float *a, const float *b);
void m4_translate_rotate(float *m, V3 t, float yaw, float pitch, float roll);
void m4_rot_x(float *m, float a);
void m4_rot_y(float *m, float a);
void m4_rot_z(float *m, float a);
void m4_rot_axis(float *m, V3 axis, float a);
void m4_scale_apply(float *m, float sx, float sy, float sz);

typedef struct {
    int tris;          /* polygons submitted this frame */
    int verts;
} RenderStats;
extern RenderStats rstats;
extern Camera r_cam;
extern pvr_ptr_t r_textures[];

#endif
