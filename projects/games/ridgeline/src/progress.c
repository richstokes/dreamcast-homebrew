/* Ridgeline progress (progress.h).

   The trail log: an area's three ways up and its extreme trail each take
   three stamps, for reaching the top that way, doing it clean (not one
   recover from the start of the drive; on the extreme trail, from its
   bottom gate) and doing it after dark. Viewpoints stay found from drive
   to drive. Both live in the save (records.h), and unlocks.h says which
   trucks they have earned.

   The extreme trail is driven gate to gate: a run starts when the truck
   comes up through the bottom gate and ends at the top one, the trail's
   points passed in order on the way (within TRAIL_NEAR of each, so there
   is no going round an obstacle), on a clock of its own. A recover on the
   trail goes back to the last rest passed (the level's word; the HUD says
   checkpoint), not to the nearest bit of trail: what a mistake costs is
   the stretch since. */
#include "progress.h"
#include "records.h"
#include "truck.h"
#include "world.h"
#include "hud.h"
#include "audio.h"
#include "menu.h"
#include "sound_assets.h"

/* What the tests read of all this. Not in a game build: a print is a round
   trip to the Mac with interrupts off under dcload-ip (30-130 ms measured
   on the console at a trail's gate), and serial time without it, in the
   middle of a frame. */
#ifdef RL_AUTOTEST
#define LOG(...) printf(__VA_ARGS__)
#else
#define LOG(...) ((void)0)
#endif

#define TRAIL_NEAR 9.0f     /* metres from a point of the trail that counts as passing it */
#define TRAIL_LOST 45.0f    /* further than this from all of it, the run is abandoned */
#define TRAIL_GATE 3        /* the bottom gate: this many points (4 m each) up the trail */
#define NIGHT_DARK 0.5f     /* pal->dark from which a drive is after dark */

static bool clean, jumped;
static struct {
    bool on, done, clean;
    int reached;            /* the furthest point of the trail passed */
    int spot;               /* the next of its places to come to */
    int rest;               /* the last rest passed (an index into lvl_spots), -1: the gate */
    float t;
    int frame;
} trail;
/* The last run's time as the HUD says it, and the stamps it was first to earn. */
static char trail_time[40];
static uint16_t trail_new;
/* Trucks earned and not yet announced, a bit a model: said one at a time,
   when the HUD has nothing else to say. */
static uint8_t to_tell;

void progress_done(Done *d) {
    for(int a = 0; a < AREA_COUNT; a++) {
        d->views[a] = records_views(a);
        d->stamps[a] = records_stamps(a);
    }
}

bool progress_unlocked(int model) {
#if defined(RL_AUTOTEST) && !defined(RL_AT_LOCKS)
    (void)model;
    return true;    /* test builds drive whatever they are told to */
#else
    if(records_cheat()) return true;
    Done d;
    progress_done(&d);
    return unlock_met(model, &d);
#endif
}

int progress_next_unlocked(int model, int dir) {
    model = (model % TRUCK_MODELS + TRUCK_MODELS) % TRUCK_MODELS;
    if(dir == 0 && progress_unlocked(model)) return model;
    if(dir == 0) dir = 1;
    for(int k = 1; k <= TRUCK_MODELS; k++) {
        int m = (model + dir * k + TRUCK_MODELS * k) % TRUCK_MODELS;
        if(progress_unlocked(m)) return m;
    }
    return 0;
}

void progress_need(int model, const char **need, char *count, int size) {
    Done d;
    progress_done(&d);
    int have, want;
    unlock_progress(model, &d, &have, &want);
    *need = rl_unlocks[model % UNLOCK_TRUCKS].need;
    if(want > 1) snprintf(count, size, "%d OF %d", have, want);
    else count[0] = '\0';
}

int progress_area_percent(int area) {
    const RlLevel *a = rl_levels[area];
    int ways = a->nroutes > LVL_ROUTE_EXTREME ? LOG_WAYS : LOG_WAYS - 1;
    uint16_t st = records_stamps(area) & (uint16_t)(STAMP_SUMMIT - 1);
    if(ways < LOG_WAYS) st &= (uint16_t)(STAMP_BIT(LOG_EXTREME, 1) - 1);
    int have = __builtin_popcount(st) + __builtin_popcount(records_views(area) & ((1u << a->nviews) - 1));
    int all = ways * STAMPS_A_WAY + a->nviews;
    return all ? have * 100 / all : 0;
}

void progress_cheat_toggle(void) {
    records_set_cheat(!records_cheat());
    /* Nothing to announce for trucks the code handed over. */
    to_tell = 0;
}

/* Whatever the save has just gained: the trucks it has earned that the
   player has not been told of. */
static void check_unlocks(void) {
    Done d;
    progress_done(&d);
    uint8_t told = records_told(), now = told;
    for(int m = 0; m < TRUCK_MODELS; m++)
        if(rl_unlocks[m].kind != UNLOCK_FREE && unlock_met(m, &d) && !(told & (1u << m))) {
            now |= (uint8_t)(1u << m);
            to_tell |= (uint8_t)(1u << m);
        }
    records_set_told(now);
}

static void tell_unlocks(void) {
    /* Each a banner of its own, behind whatever earned it (hud.h). */
    while(to_tell && hud_message_room() > 0) {
        int m = __builtin_ctz(to_tell);
        to_tell &= (uint8_t)~(1u << m);
        char line[64];
        snprintf(line, sizeof(line), "%s UNLOCKED", menu_truck_name(m));
        hud_message(line, 0xffffe070, 5.0f);
        hud_message_sub("WAITING FOR YOU AT THE TRUCK SELECT");
        hud_message_sound(RL_SFX_SUMMIT, 200);
        LOG("Ridgeline progress: %s unlocked\n", menu_truck_name(m));
    }
}

/* The names of the stamps in bits, for a message: "TOP, CLEAN". */
static void stamp_names(uint16_t bits, char *out, int size) {
    static const char *const name[STAMPS_A_WAY] = {"TOP", "CLEAN", "AFTER DARK"};
    int n = 0;
    out[0] = '\0';
    for(int k = 0; k < STAMPS_A_WAY; k++)
        if(bits & (1u << k)) n += snprintf(out + n, n < size ? size - n : 0, "%s%s", n ? ", " : "", name[k]);
}

void progress_drive_start(void) {
    clean = true;
    jumped = false;
    memset(&trail, 0, sizeof(trail));
    trail.rest = -1;
    check_unlocks();    /* a save from before the trucks were locked: what it had already earned */
    to_tell = 0;
}

bool progress_clean(void) {
    return clean;
}

bool progress_trail_on(void) {
    return trail.on;
}

float progress_trail_clock(void) {
    return trail.t;
}

bool progress_trail_done(void) {
    return trail.done;
}

void progress_jumped(void) {
    jumped = true;
    clean = false;
    trail.on = false;
}

bool progress_view(int i) {
    bool fresh = records_add_views(game.area, (uint16_t)(1u << i));
    if(fresh) check_unlocks();
    return fresh;
}

static float flat_d2(const lvl_pt_t *q, V3 p) {
    float dx = q->x - p.x, dz = q->z - p.z;
    return dx * dx + dz * dz;
}

/* The top gate (or the summit, where the trail ends under it): the time,
   the stamps, and the message unless the summit's own is about to say it. */
static void trail_finish(bool quiet) {
    const lvl_route_t *r = lvl_extreme;
    trail.on = false;
    trail.done = true;
    uint16_t bits = STAMP_TOP;
    if(trail.clean) bits |= STAMP_CLEAN;
    if(pal->dark > NIGHT_DARK) bits |= STAMP_NIGHT;
    trail_new = (uint16_t)(((STAMP_BIT(LOG_EXTREME, bits) & ~records_stamps(game.area)) >> (LOG_EXTREME * STAMPS_A_WAY)) & 7);
    records_add_stamps(game.area, STAMP_BIT(LOG_EXTREME, bits));
    records_extreme((uint32_t)(trail.t * 10.0f + 0.5f), trail_time, sizeof(trail_time));
    LOG("Ridgeline progress: %s driven in %.1f s%s, stamps %x (new %x)\n", r->name, (double)trail.t,
           trail.clean ? ", clean" : "", bits, trail_new);
    if(!quiet) {
        char line[64], names[40];
        snprintf(line, sizeof(line), "%s DRIVEN!", r->name);
        hud_message(line, 0xffffe070, 6.0f);
        hud_message_sub(trail_time);
        stamp_names(trail_new, names, sizeof(names));
        if(trail_new) {
            snprintf(line, sizeof(line), "TRAIL LOG: %s", names);
            hud_message_sub2(line);
        }
        trail_new = 0;
        hud_message_sound(RL_SFX_SUMMIT, 240);
    }
    check_unlocks();
}

/* Does the trail end at the summit itself? Then the summit is its top gate. */
static bool ends_at_summit(const lvl_route_t *r) {
    const lvl_pt_t *q = &r->pts[r->own1];
    float dx = q->x - LEVEL_SUMMIT_X, dz = q->z - LEVEL_SUMMIT_Z;
    return dx * dx + dz * dz < 30.0f * 30.0f;
}

void progress_update(float dt) {
    tell_unlocks();
    if(!LVL_HAS_EXTREME) return;
    const lvl_route_t *r = lvl_extreme;
    const Truck *p = &trucks[0];
    if(!trail.on) {
        /* Up through the bottom gate: a few truck lengths in from where
           the trail leaves its road (a truck going by on the road is not
           on it), pointing the way it goes. */
        int g = r->own0 + TRAIL_GATE < r->own1 ? r->own0 + TRAIL_GATE : r->own0;
        const lvl_pt_t *g0 = &r->pts[g], *g1 = &r->pts[g + 2 <= r->own1 ? g + 2 : r->own1];
        if(flat_d2(g0, p->pos) > 5.5f * 5.5f || fabsf(g0->y - p->pos.y) > 5.0f) return;
        V3 way = vnorm(v3(g1->x - g0->x, 0, g1->z - g0->z));
        if(vdot(p->ax[2], way) < 0.4f || p->speed < 0.5f) return;
        bool again = trail.done;
        memset(&trail, 0, sizeof(trail));
        trail.on = trail.clean = true;
        trail.done = again;
        trail.reached = g;
        trail.rest = -1;
        while(trail.spot < LVL_SPOTS && lvl_spots[trail.spot].pt <= trail.reached) {
            if(lvl_spots[trail.spot].rest) trail.rest = trail.spot;
            trail.spot++;
        }
        hud_message(r->name, 0xffff9050, 4.0f);
        hud_message_sub("EXTREME TRAIL");
        hud_message_sub2("RECOVER RETURNS TO THE LAST CHECKPOINT");
        hud_message_sound(RL_SFX_CHIME, 200);
        LOG("Ridgeline progress: on %s\n", r->name);
        return;
    }
    trail.t += dt;
    /* The trail's points in order, the next few of them: none skipped by
       more than a truck's length. */
    for(int k = trail.reached + 1; k <= trail.reached + 3 && k <= r->own1; k++)
        if(flat_d2(&r->pts[k], p->pos) < TRAIL_NEAR * TRAIL_NEAR && fabsf(r->pts[k].y - p->pos.y) < 6.0f) trail.reached = k;
    /* Its places, as each is come to. Only a rest says so, and only
       that it is one: the places' names coming up all the way along were
       a distraction from the driving. */
    while(trail.spot < LVL_SPOTS && lvl_spots[trail.spot].pt <= trail.reached) {
        const lvl_spot_t *s = &lvl_spots[trail.spot];
        if(s->rest) {
            trail.rest = trail.spot;
            hud_status("CHECKPOINT", 0xfff8f0dc, 1.6f);
        }
        trail.spot++;
    }
    if(trail.reached >= r->own1 - 1) {
        trail_finish(false);
        return;
    }
    /* Left it: out the way it came in, or wandered off for good. */
    if(++trail.frame % 30 == 0) {
        float best = 1e12f;
        for(int k = r->own0; k <= r->own1; k++) best = fminf(best, flat_d2(&r->pts[k], p->pos));
        bool out = trail.reached <= r->own0 + TRAIL_GATE + 2 && flat_d2(&r->pts[r->own0], p->pos) < 6.0f * 6.0f;
        if(best > TRAIL_LOST * TRAIL_LOST || out) {
            trail.on = false;
            if(!out) {
                hud_message("OFF THE TRAIL", 0xffff9050, 3.5f);
                hud_message_sub("DRIVE BACK TO ITS GATE TO START OVER");
            }
            LOG("Ridgeline progress: left %s at point %d\n", r->name, trail.reached);
        }
    }
}

bool progress_recover(void) {
    clean = false;
    if(!trail.on) return false;
    const lvl_route_t *r = lvl_extreme;
    Truck *p = &trucks[0];
    trail.clean = false;
    int pt = trail.rest >= 0 && lvl_spots[trail.rest].pt > r->own0 + TRAIL_GATE ? lvl_spots[trail.rest].pt : r->own0 + TRAIL_GATE;
    const lvl_pt_t *a = &r->pts[pt], *b = &r->pts[pt + 1 <= r->own1 ? pt + 1 : pt];
    truck_place(p, v3(a->x, 0, a->z), a == b ? truck_heading(p) : atan2f(b->x - a->x, b->z - a->z));
    truck_set_mode(p, DRIVE_LOCK);
    trail.reached = pt;
    /* The places after it are still to come. */
    trail.spot = 0;
    while(trail.spot < LVL_SPOTS && lvl_spots[trail.spot].pt <= pt) trail.spot++;
    char line[48];
    snprintf(line, sizeof(line), "BACK AT %s", trail.rest >= 0 ? lvl_spots[trail.rest].name : r->name);
    hud_status(line, 0xfff8f0dc, 2.0f);
    LOG("Ridgeline progress: recovered on %s to point %d\n", r->name, pt);
    return true;
}

bool progress_summit_trail(char *msg, int size) {
    msg[0] = '\0';
    if(jumped || !LVL_HAS_EXTREME) return false;
    /* A trail that ends under the summit: the summit is its top gate. */
    if(trail.on && ends_at_summit(lvl_extreme) && trail.reached >= lvl_extreme->own1 - 8) trail_finish(true);
    if(!trail.done) return false;
    snprintf(msg, size, "BY %s   %s", lvl_extreme->name, trail_time);
    return true;
}

void progress_summit(int way, char *msg, int size) {
    msg[0] = '\0';
    if(jumped) return;
    uint16_t bits = STAMP_SUMMIT, was = records_stamps(game.area), fresh = 0;
    if(way == WAY_EXTREME) {
        /* Stamped at its top gate; said here if the summit was that gate. */
        fresh = trail_new;
        trail_new = 0;
    }
    else if(way >= WAY_LOGGING && way <= WAY_CREEK) {
        uint16_t st = STAMP_TOP;
        if(clean) st |= STAMP_CLEAN;
        if(pal->dark > NIGHT_DARK) st |= STAMP_NIGHT;
        bits |= STAMP_BIT(way, st);
    }
    records_add_stamps(game.area, bits);
    if(way >= WAY_LOGGING && way <= WAY_CREEK)
        fresh = (uint16_t)(((records_stamps(game.area) & ~was) >> (way * STAMPS_A_WAY)) & 7);
    if(fresh) {
        char names[40];
        stamp_names(fresh, names, sizeof(names));
        snprintf(msg, size, "TRAIL LOG: %s", names);
    }
    LOG("Ridgeline progress: summit by way %d, stamps %04x\n", way, records_stamps(game.area));
    check_unlocks();
}
