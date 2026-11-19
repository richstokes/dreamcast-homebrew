/* Ridgeline effects: dust, gravel, mud, snow and spray from the tyres. */
#ifndef RL_FX_H
#define RL_FX_H

#include "rl.h"

void fx_init(void);
void fx_clear(void);
void fx_update(float dt);
void fx_draw(void);      /* TR list */
void fx_splash(V3 p, float strength);
/* n bits of something broken (glass, plastic) flung from p, moving with v. */
void fx_burst(V3 p, V3 v, uint32_t col, float size, int n);
/* A wisp of steam rising from p (a holed radiator), moving with v. */
void fx_steam(V3 p, V3 v);
/* How many prints a truck's tyres have laid that are still held. */
int fx_track_points(int truck);

#endif
