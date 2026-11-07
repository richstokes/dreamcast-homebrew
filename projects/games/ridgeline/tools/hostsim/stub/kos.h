/* Host stand-in for KallistiOS, for tools/hostsim (physics on the Mac). */
#ifndef HOSTSIM_KOS_H
#define HOSTSIM_KOS_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
static inline uint64_t timer_us_gettime64(void) { return 0; }
#define KOS_INIT_FLAGS(x)
#endif
