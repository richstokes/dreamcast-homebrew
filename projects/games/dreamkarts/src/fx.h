/* DreamKarts particles: drift sparks, boost flames, dust and bursts. */
#ifndef DK_FX_H
#define DK_FX_H

#include "kart.h"

void fx_init(void);
void fx_clear(void);
void fx_update(float dt);
/* Spawn per-kart effects from its state and this step's events. */
void fx_kart(const Kart *k, float dt);
void fx_confetti(V3 p, int count);
/* White water thrown up where something breaks the sea surface. */
void fx_splash(V3 p, float strength);
void fx_draw(void);

/* Spark colour for a mini-turbo stage (0..3). */
uint32_t fx_stage_color(int stage);

#endif
