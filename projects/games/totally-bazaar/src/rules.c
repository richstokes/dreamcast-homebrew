/* Totally Bazaar rules engine. See rules.h. */
#include "rules.h"

#include <stdio.h>
#include <string.h>

const uint8_t jp_card_totals[GOOD_TYPES] = { 6, 6, 6, 8, 8, 10, 11 };

const char *const jp_good_names[GOOD_TYPES] = {
    "Diamonds", "Gold", "Silver", "Cloth", "Spice", "Leather", "Camels"
};

const char *const jp_good_singular[GOOD_TYPES] = {
    "Diamond", "Gold", "Silver", "Cloth", "Spice", "Leather", "Camel"
};

/* Goods token stacks, highest value on top (index 0). */
static const uint8_t token_values[TRADE_GOODS][MAX_GOOD_TOKENS] = {
    { 7, 7, 5, 5, 5 },
    { 6, 6, 5, 5, 5 },
    { 5, 5, 5, 5, 5 },
    { 5, 3, 3, 2, 2, 1, 1 },
    { 5, 3, 3, 2, 2, 1, 1 },
    { 4, 3, 2, 1, 1, 1, 1, 1, 1 }
};
static const uint8_t token_counts[TRADE_GOODS] = { 5, 5, 5, 7, 7, 9 };

static const uint8_t bonus_values[BONUS_SIZES][MAX_BONUS_TOKENS] = {
    { 3, 3, 2, 2, 2, 1, 1 },
    { 6, 6, 5, 5, 4, 4 },
    { 10, 10, 9, 8, 8 }
};
static const uint8_t bonus_counts[BONUS_SIZES] = { 7, 6, 5 };

uint32_t jp_rand(uint32_t *state) {
    uint32_t x = *state ? *state : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int rand_below(game_t *g, int n) {
    return (int)(jp_rand(&g->rng) % (uint32_t)n);
}

static void shuffle(game_t *g, uint8_t *items, int count) {
    int i;
    for(i = count - 1; i > 0; --i) {
        int j = rand_below(g, i + 1);
        uint8_t t = items[i];
        items[i] = items[j];
        items[j] = t;
    }
}

static int popcount8(uint8_t v) {
    int n = 0;
    while(v) {
        n += v & 1u;
        v >>= 1;
    }
    return n;
}

bool jp_is_precious(int good) {
    return good == GOOD_DIAMOND || good == GOOD_GOLD || good == GOOD_SILVER;
}

void jp_new_game(game_t *g, int num_players, uint32_t seed, int first_player) {
    int id = 0;
    int t, i;

    memset(g, 0, sizeof(*g));
    g->rng = seed ? seed : 0x2026u;
    if(num_players < MIN_PLAYERS)
        num_players = MIN_PLAYERS;
    if(num_players > MAX_PLAYERS)
        num_players = MAX_PLAYERS;
    g->num_players = (uint8_t)num_players;
    for(t = 0; t < GOOD_TYPES; ++t)
        for(i = 0; i < jp_card_totals[t]; ++i)
            g->card_type[id++] = (uint8_t)t;
    if(first_player < 0 || first_player >= num_players)
        first_player = rand_below(g, num_players);
    jp_start_round(g, first_player);
}

static void refill_market(game_t *g, move_result_t *res) {
    int s;
    for(s = 0; s < MARKET_SIZE; ++s) {
        if(g->market[s] != NO_CARD)
            continue;
        if(g->deck_count == 0)
            return;
        g->market[s] = g->deck[--g->deck_count];
        if(res)
            res->cards_drawn++;
    }
}

void jp_start_round(game_t *g, int starter) {
    uint8_t first_camel = (uint8_t)(CARD_COUNT - CAMEL_CARDS);
    int p, s, t, i, n = 0;

    for(p = 0; p < MAX_PLAYERS; ++p) {
        uint8_t seals = g->players[p].seals;
        memset(&g->players[p], 0, sizeof(player_t));
        g->players[p].seals = seals;
    }
    for(t = 0; t < TRADE_GOODS; ++t) {
        memcpy(g->goods_tokens[t], token_values[t], MAX_GOOD_TOKENS);
        g->goods_token_count[t] = token_counts[t];
    }
    for(t = 0; t < BONUS_SIZES; ++t) {
        memcpy(g->bonus_tokens[t], bonus_values[t], MAX_BONUS_TOKENS);
        g->bonus_token_count[t] = bonus_counts[t];
        shuffle(g, g->bonus_tokens[t], bonus_counts[t]);
    }

    /* Three camels start face up in the market. */
    for(s = 0; s < MARKET_SIZE; ++s)
        g->market[s] = NO_CARD;
    for(s = 0; s < 3; ++s)
        g->market[s] = (uint8_t)(first_camel + s);
    for(i = 0; i < CARD_COUNT; ++i)
        if(i < first_camel || i >= first_camel + 3)
            g->deck[n++] = (uint8_t)i;
    g->deck_count = (uint8_t)n;
    shuffle(g, g->deck, n);
    g->discard_count = 0;

    /* Deal five cards each; camels go straight into the herd. */
    for(i = 0; i < START_HAND; ++i) {
        for(p = 0; p < g->num_players; ++p) {
            int who = (starter + p) % g->num_players;
            player_t *pl = &g->players[who];
            uint8_t card = g->deck[--g->deck_count];
            if(g->card_type[card] == GOOD_CAMEL)
                pl->herd[pl->herd_count++] = card;
            else
                pl->hand[pl->hand_count++] = card;
        }
    }
    refill_market(g, NULL);

    g->current = (uint8_t)starter;
    g->round_starter = (uint8_t)starter;
    g->round_number++;
    g->round_over = false;
    g->camel_token_winner = -1;
    g->round_winners = 0;
    g->turn_count = 0;
}

int jp_hand_counts(const game_t *g, int player, uint8_t counts[GOOD_TYPES]) {
    const player_t *pl = &g->players[player];
    int i;
    memset(counts, 0, GOOD_TYPES);
    for(i = 0; i < pl->hand_count; ++i)
        counts[g->card_type[pl->hand[i]]]++;
    counts[GOOD_CAMEL] = pl->herd_count;
    return pl->hand_count;
}

int jp_market_counts(const game_t *g, uint8_t counts[GOOD_TYPES]) {
    int s, n = 0;
    memset(counts, 0, GOOD_TYPES);
    for(s = 0; s < MARKET_SIZE; ++s) {
        if(g->market[s] != NO_CARD) {
            counts[g->card_type[g->market[s]]]++;
            n++;
        }
    }
    return n;
}

move_error_t jp_validate(const game_t *g, const move_t *m) {
    const player_t *pl = &g->players[g->current];
    uint8_t hand[GOOD_TYPES];
    int s, t;

    if(g->round_over)
        return MOVE_ERR_BAD;
    jp_hand_counts(g, g->current, hand);

    switch(m->type) {
    case MOVE_TAKE_ONE: {
        int slot = -1;
        if(popcount8(m->market_mask) != 1)
            return m->market_mask ? MOVE_ERR_EXCHANGE_COUNT : MOVE_ERR_NOTHING;
        for(s = 0; s < MARKET_SIZE; ++s)
            if(m->market_mask & (1u << s))
                slot = s;
        if(g->market[slot] == NO_CARD)
            return MOVE_ERR_BAD;
        if(g->card_type[g->market[slot]] == GOOD_CAMEL)
            return MOVE_ERR_TAKE_CAMEL_SINGLY;
        if(pl->hand_count >= HAND_LIMIT)
            return MOVE_ERR_HAND_FULL;
        return MOVE_OK;
    }
    case MOVE_TAKE_CAMELS:
        for(s = 0; s < MARKET_SIZE; ++s)
            if(g->market[s] != NO_CARD &&
               g->card_type[g->market[s]] == GOOD_CAMEL)
                return MOVE_OK;
        return MOVE_ERR_NO_CAMELS;
    case MOVE_EXCHANGE: {
        int k = popcount8(m->market_mask);
        int given = m->give_camels;
        int given_goods = 0;
        uint8_t taken_types = 0;
        if(k < 2)
            return MOVE_ERR_EXCHANGE_MIN;
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(!(m->market_mask & (1u << s)))
                continue;
            if(g->market[s] == NO_CARD)
                return MOVE_ERR_BAD;
            t = g->card_type[g->market[s]];
            if(t == GOOD_CAMEL)
                return MOVE_ERR_TAKE_CAMEL_SINGLY;
            taken_types |= (uint8_t)(1u << t);
        }
        for(t = 0; t < TRADE_GOODS; ++t) {
            if(m->give[t] > hand[t])
                return MOVE_ERR_NOT_ENOUGH;
            if(m->give[t] && (taken_types & (1u << t)))
                return MOVE_ERR_SAME_TYPE;
            given_goods += m->give[t];
        }
        given += given_goods;
        if(m->give_camels > pl->herd_count)
            return MOVE_ERR_NOT_ENOUGH;
        if(given != k)
            return MOVE_ERR_EXCHANGE_COUNT;
        if(pl->hand_count - given_goods + k > HAND_LIMIT)
            return MOVE_ERR_HAND_FULL;
        return MOVE_OK;
    }
    case MOVE_SELL:
        if(m->sell_good == GOOD_CAMEL)
            return MOVE_ERR_SELL_CAMEL;
        if(m->sell_good >= TRADE_GOODS || m->sell_count == 0)
            return MOVE_ERR_NOTHING;
        if(m->sell_count > hand[m->sell_good])
            return MOVE_ERR_NOT_ENOUGH;
        if(jp_is_precious(m->sell_good) && m->sell_count < 2)
            return MOVE_ERR_SELL_PRECIOUS;
        return MOVE_OK;
    default:
        return MOVE_ERR_BAD;
    }
}

const char *jp_error_text(move_error_t e) {
    switch(e) {
    case MOVE_OK: return "";
    case MOVE_ERR_NOTHING: return "Select cards first";
    case MOVE_ERR_HAND_FULL: return "Hand limit is 7 cards";
    case MOVE_ERR_NO_CAMELS: return "No camels in the market";
    case MOVE_ERR_TAKE_CAMEL_SINGLY: return "Camels must all be taken together";
    case MOVE_ERR_EXCHANGE_COUNT: return "Give back as many cards as you take";
    case MOVE_ERR_EXCHANGE_MIN: return "Exchanges take at least 2 goods";
    case MOVE_ERR_SAME_TYPE: return "Can't take and give the same goods";
    case MOVE_ERR_NOT_ENOUGH: return "You don't have those cards";
    case MOVE_ERR_SELL_MIXED: return "Sell only one type of goods";
    case MOVE_ERR_SELL_PRECIOUS: return "Sell at least 2 of a precious good";
    case MOVE_ERR_SELL_CAMEL: return "Camels can't be sold";
    case MOVE_ERR_MIXED_ACTION: return "Take or sell - not both";
    default: return "Not a legal move";
    }
}

static void hand_remove_type(game_t *g, player_t *pl, int type, uint8_t *out) {
    int i;
    for(i = pl->hand_count - 1; i >= 0; --i) {
        if(g->card_type[pl->hand[i]] == type) {
            *out = pl->hand[i];
            memmove(&pl->hand[i], &pl->hand[i + 1],
                    (size_t)(pl->hand_count - i - 1));
            pl->hand_count--;
            return;
        }
    }
    *out = NO_CARD;
}

int jp_token_peek(const game_t *g, int good, int i) {
    int top;
    if(good < 0 || good >= TRADE_GOODS || i < 0 ||
       i >= g->goods_token_count[good])
        return 0;
    top = token_counts[good] - g->goods_token_count[good];
    return g->goods_tokens[good][top + i];
}

int jp_tokens_empty_types(const game_t *g) {
    int t, n = 0;
    for(t = 0; t < TRADE_GOODS; ++t)
        if(g->goods_token_count[t] == 0)
            n++;
    return n;
}

int jp_player_score(const game_t *g, int player) {
    const player_t *pl = &g->players[player];
    int i, total = 0;
    for(i = 0; i < pl->goods_token_count; ++i)
        total += pl->goods_tokens[i];
    for(i = 0; i < pl->bonus_token_count; ++i)
        total += pl->bonus_tokens[i];
    if(pl->camel_token)
        total += CAMEL_BONUS;
    return total;
}

void jp_score_round(game_t *g) {
    int p, best_herd = -1, herd_ties = 0;
    int best = -1;

    /* Camel token: strictly the largest herd. */
    for(p = 0; p < g->num_players; ++p) {
        int h = g->players[p].herd_count;
        if(h > best_herd) {
            best_herd = h;
            herd_ties = 1;
            g->camel_token_winner = (int8_t)p;
        }
        else if(h == best_herd) {
            herd_ties++;
        }
    }
    if(herd_ties != 1 || best_herd <= 0)
        g->camel_token_winner = -1;
    if(g->camel_token_winner >= 0)
        g->players[g->camel_token_winner].camel_token = 1;

    for(p = 0; p < g->num_players; ++p)
        g->players[p].round_score = (uint16_t)jp_player_score(g, p);

    /* Seal: most rupees, then most bonus tokens, then most goods tokens. */
    g->round_winners = 0;
    for(p = 0; p < g->num_players; ++p) {
        const player_t *pl = &g->players[p];
        int key = pl->round_score * 10000 + pl->bonus_token_count * 100 +
                  pl->goods_token_count;
        if(key > best) {
            best = key;
            g->round_winners = (uint8_t)(1u << p);
        }
        else if(key == best) {
            g->round_winners |= (uint8_t)(1u << p);
        }
    }
    for(p = 0; p < g->num_players; ++p)
        if(g->round_winners & (1u << p))
            g->players[p].seals++;

    /* The game ends when one player alone leads with two or more seals. */
    {
        int top = 0, leaders = 0, leader = 0;
        for(p = 0; p < g->num_players; ++p) {
            int s = g->players[p].seals;
            if(s > top) {
                top = s;
                leaders = 1;
                leader = p;
            }
            else if(s == top) {
                leaders++;
            }
        }
        g->game_over = top >= SEALS_TO_WIN && leaders == 1;
        g->game_winner = (uint8_t)leader;
    }
}

void jp_next_round(game_t *g) {
    /* The round's loser starts; ties go to the earliest after the last
     * starter so play keeps rotating. */
    int lowest = 1 << 30, starter = 0, i;
    for(i = 1; i <= g->num_players; ++i) {
        int p = (g->round_starter + i) % g->num_players;
        if(g->players[p].round_score < lowest) {
            lowest = g->players[p].round_score;
            starter = p;
        }
    }
    jp_start_round(g, starter);
}

bool jp_apply(game_t *g, const move_t *m, move_result_t *out) {
    move_result_t local;
    move_result_t *res = out ? out : &local;
    player_t *pl = &g->players[g->current];
    int s, t, i;

    memset(res, 0, sizeof(*res));
    if(jp_validate(g, m) != MOVE_OK)
        return false;
    res->move = *m;
    res->player = g->current;

    switch(m->type) {
    case MOVE_TAKE_ONE:
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(m->market_mask & (1u << s)) {
                pl->hand[pl->hand_count++] = g->market[s];
                g->market[s] = NO_CARD;
            }
        }
        refill_market(g, res);
        break;
    case MOVE_TAKE_CAMELS:
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(g->market[s] != NO_CARD &&
               g->card_type[g->market[s]] == GOOD_CAMEL) {
                pl->herd[pl->herd_count++] = g->market[s];
                g->market[s] = NO_CARD;
                res->move.market_mask |= (uint8_t)(1u << s);
            }
        }
        refill_market(g, res);
        break;
    case MOVE_EXCHANGE: {
        uint8_t given[MARKET_SIZE];
        uint8_t taken[MARKET_SIZE];
        int ng = 0, nt = 0;
        for(i = 0; i < m->give_camels; ++i)
            given[ng++] = pl->herd[--pl->herd_count];
        for(t = 0; t < TRADE_GOODS; ++t)
            for(i = 0; i < m->give[t]; ++i)
                hand_remove_type(g, pl, t, &given[ng++]);
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(m->market_mask & (1u << s)) {
                taken[nt++] = g->market[s];
                g->market[s] = given[nt - 1];
            }
        }
        for(i = 0; i < nt; ++i)
            pl->hand[pl->hand_count++] = taken[i];
        break;
    }
    case MOVE_SELL: {
        int good = m->sell_good;
        for(i = 0; i < m->sell_count; ++i) {
            uint8_t card;
            hand_remove_type(g, pl, good, &card);
            g->discard[g->discard_count++] = card;
            if(g->goods_token_count[good] > 0) {
                int top = token_counts[good] - g->goods_token_count[good];
                uint8_t v = g->goods_tokens[good][top];
                g->goods_token_count[good]--;
                pl->goods_tokens[pl->goods_token_count] = v;
                pl->goods_token_good[pl->goods_token_count] = (uint8_t)good;
                pl->goods_token_count++;
                res->token_values[res->tokens_taken++] = v;
                res->points += v;
            }
        }
        if(m->sell_count >= 3) {
            int size = m->sell_count >= 5 ? 2 : m->sell_count - 3;
            if(g->bonus_token_count[size] > 0) {
                uint8_t v = g->bonus_tokens[size][--g->bonus_token_count[size]];
                pl->bonus_tokens[pl->bonus_token_count] = v;
                pl->bonus_token_size[pl->bonus_token_count] = (uint8_t)(size + 3);
                pl->bonus_token_count++;
                res->bonus_value = v;
                res->bonus_size = (uint8_t)(size + 3);
                res->points += v;
            }
        }
        break;
    }
    default:
        return false;
    }

    g->turn_count++;
    for(s = 0; s < MARKET_SIZE; ++s)
        if(g->market[s] == NO_CARD)
            g->round_over = true;
    if(jp_tokens_empty_types(g) >= 3)
        g->round_over = true;
    res->ended_round = g->round_over;
    if(g->round_over)
        jp_score_round(g);
    else
        g->current = (uint8_t)((g->current + 1) % g->num_players);
    return true;
}

/* ---- Move enumeration (used by the CPU and the self-test) ---- */

typedef struct {
    move_t *out;
    int max;
    int n;
} move_list_t;

static void push_move(move_list_t *l, const move_t *m) {
    if(l->n < l->max)
        l->out[l->n] = *m;
    l->n++;
}

static void enum_give(const game_t *g, move_list_t *l, move_t *m,
                      const uint8_t hand[GOOD_TYPES], uint8_t taken_types,
                      int type, int remaining) {
    int c;
    if(remaining == 0) {
        if(jp_validate(g, m) == MOVE_OK)
            push_move(l, m);
        return;
    }
    if(type >= TRADE_GOODS)
        return;
    if(taken_types & (1u << type)) {
        enum_give(g, l, m, hand, taken_types, type + 1, remaining);
        return;
    }
    for(c = 0; c <= hand[type] && c <= remaining; ++c) {
        m->give[type] = (uint8_t)c;
        enum_give(g, l, m, hand, taken_types, type + 1, remaining - c);
    }
    m->give[type] = 0;
}

int jp_enumerate_moves(const game_t *g, move_t *out, int max) {
    move_list_t l = { out, max, 0 };
    const player_t *pl = &g->players[g->current];
    uint8_t hand[GOOD_TYPES];
    uint8_t seen_take = 0;
    uint32_t seen_sigs[32];
    int nsigs = 0;
    move_t m;
    int s, t, c;

    if(g->round_over)
        return 0;
    jp_hand_counts(g, g->current, hand);

    /* Take one card: one per goods type in the market. */
    for(s = 0; s < MARKET_SIZE; ++s) {
        int ty;
        if(g->market[s] == NO_CARD)
            continue;
        ty = g->card_type[g->market[s]];
        if(ty == GOOD_CAMEL || (seen_take & (1u << ty)))
            continue;
        seen_take |= (uint8_t)(1u << ty);
        memset(&m, 0, sizeof(m));
        m.type = MOVE_TAKE_ONE;
        m.market_mask = (uint8_t)(1u << s);
        if(jp_validate(g, &m) == MOVE_OK)
            push_move(&l, &m);
    }

    memset(&m, 0, sizeof(m));
    m.type = MOVE_TAKE_CAMELS;
    if(jp_validate(g, &m) == MOVE_OK)
        push_move(&l, &m);

    for(t = 0; t < TRADE_GOODS; ++t) {
        int lo = jp_is_precious(t) ? 2 : 1;
        for(c = lo; c <= hand[t]; ++c) {
            memset(&m, 0, sizeof(m));
            m.type = MOVE_SELL;
            m.sell_good = (uint8_t)t;
            m.sell_count = (uint8_t)c;
            push_move(&l, &m);
        }
    }

    /* Exchanges: each distinct multiset of market goods, every legal give. */
    for(c = 3; c < 32; ++c) {
        uint8_t mask = (uint8_t)c;
        uint8_t taken_types = 0;
        uint32_t sig = 0;
        int k = popcount8(mask), cam, dup = 0, i;
        bool ok = true;
        if(k < 2)
            continue;
        for(s = 0; s < MARKET_SIZE; ++s) {
            if(!(mask & (1u << s)))
                continue;
            if(g->market[s] == NO_CARD ||
               g->card_type[g->market[s]] == GOOD_CAMEL) {
                ok = false;
                break;
            }
            t = g->card_type[g->market[s]];
            taken_types |= (uint8_t)(1u << t);
            sig += 1u << (t * 4);
        }
        if(!ok)
            continue;
        for(i = 0; i < nsigs; ++i)
            if(seen_sigs[i] == sig)
                dup = 1;
        if(dup)
            continue;
        seen_sigs[nsigs++] = sig;
        for(cam = 0; cam <= k && cam <= pl->herd_count; ++cam) {
            memset(&m, 0, sizeof(m));
            m.type = MOVE_EXCHANGE;
            m.market_mask = mask;
            m.give_camels = (uint8_t)cam;
            enum_give(g, &l, &m, hand, taken_types, 0, k - cam);
        }
    }
    return l.n < max ? l.n : max;
}

bool jp_check_invariants(const game_t *g, char *why, int why_len) {
    uint8_t seen[CARD_COUNT];
    int p, i, s, t;
    memset(seen, 0, sizeof(seen));

#define MARK(id, where)                                                    \
    do {                                                                   \
        if((id) >= CARD_COUNT) {                                           \
            snprintf(why, (size_t)why_len, "bad id %d in %s", (int)(id),   \
                     where);                                               \
            return false;                                                  \
        }                                                                  \
        if(seen[(id)]++) {                                                 \
            snprintf(why, (size_t)why_len, "card %d duplicated (%s)",      \
                     (int)(id), where);                                    \
            return false;                                                  \
        }                                                                  \
    } while(0)

    for(i = 0; i < g->deck_count; ++i)
        MARK(g->deck[i], "deck");
    for(i = 0; i < g->discard_count; ++i)
        MARK(g->discard[i], "discard");
    for(s = 0; s < MARKET_SIZE; ++s)
        if(g->market[s] != NO_CARD)
            MARK(g->market[s], "market");
        else if(!g->round_over) {
            snprintf(why, (size_t)why_len, "empty market slot mid-round");
            return false;
        }
    for(p = 0; p < g->num_players; ++p) {
        const player_t *pl = &g->players[p];
        if(pl->hand_count > HAND_LIMIT) {
            snprintf(why, (size_t)why_len, "player %d hand %d", p,
                     pl->hand_count);
            return false;
        }
        for(i = 0; i < pl->hand_count; ++i) {
            MARK(pl->hand[i], "hand");
            if(g->card_type[pl->hand[i]] == GOOD_CAMEL) {
                snprintf(why, (size_t)why_len, "camel in hand");
                return false;
            }
        }
        for(i = 0; i < pl->herd_count; ++i) {
            MARK(pl->herd[i], "herd");
            if(g->card_type[pl->herd[i]] != GOOD_CAMEL) {
                snprintf(why, (size_t)why_len, "goods in herd");
                return false;
            }
        }
    }
#undef MARK
    for(i = 0; i < CARD_COUNT; ++i) {
        if(!seen[i] && !(g->round_over)) {
            snprintf(why, (size_t)why_len, "card %d missing", i);
            return false;
        }
    }
    /* Token conservation. */
    for(t = 0; t < TRADE_GOODS; ++t) {
        int held = 0;
        for(p = 0; p < g->num_players; ++p)
            for(i = 0; i < g->players[p].goods_token_count; ++i)
                if(g->players[p].goods_token_good[i] == t)
                    held++;
        if(held + g->goods_token_count[t] != token_counts[t]) {
            snprintf(why, (size_t)why_len, "token count mismatch for %s",
                     jp_good_names[t]);
            return false;
        }
    }
    return true;
}
