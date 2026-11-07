/* Engine: static scenery. A level's fixed geometry (terrain, trees, rocks,
   buildings) is built once into a vertex pool as world-space strips, each
   with a bounding sphere, grouped into chunks with a sphere and a range of
   camera distances. Drawing walks the chunks of one list, skips those out
   of range or out of view, and sends each strip through r_strip_bounded,
   which takes whole strips on its sphere and tests vertices only for
   strips crossing the screen's edge.

   Levels of detail are chunks with adjoining ranges over the same ground:
   the near version for [0, 120) m and a coarser one for [120, 400) m, say.
   On the console a strip costs about as much as 4 vertices and a vertex
   ~0.4 us (DreamKarts' measurements), so fewer, longer strips win: join
   small pieces into one strip with sc_join(). */
#ifndef ENG_SCENERY_H
#define ENG_SCENERY_H

#include "render.h"

typedef struct {
    const pvr_poly_hdr_t *hdr;
    uint32_t first;
    uint16_t count;
    uint16_t pad;
    V3 c;               /* bounding sphere */
    float r;
} ScStrip;

typedef struct {
    V3 c;
    float r;
    V3 lc;              /* where the distance range is measured from */
    float near2, far2;  /* squared camera distance range */
    uint32_t first_strip;
    uint16_t nstrips;
    uint8_t list;       /* HF_OP, HF_PT or HF_TR */
    uint8_t tag;        /* the game's own: what kind of scenery */
} ScChunk;

typedef struct {
    RV *pool;
    uint32_t pool_n, pool_max;
    ScStrip *strips;
    uint32_t strip_n, strip_max;
    ScChunk *chunks;
    uint32_t chunk_n, chunk_max;
    bool full;          /* something did not fit */
    /* Builder state. */
    ScChunk *cur;
    bool lc_set;
    uint32_t strip_first;
    const pvr_poly_hdr_t *hdr;
} Scenery;

bool sc_init(Scenery *s, uint32_t max_verts, uint32_t max_strips, uint32_t max_chunks);
void sc_free(Scenery *s);
void sc_clear(Scenery *s);

/* Building: a chunk drawn in list between near and far metres of the
   camera, then strips in it. */
void sc_chunk_begin(Scenery *s, int list, float near_m, float far_m, int tag);
void sc_chunk_end(Scenery *s);
/* Measure the open chunk's distance range from p rather than its sphere's
   centre: the levels of detail of one patch of ground then share a
   reference point, so exactly one of them draws at any distance. */
void sc_chunk_lod_point(Scenery *s, V3 p);
void sc_strip_begin(Scenery *s, const pvr_poly_hdr_t *hdr);
void sc_vert(Scenery *s, V3 p, float u, float v, uint32_t c);
/* sc_vert joined on to the strip so far with a degenerate pair (the last
   vertex again, this one twice): many small pieces as one strip. The
   renderer's headers cull zero-area triangles (PVR_CULLING_SMALL), so the
   joins never show, on Flycast or the console. */
void sc_join(Scenery *s, V3 p, float u, float v, uint32_t c);
void sc_strip_end(Scenery *s);

/* Draw every chunk of list (HF_OP/HF_PT/HF_TR) the camera can see; r_cam
   must be set. tag_mask picks chunk tags (bit per tag). Returns the
   vertices handed to the renderer (before its culling). */
int sc_draw(const Scenery *s, int list, uint32_t tag_mask);
/* Draw one chunk (by index) if in range and in view, whatever its tag: for
   chunks the game picks itself. Returns the vertices handed over. */
int sc_draw_chunk(const Scenery *s, uint32_t index);

#endif
