/* Engine: the memory card prompt. A game with a save asks here when it
   cannot settle on a card by itself (../vmusave.h's vmusave_decide): two
   or more cards hold a save, or none does and the first card has no room
   for one. A panel over the title screen in the menu chrome (ui.h),
   worked with player 1's controller. */
#ifndef ENG_CARDPICK_H
#define ENG_CARDPICK_H

#include "ui.h"
#include "../vmusave.h"

enum {
    CARDPICK_WHICH,   /* which card's save to use: the cards holding one */
    CARDPICK_ROOM,    /* the first card is full: the cards with room, or to play without saving */
    CARDPICK_WHERE    /* the save's own card has gone or filled: the same choice, and a row
                         says whose save it would be written over */
};
enum {
    CARDPICK_WAIT = -3,     /* still up */
    CARDPICK_MOVED = -2,    /* still up, and the cursor moved (for the game's sound) */
    CARDPICK_NO_SAVE = -1   /* the player would rather not save */
};

/* Opens the prompt on a survey's slots (copied; their files are not
   kept). notes, if not NULL, is a word a slot on its save for the player
   to tell two apart by: a name, say. need is the save's size in blocks. */
void cardpick_open(int kind, const vmusave_slot_t *slots, const char *const *notes, int count, int need);
bool cardpick_up(void);
/* Once a step while it is up: CARDPICK_WAIT or CARDPICK_MOVED, or the
   answer, which closes it: the slot picked, or CARDPICK_NO_SAVE. */
int cardpick_update(void);
/* In the translucent list, after everything it should cover. */
void cardpick_draw(void);

#endif
