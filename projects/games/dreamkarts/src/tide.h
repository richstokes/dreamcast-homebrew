/* Emerald Beach's tide: the sea coming in over the outside of the road. */
#ifndef DK_TIDE_H
#define DK_TIDE_H

#include "kart.h"

void tide_init(void);
/* In this race at all: offline, and not in Time Trial, where laps are compared. */
bool tide_on(void);
/* The lateral offset the water reaches at lap distance s (off the road's
   outside when it is out). */
float tide_edge(float s);
bool tide_wet(const Kart *k);
void tide_update(float dt);
void tide_draw(void);

#endif
