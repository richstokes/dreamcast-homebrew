/* Ridgeline best times (records.h). The way up is worked out as the player
   climbs: every few frames the distance driven since the last look goes
   to the climbing route the truck is on (within 9 m of its centreline),
   or to their own way; distance on the shared forest road and summit
   trail counts for neither.

   A save rewrites the file and the card's directory and FAT, a maple
   frame or more per block, so it runs on a worker thread: written inline
   it froze the summit message for about half a second on the console.

   Which card: every card is looked at (records_cards, on a thread, the
   title waiting for it: records_looking). A save on one of
   them is read and that card is the save's from then on; with none, it is
   the first card. Two saves, or a first card with no room, are the
   player's to settle (the title asks, engine/cardpick.h), and until they
   have nothing is written. A save that fails says so, and the next one
   tries again; back at the title the cards are looked at again, and if
   the save's card has gone or filled the player says where it goes. */
#include "records.h"
#include "engine/cardpick.h"
#include "truck.h"
#include "terrain.h"
#include "unlocks.h"
#include <kos/mutex.h>
#include <kos/thread.h>
#include <stddef.h>
#include <string.h>

/* A time's line in the log, for the tests. Not in a game build: these are
   printed from the frame a summit is reached in, and a print there is a
   stall (progress.c). */
#ifdef RL_AUTOTEST
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

#define FILENAME "RIDGELINE"
#define NEAR_M 9.0f

#define REC_TRUCKS 8   /* rows the file has room for */
#define REC_WAYS 4     /* the ways up a row holds: three routes and your own (the extreme trail's times are apart) */
_Static_assert(TRUCK_MODELS <= REC_TRUCKS, "the save has no row for this truck");
_Static_assert(AREA_COUNT <= REC_AREAS, "the save has no row for this area");

/* What the player has done (records.h), after the times. */
typedef struct {
    char tag[4];                                   /* "PRG1" */
    uint16_t views[REC_AREAS];
    uint16_t stamps[REC_AREAS];
    uint32_t extreme_best[REC_AREAS][REC_TRUCKS];  /* the extreme trail gate to gate, tenths of a second */
    uint8_t cheat, told;
    uint8_t hud;                                   /* HUD_*: the pause menu's choice (0, all of it, in a save from before) */
    uint8_t pad;
} progress_t;

typedef struct {
    char magic[8];
    int32_t version;
    /* Cascade Ridge's times. */
    uint32_t best[2][REC_WAYS];    /* tenths of a second, 0 for none: Cascade 84, Kestrel */
    char name[12];                 /* the online name (version 2; version 1 saves end before it) */
    /* The trucks after the first two (Bobcat, FFR, Taco, Goose, One Ten, Eagle...), a row each in
       model order. Appended to version 2 so older builds still read the
       rest; a save from a build with fewer trucks is shorter. */
    uint32_t best_more[REC_TRUCKS - 2][REC_WAYS];
    /* The areas after Cascade Ridge in order, a row a truck. Still
       version 2: an older build reads what it knows and writes a shorter
       save, which forgets the rest. There is room here for every area
       the save will ever hold, so that what follows stays where it is
       when an area is added. */
    uint32_t area_best[REC_AREAS - 1][REC_TRUCKS][REC_WAYS];
    /* Progress: a save from before it ends above, and reads as none. */
    progress_t prog;
} record_t;
#define RECORD_V1_BYTES (int)offsetof(record_t, name)
#define RECORD_V2_BYTES (int)offsetof(record_t, best_more)
#define SAVE_BLOCKS vmusave_blocks(1, sizeof(record_t))   /* with its icon */

/* Test builds never touch a VMU, except the save test, which runs on
   blank cards (see the README). */
#if !defined(RL_AUTOTEST) || defined(RL_AT_VMUTEST)
#define RL_USE_VMU 1
static const char magic[8] = {'R', 'I', 'D', 'G', 'E', 'S', 'V', '1'};
static const char prog_tag[4] = {'P', 'R', 'G', '1'};
/* The card: none that will do yet, the cards being looked at, the player
   being asked which, settled (card_port and card_unit, which the worker
   reads), or the player would rather not save. */
enum { CARD_NONE, CARD_LOOKING, CARD_ASK, CARD_SET, CARD_OFF };
static int card_state, card_port = -1, card_unit = -1;
static vmusave_slot_t slots[VMUSAVE_SLOTS];   /* the last look; their files are kept only while asking */
static char notes[VMUSAVE_SLOTS][12];         /* each save's online name, for the question */
static int nslots, ask_kind;
static bool unsaved;                 /* something changed with no card to write it to */
static volatile bool write_failed;   /* the worker's: the last save did not reach the card */
static bool fail_told, drop_told;    /* the player has been told: of that, and of saves with no card to go to */
static uint32_t looked_mask;         /* the cards in at the last look (vmusave_plugged) */
static bool looked;

/* The record waiting for the card, and whether a worker is on it. */
static mutex_t lock = MUTEX_INITIALIZER;
static record_t pending;
static bool have_pending, worker_on;
#endif
static record_t rec;
static bool from_card;   /* rec began as a card's save */
/* What is in memory is the save: a card's has been read, or one has been
   started on a card. From then on no card's copy replaces it (the card's
   may be older: a write can fail), and the cards are only somewhere to
   write it. */
static bool stands;

static float tally[WAY_COUNT];
static V3 last_pos;
static bool have_last, jumped;
static int frame;

static void save(void);

#ifdef RL_USE_VMU
/* A card's file as a record; false if it is not a save of ours. */
static bool parse(const vmusave_slot_t *slot, record_t *out) {
    const uint8_t *data;
    record_t r;
    memset(&r, 0, sizeof(r));
    int n = vmusave_slot_payload(slot, &data);
    if(n > (int)sizeof(r)) n = (int)sizeof(r);
    if(n > 0) memcpy(&r, data, (size_t)n);
    if(n < RECORD_V1_BYTES || memcmp(r.magic, magic, sizeof(magic))) return false;
    if(!(r.version == 1 && n >= RECORD_V1_BYTES) && !(r.version == 2 && n >= RECORD_V2_BYTES)) return false;
    if(r.version == 1) memset(r.name, 0, sizeof(r.name));
    /* Rows a shorter save lacks stay as cleared above: no time yet. A row
       cut short is cleared too. */
    int rows = n < RECORD_V2_BYTES ? 0 : (n - RECORD_V2_BYTES) / (int)sizeof(r.best_more[0]);
    for(int i = rows; i < REC_TRUCKS - 2; i++) memset(r.best_more[i], 0, sizeof(r.best_more[i]));
    /* And the areas: a save from before the latest of them is that
       much shorter. */
    int areas = n < (int)offsetof(record_t, area_best) ? 0 : (n - (int)offsetof(record_t, area_best)) / (int)sizeof(r.area_best[0]);
    for(int i = areas < 0 ? 0 : areas; i < REC_AREAS - 1; i++) memset(r.area_best[i], 0, sizeof(r.area_best[i]));
    if(n < (int)sizeof(r) || memcmp(r.prog.tag, prog_tag, sizeof(prog_tag))) memset(&r.prog, 0, sizeof(r.prog));
    r.name[sizeof(r.name) - 1] = '\0';
    *out = r;
    return true;
}

/* A card's save is read once. From then on what is in memory is that and
   everything gained since, and the card may have less: a save can still
   be on its way out, or have failed (a full card, or a write that timed
   out). Read again when going online, it took the code's trucks back. */
/* Lower of two times, 0 being none. */
static uint32_t best_of(uint32_t a, uint32_t b) {
    return !a ? b : !b ? a : a < b ? a : b;
}

static void take(const record_t *r) {
    /* With whatever this session has earned before the card turned up
       (played with none in, say): the better of each time, everything
       either has found, the name and HUD choice made here if any. */
    record_t was = rec;
    rec = *r;
    uint32_t *t = rec.best[0], *w = was.best[0];
    for(size_t i = 0; i < sizeof(rec.best) / sizeof(uint32_t); i++) t[i] = best_of(t[i], w[i]);
    t = rec.best_more[0], w = was.best_more[0];
    for(size_t i = 0; i < sizeof(rec.best_more) / sizeof(uint32_t); i++) t[i] = best_of(t[i], w[i]);
    t = rec.area_best[0][0], w = was.area_best[0][0];
    for(size_t i = 0; i < sizeof(rec.area_best) / sizeof(uint32_t); i++) t[i] = best_of(t[i], w[i]);
    t = rec.prog.extreme_best[0], w = was.prog.extreme_best[0];
    for(size_t i = 0; i < sizeof(rec.prog.extreme_best) / sizeof(uint32_t); i++) t[i] = best_of(t[i], w[i]);
    for(int a = 0; a < REC_AREAS; a++) {
        rec.prog.views[a] |= was.prog.views[a];
        rec.prog.stamps[a] |= was.prog.stamps[a];
    }
    rec.prog.cheat |= was.prog.cheat;
    rec.prog.told |= was.prog.told;
    if(was.name[0]) memcpy(rec.name, was.name, sizeof(rec.name));
    if(was.prog.hud) rec.prog.hud = was.prog.hud;
    from_card = true;
    game.hud_mode = rec.prog.hud < HUD_MODES ? rec.prog.hud : HUD_FULL;
    /* A best time is a summit reached: a save from before there was a
       trail log gets the stamps its times prove (the top by each way that
       has one, and the summit), and with them the trucks those earn.
       Nothing is written until something else saves. */
    for(int a = 0; a < AREA_COUNT; a++)
        for(int t = 0; t < REC_TRUCKS; t++)
            for(int w = 0; w < REC_WAYS; w++) {
                uint32_t b = a == AREA_CASCADE ? (t < 2 ? rec.best[t][w] : rec.best_more[t - 2][w]) : rec.area_best[a - 1][t][w];
                if(!b) continue;
                rec.prog.stamps[a] |= STAMP_SUMMIT;
                if(w < WAY_OWN) rec.prog.stamps[a] |= STAMP_BIT(w, STAMP_TOP);
            }
}

/* slots[i] is the card from here on: its save is read, unless what is in
   memory stands already, and anything that changed with no card to go to
   is written. */
static void use_card(int i) {
    record_t r;
    card_port = slots[i].port;
    card_unit = slots[i].unit;
    card_state = CARD_SET;
    if(!stands && slots[i].save && parse(&slots[i], &r)) {
        take(&r);
        printf("Ridgeline records: loaded from VMU %c%d\n", 'A' + card_port, card_unit);
    }
    stands = true;
    drop_told = false;
    vmusave_survey_free(slots, nslots);
    /* Whatever changed with no card to go to is written now (a card's
       save just read has it merged in). */
    bool write = unsaved;
    unsaved = false;
    if(write) save();
}
#endif

#ifdef RL_USE_VMU
/* The look at the cards runs on a thread of its own (two cards are 0.6 s
   of block reads): it fills slots and says when it is done. */
static kthread_t *look_thd;
static volatile bool look_done;
static uint64_t look_t0;

static void *look_main(void *arg) {
    (void)arg;
    nslots = vmusave_survey(FILENAME, slots, VMUSAVE_SLOTS);
    look_done = true;
    return NULL;
}

/* What the look found, settled or made a question of (the main thread). */
static void look_landed(void) {
    int home = -1, readable = 0;
    looked = true;
    card_state = CARD_NONE;
    if(nslots)
        printf("Ridgeline records: looked at %d cards in %lu ms\n", nslots, (unsigned long)(timer_ms_gettime64() - look_t0));
    for(int i = 0; i < nslots; i++) {
        record_t r;
        notes[i][0] = '\0';
        if(slots[i].free_blocks >= 0) readable++;
        if(slots[i].save && !parse(&slots[i], &r)) slots[i].save = false;
        else if(slots[i].save) snprintf(notes[i], sizeof(notes[i]), "%s", r.name[0] ? r.name : "NO NAME");
        if(slots[i].port == card_port && slots[i].unit == card_unit) home = i;
    }
    if(stands) {
        /* What is in memory is the save and only wants somewhere to go:
           the card it is on, while that is in and has room. Failing that
           the player says where, as it would be written over any save
           another card holds. */
        if(home >= 0 && vmusave_fits(&slots[home], SAVE_BLOCKS)) use_card(home);
        else if(readable) {
            card_state = CARD_ASK;
            ask_kind = CARDPICK_WHERE;
        }
        else vmusave_survey_free(slots, nslots);
        return;
    }
    int pick = 0;
    switch(vmusave_decide(slots, nslots, SAVE_BLOCKS, &pick)) {
        case VMUSAVE_USE: use_card(pick); break;
        case VMUSAVE_ASK_WHICH:
            card_state = CARD_ASK;
            ask_kind = CARDPICK_WHICH;
            break;
        case VMUSAVE_ASK_ROOM:
            card_state = CARD_ASK;
            ask_kind = CARDPICK_ROOM;
            printf("Ridgeline records: no room on VMU %c%d (%d blocks free, the save is %d)\n", 'A' + slots[pick].port,
                   slots[pick].unit, slots[pick].free_blocks, SAVE_BLOCKS);
            break;
        default: vmusave_survey_free(slots, nslots); break;
    }
}
#endif

void records_cards(void) {
#ifdef RL_USE_VMU
    if(card_state == CARD_LOOKING || card_state == CARD_ASK) return;
    uint32_t plugged = vmusave_plugged();
    bool same = looked && plugged == looked_mask;
    if(card_state == CARD_SET && !write_failed && maple_enum_dev(card_port, card_unit)) return;
    /* Declined, or no card that would do: nothing to learn until one
       comes or goes. (A card that will not read costs its three tries
       each look, and the boot and the title that follows both ask.) */
    if((card_state == CARD_OFF || card_state == CARD_NONE) && same) return;
    records_finish();
    if(write_failed) unsaved = true;   /* what the card missed goes to the next one */
    write_failed = fail_told = false;
    vmusave_survey_free(slots, nslots);
    nslots = 0;
    looked_mask = plugged;
    look_t0 = timer_ms_gettime64();
    look_done = false;
    card_state = CARD_LOOKING;
    look_thd = thd_create(false, look_main, NULL);
    if(!look_thd) look_main(NULL);   /* no thread to be had: the slow way */
#endif
}

bool records_looking(void) {
#ifdef RL_USE_VMU
    if(card_state != CARD_LOOKING) return false;
    if(!look_done) return true;
    if(look_thd) thd_join(look_thd, NULL);
    look_thd = NULL;
    look_landed();
#endif
    return false;
}

int records_card_ask(const vmusave_slot_t **out, const char *const **out_notes, int *count, int *need) {
#ifdef RL_USE_VMU
    static const char *note_ptr[VMUSAVE_SLOTS];
    if(card_state != CARD_ASK) return -1;
    for(int i = 0; i < nslots; i++) note_ptr[i] = notes[i];
    *out = slots;
    *out_notes = note_ptr;
    *count = nslots;
    *need = SAVE_BLOCKS;
    return ask_kind;
#else
    return -1;
#endif
}

void records_card_pick(int slot) {
#ifdef RL_USE_VMU
    if(card_state != CARD_ASK) return;
    if(slot >= 0 && slot < nslots) {
        use_card(slot);
        return;
    }
    card_state = CARD_OFF;
    vmusave_survey_free(slots, nslots);
    printf("Ridgeline records: playing without saving\n");
#endif
}

int records_notice(void) {
#ifdef RL_USE_VMU
    if(!write_failed) fail_told = false;
    else if(!fail_told) {
        fail_told = true;
        return REC_NOTICE_FAILED;
    }
    if(unsaved && card_state == CARD_NONE && !drop_told) {
        drop_told = true;
        return REC_NOTICE_NO_CARD;
    }
#endif
    return 0;
}

#ifdef RL_AT_VMUTEST
bool records_cards_wait(void) {
    records_cards();
    while(records_looking()) thd_sleep(10);
    return from_card;
}

void records_forget(void) {
    while(records_looking()) thd_sleep(10);
    records_finish();
    vmusave_survey_free(slots, nslots);
    memset(&rec, 0, sizeof(rec));
    from_card = stands = unsaved = fail_told = drop_told = looked = false;
    write_failed = false;
    card_state = CARD_NONE;
    card_port = card_unit = -1;
    nslots = 0;
}
#endif

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
    maple_device_t *dev = maple_enum_dev(card_port, card_unit);
    if(!dev || !(dev->info.functions & MAPLE_FUNC_MEMCARD)) {
        printf("Ridgeline records: VMU %c%d has gone\n", 'A' + card_port, card_unit);
        write_failed = true;
        return;
    }
    static uint8_t pixels[512];
    vmu_pkg_t pkg;
    vmusave_header(&pkg, "RIDGELINE", "Ridgeline", "Ridgeline times and progress");
    /* Palette (ARGB4444): blank, snow, rock, forest, amber. */
    static const uint16_t palette[] = {0x0000, 0xffff, 0xf888, 0xf363, 0xfea3};
    vmusave_icon(&pkg, pixels, icon_art, ".SRGY", palette);
    /* (a write that times out fails too: twice more before giving up) */
    int rv = -1;
    for(int tries = 0; tries < 3 && rv < 0; tries++) {
        if(tries) thd_sleep(50);
        rv = vmusave_write(dev, FILENAME, &pkg, r, sizeof(*r));
    }
    write_failed = rv < 0;
    if(rv < 0)
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
    /* No card settled on: kept for the one the next look at them
       (records_cards, at the title) turns up. */
    if(card_state != CARD_SET) {
        unsaved = true;
        return;
    }
    mutex_lock(&lock);
    pending = rec;
    memcpy(pending.magic, magic, sizeof(magic));
    memcpy(pending.prog.tag, prog_tag, sizeof(prog_tag));
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
        bool busy = worker_on || have_pending || (card_state == CARD_LOOKING && !look_done);
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
    /* (The extreme trail's own stretch counts as your own way: its time
       is taken between its gates, trail.c.) */
    if(near < 0 || near == LVL_ROUTE_EXTREME) tally[WAY_OWN] += moved;
    else if(near >= 1 && near <= 3) tally[near - 1] += moved;
}

const char *records_way_name(int way) {
    if(way == WAY_EXTREME) return LVL_HAS_EXTREME ? lvl_extreme->name : "";
    return way < WAY_OWN ? lvl_routes[1 + way].name : "YOUR OWN WAY";
}

const char *records_way_short(int way) {
    if(way == WAY_EXTREME) return LVL_HAS_EXTREME ? lvl_extreme->short_name : "";
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
    if(way == WAY_EXTREME) return &rec.prog.extreme_best[game.area][truck];
    if(game.area != AREA_CASCADE) return &rec.area_best[game.area - 1][truck][way];
    return truck < 2 ? &rec.best[truck][way] : &rec.best_more[truck - 2][way];
}

uint32_t records_best(int truck, int way) {
    return *best_slot(truck, way);
}

bool records_summit(char *msg, int size, int *way_out) {
    char t[16];
    uint32_t tenths = (uint32_t)(game.clock * 10.0f + 0.5f);
    records_format(tenths, t, sizeof(t));
    if(way_out) *way_out = -1;
    if(jumped) {
        snprintf(msg, size, "%s   NO RECORD AFTER A VIEWPOINT JUMP", t);
        return false;
    }
    int way = WAY_LOGGING;
    for(int w = 1; w < 3; w++)
        if(tally[w] > tally[way]) way = w;
    if(tally[way] < tally[WAY_OWN]) way = WAY_OWN;
    if(way_out) *way_out = way;
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
    LOG("Ridgeline records: summit %s by way %d (tally %.0f %.0f %.0f own %.0f m)%s\n", t, way, (double)tally[0],
           (double)tally[1], (double)tally[2], (double)tally[3], better ? ", a new best" : "");
    return better;
}

bool records_extreme(uint32_t tenths, char *msg, int size) {
    char t[16];
    records_format(tenths, t, sizeof(t));
    uint32_t *b = best_slot(game.truck_model, WAY_EXTREME);
    bool better = *b == 0 || tenths < *b;
    if(better) {
        *b = tenths;
        snprintf(msg, size, "%s   NEW BEST!", t);
        save();
    }
    else {
        char bt[16];
        records_format(*b, bt, sizeof(bt));
        snprintf(msg, size, "%s   BEST %s", t, bt);
    }
    LOG("Ridgeline records: %s in %s%s\n", records_way_name(WAY_EXTREME), t, better ? ", a new best" : "");
    return better;
}

/* ---- Progress ---- */

uint16_t records_views(int area) {
    return rec.prog.views[area];
}

uint16_t records_stamps(int area) {
    return rec.prog.stamps[area];
}

bool records_add_views(int area, uint16_t bits) {
    if(!(bits & ~rec.prog.views[area])) return false;
    rec.prog.views[area] |= bits;
    save();
    return true;
}

bool records_add_stamps(int area, uint16_t bits) {
    if(!(bits & ~rec.prog.stamps[area])) return false;
    rec.prog.stamps[area] |= bits;
    save();
    return true;
}

bool records_cheat(void) {
    return rec.prog.cheat != 0;
}

void records_set_cheat(bool on) {
    if((rec.prog.cheat != 0) == on) return;
    rec.prog.cheat = on;
    save();
}

uint8_t records_told(void) {
    return rec.prog.told;
}

void records_set_told(uint8_t models) {
    if(rec.prog.told == models) return;
    rec.prog.told = models;
    save();
}

void records_set_hud(int mode) {
    if(rec.prog.hud == mode) return;
    rec.prog.hud = (uint8_t)mode;
    save();
}
