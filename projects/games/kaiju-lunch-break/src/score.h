/* The lunch bill: damage in dollars, and the combo that multiplies it.

   Anything destroyed joins a chain. While the chain's timer runs, its
   damage goes into a pot and the multiplier climbs with the chain's
   length and with tricks (dominoes, pancakes, a block levelled...). When
   the timer runs out the pot is banked, times the multiplier. Variety
   pays: the first use of each kind of attack in a chain is worth more,
   the same one over and over goes stale, and a trick pays in full three
   times a chain, then every third time. */
#ifndef KL_SCORE_H
#define KL_SCORE_H

#include "kl.h"

enum {
    TR_DOMINO, TR_TIMBER, TR_PANCAKE, TR_SWATTED, TR_FAST_FOOD, TR_BUS_LUNCH, TR_TANK_YOU, TR_AIR_MEAL,
    TR_BREAKING_NEWS, TR_HOME_RUN, TR_SLICER, TR_BELLY_FLOP, TR_SHOCKWAVE, TR_CHAIN_REACTION, TR_LANDMARK,
    TR_BLOCK_PARTY, TR_DESSERT, TR_TOP_GUN, TR_DELIVERY, TR_MIX_UP, TR_FULL_COURSE, TR_ROAR, TR_LUMBERJACK,
    TR_BULLDOZER, TR_AIR_MAIL, TR_TSUNAMI,
    TR_COUNT
};

#define SCORE_FEED 5
typedef struct {
    uint64_t total;          /* banked, thousands of dollars */
    uint64_t pot;            /* this chain's, before the multiplier */
    uint64_t damage;         /* everything done so far, no multiplier: what the army goes by */
    int chain, mult, bonus;
    float timer, timer_max;
    int best_mult;
    uint64_t best_combo;     /* the biggest pot times multiplier banked */
    int buildings, vehicles, eaten, tricks;
    uint32_t used;           /* attack kinds this chain */
    int last_src, repeat;
    bool roared;
    /* The HUD's: the trick feed, the multiplier's pop and the last bank. */
    struct { const char *name; int bonus, times; float age; } feed[SCORE_FEED];
    uint8_t done[32];        /* how often each trick has paid this chain */
    float pop, bank_age;
    uint64_t bank_amount;
    int bank_mult;
    float energy_gain;       /* for the kaiju to collect */
} Score;
extern Score score;

void score_reset(void);
void score_update(float dt);
/* Something came down or was eaten: value in thousands of dollars. */
void score_hit(int src, uint32_t value_k, V3 at);
/* Damage short of bringing something down: straight to the total. */
void score_chip(uint32_t value_k);
/* A chunk out of a building: into the chain's pot, and a little more
   time on its clock, without lengthening the chain. */
void score_chunk(uint32_t value_k);
void score_trick(int trick, int n);
void score_hurt(void);
void score_roar(void);
void score_bank(void);
const char *score_rank(int mult);
/* The final grade, 0 (D) to 5 (S+). */
int score_grade(uint64_t total);
/* "$1,234,567,000" */
void score_money(char *out, size_t n, uint64_t value_k);

#endif
