#ifndef ISLAND_DOLPHINS_H
#define ISLAND_DOLPHINS_H
#include "engine.h"

#define DOLPHIN_PODS 8
#define DOLPHIN_CAPACITY (DOLPHIN_PODS * 2)
#define DOLPHIN_SPLASH_LIFE 1.65f

typedef struct {
    int act;
    Vec3 center; /* Y is the local water surface. */
    Vec3 direction;
    float first,period;
} DolphinPod;
typedef struct {
    Vec3 position,right,up,forward;
    float scale,tail;
    unsigned pod;
} Dolphin;
typedef struct {Vec3 position;float age,strength;} DolphinSplash;
typedef struct {
    Dolphin dolphins[DOLPHIN_CAPACITY];
    DolphinSplash splashes[DOLPHIN_CAPACITY * 2];
    unsigned count,splash_count;
} DolphinFrame;
extern const DolphinPod dolphin_pods[DOLPHIN_PODS];

/* Absolute-time sampling also works after travel, frame skips and benchmarks.
   All storage belongs to the caller; there are no particle allocations. */
void dolphins_sample(int act,float time,DolphinFrame *frame);
#endif
