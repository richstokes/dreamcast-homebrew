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

void terrain_init(void);

/* The heightfield alone, triangle by triangle as it is drawn. */
float terrain_height(float x, float z);
float terrain_height_n(float x, float z, V3 *n);
/* What a tyre stands on at (x, z): the terrain or the top of a boulder,
   log or ledge there, whichever is higher; its normal and surface. */
float ground_at(float x, float z, V3 *n, int *surf);
/* ground_at, but an obstacle whose top is above ymax isn't ground: it's a
   wall there (obstacle_contacts pushes back). Without the limit, a tyre
   touching the footprint of something tall (the lookout tower) found the
   ground metres above it and the bump stop launched the truck. */
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
   drawn; the prebuilt far terrain draws with the scenery. */
int terrain_draw_near(void);
/* The prebuilt far terrain's skirts, where a neighbour's grid differs. */
int terrain_draw_far_skirts(const Scenery *sc);
/* Water chunks, back to front. */
void terrain_draw_water(const Scenery *sc);

/* The ground's vertices: height and colour side by side, so the terrain
   drawn each frame reads one cache line for both. The colours are relit
   as the day goes by (tod.c), in a copy of the level's. */
typedef struct { float h; uint32_t c; } GroundVert;
extern const GroundVert *lvl_ground;

extern const float *lvl_water;
extern const uint8_t *lvl_surf;

/* Relighting (tod.c): a pass colours grid vertices [from, to) for the
   palette and light given to setup (from the level's albedo, normals and
   horizon map), then copies them into the prebuilt far terrain's pool
   vertices [from, to) of terrain_pool_n(), tinting the water too. */
void terrain_relight_setup(const Palette *p, V3 light);
void terrain_relight(int from, int to);
int terrain_pool_n(void);
void terrain_relight_pool(Scenery *sc, int from, int to);

static inline int lvl_clampi(int v) { return v < 0 ? 0 : v > LEVEL_N - 1 ? LEVEL_N - 1 : v; }
static inline float lvl_h(int i, int j) { return lvl_ground[lvl_clampi(j) * LEVEL_N + lvl_clampi(i)].h; }
static inline uint32_t lvl_c(int i, int j) { return lvl_ground[lvl_clampi(j) * LEVEL_N + lvl_clampi(i)].c; }

#endif
