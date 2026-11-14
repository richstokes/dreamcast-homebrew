/* DreamKarts settings, kept in a save file on a VMU (see save.c): the
   online name, the music switch and volume, the drift counter switch and
   the custom kart. */
#ifndef DK_SAVE_H
#define DK_SAVE_H

#include "dk.h"
#include "kart.h"

#define SAVE_NAME_LEN 11   /* NET_NAME_LEN: 10 characters and a NUL */
#define SAVE_VOLUME_MAX 10
#define SAVE_VOLUME_DEFAULT 10

/* Where the drift counter shows. */
enum { DRIFT_SHOW_OFF, DRIFT_SHOW_ROAM, DRIFT_SHOW_ALWAYS, DRIFT_SHOW_COUNT };

typedef struct {
    char name[SAVE_NAME_LEN];   /* online name; empty until one is typed */
    bool music;                 /* the music switch */
    int volume;                 /* music volume, 0 (off) to SAVE_VOLUME_MAX */
    bool custom_on;             /* a custom kart has been saved */
    KartCustom custom;          /* its colours */
    int drift_show;             /* DRIFT_SHOW_*: off, Free Roam only, every mode */
} SaveData;

/* The settings in force: the card's, or the defaults, until the player
   changes something; then theirs. */
extern SaveData save;

/* Defaults, then a background read of the card. */
void save_init(void);
/* Read the card again (for one put in since). A read never undoes a change
   the player made this session. */
void save_load(void);
/* Once a frame: true the frame a read lands and changes `save`, so the
   caller can apply it (the music switch). */
bool save_tick(void);
/* Counts the reads that have landed, for screens showing a setting. */
int save_gen(void);
/* Write `save` to the card in the background, unless it already holds it. */
void save_store(void);
/* Leaving the game: wait up to 3 s for a card read or write to finish. */
void save_finish(void);

/* The memory card (see save.c). At the title: looks at the cards again
   after a write that failed, or with none settled once one has come or
   gone. The title waits while a read is under way (save_looking), so a
   question about the cards comes up there and not later. */
void save_title(void);
bool save_looking(void);
/* The question waiting for the player, if there is one: CARDPICK_WHICH or
   CARDPICK_ROOM (engine/cardpick.h) with what to show it, else -1. */
#include "vmusave.h"
int save_card_ask(const vmusave_slot_t **slots, const char *const **notes, int *count, int *need);
/* Their answer: one of those slots, or less than 0 to play without saving. */
void save_card_pick(int slot);
/* True once after a write the card did not take (full, or pulled). */
bool save_failed(void);
#ifdef DK_AT_CARDS
void save_forget(void);   /* the card test: as at boot, nothing read and no card */
#endif

#endif
