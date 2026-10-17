/* DreamKarts kart and Chao driver rendering. */
#ifndef DK_MODELS_RENDER_H
#define DK_MODELS_RENDER_H

#include "kart.h"

void models_init(void);
/* Recompute a kart's per-vertex paint after its colours change. */
void models_paint_kart(int index);
void models_draw_karts(void);          /* opaque list */
void models_draw_kart(int index);      /* single kart, opaque list */
void models_draw_shadows(void);        /* translucent list */
/* Kart-space exhaust tips, for flames and smoke. */
V3 models_exhaust_world(const Kart *k, int which);
V3 models_wheel_world(const Kart *k, int wheel);

#endif
