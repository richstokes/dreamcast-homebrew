/* Totally Bazaar rules self-test. Built into the ELF with -DBAZAAR_SELFTEST and run
 * in Flycast; results go to the serial console. */
#include "selftest.h"

#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "rules.h"

static int failures;
static int checks;

#define EXPECT(cond, ...)                                                  \
    do {                                                                   \
        checks++;                                                          \
        if(!(cond)) {                                                      \
            failures++;                                                    \
            printf("BAZAAR SELFTEST FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                                           \
            printf("\n");                                                  \
        }                                                                  \
    } while(0)

/* Builds a controlled position: every card not placed explicitly stays in
 * the deck so the invariants still hold. */
typedef struct {
    int market[MARKET_SIZE];
    int hand[MAX_PLAYERS][GOOD_TYPES];   /* counts, camels go to the herd */
} setup_t;

static void build(game_t *g, int players, const setup_t *s) {
    uint8_t used[CARD_COUNT];
    int p, t, i, slot, n = 0;

    jp_new_game(g, players, 1234, 0);
    memset(used, 0, sizeof(used));
    for(p = 0; p < MAX_PLAYERS; ++p) {
        g->players[p].hand_count = 0;
        g->players[p].herd_count = 0;
    }
    for(slot = 0; slot < MARKET_SIZE; ++slot) {
        for(i = 0; i < CARD_COUNT; ++i) {
            if(!used[i] && g->card_type[i] == s->market[slot]) {
                used[i] = 1;
                g->market[slot] = (uint8_t)i;
                break;
            }
        }
    }
    for(p = 0; p < players; ++p) {
        for(t = 0; t < GOOD_TYPES; ++t) {
            int c = s->hand[p][t];
            for(i = 0; i < CARD_COUNT && c > 0; ++i) {
                if(used[i] || g->card_type[i] != t)
                    continue;
                used[i] = 1;
                c--;
                if(t == GOOD_CAMEL)
                    g->players[p].herd[g->players[p].herd_count++] = (uint8_t)i;
                else
                    g->players[p].hand[g->players[p].hand_count++] = (uint8_t)i;
            }
        }
    }
    for(i = 0; i < CARD_COUNT; ++i)
        if(!used[i])
            g->deck[n++] = (uint8_t)i;
    g->deck_count = (uint8_t)n;
    g->current = 0;
}

static void test_setup(void) {
    int players;
    for(players = 2; players <= 4; ++players) {
        game_t g;
        uint8_t market[GOOD_TYPES];
        char why[96];
        int p;
        jp_new_game(&g, players, 77u + (uint32_t)players, 0);
        jp_market_counts(&g, market);
        EXPECT(market[GOOD_CAMEL] >= 3, "market starts with 3+ camels");
        EXPECT(g.deck_count == CARD_COUNT - 3 - 2 - 5 * players,
               "deck count %d for %d players", g.deck_count, players);
        for(p = 0; p < players; ++p)
            EXPECT(g.players[p].hand_count + g.players[p].herd_count == 5,
                   "player %d dealt five", p);
        EXPECT(jp_check_invariants(&g, why, sizeof(why)), "setup: %s", why);
        EXPECT(g.round_number == 1, "round one");
    }
}

static void test_take_rules(void) {
    game_t g;
    setup_t s;
    move_t m;
    move_result_t r;

    memset(&s, 0, sizeof(s));
    s.market[0] = GOOD_CAMEL;
    s.market[1] = GOOD_DIAMOND;
    s.market[2] = GOOD_CAMEL;
    s.market[3] = GOOD_LEATHER;
    s.market[4] = GOOD_SPICE;
    s.hand[0][GOOD_CLOTH] = 3;
    s.hand[0][GOOD_CAMEL] = 2;
    s.hand[1][GOOD_GOLD] = 2;
    build(&g, 2, &s);

    memset(&m, 0, sizeof(m));
    m.type = MOVE_TAKE_ONE;
    m.market_mask = 1u << 0;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_TAKE_CAMEL_SINGLY,
           "single camel take rejected");
    m.market_mask = 1u << 1;
    EXPECT(jp_validate(&g, &m) == MOVE_OK, "single diamond take ok");
    m.market_mask = (1u << 1) | (1u << 3);
    EXPECT(jp_validate(&g, &m) != MOVE_OK, "take one with two cards rejected");

    m.market_mask = 1u << 1;
    EXPECT(jp_apply(&g, &m, &r), "apply take one");
    EXPECT(g.players[0].hand_count == 4, "hand grew to 4");
    EXPECT(r.cards_drawn == 1, "one card drawn into the market");
    EXPECT(g.current == 1, "turn passes");

    /* Player 2 takes every camel. */
    memset(&m, 0, sizeof(m));
    m.type = MOVE_TAKE_CAMELS;
    {
        uint8_t mk[GOOD_TYPES];
        int camels;
        jp_market_counts(&g, mk);
        camels = mk[GOOD_CAMEL];
        EXPECT(jp_apply(&g, &m, &r), "take camels");
        EXPECT(g.players[1].herd_count == camels, "all %d camels taken",
               camels);
        jp_market_counts(&g, mk);
        EXPECT(mk[GOOD_CAMEL] == 0 || r.cards_drawn > 0,
               "camels replaced from the deck");
    }

    /* Hand limit: seven cards blocks a single take. */
    memset(&s, 0, sizeof(s));
    s.market[0] = GOOD_SILVER;
    s.market[1] = GOOD_SILVER;
    s.market[2] = GOOD_CAMEL;
    s.market[3] = GOOD_GOLD;
    s.market[4] = GOOD_SPICE;
    s.hand[0][GOOD_LEATHER] = 4;
    s.hand[0][GOOD_CLOTH] = 3;
    s.hand[0][GOOD_CAMEL] = 1;
    build(&g, 2, &s);
    memset(&m, 0, sizeof(m));
    m.type = MOVE_TAKE_ONE;
    m.market_mask = 1u << 3;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_HAND_FULL, "hand limit 7");
    m.type = MOVE_TAKE_CAMELS;
    EXPECT(jp_validate(&g, &m) == MOVE_OK, "camels ignore hand limit");
}

static void test_exchange_rules(void) {
    game_t g;
    setup_t s;
    move_t m;
    move_result_t r;
    char why[96];

    memset(&s, 0, sizeof(s));
    s.market[0] = GOOD_GOLD;
    s.market[1] = GOOD_GOLD;
    s.market[2] = GOOD_LEATHER;
    s.market[3] = GOOD_CAMEL;
    s.market[4] = GOOD_SPICE;
    s.hand[0][GOOD_LEATHER] = 2;
    s.hand[0][GOOD_CLOTH] = 3;
    s.hand[0][GOOD_CAMEL] = 3;
    build(&g, 2, &s);

    memset(&m, 0, sizeof(m));
    m.type = MOVE_EXCHANGE;
    m.market_mask = 1u << 0;
    m.give[GOOD_CLOTH] = 1;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_EXCHANGE_MIN, "exchange needs 2");

    m.market_mask = (1u << 0) | (1u << 2);
    m.give[GOOD_CLOTH] = 1;
    m.give[GOOD_LEATHER] = 1;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_SAME_TYPE,
           "can't take and give leather");

    memset(m.give, 0, sizeof(m.give));
    m.give[GOOD_CLOTH] = 1;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_EXCHANGE_COUNT, "counts match");

    m.market_mask = (1u << 0) | (1u << 3);
    m.give[GOOD_CLOTH] = 2;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_TAKE_CAMEL_SINGLY,
           "camels can't be exchanged for");

    m.market_mask = (1u << 0) | (1u << 1) | (1u << 4);
    memset(m.give, 0, sizeof(m.give));
    m.give_camels = 3;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_HAND_FULL,
           "camels in an exchange count toward the hand limit");

    m.give_camels = 1;
    m.give[GOOD_CLOTH] = 2;
    EXPECT(jp_validate(&g, &m) == MOVE_OK, "2 cloth + camel for 2 gold+spice");
    EXPECT(jp_apply(&g, &m, &r), "apply exchange");
    EXPECT(g.players[0].hand_count == 6, "hand now 6 (%d)",
           g.players[0].hand_count);
    EXPECT(g.players[0].herd_count == 2, "herd now 2");
    {
        uint8_t mk[GOOD_TYPES];
        jp_market_counts(&g, mk);
        EXPECT(mk[GOOD_CLOTH] == 2 && mk[GOOD_CAMEL] == 2 &&
               mk[GOOD_LEATHER] == 1, "market got the given cards");
    }
    EXPECT(r.cards_drawn == 0, "exchanges never draw");
    EXPECT(jp_check_invariants(&g, why, sizeof(why)), "exchange: %s", why);
}

static void test_sell_rules(void) {
    game_t g;
    setup_t s;
    move_t m;
    move_result_t r;

    memset(&s, 0, sizeof(s));
    s.market[0] = GOOD_CAMEL;
    s.market[1] = GOOD_CAMEL;
    s.market[2] = GOOD_CAMEL;
    s.market[3] = GOOD_CLOTH;
    s.market[4] = GOOD_SPICE;
    s.hand[0][GOOD_DIAMOND] = 1;
    s.hand[0][GOOD_LEATHER] = 5;
    s.hand[1][GOOD_GOLD] = 3;
    build(&g, 2, &s);

    memset(&m, 0, sizeof(m));
    m.type = MOVE_SELL;
    m.sell_good = GOOD_DIAMOND;
    m.sell_count = 1;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_SELL_PRECIOUS,
           "single diamond can't sell");
    m.sell_good = GOOD_CAMEL;
    EXPECT(jp_validate(&g, &m) == MOVE_ERR_SELL_CAMEL, "camels can't sell");
    m.sell_good = GOOD_LEATHER;
    m.sell_count = 5;
    EXPECT(jp_apply(&g, &m, &r), "sell five leather");
    EXPECT(r.tokens_taken == 5, "five tokens");
    EXPECT(r.token_values[0] == 4 && r.token_values[1] == 3 &&
           r.token_values[2] == 2 && r.token_values[3] == 1,
           "highest tokens first");
    EXPECT(r.bonus_size == 5 && r.bonus_value >= 8 && r.bonus_value <= 10,
           "five-card bonus %d", r.bonus_value);
    EXPECT(r.points == 4 + 3 + 2 + 1 + 1 + r.bonus_value, "points %d",
           r.points);
    EXPECT(g.goods_token_count[GOOD_LEATHER] == 4, "four leather left");
    EXPECT(g.discard_count == 5, "five discards");

    m.sell_good = GOOD_GOLD;
    m.sell_count = 3;
    EXPECT(jp_apply(&g, &m, &r), "sell three gold");
    EXPECT(r.points == 6 + 6 + 5 + r.bonus_value && r.bonus_size == 3 &&
           r.bonus_value >= 1 && r.bonus_value <= 3, "gold sale %d",
           r.points);
}

static void test_round_end(void) {
    game_t g;
    setup_t s;
    move_t m;
    move_result_t r;

    /* Two empty stacks, then emptying silver ends the round. */
    memset(&s, 0, sizeof(s));
    s.market[0] = GOOD_CAMEL;
    s.market[1] = GOOD_CLOTH;
    s.market[2] = GOOD_CLOTH;
    s.market[3] = GOOD_CLOTH;
    s.market[4] = GOOD_SPICE;
    s.hand[0][GOOD_SILVER] = 5;
    s.hand[0][GOOD_CAMEL] = 2;
    s.hand[1][GOOD_GOLD] = 2;
    s.hand[1][GOOD_CAMEL] = 2;
    build(&g, 2, &s);
    g.goods_token_count[GOOD_DIAMOND] = 0;
    g.goods_token_count[GOOD_LEATHER] = 0;
    memset(&m, 0, sizeof(m));
    m.type = MOVE_SELL;
    m.sell_good = GOOD_SILVER;
    m.sell_count = 5;
    /* Tokens moved by hand break conservation; this test checks ends. */
    EXPECT(jp_apply(&g, &m, &r), "sell silver");
    EXPECT(g.round_over && r.ended_round, "three empty stacks end the round");
    EXPECT(g.camel_token_winner == -1, "tied herds: no camel token");
    EXPECT(g.players[0].seals == 1, "player one wins the seal");
    EXPECT(!g.game_over, "one seal is not a win");

    /* Deck exhaustion ends the round when the market can't refill. */
    memset(&s, 0, sizeof(s));
    s.market[0] = GOOD_CAMEL;
    s.market[1] = GOOD_CLOTH;
    s.market[2] = GOOD_CLOTH;
    s.market[3] = GOOD_CLOTH;
    s.market[4] = GOOD_SPICE;
    s.hand[0][GOOD_CAMEL] = 4;
    s.hand[1][GOOD_GOLD] = 2;
    build(&g, 2, &s);
    g.deck_count = 0;
    memset(&m, 0, sizeof(m));
    m.type = MOVE_TAKE_ONE;
    m.market_mask = 1u << 4;
    EXPECT(jp_apply(&g, &m, &r), "take with an empty deck");
    EXPECT(g.round_over, "empty deck ends the round");
    EXPECT(g.camel_token_winner == 0, "largest herd gets the camel token");
    EXPECT(g.players[0].round_score == CAMEL_BONUS, "camel token is 5");

    /* Second seal wins the game; the loser starts the next round. */
    g.players[0].seals = 1;
    g.players[0].seals++;
    jp_score_round(&g);
    EXPECT(g.game_over && g.game_winner == 0, "two seals win");
}

static void test_next_round_starter(void) {
    game_t g;
    jp_new_game(&g, 3, 99, 0);
    g.round_over = true;
    g.players[0].round_score = 40;
    g.players[1].round_score = 12;
    g.players[2].round_score = 30;
    jp_next_round(&g);
    EXPECT(g.current == 1 && g.round_number == 2, "loser starts round 2");
    EXPECT(g.players[1].goods_token_count == 0, "tokens reset");
}

static int random_move(const game_t *g, uint32_t *rng, move_t *out) {
    static move_t moves[1024];
    int n = jp_enumerate_moves(g, moves, 1024);
    if(n <= 0)
        return 0;
    *out = moves[jp_rand(rng) % (uint32_t)n];
    return 1;
}

/* kind: 0 random, 1 easy, 2 hard */
static int play_game(int players, uint32_t seed, const int kinds[MAX_PLAYERS],
                     int *rounds_out, int *turns_out) {
    game_t g;
    uint32_t rng = seed * 2654435761u + 1u;
    char why[96];
    int turns = 0;

    jp_new_game(&g, players, seed, -1);
    while(!g.game_over) {
        while(!g.round_over) {
            move_t m;
            move_result_t r;
            int k = kinds[g.current];
            int ok = k == 0 ? random_move(&g, &rng, &m) :
                     ai_choose_move(&g, k == 1 ? AI_EASY : AI_HARD, &rng, &m);
            if(!ok) {
                printf("BAZAAR SELFTEST FAIL: no legal move (seed %lu)\n",
                       (unsigned long)seed);
                failures++;
                return -1;
            }
            if(jp_validate(&g, &m) != MOVE_OK || !jp_apply(&g, &m, &r)) {
                printf("BAZAAR SELFTEST FAIL: generated illegal move\n");
                failures++;
                return -1;
            }
            if(!jp_check_invariants(&g, why, sizeof(why))) {
                printf("BAZAAR SELFTEST FAIL: invariant %s (seed %lu turn %d)\n",
                       why, (unsigned long)seed, turns);
                failures++;
                return -1;
            }
            if(++turns > 5000) {
                printf("BAZAAR SELFTEST FAIL: game does not end\n");
                failures++;
                return -1;
            }
        }
        if(!g.game_over) {
            if(g.round_number > 20) {
                printf("BAZAAR SELFTEST FAIL: too many rounds\n");
                failures++;
                return -1;
            }
            jp_next_round(&g);
        }
    }
    checks++;
    if(rounds_out)
        *rounds_out = g.round_number;
    if(turns_out)
        *turns_out = turns;
    return g.game_winner;
}

static void test_full_games(void) {
    int players, i;
    for(players = 2; players <= 4; ++players) {
        int kinds[MAX_PLAYERS] = { 0, 1, 2, 2 };
        int rounds_total = 0, turns_total = 0;
        for(i = 0; i < 40; ++i) {
            int rounds = 0, turns = 0;
            kinds[0] = i & 1 ? 0 : 2;
            if(play_game(players, 1000u + (uint32_t)i * 7u + (uint32_t)players,
                         kinds, &rounds, &turns) < 0)
                return;
            rounds_total += rounds;
            turns_total += turns;
        }
        printf("BAZAAR SELFTEST: %d players, 40 games, avg %d.%d rounds, "
               "avg %d turns/game\n", players, rounds_total / 40,
               (rounds_total * 10 / 40) % 10, turns_total / 40);
    }
}

static void test_ai_strength(void) {
    int i, hard_wins = 0, easy_hard = 0;
    int kinds[MAX_PLAYERS];
    for(i = 0; i < 60; ++i) {
        int hard_seat = i & 1;
        kinds[hard_seat] = 2;
        kinds[hard_seat ^ 1] = 0;
        if(play_game(2, 5000u + (uint32_t)i, kinds, NULL, NULL) == hard_seat)
            hard_wins++;
        kinds[hard_seat] = 2;
        kinds[hard_seat ^ 1] = 1;
        if(play_game(2, 9000u + (uint32_t)i, kinds, NULL, NULL) == hard_seat)
            easy_hard++;
    }
    printf("BAZAAR SELFTEST: CPU Hard beat random %d/60, beat Easy %d/60\n",
           hard_wins, easy_hard);
    EXPECT(hard_wins >= 54, "hard CPU should crush random play");
    EXPECT(easy_hard >= 33, "hard CPU should beat easy CPU");
}

int jp_selftest(void) {
    failures = 0;
    checks = 0;
    test_setup();
    test_take_rules();
    test_exchange_rules();
    test_sell_rules();
    test_round_end();
    test_next_round_starter();
    test_full_games();
    test_ai_strength();
    if(failures == 0)
        printf("BAZAAR SELFTEST: ALL TESTS PASS (%d checks).\n", checks);
    else
        printf("BAZAAR SELFTEST: FAILED (%d of %d checks).\n", failures, checks);
    return failures;
}
