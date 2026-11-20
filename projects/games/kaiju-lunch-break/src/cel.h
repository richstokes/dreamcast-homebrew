/* The comic-book look: two hard bands of light and a black ink line.

   Flat surfaces (buildings, vehicles) take one of three tones by how far
   they face the sun. Curved ones (the kaiju) look the tone up in a ramp
   texture by n.L at each vertex, so the terminator is a hard line across
   a triangle rather than a Gouraud smear. The ink is a shell: the surface
   pushed out a little and drawn black with only its back faces showing,
   which leaves a line round every silhouette. */
#ifndef KL_CEL_H
#define KL_CEL_H

#include "kl.h"
#include "engine/mesh.h"

#define INK_COL 0xff16121eu
extern const V3 cel_sun;            /* toward the sun */

void cel_init(void);
/* A flat face's colour: tint in the light, mid tone or shade. */
uint32_t cel_band(uint32_t tint, float ndotl);
/* Untextured black, back faces only: for ink shells built as strips. */
const pvr_poly_hdr_t *cel_ink_hdr(void);
const pvr_poly_hdr_t *cel_flat_hdr(void);

/* A ramp-shaded model with its ink shell (tools/build_models.py). */
typedef struct {
    RV *v;
    const eng_mvert_t *src;
    const uint16_t *idx;
    int nv, ni;
    EngMesh ink;
} CelMesh;
bool cel_mesh_init(CelMesh *c, int km, int km_ink);
/* plate: the colour of paint slot 2; flash: 0-1 toward white. */
void cel_mesh_draw(const CelMesh *c, const Xf *xf, uint32_t plate, float flash);

/* A flat-shaded model: each lighting group takes one band. */
typedef struct {
    EngMesh m, ink;
    bool has_ink;
} FlatMesh;
enum { FM_CAR, FM_BUS, FM_TANK, FM_TURRET, FM_HELI, FM_ROTOR, FM_JET, FM_CUBE, FM_DONUT, FM_WTANK, FM_MISSILE, FM_COUNT };
extern FlatMesh cel_flat[FM_COUNT];
bool flat_mesh_init(FlatMesh *f, int km, int km_ink);
/* own: the colour of paint slot 1. */
void flat_mesh_draw(const FlatMesh *f, const Xf *xf, uint32_t own, bool ink);
/* The same under a matrix with scale in it (l: the sun in the model's axes). */
void flat_mesh_draw_m(const FlatMesh *f, const float *m, V3 l, uint32_t own, bool ink);

#endif
