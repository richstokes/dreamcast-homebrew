/* Ridgeline on the VMU screens: the mountain in the menus, an inclinometer
   and the altitude while driving. */
#ifndef RL_VMUSCREEN_H
#define RL_VMUSCREEN_H

#include "rl.h"

/* Once a frame: draws each VMU's screen and sends the ones that changed. */
void vmuscreen_update(float dt);

#endif
