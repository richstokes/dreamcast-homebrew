/* DreamKarts title screen and race setup menu. */
#ifndef DK_TITLE_H
#define DK_TITLE_H

#include "dk.h"

void title_init(void);
void title_enter(void);
void title_update(float dt);
void title_camera(Camera *cam, float dt);
void title_draw(void);

#endif
