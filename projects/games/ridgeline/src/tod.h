/* Ridgeline time of day: the clock, where the sun and the moon are, the
   palette the hour calls for, and relighting the world to it a slice at
   a time. */
#ifndef RL_TOD_H
#define RL_TOD_H

#include "rl.h"

/* Game minutes per real second: an hour of the day goes by in a minute. */
#define TOD_SECONDS_PER_HOUR 60.0f

/* Where a drive can start (the truck select's START AT row). */
enum { TOD_DAWN, TOD_MORNING, TOD_AFTERNOON, TOD_DUSK, TOD_NIGHT, TOD_COUNT };
extern const char *const tod_names[TOD_COUNT];
extern const float tod_hours[TOD_COUNT];

typedef struct {
    float hour;          /* 0 .. 24 */
    V3 sun, moon;        /* unit directions toward each (the sun's is below the ground at night) */
    float sun_el, moon_el;   /* radians above the horizon */
    float glow_az;       /* bearing the sky warms toward: the sun's, the moon's at night */
    bool moonlit;        /* the moon is the light the world is lit by */
} Tod;
extern Tod tod;

void tod_init(float hour);
/* Jump the clock; the world catches up over the next few frames. */
void tod_set_hour(float hour);
/* Each step: advance the clock (when run_clock), work out the palette and
   the light, relight a slice of the world. A bigger budget (menus, pause,
   photo mode: the frame has time to spare) relights more of it a frame. */
void tod_update(float dt, bool run_clock, float budget);
/* Relight the whole world now (tests). */
void tod_relight_all(void);
/* "17:40" into out. */
void tod_format(float hour, char *out, int size);
/* Microseconds spent relighting since the last reset (the test builds' log). */
extern uint64_t tod_us;

#endif
