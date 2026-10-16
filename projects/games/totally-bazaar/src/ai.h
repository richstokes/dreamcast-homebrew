/* Totally Bazaar CPU opponent. Uses only public information plus its own hand. */
#ifndef BAZAAR_AI_H
#define BAZAAR_AI_H

#include "rules.h"

typedef enum {
    AI_EASY = 0,
    AI_HARD = 1
} ai_level_t;

/* Chooses a legal move for g->current. Returns false only if none exist. */
bool ai_choose_move(const game_t *g, ai_level_t level, uint32_t *rng,
                    move_t *out);

#endif
