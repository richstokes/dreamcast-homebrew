/* Ridgeline best times: to the summit, per truck and per way up, kept on
   a VMU with the player's online name. The way up is the climbing route the player spent most of the
   climb on, or their own way. */
#ifndef RL_RECORDS_H
#define RL_RECORDS_H

#include "rl.h"

enum { WAY_LOGGING, WAY_RIDGE, WAY_CREEK, WAY_OWN, WAY_COUNT };

void records_load(void);              /* at boot, from the first VMU with a save */
void records_finish(void);            /* waits for a save still being written (before leaving) */
void records_start(void);             /* a drive begins: the clock and the route tally reset */
void records_update(float dt);        /* each driving frame */
void records_jumped(void);            /* the player was moved (a viewpoint jump): no time this run */
/* At the summit: fills msg with the time and the way up (a line for under
   the summit message), saves a new best. Returns true for a new best. */
bool records_summit(char *msg, int size);
/* Tenths of a second, 0 for none. */
uint32_t records_best(int truck, int way);
const char *records_way_name(int way);
void records_format(uint32_t tenths, char *out, int size);   /* "9:41.3" */
/* The online name kept with the times ("" for none yet); setting a new
   one saves. */
const char *records_name(void);
void records_set_name(const char *name);

#endif
