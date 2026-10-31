/* DreamKarts settings, kept in a save file on a VMU (see save.c): the
   online name, the music switch and volume, and the custom kart. */
#ifndef DK_SAVE_H
#define DK_SAVE_H

#include "dk.h"
#include "kart.h"

#define SAVE_NAME_LEN 11   /* NET_NAME_LEN: 10 characters and a NUL */
#define SAVE_VOLUME_MAX 10
#define SAVE_VOLUME_DEFAULT 10

typedef struct {
    char name[SAVE_NAME_LEN];   /* online name; empty until one is typed */
    bool music;                 /* the music switch */
    int volume;                 /* music volume, 0 (off) to SAVE_VOLUME_MAX */
    bool custom_on;             /* a custom kart has been saved */
    KartCustom custom;          /* its colours */
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

#endif
