/* Ridgeline effects: dust, gravel, mud, snow and spray from the tyres. */
#ifndef RL_FX_H
#define RL_FX_H

#include "rl.h"

void fx_init(void);
void fx_clear(void);
void fx_update(float dt);
void fx_draw(void);      /* TR list */
void fx_splash(V3 p, float strength);

#endif
