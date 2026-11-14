/* Ridgeline terrain: the level's heightfield, surfaces and water, the
   obstacles the trucks hit, and the queries the physics asks of them. */
#ifndef RL_TERRAIN_H
#define RL_TERRAIN_H

#include "rl.h"
#include "level.h"
#include "engine/scenery.h"
#include "world.h"

/* How a surface drives: peak friction, rolling resistance (per unit load),
   how far a tyre sinks in (m), how bumpy it is (m) and how much grip a
   sliding tyre keeps. */
typedef struct {
    float mu, rr, sink, rough;
    float slide;        /* grip left once a tyre slides, of the peak: loose ground keeps most */
    uint32_t dust;      /* colour of what the tyres throw up */
    const char *name;
} SurfProps;
extern const SurfProps surf_props[SURF_COUNT];

/* Make area the level in use: unpack its grid into buf (32-byte aligned,
   LEVEL_GRID_BYTES, kept by the caller) and index its obstacles. The
   ground has no colours until the clock lights it (tod_init). */
bool terrain_load(int area, void *buf);

/* The heightfield alone, triangle by triangle as it is drawn. */
float terrain_height(float x, float z);
float terrain_height_n(float x, float z, V3 *n);
/* What a tyre stands on at (x, z): the terrain or the top of a boulder,
   log or ledge there, whichever is higher; its normal and surface. */
float ground_at(float x, float z, V3 *n, int *surf);
/* ground_at, but an obstacle whose top is above ymax isn't ground: it's a
   wall there (obstacle_contacts pushes back). Without the limit, a tyre
   touching the footprint of something tall (the cabin) found the ground
   metres above it and the bump stop launched the truck. The lookout's legs
   are lvl_posts, which aren't ground at all: truck.c tests them against
   the whole hull. */
float ground_below(float x, float z, float ymax, V3 *n, int *surf);
int surface_at(float x, float z);
/* The water's surface at (x, z), or -1000 where there is none. */
float water_at(float x, float z);

/* Contacts of a sphere (centre p, radius r) with the obstacles' volumes:
   boulders, logs, ledges and buildings, and tree trunks. Each gives the
   depth and the direction to push p out. */
typedef struct { V3 n; float depth; } ObsContact;
int obstacle_contacts(V3 p, float r, ObsContact *out, int max);

/* Terrain chunks (three levels of detail with skirts) and the water's
   surface into the scenery. */
enum { TAG_TERRAIN, TAG_WATER, TAG_TREES, TAG_ROCKS, TAG_PROPS, TAG_SKIRT };
void terrain_build(Scenery *sc);
/* The near terrain (within ~210 m), built from the heightmap as it is
   drawn. */
int terrain_draw_near(void);
/* A texture laid straight down on the near terrain, as a pool of light
   is: the triangles terrain_draw_near last drew, lift above them, under
   the convex quad of (x, z) corners. Being the ground's own triangles it
   bends at every crease the ground has (a patch with vertices of its own
   cuts under the ground between them). u, v, the height of the light's
   axis and how far from it the light is whole are each {at the origin,
   per metre of x, per metre of z}: ground further from the axis fades,
   and is dark at twice that, so a beam is a spot on a cliff and not a
   stripe up it. The header is the caller's. Returns the vertices sent. */
typedef struct {
    float quad[4][2];
    float u[3], v[3], axis[3], reach[3];
    float lift;
    uint32_t col;
} TerDecal;
int terrain_decal(const TerDecal *d);
/* The prebuilt far terrain, and its skirts where a neighbour's grid
   differs: the blocks' vertices sent, the skirts' in *skirts. */
int terrain_draw_far(const Scenery *sc, int *skirts);
/* Water chunks, back to front. */
/* The terrain pool's relight sources, one a vertex (water.c reads its own). */
const uint32_t *terrain_pool_src(void);

/* The ground's vertices: height and colour side by side, so the terrain
   drawn each frame reads one cache line for both. The colours are relit
   as the day goes by (tod.c), in a copy of the level's. */
typedef struct { float h; uint32_t c; } GroundVert;
extern const GroundVert *lvl_ground;

extern const float *lvl_water;
extern const uint8_t *lvl_surf;
extern const LvlLight *lvl_light;

/* Relighting (tod.c): a pass colours grid vertices [from, to) for the
   palette and light given to setup (from the level's albedo, normals and
   horizon map), then copies them into the prebuilt far terrain's pool
   vertices [from, to) of terrain_pool_n(), tinting the water too. */
void terrain_relight_setup(const Palette *p, V3 light);
/* What the ground is lit by under a palette: the sky's weight on a level
   face and the sun's on one square to it. */
void terrain_light(const Palette *p, float sky[3], float sun[3]);
void terrain_relight(int from, int to);
int terrain_pool_n(void);
void terrain_relight_pool(Scenery *sc, int from, int to);
/* The water's vertices [from, to) of terrain_water_n(), coloured for the
   pass before the pool copies them: each costs several of the ground's,
   and a loch's worth met in the pool's cheap copies was a 5 ms frame on
   the console. */
int terrain_water_n(void);
void terrain_relight_water(int from, int to);

static inline int lvl_clampi(int v) { return v < 0 ? 0 : v > LEVEL_N - 1 ? LEVEL_N - 1 : v; }
static inline float lvl_h(int i, int j) { return lvl_ground[lvl_clampi(j) * LEVEL_N + lvl_clampi(i)].h; }
static inline uint32_t lvl_c(int i, int j) { return lvl_ground[lvl_clampi(j) * LEVEL_N + lvl_clampi(i)].c; }

#endif
