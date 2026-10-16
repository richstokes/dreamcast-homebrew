/* Totally Bazaar rules engine: the complete Jaipur card game, 2 to 4 players.
 *
 * Every one of the 55 physical cards has a stable id so the renderer can
 * animate each card between deck, market, hands, herds and discard pile.
 * The engine is pure C with no KOS dependencies. */
#ifndef BAZAAR_RULES_H
#define BAZAAR_RULES_H

#include <stdbool.h>
#include <stdint.h>

enum {
    GOOD_DIAMOND = 0,
    GOOD_GOLD,
    GOOD_SILVER,
    GOOD_CLOTH,
    GOOD_SPICE,
    GOOD_LEATHER,
    GOOD_CAMEL,
    GOOD_TYPES = 7,
    TRADE_GOODS = 6
};

#define CARD_COUNT       55
#define MAX_PLAYERS      4
#define MIN_PLAYERS      2
#define MARKET_SIZE      5
#define HAND_LIMIT       7
#define START_HAND       5
#define CAMEL_CARDS      11
#define MAX_GOOD_TOKENS  9
#define SEALS_TO_WIN     2
#define CAMEL_BONUS      5
#define NO_CARD          0xff

/* Bonus token piles: index 0 is the three-card bonus, 1 four, 2 five+. */
#define BONUS_SIZES      3
#define MAX_BONUS_TOKENS 7

typedef enum {
    MOVE_TAKE_ONE = 0,  /* one goods card from the market, refill from deck */
    MOVE_TAKE_CAMELS,   /* every camel in the market, refill from deck */
    MOVE_EXCHANGE,      /* 2+ market goods for the same number of cards */
    MOVE_SELL           /* discard one type of goods for tokens */
} move_type_t;

typedef struct {
    uint8_t type;
    uint8_t market_mask;      /* market slots taken (take one / exchange) */
    uint8_t give[TRADE_GOODS];/* exchange: goods cards given, by type */
    uint8_t give_camels;      /* exchange: camels given from the herd */
    uint8_t sell_good;        /* sell: goods type */
    uint8_t sell_count;       /* sell: number of cards */
} move_t;

typedef enum {
    MOVE_OK = 0,
    MOVE_ERR_NOTHING,
    MOVE_ERR_HAND_FULL,
    MOVE_ERR_NO_CAMELS,
    MOVE_ERR_TAKE_CAMEL_SINGLY,
    MOVE_ERR_EXCHANGE_COUNT,
    MOVE_ERR_EXCHANGE_MIN,
    MOVE_ERR_SAME_TYPE,
    MOVE_ERR_NOT_ENOUGH,
    MOVE_ERR_SELL_MIXED,
    MOVE_ERR_SELL_PRECIOUS,
    MOVE_ERR_SELL_CAMEL,
    MOVE_ERR_MIXED_ACTION,
    MOVE_ERR_BAD
} move_error_t;

typedef struct {
    uint8_t hand[HAND_LIMIT];
    uint8_t hand_count;
    uint8_t herd[CAMEL_CARDS];
    uint8_t herd_count;
    uint8_t goods_tokens[TRADE_GOODS * MAX_GOOD_TOKENS]; /* values taken */
    uint8_t goods_token_good[TRADE_GOODS * MAX_GOOD_TOKENS];
    uint8_t goods_token_count;
    uint8_t bonus_tokens[BONUS_SIZES * MAX_BONUS_TOKENS];
    uint8_t bonus_token_size[BONUS_SIZES * MAX_BONUS_TOKENS]; /* 3, 4, 5 */
    uint8_t bonus_token_count;
    uint8_t camel_token;      /* 1 once awarded at round end */
    uint8_t seals;
    /* Round summary, filled at round end. */
    uint16_t round_score;
} player_t;

typedef struct {
    uint32_t rng;
    uint8_t num_players;
    uint8_t current;          /* player whose turn it is */
    uint8_t round_starter;
    uint8_t round_number;     /* 1-based */
    uint8_t card_type[CARD_COUNT];

    uint8_t deck[CARD_COUNT];
    uint8_t deck_count;       /* top card is deck[deck_count - 1] */
    uint8_t market[MARKET_SIZE];
    uint8_t discard[CARD_COUNT];
    uint8_t discard_count;

    uint8_t goods_tokens[TRADE_GOODS][MAX_GOOD_TOKENS]; /* [0] is the top */
    uint8_t goods_token_count[TRADE_GOODS];
    uint8_t bonus_tokens[BONUS_SIZES][MAX_BONUS_TOKENS]; /* shuffled */
    uint8_t bonus_token_count[BONUS_SIZES];

    player_t players[MAX_PLAYERS];

    bool round_over;
    bool game_over;
    int8_t camel_token_winner;           /* -1 when tied or nobody */
    uint8_t round_winners;               /* bitmask of seal winners */
    uint8_t game_winner;
    uint16_t turn_count;
} game_t;

/* Outcome of an applied move, for presentation. */
typedef struct {
    move_t move;
    uint8_t player;
    uint8_t tokens_taken;
    uint8_t token_values[MAX_GOOD_TOKENS];
    uint8_t bonus_value;      /* 0 when no bonus awarded */
    uint8_t bonus_size;
    uint8_t cards_drawn;
    uint16_t points;
    bool ended_round;
} move_result_t;

extern const uint8_t jp_card_totals[GOOD_TYPES];
extern const char *const jp_good_names[GOOD_TYPES];
extern const char *const jp_good_singular[GOOD_TYPES];

uint32_t jp_rand(uint32_t *state);
void jp_new_game(game_t *g, int num_players, uint32_t seed, int first_player);
void jp_start_round(game_t *g, int starter);

int jp_hand_counts(const game_t *g, int player, uint8_t counts[GOOD_TYPES]);
int jp_market_counts(const game_t *g, uint8_t counts[GOOD_TYPES]);
bool jp_is_precious(int good);

move_error_t jp_validate(const game_t *g, const move_t *m);
const char *jp_error_text(move_error_t e);
/* Applies a validated move, advances the turn, and resolves round/game end. */
bool jp_apply(game_t *g, const move_t *m, move_result_t *out);

/* Round end state: scores are in players[].round_score. */
void jp_score_round(game_t *g);
int jp_player_score(const game_t *g, int player);
int jp_tokens_empty_types(const game_t *g);
/* Prepare the next round after a round_over state that is not game_over. */
void jp_next_round(game_t *g);

/* i-th remaining goods token of a type from the top, or 0 when gone. */
int jp_token_peek(const game_t *g, int good, int i);

int jp_enumerate_moves(const game_t *g, move_t *out, int max);
/* Consistency check used by the self-test: every card in exactly one place. */
bool jp_check_invariants(const game_t *g, char *why, int why_len);

#endif
