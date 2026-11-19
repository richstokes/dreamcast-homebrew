/* Ridgeline: what knocks do to a truck. Scuffed panels, cracked glass,
   smashed lamps, and the bumpers, mirrors, roof lights, spare, doors, hood
   and tailgate knocked off to tumble down the hill; a meter of it all for
   the HUD. Some of it costs the truck: a stoved-in nose takes engine
   power (and steams), the whole of the damage a little more, and a side
   knocked harder than the other leaves the steering pulling that way. A
   wheel costs most: one that meets a trunk or a ledge hard enough comes off,
   and the truck is left with a bare hub that drags and that an open diff
   spins instead of driving the wheel across from it. Recovering the truck
   refits its wheels. */
#ifndef RL_DAMAGE_H
#define RL_DAMAGE_H

#include "rl.h"
#include "truck.h"

/* The body's faces, each scuffed 0..DMG_LEVELS - 1. */
enum { DMG_FRONT, DMG_REAR, DMG_LEFT, DMG_RIGHT, DMG_TOP, DMG_FACES };
#define DMG_LEVELS 4
/* The lamps a knock can put out, as bits. */
enum { DMG_HEAD_L = 1, DMG_HEAD_R = 2, DMG_TAIL_L = 4, DMG_TAIL_R = 8 };
#define DMG_PIECES 10         /* RL_PIECE_COUNT (damage.c checks) */
#define DMG_WHEEL0 DMG_PIECES /* then the four wheels (WFL..WRR), as pieces and as bits */
#define DMG_BITS (DMG_PIECES + 4)

/* A truck's damage as it is drawn, and as it goes over the wire. */
typedef struct {
    uint8_t face[DMG_FACES];  /* 0..3 */
    uint16_t pieces;          /* bit per piece gone (bit 0: RL_PART_PIECE0; from DMG_WHEEL0 the wheels) */
    uint8_t lamps;            /* DMG_HEAD_L .. */
    uint8_t percent;          /* the meter, 0..100 */
} DamageLook;

#define DMG_WIRE_BYTES 5     /* NET_DAMAGE_BYTES (online.c checks) */
void damage_pack(const DamageLook *d, uint8_t *b);
void damage_unpack(const uint8_t *b, DamageLook *d);

/* A drive begins or the area changes: every truck whole, nothing lying
   about. */
void damage_reset(void);
/* One truck whole again (the pause menu's repair; a truck changed or gone
   online), its lost pieces gone from the ground. */
void damage_repair(int truck);
/* Once a frame after the trucks have moved: this frame's knocks on the
   trucks simulated here, and the loose pieces. */
void damage_update(float dt);
/* A truck driven elsewhere (online): its damage as its console has it.
   Pieces it has lost since the last call fall off here; ones it arrived
   without were lost before we saw it. */
void damage_set(int truck, const DamageLook *d);

const DamageLook *damage_look(int truck);
/* Counts up whenever the truck's look changes (for whoever keeps its
   colours). */
unsigned damage_stamp(int truck);
/* How much of its headlights' light is left: 1, about half with one out,
   0 with both. */
float damage_beam(int truck);
/* The share of its engine's power it has lost, 0..1. */
float damage_power_loss(int truck);

/* The pieces knocked off, for drawing (truckdraw.c): boxes in the world. */
#define DMG_DEBRIS 12
typedef struct {
    bool on;
    uint8_t truck, model, livery, piece;   /* whose it was; piece from 0, a wheel from DMG_WHEEL0 */
    V3 pos;            /* where the piece's own origin (the truck's, when it was on it) now is */
    V3 ax[3];
    bool moving;       /* still tumbling: light it again */
} Debris;
const Debris *damage_debris(int i);

#endif
