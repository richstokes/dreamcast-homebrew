/* Emerald Beach's tide: the sea coming in over the outside of the road. */
#ifndef DK_TIDE_H
#define DK_TIDE_H

#include "kart.h"

void tide_init(void);
/* With Emerald Beach's world, after its terrain. */
void tide_build(void);
/* In this race at all (Emerald Beach). */
bool tide_on(void);
/* Where the tide runs, open to the sea: no outer barrier is drawn there. */
bool tide_open(float s);
/* The lateral offset the water reaches at lap distance s (off the road's
   outside when it is out). */
float tide_edge(float s);
bool tide_wet(const Kart *k);
void tide_update(float dt);
void tide_draw(void);

#endif
