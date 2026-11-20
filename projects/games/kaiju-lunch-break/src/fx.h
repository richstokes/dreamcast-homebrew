/* Effects: toon dust, impact stars, flying rubble, shock rings, comic
   words over the big hits, the breath's beam and the camera shake. */
#ifndef KL_FX_H
#define KL_FX_H

#include "kl.h"

void fx_init(void);
void fx_clear(void);
void fx_update(float dt);
void fx_draw_op(void);
void fx_draw_pt(void);
void fx_draw_tr(void);

void fx_puff(V3 p, V3 vel, float size, float life, uint32_t col);
/* n puffs scattered over a disc on the ground, billowing out and up. */
void fx_puffs(V3 c, float radius, int n, float size);
void fx_debris(V3 c, int n, float speed, uint32_t tint, float size);
void fx_boom(V3 c, float size);
void fx_ring(V3 c, float radius);
/* A comic word over a hit: level 0 (POW) to 3 (KA-BOOM). */
void fx_callout(V3 c, int level);
void fx_shake(float amount);
float fx_shake_now(void);
/* Hold the world still for a moment on a big hit. fx_frozen counts it
   down: true while the frame should not step. */
void fx_hitstop(float seconds);
bool fx_frozen(float dt);
/* A flame standing at p, in the punch-through list. */
void fx_flame(V3 p, float size, float phase);
/* The breath, mouth to end, for this frame. */
void fx_beam(V3 a, V3 b);

#endif
