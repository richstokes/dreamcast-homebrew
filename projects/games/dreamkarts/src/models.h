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
/* Where baked model geometry goes: a triangle strip at a time, into the
   caller's own mesh builder. lit vertices want the world's shading by
   their normal; the rest take their colour as given. */
typedef struct {
    void *ctx;
    void (*strip_begin)(void *ctx);
    void (*vertex)(void *ctx, V3 p, V3 n, uint32_t c, bool lit);
    void (*strip_end)(void *ctx);
} ModelSink;
/* A Chao standing at p, facing yaw, its head turned head_yaw further,
   as static scenery (Night Meet's bystanders): the driver's body and
   head at model lod 0, 1 or 3, in the Chao colour and tip given, sent
   to sink strip by strip. */
void models_bake_chao(V3 p, float yaw, float head_yaw, uint32_t chao, uint32_t tip, int lod, const ModelSink *sink);

#endif
