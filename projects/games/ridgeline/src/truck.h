/* Ridgeline trucks: rigid-body physics, suspension, tyres, drivetrain. */
#ifndef RL_TRUCK_H
#define RL_TRUCK_H

#include "rl.h"

enum { WFL, WFR, WRL, WRR };

/* A truck's build: geometry from trucks.h, the rest tuned here. */
/* The headlights: off, dipped (the lenses and a pool on the trail ahead)
   or full beam (brighter, further, and the roof lights with them). */
enum { LIGHTS_OFF, LIGHTS_DIPPED, LIGHTS_FULL, LIGHTS_COUNT };

typedef struct {
    const char *name;
    float mass;
    V3 inertia;            /* principal moments about right, up, forward */
    float cog_y;           /* centre of gravity above the ground at ride height */
    float cog_z;           /* ...and ahead of mid-wheelbase: trucks are nose-heavy */
    V3 wheel[4];           /* wheel centres at ride height, model space */
    float radius, width;
    float travel, sag;     /* suspension travel, and how much of it the weight takes */
    float k[2], c_bump[2], c_rebound[2];   /* per axle: front, rear */
    float lsd;             /* limited-slip rear: N m per rad/s of left-right difference */
    float arb_front, arb_rear;   /* anti-roll bars, N/m of difference */
    float wheel_inertia;
    float max_steer;       /* radians */
    float body_lo[3], body_hi[3];   /* collision box, model space */
    float hull_c[3], hull_h[3];     /* the box other trucks hit, wheels and all: centre, half sizes */
    float wade;            /* air intake height: deeper water stalls it */
    /* Engine and gearbox. */
    float idle_rpm, redline_rpm, peak_torque, peak_rpm;
    int gears;
    float ratio[8], reverse, final_drive, low_range;
    float brake_torque;
    int low_top_gear;      /* the highest gear low range uses */
    bool tcs;              /* fitted with traction control */
} TruckSpec;

#define LOW_RANGE_TOP_MPS 13.4f     /* ~30 mph */
#define LOW_RANGE_SHIFT_MPS 3.6f    /* ~8 mph: in or out of low range only below this */
#define REVERSE_TOP_MPS 8.94f       /* 20 mph: every truck's reverse governor */

typedef struct {
    float comp, comp_prev;      /* suspension compression from full droop, m */
    float omega, spin;          /* wheel speed (rad/s) and angle */
    float steer;
    bool contact;
    V3 centre;                  /* world */
    V3 cp, cn;                  /* contact point and normal */
    int surf;
    float load;                 /* N */
    float rl_long, rl_lat;      /* tyre (bristle) deflection, m */
    float slip;                 /* sliding speed at the contact, m/s */
    float water;                /* water depth over the contact */
    float fx, fy;               /* this step's tyre forces, for effects */
} Wheel;

typedef struct {
    float steer;      /* -1 left .. 1 right */
    float throttle;   /* 0..1 */
    float brake;      /* 0..1 */
    bool handbrake;
} TruckInput;

typedef struct Truck {
    int model, livery;
    TruckSpec spec;
    V3 pos, vel, w;           /* centre of gravity, velocity, angular velocity (world) */
    V3 ax[3];                 /* body axes in the world: right, up, forward */
    Wheel wh[4];
    /* Drivetrain. */
    int mode;                 /* DRIVE_* */
    int gear;                 /* -1 reverse, 1.. forward */
    float rpm, shift_t, shift_hold, rev_hold;
    float throttle, brake;    /* applied this step */
    float load;               /* engine load 0..1, for the sound */
    float tc;                 /* traction control's cut, 1 = none */
    bool tcs_off;             /* the driver switched traction control off */
    bool stalled;
    float stall_t, drown_t;
    /* Driver's things. */
    uint8_t lights;      /* LIGHTS_OFF, LIGHTS_DIPPED, LIGHTS_FULL */
    bool horn;
    bool cpu;
    /* Online: another console drives it (online.c poses it from the
       network, no physics here); absent while that slot is empty. */
    bool remote, absent;
    TruckInput in;
    /* Derived, for the HUD and effects. */
    float speed;              /* forward speed, m/s */
    float pitch, roll;        /* degrees, nose up and right side up positive */
    float hit;                /* the hardest knock this frame, m/s */
    float scrape;             /* chassis sliding on ground or rock */
    float water;              /* deepest water at the wheels */
    float airborne_t;
    float stuck_t;
    float dbg_chassis;        /* force the body itself carried last step, N */
    /* Previous frame's pose, for drawing between steps. */
} Truck;

extern Truck trucks[MAX_TRUCKS];

void truck_spec(TruckSpec *s, int model);
void truck_place(Truck *t, V3 base, float heading);
/* One display frame: the drive mode and gearbox, then the physics in
   substeps. */
void truck_update(Truck *t, float dt);
/* Pick the drive mode (D-pad left/right). Low range goes in or out only
   below LOW_RANGE_SHIFT_MPS. */
enum { MODE_SAME, MODE_CHANGED, MODE_TOO_FAST };
int truck_set_mode(Truck *t, int mode);
/* The highest gear the automatic will use now (fewer in low range). */
int truck_top_gear(const Truck *t);
/* Back on its wheels at the nearest point of a route behind it. */
void truck_recover(Truck *t);
/* The same on one route (-1: any). */
void truck_recover_on(Truck *t, int route);
void truck_recover_at(Truck *t, V3 at);
float truck_heading(const Truck *t);
/* A point in model space (origin on the ground at ride height) to world. */
/* How far two trucks' hulls overlap, metres (0 apart). */
float truck_overlap(const struct Truck *t, const struct Truck *u);
V3 truck_to_world(const Truck *t, V3 m);
/* A remote truck where the network says (centre of gravity, body axes,
   velocity, front wheel steer), its wheels set down on the ground here,
   turning with its speed. */
void truck_set_pose(Truck *t, V3 pos, const V3 ax[3], V3 vel, float steer, float dt);
V3 truck_dir_to_world(const Truck *t, V3 d);

#endif
