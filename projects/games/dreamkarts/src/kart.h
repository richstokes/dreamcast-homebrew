/* DreamKarts kart physics, drifting and CPU drivers. */
#ifndef DK_KART_H
#define DK_KART_H

#include "dk.h"
#include "track.h"

enum { DRIFT_NONE, DRIFT_PENDING, DRIFT_ACTIVE, DRIFT_CHARGE };

/* One-frame events for audio and effects. */
enum {
    EV_HOP = 1 << 0, EV_LAND = 1 << 1, EV_DRIFT_START = 1 << 2,
    EV_CHARGE1 = 1 << 3, EV_CHARGE2 = 1 << 4, EV_CHARGE3 = 1 << 5,
    EV_BOOST = 1 << 6, EV_DASH = 1 << 7, EV_WALL = 1 << 8,
    EV_KART_BUMP = 1 << 9, EV_LAP = 1 << 10, EV_FINISH = 1 << 11,
    EV_CHARGE_JUMP = 1 << 12, EV_ROCKET = 1 << 13, EV_BURNOUT = 1 << 14,
    EV_DRIFT_CANCEL = 1 << 15
};

typedef struct {
    float top_speed;    /* m/s on asphalt without boost */
} CCParams;

typedef struct {
    /* Physics */
    V3 pos;             /* ground contact centre; y is height above ground */
    V3 vel;             /* horizontal velocity (y unused) */
    float vy;
    float heading;      /* travel heading, radians */
    float speed;        /* signed forward speed */
    bool grounded;
    float air_t;
    float steer;        /* smoothed steering -1..1 */
    bool prev_drift_btn;

    /* Drift */
    int drift;          /* DRIFT_* */
    int drift_dir;      /* -1 left, +1 right */
    float drift_t;
    float charge;       /* mini-turbo charge units */
    int stage;          /* 0 none, 1 blue, 2 orange, 3 purple */
    int boost_stage;    /* stage of the last released slide */
    float pending_t;
    float charge_jump_t;

    /* Boosts */
    float boost_t, boost_len;
    float boost_mul;
    int last_pad;
    float burnout_t;
    float bump_t;

    /* Track & race */
    TrackPos tp;
    int surface;
    int lap;            /* laps completed + 1 once over the line */
    bool checkpoint;    /* passed the half-lap marker this lap */
    float progress;     /* lap * length + s, for ordering */
    int place;          /* 1-based */
    bool finished;
    float finish_time;
    float lap_start;
    float lap_times[RACE_LAPS];
    float best_lap;

    /* Visual state */
    float vis_yaw;      /* body yaw offset relative to heading */
    float roll, pitch;
    float squash, squash_v;
    float wheel_spin;
    float steer_vis;
    float head_tilt;
    float emote_bob;

    /* Identity */
    bool cpu;
    float skill;        /* CPU top-speed factor */
    float lane;         /* CPU lateral bias */
    float ai_drift_hold;
    bool ai_drift_btn;
    uint32_t paint1, paint2, chao, chao_tip;

    uint32_t events;
} Kart;

extern Kart karts[MAX_KARTS];
extern int kart_count;
extern const CCParams cc_params[CC_COUNT];

void kart_place_on_grid(Kart *k, int slot);
void kart_update(Kart *k, const KartInput *in, float dt, bool can_drive);
void kart_start_boost(Kart *k, float seconds, float kick);
void karts_collide(void);
void ai_drive(Kart *k, KartInput *in, float dt);
float kart_top_speed(const Kart *k);

#endif
