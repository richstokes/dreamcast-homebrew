/* DreamKarts spectating: a TV director for watching a race nobody on this
   console is driving (an online lobby joined as a watcher). */
#ifndef DK_SPECTATE_H
#define DK_SPECTATE_H

#include "dk.h"

enum { CAM_AUTO, CAM_TV, CAM_CHASE, CAM_HELI, CAM_FRONT, CAM_BLIMP, CAM_MODES };

/* Turn spectating on or off (watchers stay on between races). */
void spectate_set(bool on);
bool spectate_on(void);
/* A race is lined up: place the trackside cameras and start directing. */
void spectate_begin(void);
/* Once a step while watching: buttons and the director's choices. */
void spectate_update(float dt);
/* The watched kart (also game.player, so its engine is the one heard). */
int spectate_target(void);
void spectate_camera(Camera *cam, float dt);
/* The overlay: standings, map, the watched kart and name tags. */
void spectate_draw(void);
/* Camera mode on screen now (CAM_*), and TV cameras placed for testing. */
int spectate_shot_mode(void);
int spectate_tv_count(void);
/* Test builds: put the camera on TV camera i, looking along its stretch;
   returns the lap distance it looks at. */
float spectate_tv_preview(Camera *cam, int i);
#ifdef DK_AT_SPECTATE
bool spectate_saw_grid(void);
#endif

#endif
