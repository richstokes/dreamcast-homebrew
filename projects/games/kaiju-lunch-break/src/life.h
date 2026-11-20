/* The city's life: traffic, the crowd on the pavements, and the army
   that turns up once lunch gets out of hand. */
#ifndef KL_LIFE_H
#define KL_LIFE_H

#include "kl.h"

void life_init(void);
void life_reset(void);
void life_update(float dt);
void life_draw_op(void);
void life_draw_pt(void);

/* A foot (or a falling tower) comes down on the disc: vehicles under it
   are flattened. Returns how many. */
int life_stomp(V3 p, float r);
/* Vehicles within r of c are thrown outward. Returns how many. */
int life_launch(V3 c, float r, float power);
/* The claws: aircraft within reach in front are knocked down. */
int life_swat(V3 c, V3 fwd, float reach);
/* The jaws at mouth (and the ground under them): the nearest thing in
   reach is eaten. Returns what it was (LIFE_NONE if nothing). */
enum { LIFE_NONE, LIFE_CAR, LIFE_BUS, LIFE_TANK, LIFE_HELI, LIFE_NEWS, LIFE_JET };
int life_eat(V3 mouth, V3 ground, float r);
/* The jaws close on something to carry: a handle, or 0. It goes where
   life_hold puts it until life_throw lets it fly. */
int life_grab(V3 mouth, V3 ground, float r);
void life_hold(int handle, V3 p, float yaw);
void life_throw(int handle, V3 v);
/* What a handle is, for the HUD: "A BUS". */
const char *life_handle_name(int handle);
/* The breath from o along d. Returns how many things it burned. */
int life_beam(V3 o, V3 d, float len);
/* A fallen tower's strip of ground from p along (dx, dz). */
void life_crush_line(V3 p, float dx, float dz, float len, float half_w);
/* The roar: aircraft back off, the crowd runs. */
void life_scare(void);
/* How hard the army is trying: 0 (it has not noticed) to LIFE_THREAT_MAX. */
#define LIFE_THREAT_MAX 4
int life_threat(void);

#endif
