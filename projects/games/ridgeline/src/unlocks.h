/* Ridgeline's trucks and what earns each: two to start with, the rest for
   things done out on the mountains. Kept apart from progress.c, with no
   more than the rules in it, so tools/hostsim can check that the order
   works (`sim unlocks`: every truck can be earned driving only trucks
   already earned).

   What has been done is the save's (records.h): per area the viewpoints
   found, a bit each, and the trail log's stamps. */
#ifndef RL_UNLOCKS_H
#define RL_UNLOCKS_H

#include "level.h"
#include <stdbool.h>
#include <stdint.h>

/* The trail log: for each of an area's ways up (its three routes in the
   level's order, then its extreme trail) three stamps, and one more bit
   for the summit reached by any way at all. */
enum {
    STAMP_TOP = 1,      /* reached the summit that way (the extreme trail: drove it gate to gate) */
    STAMP_CLEAN = 2,    /* ...without a recover */
    STAMP_NIGHT = 4,    /* ...after dark */
    STAMPS_A_WAY = 3,
    LOG_WAYS = 4,
    LOG_EXTREME = 3,    /* the extreme trail's place among the ways */
    STAMP_SUMMIT = 1 << (STAMPS_A_WAY * LOG_WAYS)
};
#define STAMP_BIT(way, stamp) ((uint16_t)((stamp) << ((way) * STAMPS_A_WAY)))

typedef struct {
    uint16_t views[AREA_COUNT], stamps[AREA_COUNT];
} Done;

enum {
    UNLOCK_FREE,        /* yours from the start */
    UNLOCK_ANY_SUMMIT,  /* reach any area's summit */
    UNLOCK_SUMMIT,      /* reach area n's */
    UNLOCK_VIEWS,       /* find n viewpoints, anywhere */
    UNLOCK_EXTREME      /* drive n extreme trails, gate to gate */
};

typedef struct {
    uint8_t kind, n;
    const char *need;   /* what the truck select says while it is locked */
} Unlock;

/* In model order (trucks.h): CASCADE 84, KESTREL, BOBCAT, FFR, TACO,
   GOOSE, ONE TEN, EAGLE. The two old pickups come with the game; the
   first summit earns the Bobcat, the dunes their side-by-side and the
   Highlands their station wagon; viewpoints the Taco; and the two
   quickest things here are for the extreme trails. */
#define UNLOCK_TRUCKS 8
static const Unlock rl_unlocks[UNLOCK_TRUCKS] = {
    {UNLOCK_FREE, 0, ""},
    {UNLOCK_EXTREME, 1, "DRIVE AN EXTREME TRAIL TO ITS END"},
    {UNLOCK_ANY_SUMMIT, 0, "REACH ANY SUMMIT"},
    {UNLOCK_FREE, 0, ""},
    {UNLOCK_VIEWS, 15, "FIND 15 VIEWPOINTS"},
    {UNLOCK_SUMMIT, AREA_DUNES, "REACH THE TOP OF THE KING DUNE AT MOSES LAKE"},
    {UNLOCK_SUMMIT, AREA_LOWLAND, "REACH THE MAST ABOVE LOCH LOWLAND"},
    {UNLOCK_EXTREME, AREA_COUNT, "DRIVE ALL FOUR EXTREME TRAILS"},
};

static inline int done_views(const Done *d) {
    int n = 0;
    for(int a = 0; a < AREA_COUNT; a++) n += __builtin_popcount(d->views[a]);
    return n;
}

static inline int done_extremes(const Done *d) {
    int n = 0;
    for(int a = 0; a < AREA_COUNT; a++) n += (d->stamps[a] & STAMP_BIT(LOG_EXTREME, STAMP_TOP)) != 0;
    return n;
}

static inline int done_summits(const Done *d) {
    int n = 0;
    for(int a = 0; a < AREA_COUNT; a++) n += (d->stamps[a] & STAMP_SUMMIT) != 0;
    return n;
}

/* How far along a truck's unlock is, and how far it has to go (a count
   of things: 1 of 1 for a single deed). */
static inline void unlock_progress(int model, const Done *d, int *have, int *want) {
    const Unlock *u = &rl_unlocks[model % UNLOCK_TRUCKS];
    switch(u->kind) {
        case UNLOCK_ANY_SUMMIT: *want = 1; *have = done_summits(d) > 0; break;
        case UNLOCK_SUMMIT: *want = 1; *have = (d->stamps[u->n] & STAMP_SUMMIT) != 0; break;
        case UNLOCK_VIEWS: *want = u->n; *have = done_views(d); break;
        case UNLOCK_EXTREME: *want = u->n; *have = done_extremes(d); break;
        default: *want = 0; *have = 0; break;
    }
    if(*have > *want) *have = *want;
}

static inline bool unlock_met(int model, const Done *d) {
    int have, want;
    unlock_progress(model, d, &have, &want);
    return have >= want;
}

#endif
