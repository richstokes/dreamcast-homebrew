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
    EV_DRIFT_CANCEL = 1 << 15, EV_JUMP = 1 << 16, EV_WAVE = 1 << 17, EV_FALL = 1 << 18,
    EV_ITEM_GET = 1 << 19, EV_ITEM_USE = 1 << 20, EV_SPEAR = 1 << 21, EV_SPIN = 1 << 22,
    EV_SHIELD_UP = 1 << 23, EV_SHIELD_POP = 1 << 24, EV_MINE = 1 << 25, EV_MINE_HIT = 1 << 26
};

typedef struct {
    float top_speed;    /* m/s on asphalt without boost */
} CCParams;

typedef struct {
    /* Physics */
    V3 pos;             /* ground contact centre (world space) */
    V3 vel;             /* horizontal velocity (y unused) */
    float vy;           /* vertical speed; follows the road's slope when grounded */
    float heading;      /* travel heading, radians */
    float speed;        /* signed forward speed */
    bool grounded;
    bool big_air;       /* launched off a jump ramp: floatier gravity */
    float air_t;
    bool over_edge;     /* went off an open edge in the sky: falling */
    float gnd;          /* road height where the kart last touched down */
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
    float rev_t;        /* seconds into a held accelerate+brake burnout */
    float bump_t;
    int bump_with;      /* the kart behind the last EV_KART_BUMP */
    float land_v, wall_v;  /* impact speeds behind EV_LAND / EV_WALL */

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
    float lap_times[MAX_LAPS];
    float best_lap;

    /* Visual state */
    float vis_yaw;      /* body yaw offset relative to heading */
    float roll, pitch;
    float squash, squash_v;
    float wheel_spin;
    float steer_vis;
    float head_tilt;
    float emote_bob;
    bool skid;          /* braking hard: the tyres smoke */
    float wave_t;       /* seconds of wave left (0 = not waving) */
    float wave_back_t;  /* CPU: seconds until it waves back at a human */
    float cheer_t;      /* seconds spent cheering a win */
    float arm_up[2];    /* left/right arm off the wheel and raised, 0..1 */
    float raise_t;      /* seconds left holding a used emerald overhead */

    /* Power-ups (items.c) */
    int item;           /* ITEM_* held */
    float item_t;       /* seconds held */
    float ai_item_delay;/* CPU: seconds before it thinks about using it */
    float spin_t;       /* seconds of spin-out left (0 = driving) */
    int spin_dir;
    float shield_t;     /* seconds of barrier left */
    int spear_seq;      /* this kart's last spear number, 1-15 */
    int mine_seq;       /* this kart's last mine number, 1-15 */

    /* Identity */
    bool cpu;
    bool remote;        /* online rival driven by another console */
    int net_slot;       /* its lobby slot */
    bool gone;          /* online rival that dropped out of the race: not drawn, hit or placed */
    float skill;        /* CPU top-speed factor */
    float lane;         /* CPU lateral bias */
    float ai_drift_hold;
    bool ai_drift_btn;
    int colour;         /* kart_colours[] preset, or COLOUR_CUSTOM */
    int voice;          /* Chao voice pitch, 0..COLOUR_COUNT-1 */
    uint32_t paint1, paint2, chao, chao_tip;
    uint32_t ui_color;
    const char *name;

    uint32_t events;
} Kart;

/* Colour presets: kart body and trim paint, Chao body and tip (0 = the
   model's own), plus a flat colour for HUD dots and swatches. */
typedef struct {
    const char *name;
    uint32_t paint1, paint2, chao, tip, ui;
} KartColour;

#define COLOUR_COUNT 8
extern const KartColour kart_colours[COLOUR_COUNT];
/* A ninth choice: the player's own colours from the Customize screen, three
   picks from kart_palette[] (body paint, trim paint, Chao). Online each
   player's picks travel with their colour byte (net.h). */
#define COLOUR_CUSTOM COLOUR_COUNT
#define PALETTE_COUNT 16
typedef struct {
    const char *name;
    uint32_t rgb, tip;   /* tip: the Chao's points when this is its body */
} PaletteColour;
extern const PaletteColour kart_palette[PALETTE_COUNT];
typedef struct { uint8_t body, trim, chao; } KartCustom;
extern const KartCustom kart_custom_default;
/* The colours of a choice: a preset, or COLOUR_CUSTOM's from custom (NULL:
   the local player's saved picks). */
KartColour kart_colour_look(int colour, const KartCustom *custom);
/* The Chao voice of a choice (presets each have their own pitch). */
int kart_voice(int colour, const KartCustom *custom);
void kart_apply_look(Kart *k, int colour, const KartCustom *custom);
static inline void kart_apply_colour(Kart *k, int colour) { kart_apply_look(k, colour, NULL); }
bool kart_is_human(int index);
/* Emotes: a wave with the right arm, and both arms pumping in a cheer for
   as long as the kart has won the race. */
void kart_wave(Kart *k);
bool kart_cheering(const Kart *k);
/* Online: set a rival from the network and animate it locally. */
void kart_remote_update(Kart *k, V3 pos, float heading, float speed, float steer, int drift_dir, int stage,
                        bool boosting, bool grounded, float dt);

extern Kart karts[MAX_KARTS];
extern int kart_count;
extern int kart_jumps, kart_rescues;   /* ramp launches and gap rescues, all karts */
extern int kart_falls;                 /* drops back on after falling off an open edge */
extern const CCParams cc_params[CC_COUNT];

void kart_place_on_grid(Kart *k, int slot);
void kart_update(Kart *k, const KartInput *in, float dt, bool can_drive);
void kart_start_boost(Kart *k, float seconds, float kick);
/* Hit by a spear or a mine: a spin with the controls dead for SPIN_LEN seconds. */
void kart_spin_out(Kart *k, int dir);
void karts_collide(void);
void ai_drive(Kart *k, KartInput *in, float dt);
/* Replace a CPU's personality (the demo-race recorder varies them). */
void kart_set_ai_style(int idx, float look, float sway, float sways, float lead, float grip);
float kart_top_speed(const Kart *k);

#endif
