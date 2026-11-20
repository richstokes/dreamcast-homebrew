/* Aiming a throw. While something is in the jaws the throw is worked out
   every frame: a helicopter or a building ahead is locked on to (the
   throw bends to hit it), and the arc, where it comes down and the lock
   are drawn so the player can see where it will go. */
#ifndef KL_AIM_H
#define KL_AIM_H

#include "kl.h"

void aim_init(void);
/* Something is held at p, the kaiju facing fwd; it falls at g m/s/s. */
void aim_update(V3 p, V3 fwd, float g, float dt);
/* Nothing is held. */
void aim_off(void);
/* The velocity to throw at (valid after aim_update). */
V3 aim_velocity(void);
/* What is locked on, for the HUD ("A HELICOPTER"), or NULL. */
const char *aim_locked(void);
void aim_draw_pt(void);
void aim_draw_tr(void);

#endif
