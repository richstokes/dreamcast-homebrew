/* Totally Bazaar CPU opponent.
 *
 * One-ply evaluation of every legal move. Cards refilled from the deck are
 * treated as unknown, so the CPU never sees a hidden draw or another
 * player's hand. */
#include "ai.h"

#include <string.h>

/* Average values of the shuffled bonus piles (3, 4, 5+ cards). */
static const float bonus_average[BONUS_SIZES] = { 2.0f, 5.0f, 9.0f };

typedef struct {
    float hold;        /* discount applied to unsold goods */
    float urgency;     /* 0..1, how close the round is to ending */
} ai_ctx_t;

static float sale_value(const game_t *g, int good, int n) {
    float v = 0.0f;
    int i;
    for(i = 0; i < n; ++i)
        v += (float)jp_token_peek(g, good, i);
    if(n >= 5)
        v += bonus_average[2];
    else if(n >= 3)
        v += bonus_average[n - 3];
    return v;
}

/* Value of holding n cards of a type until a later sale. */
static float holding_value(const game_t *g, const ai_ctx_t *c, int good,
                           int n) {
    float v;
    if(n <= 0)
        return 0.0f;
    if(g->goods_token_count[good] == 0)
        return n >= 3 ? sale_value(g, good, n) * 0.5f * c->hold : 0.1f * n;
    v = sale_value(g, good, n) * c->hold;
    if(jp_is_precious(good) && n == 1)
        v *= 0.55f;
    /* Big sets are worth chasing, but tokens drain while we wait. */
    if(n >= 5)
        v *= 0.92f;
    return v;
}

static float hand_value(const game_t *g, const ai_ctx_t *c,
                        const uint8_t counts[GOOD_TYPES]) {
    float v = 0.0f;
    int t, total = 0;
    for(t = 0; t < TRADE_GOODS; ++t) {
        v += holding_value(g, c, t, counts[t]);
        total += counts[t];
    }
    /* A full hand blocks single takes and exchanges. */
    if(total >= 6)
        v -= (float)(total - 5) * 1.2f * (1.0f - c->urgency);
    return v;
}

static float herd_value(const game_t *g, int me, int herd) {
    int p, best_other = 0;
    float v;
    for(p = 0; p < g->num_players; ++p)
        if(p != me && g->players[p].herd_count > best_other)
            best_other = g->players[p].herd_count;
    /* Camels pay for exchanges; the herd lead is worth the camel token. */
    v = (float)(herd < 6 ? herd : 6) * 0.9f + (float)(herd > 6 ? herd - 6 : 0) * 0.3f;
    if(herd > best_other)
        v += CAMEL_BONUS * 0.55f;
    else if(herd == best_other && herd > 0)
        v += CAMEL_BONUS * 0.15f;
    return v;
}

/* What the next player could gain from the market we leave behind. */
static float market_gift(const game_t *g, const uint8_t market[MARKET_SIZE],
                         int unknown) {
    uint8_t counts[GOOD_TYPES];
    float best = 0.0f;
    int s, t;
    memset(counts, 0, sizeof(counts));
    for(s = 0; s < MARKET_SIZE; ++s)
        if(market[s] != NO_CARD)
            counts[g->card_type[market[s]]]++;
    for(t = 0; t < TRADE_GOODS; ++t) {
        float v;
        if(!counts[t])
            continue;
        v = (float)jp_token_peek(g, t, 0) +
            (float)(counts[t] - 1) * (float)jp_token_peek(g, t, 1) * 0.6f;
        if(v > best)
            best = v;
    }
    return best * 0.45f + (float)counts[GOOD_CAMEL] * 0.35f +
           (float)unknown * 0.6f;
}

static float evaluate(const game_t *g, const ai_ctx_t *c, const move_t *m) {
    const int me = g->current;
    const player_t *pl = &g->players[me];
    uint8_t counts[GOOD_TYPES];
    uint8_t market[MARKET_SIZE];
    int herd = pl->herd_count;
    int unknown = 0;
    float gain = 0.0f;
    float before;
    int s, t;

    jp_hand_counts(g, me, counts);
    memcpy(market, g->market, sizeof(market));
    before = hand_value(g, c, counts) + herd_value(g, me, herd) +
             market_gift(g, g->market, 0) * 0.0f;

    switch(m->type) {
    case MOVE_TAKE_ONE:
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(m->market_mask & (1u << s)) {
                counts[g->card_type[market[s]]]++;
                market[s] = NO_CARD;
                unknown++;
            }
        }
        break;
    case MOVE_TAKE_CAMELS:
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(market[s] != NO_CARD &&
               g->card_type[market[s]] == GOOD_CAMEL) {
                herd++;
                market[s] = NO_CARD;
                unknown++;
            }
        }
        /* Revealing several new cards helps the next player. */
        gain -= (float)unknown * 0.15f;
        break;
    case MOVE_EXCHANGE: {
        /* Approximate which cards land where: only types matter. */
        uint8_t given[MARKET_SIZE];
        int ng = 0, i;
        for(i = 0; i < m->give_camels; ++i)
            given[ng++] = (uint8_t)(CARD_COUNT - 1); /* any camel id */
        for(t = 0; t < TRADE_GOODS; ++t) {
            for(i = 0; i < m->give[t]; ++i) {
                int id;
                for(id = 0; id < CARD_COUNT; ++id)
                    if(g->card_type[id] == t)
                        break;
                given[ng++] = (uint8_t)id;
            }
            counts[t] = (uint8_t)(counts[t] - m->give[t]);
        }
        herd -= m->give_camels;
        ng = 0;
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(m->market_mask & (1u << s)) {
                counts[g->card_type[market[s]]]++;
                market[s] = given[ng++];
            }
        }
        break;
    }
    case MOVE_SELL: {
        int good = m->sell_good;
        int n = m->sell_count;
        int avail = g->goods_token_count[good];
        int i;
        for(i = 0; i < n && i < avail; ++i)
            gain += (float)jp_token_peek(g, good, i);
        if(n >= 5)
            gain += bonus_average[2];
        else if(n >= 3)
            gain += bonus_average[n - 3];
        counts[good] = (uint8_t)(counts[good] - n);
        /* Emptying a token stack edges the round toward its end, which is
         * good when we lead and bad otherwise; handled by urgency. */
        break;
    }
    default:
        break;
    }

    {
        float after = hand_value(g, c, counts) + herd_value(g, me, herd);
        float gift = market_gift(g, market, unknown) -
                     market_gift(g, g->market, 0) * 0.5f;
        return gain + (after - before) - gift;
    }
}

bool ai_choose_move(const game_t *g, ai_level_t level, uint32_t *rng,
                    move_t *out) {
    static move_t moves[1024];
    ai_ctx_t c;
    int n, i, best = -1;
    float best_score = -1.0e9f;
    int empty = jp_tokens_empty_types(g);

    n = jp_enumerate_moves(g, moves, 1024);
    if(n <= 0)
        return false;

    c.urgency = 0.0f;
    if(empty >= 2)
        c.urgency += 0.5f;
    if(g->deck_count < 10)
        c.urgency += (float)(10 - g->deck_count) * 0.05f;
    if(c.urgency > 1.0f)
        c.urgency = 1.0f;
    c.hold = 0.78f - 0.45f * c.urgency;

    for(i = 0; i < n; ++i) {
        float s = evaluate(g, &c, &moves[i]);
        if(level == AI_EASY) {
            /* Noisy judgement with a soft spot for quick single takes. */
            s += (float)((int)(jp_rand(rng) % 1000u) - 500) * 0.008f;
            if(moves[i].type == MOVE_TAKE_ONE)
                s += 0.5f;
        }
        else {
            s += (float)(jp_rand(rng) % 100u) * 0.0005f; /* tie break */
        }
        if(s > best_score) {
            best_score = s;
            best = i;
        }
    }
    *out = moves[best];
    return true;
}
