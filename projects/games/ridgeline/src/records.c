/* Ridgeline best times (records.h). The way up is worked out as the player
   climbs: every few frames the distance driven since the last look goes
   to the climbing route the truck is on (within 9 m of its centreline),
   or to their own way; distance on the shared forest road and summit
   trail counts for neither. */
#include "records.h"
#include "../../../common/vmusave.h"
#include "truck.h"
#include "terrain.h"
#include <stddef.h>
#include <string.h>

#define FILENAME "RIDGELINE"
#define NEAR_M 9.0f

typedef struct {
    char magic[8];
    int32_t version;
    uint32_t best[2][WAY_COUNT];   /* tenths of a second, 0 for none: Cascade 84, Kestrel */
    char name[12];                 /* the online name (version 2; version 1 saves end before it) */
    /* The trucks after the first two (Bobcat). Appended to version 2 so
       older builds still read the rest; a save without it is shorter. */
    uint32_t best_more[TRUCK_MODELS - 2][WAY_COUNT];
} record_t;
#define RECORD_V1_BYTES (int)offsetof(record_t, name)
#define RECORD_V2_BYTES (int)offsetof(record_t, best_more)

/* Test builds never touch a VMU, except the save test, which runs on
   blank cards (see the README). */
#if !defined(RL_AUTOTEST) || defined(RL_AT_VMUTEST)
#define RL_USE_VMU 1
static const char magic[8] = {'R', 'I', 'D', 'G', 'E', 'S', 'V', '1'};
static int card_port = -1, card_unit = -1;
#endif
static record_t rec;

static float tally[WAY_COUNT];
static V3 last_pos;
static bool have_last, jumped;
static int frame;

void records_load(void) {
    memset(&rec, 0, sizeof(rec));
#ifdef RL_USE_VMU
    maple_device_t *dev = vmusave_card(&card_port, &card_unit);
    if(!dev) return;
    record_t r;
    memset(&r, 0, sizeof(r));
    int n = vmusave_read(dev, FILENAME, &r, sizeof(r));
    if(n < RECORD_V1_BYTES || memcmp(r.magic, magic, sizeof(magic))) return;
    if(!(r.version == 1 && n >= RECORD_V1_BYTES) && !(r.version == 2 && n >= RECORD_V2_BYTES)) return;
    if(r.version == 1) memset(r.name, 0, sizeof(r.name));
    if(n < (int)sizeof(r)) memset(r.best_more, 0, sizeof(r.best_more));
    r.name[sizeof(r.name) - 1] = '\0';
    rec = r;
    printf("Ridgeline records: loaded from VMU %c%d\n", 'A' + dev->port, dev->unit);
#endif
}

#ifdef RL_USE_VMU
/* A mountain for the VMU file manager. */
static const char *const icon_art[32] = {
    "................................",
    "................................",
    "................................",
    "..........................YY....",
    ".........................YYYY...",
    "..........................YY....",
    "................................",
    "...............SS...............",
    "..............SSSS..............",
    ".............SSSRSS.............",
    "............SSRRRSSS............",
    "...........SSRRRRRRSS...........",
    "..........RRRRRRRRRRRR..........",
    ".........RRRRRRRRRRRRRR.........",
    "........RRRRRRRRRRRRRRRR........",
    ".......RRRRRRRRRRRRRRRRRR.......",
    "......GRRRRRRRRRRRRRRRRRRR......",
    ".....GGGRRRRRRRRRRRRRRRRRRG.....",
    "....GGGGGRRRRRRRRRRRRRRRRGGG....",
    "...GGGGGGGGRRRRRRRRRRRRGGGGGG...",
    "..GGGGGGGGGGGGRRRRRRGGGGGGGGGG..",
    ".GGGGGGGGGGGGGGGGGGGGGGGGGGGGGG.",
    "GGGGGGGGGGGGGGGGGGGGGGGGGGGGGGGG",
    "YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY",
    "YYYYYYYYYYYYYYYYYYYYYYYYYYYYYYYY",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
    "................................",
};
#endif

static void save(void) {
#ifdef RL_USE_VMU
    maple_device_t *dev = vmusave_card(&card_port, &card_unit);
    if(!dev) {
        printf("Ridgeline records: no VMU to save to\n");
        return;
    }
    static uint8_t pixels[512];
    static record_t r;
    r = rec;
    memcpy(r.magic, magic, sizeof(magic));
    r.version = 2;
    vmu_pkg_t pkg;
    vmusave_header(&pkg, "RIDGELINE", "Ridgeline", "Ridgeline best times");
    /* Palette (ARGB4444): blank, snow, rock, forest, amber. */
    static const uint16_t palette[] = {0x0000, 0xffff, 0xf888, 0xf363, 0xfea3};
    vmusave_icon(&pkg, pixels, icon_art, ".SRGY", palette);
    if(vmusave_write(dev, FILENAME, &pkg, &r, sizeof(r)) < 0)
        printf("Ridgeline records: save failed on VMU %c%d\n", 'A' + dev->port, dev->unit);
    else
        printf("Ridgeline records: saved on VMU %c%d\n", 'A' + dev->port, dev->unit);
#endif
}

void records_start(void) {
    memset(tally, 0, sizeof(tally));
    have_last = jumped = false;
    frame = 0;
}

void records_jumped(void) {
    jumped = true;
}

static float route_dist2(int r, float x, float z) {
    const lvl_route_t *rt = &lvl_routes[r];
    float best = 1e12f;
    for(int k = 0; k < rt->n; k++) {
        float dx = rt->pts[k].x - x, dz = rt->pts[k].z - z, d = dx * dx + dz * dz;
        if(d < best) best = d;
    }
    return best;
}

void records_update(float dt) {
    (void)dt;
    if(++frame % 6) return;
    V3 p = trucks[0].pos;
    if(!have_last) {
        last_pos = p;
        have_last = true;
        return;
    }
    float dx = p.x - last_pos.x, dz = p.z - last_pos.z, moved = sqrtf(dx * dx + dz * dz);
    last_pos = p;
    if(moved > 10.0f) return;    /* recovered or moved: not driven */
    int near = -1;
    float best = NEAR_M * NEAR_M;
    for(int r = 0; r < LVL_ROUTES; r++) {
        float d = route_dist2(r, p.x, p.z);
        if(d < best) {
            best = d;
            near = r;
        }
    }
    if(near < 0) tally[WAY_OWN] += moved;
    else if(near >= 1 && near <= 3) tally[near - 1] += moved;
}

const char *records_way_name(int way) {
    static const char *const names[WAY_COUNT] = {"LOGGING ROAD", "RIDGE LINE", "CREEK BED", "YOUR OWN WAY"};
    return names[way];
}

void records_format(uint32_t tenths, char *out, int size) {
    snprintf(out, size, "%lu:%02lu.%lu", (unsigned long)(tenths / 600), (unsigned long)(tenths / 10 % 60),
             (unsigned long)(tenths % 10));
}

const char *records_name(void) {
    return rec.name;
}

void records_set_name(const char *name) {
    if(strncmp(rec.name, name, sizeof(rec.name) - 1) == 0) return;
    memset(rec.name, 0, sizeof(rec.name));
    strncpy(rec.name, name, sizeof(rec.name) - 1);
    save();
}

static uint32_t *best_slot(int truck, int way) {
    truck %= TRUCK_MODELS;
    return truck < 2 ? &rec.best[truck][way] : &rec.best_more[truck - 2][way];
}

uint32_t records_best(int truck, int way) {
    return *best_slot(truck, way);
}

bool records_summit(char *msg, int size) {
    char t[16];
    uint32_t tenths = (uint32_t)(game.clock * 10.0f + 0.5f);
    records_format(tenths, t, sizeof(t));
    if(jumped) {
        snprintf(msg, size, "%s   NO RECORD AFTER A VIEWPOINT JUMP", t);
        return false;
    }
    int way = WAY_LOGGING;
    for(int w = 1; w < 3; w++)
        if(tally[w] > tally[way]) way = w;
    if(tally[way] < tally[WAY_OWN]) way = WAY_OWN;
    uint32_t *b = best_slot(game.truck_model, way);
    bool better = *b == 0 || tenths < *b;
    if(better) {
        *b = tenths;
        snprintf(msg, size, "%s %s %s   NEW BEST!", t, way == WAY_OWN ? "BY" : "BY THE", records_way_name(way));
        save();
    }
    else {
        char bt[16];
        records_format(*b, bt, sizeof(bt));
        snprintf(msg, size, "%s %s %s   BEST %s", t, way == WAY_OWN ? "BY" : "BY THE", records_way_name(way), bt);
    }
    printf("Ridgeline records: summit %s by way %d (tally %.0f %.0f %.0f own %.0f m)%s\n", t, way, (double)tally[0],
           (double)tally[1], (double)tally[2], (double)tally[3], better ? ", a new best" : "");
    return better;
}
