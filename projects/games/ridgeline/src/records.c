/* Ridgeline best times (records.h). The way up is worked out as the player
   climbs: every few frames the distance driven since the last look goes
   to the climbing route the truck is on (within 9 m of its centreline),
   or to their own way; distance on the shared forest road and summit
   trail counts for neither.

   A save rewrites the file and the card's directory and FAT, a maple
   frame or more per block, so it runs on a worker thread: written inline
   it froze the summit message for about half a second on the console. */
#include "records.h"
#include "../../../common/vmusave.h"
#include "truck.h"
#include "terrain.h"
#include <kos/mutex.h>
#include <kos/thread.h>
#include <stddef.h>
#include <string.h>

#define FILENAME "RIDGELINE"
#define NEAR_M 9.0f

#define REC_TRUCKS 8   /* rows the file has room for */
_Static_assert(TRUCK_MODELS <= REC_TRUCKS, "the save has no row for this truck");

typedef struct {
    char magic[8];
    int32_t version;
    /* Cascade Ridge's times. */
    uint32_t best[2][WAY_COUNT];   /* tenths of a second, 0 for none: Cascade 84, Kestrel */
    char name[12];                 /* the online name (version 2; version 1 saves end before it) */
    /* The trucks after the first two (Bobcat, FFR, Taco, Goose, One Ten, Eagle...), a row each in
       model order. Appended to version 2 so older builds still read the
       rest; a save from a build with fewer trucks is shorter. */
    uint32_t best_more[REC_TRUCKS - 2][WAY_COUNT];
    /* The areas after Cascade Ridge in order, a row a truck. Still
       version 2: an older build reads what it knows and writes a shorter
       save, which forgets the rest. */
    uint32_t area_best[AREA_COUNT - 1][REC_TRUCKS][WAY_COUNT];
} record_t;
#define RECORD_V1_BYTES (int)offsetof(record_t, name)
#define RECORD_V2_BYTES (int)offsetof(record_t, best_more)

/* Test builds never touch a VMU, except the save test, which runs on
   blank cards (see the README). */
#if !defined(RL_AUTOTEST) || defined(RL_AT_VMUTEST)
#define RL_USE_VMU 1
static const char magic[8] = {'R', 'I', 'D', 'G', 'E', 'S', 'V', '1'};
static int card_port = -1, card_unit = -1;   /* the worker's, once it runs */

/* The record waiting for the card, and whether a worker is on it. */
static mutex_t lock = MUTEX_INITIALIZER;
static record_t pending;
static bool have_pending, worker_on;
#endif
static record_t rec;

static float tally[WAY_COUNT];
static V3 last_pos;
static bool have_last, jumped;
static int frame;

void records_load(void) {
    records_finish();
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
    /* Rows a shorter save lacks stay as cleared above: no time yet. A row
       cut short is cleared too. */
    int rows = n < RECORD_V2_BYTES ? 0 : (n - RECORD_V2_BYTES) / (int)sizeof(r.best_more[0]);
    for(int i = rows; i < REC_TRUCKS - 2; i++) memset(r.best_more[i], 0, sizeof(r.best_more[i]));
    /* And the areas: a save from before the latest of them is that
       much shorter. */
    int areas = n < (int)offsetof(record_t, area_best) ? 0 : (n - (int)offsetof(record_t, area_best)) / (int)sizeof(r.area_best[0]);
    for(int i = areas < 0 ? 0 : areas; i < AREA_COUNT - 1; i++) memset(r.area_best[i], 0, sizeof(r.area_best[i]));
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

#ifdef RL_USE_VMU
static void write_card(const record_t *r) {
    maple_device_t *dev = vmusave_card(&card_port, &card_unit);
    if(!dev) {
        printf("Ridgeline records: no VMU to save to\n");
        return;
    }
    static uint8_t pixels[512];
    vmu_pkg_t pkg;
    vmusave_header(&pkg, "RIDGELINE", "Ridgeline", "Ridgeline best times");
    /* Palette (ARGB4444): blank, snow, rock, forest, amber. */
    static const uint16_t palette[] = {0x0000, 0xffff, 0xf888, 0xf363, 0xfea3};
    vmusave_icon(&pkg, pixels, icon_art, ".SRGY", palette);
    if(vmusave_write(dev, FILENAME, &pkg, r, sizeof(*r)) < 0)
        printf("Ridgeline records: save failed on VMU %c%d\n", 'A' + dev->port, dev->unit);
    else
        printf("Ridgeline records: saved on VMU %c%d\n", 'A' + dev->port, dev->unit);
}

/* Writes the latest pending record until none is left. */
static void *worker(void *arg) {
    (void)arg;
    static record_t r;
    mutex_lock(&lock);
    while(have_pending) {
        r = pending;
        have_pending = false;
        mutex_unlock(&lock);
        write_card(&r);
        mutex_lock(&lock);
    }
    worker_on = false;
    mutex_unlock(&lock);
    return NULL;
}
#endif

static void save(void) {
#ifdef RL_USE_VMU
    mutex_lock(&lock);
    pending = rec;
    memcpy(pending.magic, magic, sizeof(magic));
    pending.version = 2;
    have_pending = true;
    if(!worker_on) worker_on = thd_create(true, worker, NULL) != NULL;
    bool inline_write = !worker_on;
    mutex_unlock(&lock);
    if(inline_write) {   /* no thread to be had: save the slow way */
        mutex_lock(&lock);
        record_t r = pending;
        have_pending = false;
        mutex_unlock(&lock);
        write_card(&r);
    }
#endif
}

void records_finish(void) {
#ifdef RL_USE_VMU
    for(int i = 0; i < 300; i++) {   /* 3 s at most, for a card that never answers */
        mutex_lock(&lock);
        bool busy = worker_on || have_pending;
        mutex_unlock(&lock);
        if(!busy) return;
        thd_sleep(10);
    }
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
    return way < WAY_OWN ? lvl_routes[1 + way].name : "YOUR OWN WAY";
}

const char *records_way_short(int way) {
    return way < WAY_OWN ? lvl_routes[1 + way].short_name : "OWN WAY";
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
    if(game.area != AREA_CASCADE) return &rec.area_best[game.area - 1][truck][way];
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
    /* "BY THE LOGGING ROAD", "BY THE FINS", "BY YOUR OWN WAY". */
    const char *wn = records_way_name(way);
    const char *by = way == WAY_OWN || !strncmp(wn, "THE ", 4) ? "BY" : "BY THE";
    if(better) {
        *b = tenths;
        snprintf(msg, size, "%s %s %s   NEW BEST!", t, by, wn);
        save();
    }
    else {
        char bt[16];
        records_format(*b, bt, sizeof(bt));
        snprintf(msg, size, "%s %s %s   BEST %s", t, by, wn, bt);
    }
    printf("Ridgeline records: summit %s by way %d (tally %.0f %.0f %.0f own %.0f m)%s\n", t, way, (double)tally[0],
           (double)tally[1], (double)tally[2], (double)tally[3], better ? ", a new best" : "");
    return better;
}
