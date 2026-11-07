/* Ridgeline cameras. */
#ifndef RL_CAMERA_H
#define RL_CAMERA_H

#include "rl.h"

struct Truck;
void camera_reset(void);
/* game.cam_mode: 0 chase, 1 far chase, 2 bumper. */
void camera_follow(const struct Truck *t, float dt);
void camera_flyover(float t);
void camera_set(V3 pos, V3 target, float fov);
/* Photo mode: start orbiting the truck from where the camera is, then each
   frame turn with the stick and zoom with the triggers. */
void camera_photo_begin(const struct Truck *t);
void camera_photo(const struct Truck *t, float dt);

#endif
