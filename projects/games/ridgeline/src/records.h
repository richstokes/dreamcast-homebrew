/* Ridgeline's save: best times to the summit, per area, truck and way up,
   kept on a VMU with the player's online name and what they have done
   (progress.h: viewpoints found, the trail log's stamps, the trucks that
   earned). The way up is the climbing route the player spent most of the
   climb on, or their own way; the extreme trail's time is its own, gate
   to gate. */
#ifndef RL_RECORDS_H
#define RL_RECORDS_H

#include "rl.h"

enum { WAY_LOGGING, WAY_RIDGE, WAY_CREEK, WAY_OWN, WAY_EXTREME, WAY_COUNT };

/* The memory card. At boot, back at the title and going online: unless
   the card to save to is settled and still in, starts a look at every
   card, in the background. The title holds while it runs
   (records_looking, which lands what it found when it is done). One card
   with a save is read and is the card from then on; with no save
   anywhere it is the first card, if that has room. Otherwise the player
   is asked (records_card_ask) and nothing is written until they answer.
   A save read once is never read again: what is in memory stands. */
#include "../../../common/vmusave.h"
void records_cards(void);
bool records_looking(void);
/* The question waiting for the player, if there is one: CARDPICK_WHICH
   or CARDPICK_ROOM (engine/cardpick.h) with what to show it, else -1. */
int records_card_ask(const vmusave_slot_t **slots, const char *const **notes, int *count, int *need);
/* Their answer: one of those slots, or less than 0 to play without saving. */
void records_card_pick(int slot);
/* Something for the player to be told, once each: a save did not reach
   the card (full, or pulled; the next one tries again), or there is no
   card for saves to go to. 0 for nothing. */
enum { REC_NOTICE_FAILED = 1, REC_NOTICE_NO_CARD };
int records_notice(void);
#ifdef RL_AT_VMUTEST
bool records_cards_wait(void);        /* the save tests: the look, waited for; true once a save has been read */
void records_forget(void);            /* ...and as at boot, nothing read and no card */
#endif
void records_finish(void);            /* waits for a save still being written (before leaving) */
void records_start(void);             /* a drive begins: the clock and the route tally reset */
void records_update(float dt);        /* each driving frame */
void records_jumped(void);            /* the player was moved (a viewpoint jump): no time this run */
/* At the summit: fills msg with the time and the way up (a line for under
   the summit message), saves a new best. Returns true for a new best;
   *way_out (if not NULL) is the way up, WAY_*, or -1 after a jump. */
bool records_summit(char *msg, int size, int *way_out);
/* The extreme trail driven gate to gate in tenths of a second: the same,
   for the line under its message. */
bool records_extreme(uint32_t tenths, char *msg, int size);
/* Tenths of a second, 0 for none; the loaded area's. */
uint32_t records_best(int truck, int way);
/* The loaded area's ways up by name; short for the truck select's panel. */
const char *records_way_name(int way);
const char *records_way_short(int way);
void records_format(uint32_t tenths, char *out, int size);   /* "9:41.3" */
/* The online name kept with the times ("" for none yet); setting a new
   one saves. */
const char *records_name(void);
void records_set_name(const char *name);

/* What the player has done, kept with the times (progress.c gives it its
   meaning). Per area: the viewpoints ever found, a bit each, and the trail
   log's stamps. Adding bits already there does nothing; new ones save. */
#define REC_AREAS 8   /* areas the save has room for */
uint16_t records_views(int area);
uint16_t records_stamps(int area);
bool records_add_views(int area, uint16_t bits);     /* true if any were new */
bool records_add_stamps(int area, uint16_t bits);
/* Flags: the title's code has unlocked every truck; and the trucks whose
   unlocking the player has been told of, a bit a model. */
bool records_cheat(void);
void records_set_cheat(bool on);
uint8_t records_told(void);
void records_set_told(uint8_t models);
/* The pause menu's HUD choice (HUD_*), kept so the next game starts with
   it: a card's goes into game.hud_mode when the card is read. */
void records_set_hud(int mode);

#endif
