/* Each track's own textures: what its world, scenery and FlappyKarts theme
   draw, apart from the textures every screen shares (the font, logo, big
   text and effect sprites), which render_init keeps resident. A track's
   list is uploaded into one shared VRAM pool when its world is built
   (render_load_tex_set), and the pool is sized for the biggest
   list, so a new track only needs its own list here, not VRAM beside
   every other track's art. Autotest builds name any pooled texture drawn
   on a track whose list leaves it out.

   The lists came from a drawn-header audit of a race on every track plus
   the camera tour. The seaside courses share one list (world.c draws
   their scenery), as do the two sky tracks. The sky and canyon road
   headers are copied into sk_road_hdr each frame, which that audit cannot
   see, so those are listed by hand. */
#include "engine/render.h"
#include "texture_assets.h"
#include "track.h"

static const int16_t seaside[] = {
    TEX_ROAD, TEX_BOARDWALK, TEX_CURB, TEX_SAND, TEX_GRASS, TEX_BARRIER, TEX_WOOD, TEX_WATER, TEX_FOAM, TEX_ROCK,
    TEX_BARK, TEX_FROND, TEX_PALM_FAR, TEX_CLOUD, TEX_MOUNTAINS, TEX_FLARE, TEX_DASH, TEX_CHECKER, TEX_THATCH,
    TEX_CROWD, TEX_BANNER, TEX_STONE, -1
};

static const int16_t sky[] = {
    TEX_DASH, TEX_CHECKER, TEX_SKYROAD_0, TEX_SKYROAD_1, TEX_SKYROAD_2, TEX_SKYROAD_3, TEX_SKYROAD_4, TEX_SKYROAD_5,
    TEX_SKYROAD_6, TEX_SKYROAD_7, TEX_SKYROAD_8, TEX_SKYROAD_9, TEX_SKYROAD_10, TEX_SKYROAD_11, TEX_SKYROAD_12,
    TEX_SKYROAD_13, TEX_SKYROAD_14, TEX_SKYROAD_15, TEX_NIGHTSKY, TEX_SKYLINE, TEX_MOON, TEX_CLOUDSEA, TEX_TOWER,
    TEX_SIGNS, -1
};

static const int16_t canyon[] = {
    TEX_CURB, TEX_BARRIER, TEX_WATER, TEX_BARK, TEX_FROND, TEX_PALM_FAR, TEX_CLOUD, TEX_DASH, TEX_CHECKER,
    TEX_THATCH, TEX_CANYONROAD, TEX_MEADOW, TEX_ISLANDROCK, TEX_EHCHECKER, TEX_SUNFLOWER, TEX_TOTEM, TEX_BUSH,
    TEX_WATERFALL, TEX_RETROSUN, -1
};

static const int16_t tsudoken[] = {
    TEX_CLOUD, TEX_DASH, TEX_CHECKER, TEX_TKROAD, TEX_TKCURB, TEX_VERGE, TEX_PADDY, TEX_HOARDING, TEX_ISHIGAKI,
    TEX_KAWARA, TEX_SAKURA, TEX_SAKURAFAR, TEX_SAKURABARK, TEX_PETAL, TEX_TKCROWD, TEX_PLASTER, TEX_TKBANNER,
    TEX_PITWALL, TEX_SUNDISC, TEX_STREAM, -1
};

static const int16_t apple[] = {
    TEX_CLOUD, TEX_DASH, TEX_CHECKER, TEX_AVROAD, TEX_AVCURB, TEX_AVDIRT, TEX_AVDESERT, TEX_AVTYRES, TEX_AVMETAL,
    TEX_AVCROWD, TEX_AVJOSHUA, TEX_AVJBARK, TEX_AVSCRUB, TEX_AVBANNER, TEX_AVSTUCCO, TEX_AVROCK, TEX_AVFAR,
    TEX_AVTUMBLE, TEX_AVDUST, TEX_AVMTN, TEX_AVRANGE, -1
};

static const int16_t sf[] = {
    TEX_CLOUD, TEX_FLARE, TEX_DASH, TEX_CHECKER, TEX_SFROAD, TEX_SFRAILS, TEX_SFBRICK, TEX_SFCURB, TEX_SFWALK, TEX_SFRAILING, TEX_SFPIER,
    TEX_SFPIERRAIL, TEX_SFFACADE, TEX_SFBLDG, TEX_SFROOF, TEX_SFHEDGE, TEX_SFSHED, TEX_SFSIGN, TEX_SFBAY, TEX_SFFOG,
    TEX_SFPALM, TEX_SFPALMBARK, TEX_SFCABLECAR, TEX_SFFACADE2, TEX_SFSTREET, TEX_SFGRASS, TEX_SFSTALL, TEX_SFSIGNS, -1
};

/* Free Roam's lot borrows from Tsudoken, Apple Valley and the sky tracks. */
static const int16_t lot[] = {
    TEX_ROAD, TEX_CURB, TEX_GRASS, TEX_WATER, TEX_DASH, TEX_CHECKER, TEX_BANNER, TEX_SKYROAD_0, TEX_SKYROAD_1,
    TEX_SKYROAD_2, TEX_SKYROAD_3, TEX_SKYROAD_4, TEX_SKYROAD_5, TEX_SKYROAD_6, TEX_SKYROAD_7, TEX_SKYROAD_8,
    TEX_SKYROAD_9, TEX_SKYROAD_10, TEX_SKYROAD_11, TEX_SKYROAD_12, TEX_SKYROAD_13, TEX_SKYROAD_14, TEX_SKYROAD_15,
    TEX_NIGHTSKY, TEX_SKYLINE, TEX_MOON, TEX_TOWER, TEX_SIGNS, TEX_TKROAD, TEX_ISHIGAKI, TEX_KAWARA, TEX_SAKURA,
    TEX_SAKURABARK, TEX_PLASTER, TEX_PITWALL, TEX_AVROAD, TEX_AVMETAL, TEX_AVCROWD, TEX_AVBANNER, TEX_AVSTUCCO, -1
};

const int16_t *const track_textures[TRACK_COUNT] = {
    [TRACK_BEACH] = seaside, [TRACK_FIGURE8] = seaside, [TRACK_HARBOUR] = seaside, [TRACK_TEMPLE] = seaside,
    [TRACK_SKYWAY] = sky, [TRACK_CYBERSTONE] = sky, [TRACK_CANYON] = canyon, [TRACK_TSUDOKEN] = tsudoken,
    [TRACK_APPLE] = apple, [TRACK_LOT] = lot, [TRACK_SF] = sf,
};
