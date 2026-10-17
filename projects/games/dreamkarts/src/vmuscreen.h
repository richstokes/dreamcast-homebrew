/* DreamKarts on the VMU screens: logo, countdown, place, laps, results. */
#ifndef DK_VMUSCREEN_H
#define DK_VMUSCREEN_H

#include "dk.h"

/* Once a frame: draws each VMU's screen and sends the ones that changed. */
void vmuscreen_update(float dt);

#endif
