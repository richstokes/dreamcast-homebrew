/* DreamKarts controller vibration through a Jump Pack (Puru Puru). */
#ifndef DK_RUMBLE_H
#define DK_RUMBLE_H

#include "dk.h"
#include "kart.h"

/* Power is 1..7. Freq is the pack's vibration frequency field: low values
   rattle, high values buzz. */
void rumble_pulse(int player, int power, int freq, float seconds);
/* A level held for as long as it's asked for each step (curbs, sand). */
void rumble_hold(int player, int power, int freq);
/* Pulses and holds for a human player's kart events this step. */
void rumble_kart(int player, const Kart *k);
/* Once per displayed frame; inactive (menus, pause) stops every pack. */
void rumble_update(float dt, bool active);
void rumble_shutdown(void);

extern int rumble_sends;   /* effects sent to a pack, for the autotest */

#endif
