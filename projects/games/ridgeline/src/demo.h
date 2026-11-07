/* Ridgeline demo mode: the title left alone for ten seconds sends four
   CPU trucks up the mountain and cuts the camera between them, chase,
   helicopter and trackside shots a few seconds each, until a button is
   pressed. */
#ifndef RL_DEMO_H
#define RL_DEMO_H

#include "rl.h"

#define DEMO_IDLE_SECONDS 10.0f

void demo_start(void);
/* Back to the title (any button, or the demo has run its course). */
void demo_stop(void);
/* The drive step while game.demo: CPU drivers, physics and the camera. */
void demo_update(float dt);
/* TR list, in place of the HUD: the map, who we are watching, PRESS START. */
void demo_draw(void);

#endif
