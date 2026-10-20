/* DreamKarts power-ups: Chaos Emeralds on the track, the items they give
   and what they do.

   Each track has a few invisible bands across the road (track.bands), each
   with four slots spread over its width. One slot per band is lit each
   race, picked from a seed every console shares, and holds a spinning
   emerald whose colour is its item:

     blue   Power Sneakers  a boost, longer the further back you are
     red    Chaos Spear     a homing shot along the track at the kart ahead
                            (or, with down held, the kart behind)
     green  Barrier         a bubble that soaks up one Spear or Mine
     yellow Mine            dropped on the road behind the kart; spins out
                            whoever drives over it (the dropper included)

   A kart holds one item at a time and fires it with Y. A taken emerald
   comes back in the same slot a few seconds later, in a new colour.

   Spears and mines live in track coordinates (lap distance and lateral
   offset), so spears follow bends, never hit walls and never fall off the
   sky tracks, and a mine sits on the road wherever it lands. Online every
   console runs its own copy of each spear and mine from the owner's
   packets and judges hits on its own kart only (net.c, online.c). */
#ifndef DK_ITEMS_H
#define DK_ITEMS_H

#include "kart.h"

enum { ITEM_NONE, ITEM_SNEAKERS, ITEM_SPEAR, ITEM_SHIELD, ITEM_MINE, ITEM_COUNT };

#define ITEM_SLOTS 4
#define SPIN_LEN 1.1f        /* seconds a spun-out kart is out of control */
#define SPIN_SPEED_MUL 0.35f /* what is left of its speed */
#define SHIELD_LEN 6.0f
#define MINE_LIFE 25.0f      /* seconds a mine waits on the road */

void items_init(void);
/* A new race: lit slots and colours from seed, nothing held, no spears. */
void items_reset(uint32_t seed);
/* Emerald timers, spears in flight and their hits. Once a step, after the
   karts have moved. */
void items_update(float dt);
/* Pickups and item use for a kart this console drives (after its
   kart_update); in->item fires the held item, in->item_back backwards. */
void items_kart(Kart *k, const KartInput *in, float dt);
/* Draws: the emeralds, held gems, barriers, mines and spears (translucent
   list, after the world and shadows, before the particles). */
void items_draw(void);
/* The held item as a small gem at (x, y) in the overlay, scale k. */
void items_draw_hud(int player, float x, float y, float k);
/* CPU drivers: steer toward a lit emerald near the line and round mines,
   use what they hold. */
float items_ai_lane(const Kart *k, float s, float d);
void items_ai(Kart *k, KartInput *in, float dt);
/* The nearest spear coming for k, metres away, or a large value. */
float items_threat(const Kart *k);
/* Metres to the nearest mine ahead of k in its part of the road, or a
   large value. */
float items_mine_ahead(const Kart *k);
uint32_t items_colour(int item);
const char *items_name(int item);
/* Lateral offset of a band's slot. */
float items_slot_d(int band, int slot);
/* Emerald world position (centre of the gem) and whether it is there. */
bool items_emerald(int band, V3 *pos, int *item);

/* Online: the item state in a kart packet. */
typedef struct {
    int item;            /* ITEM_* held */
    bool shield, spun;
    int spear_seq;       /* 1-15 while this kart's spear flies, 0 none */
    bool spear_back;
    float spear_s, spear_d;
    int spear_target;    /* lobby slot, or -1 */
    int mine_seq;        /* 1-15 while this kart's mine is on the road, 0 none */
    float mine_s, mine_d;
} NetItems;
void items_pack(const Kart *k, NetItems *out);
/* A rival's item state from the network: held gem, barrier, spin-out, its
   spear (spawned here on a new sequence number, moved on by lead seconds,
   the age of the state it came in) and its mine (spawned on a new number,
   gone when the number goes). */
void items_remote(Kart *k, const NetItems *in, float lead, float dt);

/* Counts for the autotest summary: emeralds taken, items used, spears
   fired, mines dropped, hits (barriers included) and spin-outs, all karts. */
extern int items_got, items_used, items_fired, items_dropped, items_hits, items_spins;

#endif
