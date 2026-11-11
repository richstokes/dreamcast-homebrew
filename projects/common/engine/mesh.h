/* Engine: rigid meshes for r_mesh_kv (vehicles, characters, props that
   move).

   A model builder's output (a triangle list whose vertices carry a
   position, an int8 normal, a paint slot and a colour) becomes a KMesh at
   boot: the triangles as strips, the distinct positions r_mesh_kv
   transforms, and lighting groups. Vertices that share a normal, a paint
   slot and a colour light alike, so the game lights each group once a
   frame (eng_mesh_light) and every vertex takes its group's colour.

   Measured in DreamKarts on the console: transforming distinct positions
   only and sending strips saved 0.2-0.4 ms CPU and 0.05-0.25 ms of render
   time over a plain triangle list, and lighting per group rather than per
   vertex was the biggest single win. */
#ifndef ENG_MESH_H
#define ENG_MESH_H

#include "render.h"

/* A model vertex as the builders write it: normal * 127. */
typedef struct {
    float x, y, z;
    int8_t nx, ny, nz;
    uint8_t paint;
    uint32_t argb;
} eng_mvert_t;

typedef struct {
    KMesh km;               /* for r_mesh_kv */
    int ngroups;
    float (*gnrm)[3];       /* each group's unit normal */
    uint8_t *gpaint;        /* its paint slot */
    uint32_t *gargb;        /* its colour */
    uint16_t *gvert;        /* one of its vertices, for attributes the game keeps per vertex */
    float radius;           /* bounding sphere about the mesh's origin */
    float (*gcolf)[4];      /* eng_mesh_light's: each group's colour as floats and its brightness, made on first use */
    int nslots;             /* ...and one past the highest paint slot in use */
} EngMesh;

/* Build m from nv vertices and ni indices (a triangle list, wound so that
   cross(b - a, c - a) faces out). key, if given, is one more byte per
   vertex that joins the lighting group key (a blend weight the game
   paints by, say). Allocates; false, with nothing left allocated, if out
   of memory or over the renderer's limits. */
bool eng_mesh_build(EngMesh *m, const eng_mvert_t *v, int nv, const uint16_t *idx, int ni, const uint8_t *key);
void eng_mesh_free(EngMesh *m);
/* Strips and strip vertices in m (its triangles come as km.ni / 3). */
void eng_mesh_stats(const EngMesh *m, int *strips, int *strip_verts);

/* The usual lighting: each group's colour (or paint[slot] for slots with a
   non-zero entry, scaled by the vertex's brightness against ref[slot]) lit
   by a sun from direction L (unit, in mesh space, toward the light), a
   sky from above and a bounce from below:
       c * (ambient + sky * (0.5 + 0.5 ny) + sun * max(0, n.L))
   unlit_mask: paint slots drawn at full colour (lamps, lit lenses).
   Paint slots are below ENG_PAINT_SLOTS. Clobbers XMTRX (the three
   channels' light comes from one FTRV): load the model's matrix after. */
#define ENG_PAINT_SLOTS 16
typedef struct {
    V3 L;
    float sun[3], sky[3], ambient[3];
    const uint32_t *paint;      /* per slot, 0: keep the vertex colour */
    const uint32_t *ref;        /* per slot reference colour for paint */
    uint32_t unlit_mask;
} EngLight;
void eng_mesh_light(const EngMesh *m, const EngLight *l, uint32_t *out);

/* When to light a model instance again. A moving model's light turns
   slowly, so its groups' colours can be kept and worked out every other
   frame: half the lighting, and, with slot (the instance's number, say)
   putting half of them on odd frames and half on even, about the same
   work each frame. a is the instance's record (zeroed: never lit). key is
   everything that changes its colours at once: lamps, paint, which mesh
   or level of detail is drawn. frame counts displayed frames from 1.
   True: light it now (the record is updated); false: last frame's colours
   still stand. A second call in one frame (another view) is false. */
typedef struct { uint32_t key, frame; } EngLitAge;
static inline bool eng_lit_due(EngLitAge *a, uint32_t key, uint32_t frame, unsigned slot) {
    if(a->frame == frame) return false;
    if(a->frame && a->key == key && frame - a->frame < 2 && ((frame + slot) & 1)) return false;
    a->key = key;
    a->frame = frame;
    return true;
}

#endif
