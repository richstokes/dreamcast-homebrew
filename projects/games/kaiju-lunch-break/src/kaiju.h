/* The kaiju: eighteen rigid parts posed by code every frame.
   (Its moves: claws, tail, stomp, breath, chomp or grab-and-throw, roar,
   and a charge. It can wade out to chest depth.) The feet
   plant and step on their own as the body moves, the legs reach for them
   with two-bone IK, and the tail trails as a chain. */
#ifndef KL_KAIJU_H
#define KL_KAIJU_H

#include "kl.h"

enum { ACT_NONE, ACT_SWIPE, ACT_TAIL, ACT_JUMP, ACT_CHOMP, ACT_ROAR };

typedef struct {
    V3 pos;
    float heading, speed, turn_rate;
    float y;                 /* the feet's height in a jump */
    int action;
    float at;                /* seconds into the action */
    bool hit_done, queued;
    int arm;                 /* which arm swipes next */
    float energy;            /* the breath's charge, 0-1 */
    float flash, squash, roar_cd, look, jaw;
    bool breathing, sliced;
    int breath_kills;
    struct { V3 p, from; float t, dur, lift; bool planted; } foot[2];
    float tail_a[5];
    float clock;
    /* In the sea: how far the bed is under the feet. */
    float sink, splash;
    /* The charge: head down, through what is small, into what is not. */
    bool dashing;
    int dash_from;           /* buildings standing when it began */
    float stagger, dust;
    /* Carried in the jaws: a life.h handle (a vehicle, helicopter or
       person), or negative for a city chunk or a tree; 0 nothing. */
    int held;
    bool want_grab;
    float hold_t, toss;
    float grab_pop;          /* counts down the HUD's "GOT A BUS!" */
    /* A stomp in the sea sends a wave ashore. */
    V3 wave_c;
    float wave_r;
    int wave_from;
    /* Where things are this frame, in the world. */
    V3 head, mouth, mouth_dir;
} Kaiju;
extern Kaiju kj;

bool kaiju_init(void);
void kaiju_reset(void);
void kaiju_update(const Ctl *c, float dt);
void kaiju_draw(void);
void kaiju_draw_pt(void);
/* A shell or missile lands. */
void kaiju_hurt(void);
/* What is in the jaws, for the HUD (NULL: nothing). */
const char *kaiju_carrying(void);

#endif
