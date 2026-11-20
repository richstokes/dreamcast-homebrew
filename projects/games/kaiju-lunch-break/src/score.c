/* The lunch bill (score.h). */
#include "score.h"
#include "audio.h"

Score score;

static const struct { const char *name; int bonus; } tricks[TR_COUNT] = {
    [TR_DOMINO] = {"DOMINO", 2},
    [TR_TIMBER] = {"TIMBER!", 2},
    [TR_PANCAKE] = {"PANCAKE", 1},
    [TR_SWATTED] = {"SWATTED", 1},
    [TR_FAST_FOOD] = {"FAST FOOD", 1},
    [TR_BUS_LUNCH] = {"PACKED LUNCH", 1},
    [TR_TANK_YOU] = {"TANK YOU", 2},
    [TR_AIR_MEAL] = {"IN-FLIGHT MEAL", 2},
    [TR_BREAKING_NEWS] = {"BREAKING NEWS", 3},
    [TR_HOME_RUN] = {"HOME RUN", 1},
    [TR_SLICER] = {"SKYLINE SLICER", 2},
    [TR_BELLY_FLOP] = {"BELLY FLOP", 1},
    [TR_SHOCKWAVE] = {"SHOCKWAVE", 1},
    [TR_CHAIN_REACTION] = {"CHAIN REACTION", 1},
    [TR_LANDMARK] = {"LANDMARK DOWN", 4},
    [TR_BLOCK_PARTY] = {"BLOCK PARTY", 2},
    [TR_DESSERT] = {"DESSERT!", 4},
    [TR_TOP_GUN] = {"TOP GUN DOWN", 5},
    [TR_DELIVERY] = {"SPECIAL DELIVERY", 1},
    [TR_MIX_UP] = {"MIX-UP", 1},
    [TR_FULL_COURSE] = {"FULL COURSE MEAL", 4},
    [TR_ROAR] = {"ROAR!", 1},
    [TR_LUMBERJACK] = {"LUMBERJACK", 1},
    [TR_BULLDOZER] = {"BULLDOZER", 2},
    [TR_AIR_MAIL] = {"AIR MAIL", 3},
    [TR_TSUNAMI] = {"TSUNAMI", 3},
};

void score_reset(void) {
    memset(&score, 0, sizeof(score));
    score.mult = 1;
    score.last_src = -1;
    score.bank_age = 99;
    for(int i = 0; i < SCORE_FEED; i++) score.feed[i].age = 99;
}

static void recount(void) {
    /* Eight things in the chain a step; a trick's point is worth two. */
    int m = 1 + (score.chain + 2 * score.bonus) / 8, before = score.mult;
    score.mult = m > 100 ? 100 : m;
    /* The combo reaching ALL YOU CAN EAT gets a word, and so do 50x and the top. */
    if(before < 100 && score.mult >= 100) audio_sfx(KL_SFX_MONSTERKILL, 255);
    else if(before < 50 && score.mult >= 50) audio_sfx(KL_SFX_INSURANCE, 255);
    else if(before < 25 && score.mult >= 25) audio_sfx(KL_SFX_WOW, 255);
    if(score.mult > score.best_mult) score.best_mult = score.mult;
    score.timer_max = 3.2f - fminf(1.5f, (float)score.mult * 0.04f);
}

void score_bank(void) {
    if(!score.chain && !score.pot) return;
    uint64_t amount = score.pot * (uint64_t)score.mult;
    score.total += amount;
    if(amount > score.best_combo) score.best_combo = amount;
    score.bank_amount = amount;
    score.bank_mult = score.mult;
    score.bank_age = 0;
    if(score.mult >= 3) audio_sfx(KL_SFX_BANK, 200);
    score.pot = 0;
    score.chain = score.bonus = 0;
    score.used = 0;
    score.last_src = -1;
    score.repeat = 0;
    score.roared = false;
    memset(score.done, 0, sizeof(score.done));
    score.timer = 0;
    recount();
}

void score_hit(int src, uint32_t value_k, V3 at) {
    (void)at;
    /* The same move over and over stops building the chain. */
    if(src == score.last_src) score.repeat++;
    else score.repeat = 1;
    score.last_src = src;
    if(score.repeat <= 6) score.chain++;
    score.pot += value_k;
    score.damage += value_k;
    if(!(score.used & (1u << src))) {
        score.used |= 1u << src;
        int kinds = __builtin_popcount(score.used);
        if(kinds == 4) score_trick(TR_MIX_UP, 1);
        else if(kinds == 7) score_trick(TR_FULL_COURSE, 1);
    }
    recount();
    score.timer = score.timer_max;
    score.pop = 1.0f;
    score.energy_gain += 0.035f + fminf((float)value_k * 2.5e-6f, 0.12f);
    int m = score.mult > 24 ? 24 : score.mult;
    audio_sfx_rate(KL_SFX_TICK, 110, 0.8f + (float)m * 0.05f);
}

void score_chip(uint32_t value_k) {
    score.total += value_k;
    score.damage += value_k;
}

void score_chunk(uint32_t value_k) {
    score.damage += value_k;
    if(score.chain) {
        score.pot += value_k;
        score.timer = fminf(score.timer_max, score.timer + 0.35f);
    }
    else score.total += value_k;
    score.energy_gain += 0.008f;
}

void score_trick(int trick, int n) {
    (void)n;
    int times = score.done[trick] < 255 ? ++score.done[trick] : 255;
    int bonus = times <= 3 || times % 3 == 0 ? tricks[trick].bonus : 0;
    score.bonus += bonus;
    score.tricks++;
    if(score.feed[0].name == tricks[trick].name && score.feed[0].age < 1.5f) {
        /* More of the same: one line that counts up. */
        score.feed[0].bonus += bonus;
        score.feed[0].times++;
    }
    else if(bonus) {   /* (one that has stopped paying is not news) */
        for(int i = SCORE_FEED - 1; i > 0; i--) score.feed[i] = score.feed[i - 1];
        score.feed[0].name = tricks[trick].name;
        score.feed[0].bonus = bonus;
        score.feed[0].times = 1;
    }
    if(bonus) score.feed[0].age = 0;
    recount();
    score.timer = fminf(score.timer_max, fmaxf(score.timer, 1.0f) + 0.8f);
    score.pop = 1.0f;
    audio_sfx(KL_SFX_TRICK, 170);
}

void score_hurt(void) {
    if(score.chain) score.timer -= 0.9f;
}

void score_roar(void) {
    if(!score.chain || score.roared) return;
    score.roared = true;
    score_trick(TR_ROAR, 1);
    score.timer = score.timer_max;
}

void score_update(float dt) {
    for(int i = 0; i < SCORE_FEED; i++) score.feed[i].age += dt;
    score.bank_age += dt;
    score.pop = approach(score.pop, 0, dt * 4.0f);
    if(score.chain || score.pot) {
        score.timer -= dt;
        if(score.timer <= 0) {
            if(score.mult < 3 && score.chain) audio_sfx(KL_SFX_DROP, 90);
            score_bank();
        }
    }
}

const char *score_rank(int mult) {
    return mult >= 60 ? "EXTINCTION BUFFET" : mult >= 40 ? "FEEDING FRENZY" : mult >= 25 ? "ALL YOU CAN EAT" :
           mult >= 15 ? "MAIN COURSE" : mult >= 8 ? "APPETIZER" : mult >= 3 ? "SNACK" : "";
}

int score_grade(uint64_t total) {
    /* Thousands of dollars: five billion is a snack, 350 a legend. */
    static const uint64_t steps[5] = {5000000ull, 20000000ull, 60000000ull, 150000000ull, 350000000ull};
    int g = 0;
    while(g < 5 && total >= steps[g]) g++;
    return g;
}

void score_money(char *out, size_t n, uint64_t value_k) {
    char digits[32], tmp[48];
    snprintf(digits, sizeof(digits), "%llu", (unsigned long long)value_k);
    int len = (int)strlen(digits), o = 0;
    tmp[o++] = '$';
    for(int i = 0; i < len; i++) {
        tmp[o++] = digits[i];
        if((len - 1 - i) % 3 == 0) tmp[o++] = ',';
    }
    if(value_k == 0) o = 2;   /* "$0" */
    else {
        memcpy(tmp + o, "000", 3);
        o += 3;
    }
    tmp[o] = 0;
    snprintf(out, n, "%s", tmp);
}
