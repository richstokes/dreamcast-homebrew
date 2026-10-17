/* DreamKarts: a pair of dolphins that leap offshore on the final lap. */
#ifndef DK_DOLPHINS_H
#define DK_DOLPHINS_H

#include "dk.h"

void dolphins_init(void);
void dolphins_reset(void);
/* Watch the humans' progress and run any leaping show. Race states only. */
void dolphins_update(float dt);
/* Opaque list, current view. */
void dolphins_draw(void);
/* Seconds into the newest show, or -1 when none is running. */
float dolphins_show_time(void);

#endif
