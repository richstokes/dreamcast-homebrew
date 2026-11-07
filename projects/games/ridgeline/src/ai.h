/* Ridgeline CPU drivers. */
#ifndef RL_AI_H
#define RL_AI_H

#include "rl.h"

/* Truck ti will take the forest road, then route (1 logging road,
   2 ridge line, 3 creek bed), then the summit trail. */
void ai_start(int ti, int route);
void ai_drive(int ti, float dt);
bool ai_done(int ti);
float ai_progress(int ti);

#endif
