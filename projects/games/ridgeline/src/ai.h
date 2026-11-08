/* Ridgeline CPU drivers. */
#ifndef RL_AI_H
#define RL_AI_H

#include "rl.h"

/* Truck ti will take the forest road, then route (1 logging road,
   2 ridge line, 3 creek bed), then the summit trail. */
void ai_start(int ti, int route);
#ifdef RL_FILM
/* Film builds: already on route `on` (level.h order) at point pt, setting
   off after delay seconds. */
void ai_start_on(int ti, int route, int on, int pt, float delay);
#endif
void ai_drive(int ti, float dt);
bool ai_done(int ti);
float ai_progress(int ti);
/* The route truck ti is on now, for a caption. */
const char *ai_route_name(int ti);

#endif
