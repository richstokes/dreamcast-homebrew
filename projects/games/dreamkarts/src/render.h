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
    HF_POINT = 1 << 7,
    HF_ZWRITE = 1 << 8,   /* translucent polygons that still write depth */
    HF_SPEC = 1 << 9      /* add the vertices' offset colour (r_strip_spec) */
};

#define TEX_NONE -1

/* The Tile Accelerator's vertex buffer (pvr_init): a hard ceiling, past
   which the console stalls (Tsudoken Drift's split screen passed 1 MB).
   1536 until the shadow volumes' bins needed the VRAM. */
#define DK_VTX_BUF_KIB 1472

void render_init(void);
void render_shutdown(void);
/* Mipmap D-adjust for the opaque (RGB565) mipmapped textures:
   DK_MIP_ROAD_BIAS for the surfaces driven on (render.c tex_mip_bias),
   DK_MIP_BIAS for everything beside them. The PVR picks a pixel's mip level
   from the long side of its texel footprint, and the chase camera sees the
   road at about 9 degrees, so at the hardware's own pick (1.0) the road's
   grain, edge lines and curbs go soft 10-20 m out. Flycast hides this when
   its anisotropic filtering is on (rend.AnisotropicFiltering).

   Measured on the console from fixed chase views (2026-10-26): 0.5 on
   every opaque texture picks one level sharper (on the road each level
   then reaches ~1.4x as far) for 2-7% more PVR time; the same on the
   ARGB4444 textures (foliage, sprites, effects) sharpens nothing that
   shows and more than doubles the extra time, so they stay at 1.0. 0.25
   aliases the boardwalk's planks and the supersample bit doubles the PVR
   time. The field only steps in quarters. Sharper means more shimmer, and
   road 0.5 with the sides at 1.0 was the best balance by eye in play on
   the console. The asphalt's square texels (world_int.h ROAD_V_M) and the
   Lanczos mips (tools/pvr_texture.py) help at no PVR cost. A compromise we
   may revisit, e.g. sides at 0.75 or a fractional bias baked into the mips. */
#ifndef DK_MIP_BIAS
#define DK_MIP_BIAS PVR_MIPBIAS_1_00
#endif
#ifndef DK_MIP_ROAD_BIAS
#define DK_MIP_ROAD_BIAS PVR_MIPBIAS_0_50
#endif
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
/* r_strip with an offset colour per vertex (headers made with HF_SPEC). */
void r_strip_spec(const RV *v, const uint32_t *spec, int n);
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
/* A kart mesh: KV vertices (u = v = 0, vertex i coloured by its group),
   the triangle list, and for the fast path taken when no vertex is outside
   a clip plane, the vertices' distinct positions {x, y, z, -} and the
   triangles as strips: {n, then n pairs (position index * 16, group * 4)}
   per strip, 0 after the last. Strip triangle k is vertices k, k+1, k+2,
   the first two swapped when k is odd; a strip has at most
   R_MESH_STRIP_TRIS triangles. */
#define R_MESH_STRIP_TRIS 32
typedef struct {
    const KV *v;
    int nv;
    const uint16_t *idx;
    int ni;
    const float (*pos)[4];
    int npos;
    const uint16_t *strips;
} KMesh;
/* Draw a kart mesh with back faces culled, coloured col[group]. */
void r_mesh_kv(const KMesh *m, const uint32_t *col);

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

/* A closed modifier volume in the opaque modifier list: triangles tri over
   the world points pts. Opaque and punch-through surfaces inside it keep
   DK_SHADOW_SCALE of their colour. Draws nothing and returns false when a
   point is behind the near plane: cut open there, the volume would turn
   inside out on screen. */
#ifndef DK_SHADOW_SCALE
#define DK_SHADOW_SCALE 0.75f
#endif
bool r_modvol(const V3 *pts, int np, const uint8_t (*tri)[3], int nt);

bool r_sphere_visible(V3 c, float radius);
/* r_sphere_visible's frustum: the near plane, and the screen's half-width
   and half-height a unit of depth away. */
#define R_NEAR_W 0.25f
void r_frustum_slopes(float *tx, float *ty);
bool r_project(V3 p, float *sx, float *sy, float *inv_w);
/* Depth along the view, and the near plane geometry is clipped at. */
float r_view_depth(V3 p);
#define R_NEAR_W 0.25f

/* Column-major 4x4 helpers. m4_mul and m4_translate_rotate compute in
   XMTRX (several times faster than C on the SH-4) and put it back before
   returning: fx.c builds kart matrices between billboards that need the
   view-projection still loaded. Any helper that touches XMTRX must too. */
void m4_identity(float *m);
void m4_mul(float *out, const float *a, const float *b);
void m4_translate_rotate(float *m, V3 t, float yaw, float pitch, float roll);
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
