/* DreamKarts title screen and race setup menu. */
#ifndef DK_TITLE_H
#define DK_TITLE_H

#include "dk.h"

void title_init(void);
void title_enter(void);
/* Back to the main menu's Settings page (from the online name entry). */
void title_settings_enter(void);
/* The track select, from the results of a single race. */
void title_tracks_enter(void);
void title_update(float dt);
void title_camera(Camera *cam, float dt);
/* Two-player kart select: each player's camera on their own kart. */
void title_kart_cameras(void);
void title_draw(void);
/* Boot loading screen: progress 0..1, what is loading, seconds since boot. */
void title_draw_loading(float progress, const char *what, float t);
#ifdef DK_AT_SETTINGS
void title_settings_autotest(float dt);
#endif

#endif
