/* Ridgeline levels: the areas to drive, one loaded at a time.

   Every area is a 257 x 257 heightfield 3 m to the vertex with a surface,
   water, relighting inputs and a horizon map per vertex (tools/
   build_level.py packs them; level.c unpacks the chosen one), and lists
   of what stands on it: routes, obstacles, trees, rock formations, props,
   viewpoints and the start grid, in assets/generated/level_<area>.h.

   The rest of the game reads the loaded area through `lvl` and the LVL_
   and lvl_ names below, which were one level's constants before there
   were two. */
#ifndef RL_LEVEL_H
#define RL_LEVEL_H

#include <stdbool.h>
#include <stdint.h>

enum { AREA_CASCADE, AREA_MOAB, AREA_DUNES, AREA_COUNT };

/* The grid, the same for every area (tools/levelkit.py). */
#define LEVEL_N 257
#define LEVEL_D 3.0f
#define LEVEL_ORG -384.0f
#define LEVEL_SIZE 768.0f
/* The horizon map: the skyline's elevation in half-degrees from every
   other vertex, in LEVEL_HZ_DIRS bearings from LEVEL_HZ_AZ0 degrees. */
#define LEVEL_HZ_N 129
#define LEVEL_HZ_DIRS 21
#define LEVEL_HZ_AZ0 45
#define LEVEL_HZ_STEP 15

enum {
    SURF_GRASS, SURF_FOREST, SURF_DIRT, SURF_GRAVEL, SURF_ROCK, SURF_MUD, SURF_SCREE, SURF_SNOW, SURF_WETROCK,
    SURF_SAND, SURF_SLICKROCK, SURF_HARDPACK, SURF_DEEPSAND, SURF_COUNT
};

enum {
    PROP_CABIN, PROP_SIGN, PROP_TABLE, PROP_CAIRN, PROP_TOWER, PROP_BRIDGE, PROP_WRECK, PROP_ARCH, PROP_TENT,
    PROP_KIOSK, PROP_RAMADA,
    PROP_PANEL,     /* a slab of varnished rock with petroglyphs pecked into it */
    PROP_TRACKS,    /* a dinosaur's trackway across the slickrock, from here along yaw */
    PROP_ADIT,      /* a mine's portal in the foot of a cliff, rails and an ore cart on its dump */
    PROP_PLATE,     /* a trail's rating on a post: var 0 green circle, 1 blue square, 2 black diamond */
    PROP_PYLON,     /* a powerline's H-frame (dunes.c hangs the wires between them) */
    PROP_CAMPER,    /* a travel trailer parked up, its awning out */
    PROP_FLAG,      /* a whip and its pennant stuck in the sand */
    PROP_FIREPIT,   /* a ring of stones with a fire in it (campfire.c), camp chairs and a cooler by it */
    PROP_QUAD,      /* a quad bike parked up, a whip on its tail */
    PROP_RAIL,      /* a sand rail: a tube-framed buggy on paddle tyres */
    PROP_KIND_COUNT
};
#define LVL_BRIDGE_HX 2.90f
#define LVL_BRIDGE_HZ 7.00f

/* Routes: points every ~4 m with heights, for the CPU drivers, the map
   and progress. Route 0 runs from the start to the fork, 1 to 3 are the
   ways up from there and 4 the last climb they share. */
typedef struct { float x, y, z; } lvl_pt_t;
typedef struct {
    const char *name, *short_name;
    const lvl_pt_t *pts;
    uint16_t n;
    uint8_t difficulty;
    float half;
} lvl_route_t;
#define LVL_ROUTE_SLOTS 5

/* Obstacles the trucks collide with. Spheres (boulders): centre, radius. */
typedef struct { float x, y, z, r; } lvl_sphere_t;
/* Capsules (logs): end points, radius. */
typedef struct { float ax, ay, az, bx, by, bz, r; } lvl_capsule_t;
/* Boxes (rock ledges, buildings): centre, half extents, yaw about Y. The
   first nrock_boxes are rock ledges, drawn as rock; the rest are props'. */
typedef struct { float x, y, z, hx, hy, hz, yaw; } lvl_box_t;
/* Posts (the lookout's legs): from the foot up to the top, radius r. */
typedef struct { float x0, y0, z0, x1, y1, z1, r; } lvl_post_t;
/* Trees: base, kind (by area: world.c), scale, turn. */
typedef struct { float x, y, z; uint8_t kind; uint8_t pad[3]; float scale, yaw; } lvl_tree_t;
typedef struct { float x, y, z, yaw; uint8_t kind, var; uint8_t pad[2]; } lvl_prop_t;
/* Viewpoints to find: where, and the name shown. */
typedef struct { float x, y, z; const char *name; } lvl_view_t;
/* Where mist lies at dawn and dusk: centre and radius. */
typedef struct { float x, z, r; } lvl_mist_t;
/* Hawks circling: centre, radius, height over the ground, phase, turn. */
typedef struct { float x, z, r, h, phase, dir; } lvl_hawk_t;

/* Rock formations (arches, balanced rocks, goblins, slabs): each a skin
   lofted over a run of rings along its spine. A ring is an ellipse about
   its centre, across the spine: ra to the side, rb the other way (up, for
   a spine lying flat). world.c builds the skin and roughens it. */
typedef struct { float x, y, z, ra, rb; } lvl_ring_t;
enum {
    ROCK_SMOOTH = 1,    /* hardly roughened: a slab */
    ROCK_CAP = 2,       /* a harder, paler bed across the top third */
    ROCK_DARK = 4       /* varnished: a darker family of the area's rock */
};
typedef struct {
    uint16_t first, n;      /* its rings in lvl_rings */
    uint8_t sides;          /* round the ring, near */
    uint8_t flags;
    uint16_t seed;
} lvl_rock_t;

#define LVL_MAX_TREES 2048
#define LVL_MAX_BOXES 96
#define LVL_MAX_PROPS 56
#define LVL_MAX_VIEWS 16
#define LVL_MAX_ROCKS 96
#define LVL_ROCK_RINGS 24      /* the most rings one rock is lofted over */

typedef struct {
    const char *name;               /* "CASCADE RIDGE" */
    const char *blurb;              /* a line for the area select */
    int base_ft;                    /* the altitude of height 0, for the HUD */
    float lake_y, start_y;
    float summit_x, summit_y, summit_z;
    const lvl_route_t *routes;
    const lvl_sphere_t *spheres;
    const lvl_capsule_t *capsules;
    const lvl_box_t *boxes;
    const lvl_post_t *posts;
    const lvl_tree_t *trees;
    const lvl_prop_t *props;
    const lvl_view_t *views;
    const float (*spawns)[4];       /* the start grid: position and heading (radians, 0 = north) */
    const lvl_mist_t *mists;
    const lvl_hawk_t *hawks;
    const float (*herds)[2];        /* where the deer graze */
    const lvl_ring_t *rings;
    const lvl_rock_t *rocks;
    uint16_t nroutes, nspheres, ncapsules, nboxes, nrock_boxes, nposts, ntrees, nprops, nviews, nspawns, nmists,
        nhawks, nherds, nrings, nrocks;
} RlLevel;

/* How an area looks and sounds, beyond what its level lists: what the
   code that draws it asks. */
typedef struct {
    int ground_tex;                /* TEX_*: the detail the ground's colours tint */
    uint32_t water_shallow, water_deep;
    uint8_t wet[3];                /* what a bed under water tints toward */
    uint32_t boulder[3];           /* boulders' albedos, picked by each one's seed */
    uint32_t ledge;                /* rock ledges' */
    uint32_t dust;                 /* what tyres raise off dry ground; 0: by the surface */
    bool snow;                     /* snow blows about the top */
    bool fireflies;                /* over the grass after dusk */
    bool cloud_cap;                /* a cap of cloud sits on the summit */
    float woods;                   /* how much of the forest's sound: birdsong, the trees */
    bool lake;                     /* still water by the acre: drawn coarser sooner (water.c), and quieter */
    bool fit_split;                /* each cell of the ground split the way its heights lie (terrain.c) */
} LvlLook;

extern const RlLevel *lvl;
extern const LvlLook *look;
extern int lvl_area;               /* AREA_* of the level loaded */
extern const RlLevel *const rl_levels[AREA_COUNT];

#define LVL_ROUTES (lvl->nroutes)
#define lvl_routes (lvl->routes)
#define LVL_SPHERES (lvl->nspheres)
#define lvl_spheres (lvl->spheres)
#define LVL_CAPSULES (lvl->ncapsules)
#define lvl_capsules (lvl->capsules)
#define LVL_BOXES (lvl->nboxes)
#define LVL_ROCK_BOXES (lvl->nrock_boxes)
#define lvl_boxes (lvl->boxes)
#define LVL_POSTS (lvl->nposts)
#define lvl_posts (lvl->posts)
#define LVL_TREES (lvl->ntrees)
#define lvl_trees (lvl->trees)
#define LVL_PROPS (lvl->nprops)
#define lvl_props (lvl->props)
#define LVL_VIEWS (lvl->nviews)
#define lvl_views (lvl->views)
#define LVL_SPAWNS (lvl->nspawns)
#define lvl_spawns (lvl->spawns)
#define LEVEL_LAKE_Y (lvl->lake_y)
#define LEVEL_START_Y (lvl->start_y)
#define LEVEL_SUMMIT_X (lvl->summit_x)
#define LEVEL_SUMMIT_Y (lvl->summit_y)
#define LEVEL_SUMMIT_Z (lvl->summit_z)

/* The loaded area's grid, unpacked: heights beside the ground's colours
   (terrain.h), the water's surface (-1000: none), the surface ids, the
   relighting inputs and the horizon map. */
#define LVL_FACE 1   /* LvlLight.flags: on a cliff's face */
typedef struct { uint8_t r, g, b; int8_t nx, nz; uint8_t deep, flags, pad; } LvlLight;
typedef struct {
    void *ground;            /* GroundVert[N * N] */
    float *water;
    uint8_t *surf;
    LvlLight *light;
    uint8_t *hz;             /* [LEVEL_HZ_DIRS][LEVEL_HZ_N][LEVEL_HZ_N] */
} LvlGrid;
#define LEVEL_GRID_BYTES (LEVEL_N * LEVEL_N * (8 + 4 + 8) + ((LEVEL_N * LEVEL_N + 3) & ~3) + \
                          LEVEL_HZ_DIRS * LEVEL_HZ_N * LEVEL_HZ_N + 32)

/* Make area the level in use and unpack its grid into buf (32-byte
   aligned, LEVEL_GRID_BYTES). The ground's colours are left for the clock
   to light (tod.c). False if the packed grid is damaged. */
bool level_load(int area, void *buf, LvlGrid *out);

#endif
