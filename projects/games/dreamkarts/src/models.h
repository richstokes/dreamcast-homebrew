/* DreamKarts kart and Chao driver rendering. */
#ifndef DK_MODELS_RENDER_H
#define DK_MODELS_RENDER_H

#include "kart.h"

void models_init(void);
/* Recompute a kart's per-vertex paint after its colours change. */
void models_paint_kart(int index);
/* Opaque list; view_kart is the kart this view follows (full detail). */
void models_draw_karts(int view_kart);
/* Call once per rendered frame (lighting is cached across views). */
void models_begin_frame(void);
void models_draw_kart(int index);      /* single kart, opaque list */
void models_draw_shadow_volumes(int view);  /* opaque modifier list */
void models_draw_shadows(int view);   /* translucent list */
/* Kart-space exhaust tips, for flames and smoke. */
V3 models_exhaust_world(const Kart *k, int which);
V3 models_wheel_world(const Kart *k, int wheel);
/* A kart-space point (x right, y up, z forward, metres) in the world. */
V3 models_kart_point_world(const Kart *k, V3 local);
/* How far the kart's lowest tyre is drawn under the road as drawn
   (negative: clear by that much; a large negative value off the road).
   For the autotest. */
float models_kart_sink(const Kart *k);

#endif
