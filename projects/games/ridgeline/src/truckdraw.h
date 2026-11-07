/* Ridgeline: drawing the trucks, their shadows and lights. */
#ifndef RL_TRUCKDRAW_H
#define RL_TRUCKDRAW_H

#include "rl.h"

void truckdraw_init(void);
void truckdraw_draw_op(void);   /* bodies, wheels and axles */
void truckdraw_draw_tr(void);   /* shadows, headlight glows and the light ahead */

#endif
