/* Ridgeline CPU drivers. */
#ifndef RL_AI_H
#define RL_AI_H

#include "rl.h"

/* Truck ti will take the forest road, then route (1 logging road,
   2 ridge line, 3 creek bed), then the summit trail; from there it goes
   up and down the mountain, another way each time. Route 5 is the
   area's extreme trail, all the way to the top. */
void ai_start(int ti, int route);
#if defined(RL_FILM) || defined(RL_AI_TRACE)
/* Film builds and tools/hostsim: already on route `on` (level.h order) at
   point pt, setting off after delay seconds. */
void ai_start_on(int ti, int route, int on, int pt, float delay);
#endif
#ifdef RL_AI_TRACE
/* How often a driver has been winched or recovered (tools/hostsim: an
   extreme trail is proven by a climb with none). */
extern int ai_helped;
#endif
void ai_drive(int ti, float dt);
/* Has been to the summit. */
bool ai_done(int ti);
float ai_progress(int ti);
/* The route truck ti is on now, for a caption. */
const char *ai_route_name(int ti);

#endif
