/* The best lunch so far, on a memory card. Every card is looked at when
   the title comes up (../../common/vmusave.h): one save is loaded, two are
   the player's choice, and a full first card is asked about, with the
   engine's card prompt over the title. */
#ifndef KL_SAVE_H
#define KL_SAVE_H

#include "kl.h"

typedef struct {
    uint32_t magic;
    uint32_t best_mult;
    uint64_t best_total;      /* thousands of dollars */
    uint32_t grade, lunches;
    uint32_t options;         /* SAVE_OPT_* (the first saves stopped before this) */
    uint32_t spare;
} Save;
#define SAVE_OPT_NO_MUSIC 1u
extern Save save;

/* Look at the cards, if one has come or gone since the last look. */
void save_cards(void);
/* Once a step on the title: true while the cards are being read or a
   question about them is up, and nothing else should take the pad. */
bool save_hold(void);
/* The question, over the title. */
void save_draw(void);
/* A finished lunch. True if it beat the record. */
bool save_submit(uint64_t total, int mult, int grade);
/* An option changed: write it, if there is a card to write to. */
void save_changed(void);
#ifdef KL_AT_VMUTEST
/* For the card test: wait out a look (false if it ended in a question),
   and forget what is in memory. */
bool save_wait(void);
void save_forget(void);
#endif

#endif
