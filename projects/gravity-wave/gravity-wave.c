/*
 * Gravity Wave
 *
 * A cinematic infinite 3D arcade flight game for Sega Dreamcast. Procedural
 * terrain and encounters are combined with baked PVR-native material art.
 */

#include <kos.h>

#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/purupuru.h>
#include <dc/pvr.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/video.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/generated/texture_assets.h"
#include "model_data.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define SCREEN_W             640.0f
#define SCREEN_H             480.0f
#define SCREEN_CX            320.0f
#define SCREEN_CY            240.0f
#define PI                   3.14159265358979323846f
#define TAU                  (2.0f * PI)
#define FOCAL_CRUISE         430.0f
#define FOCAL_BOOST          392.0f
#define FOCAL_BRAKE          456.0f
#define NEAR_PLANE           5.0f
#define FAR_PLANE            1450.0f

#define TERRAIN_COLS         23
#define TERRAIN_ROWS         41
#define TERRAIN_SPACING_X    26.0f
#define TERRAIN_SPACING_Z    35.0f
#define TERRAIN_NEAR_Z       80.0f
#define MAX_ENEMIES          28
#define MAX_SHOTS            36
#define MAX_ENEMY_SHOTS      28
#define MAX_PARTICLES        176
#define MAX_GATES            5
#define MAX_PICKUPS          12
#define STAR_COUNT           54
#define BIOME_LENGTH         4300.0f
#define BIOME_BLEND_LENGTH   480.0f
#define SCENERY_SEGMENT      96.0f
#define COURSE_PHRASES_PER_BIOME 6
#define COURSE_PHRASE_LENGTH \
    (BIOME_LENGTH / (float)COURSE_PHRASES_PER_BIOME)
#define COURSE_ROUTE_KNOTS_PER_BIOME 10
#define COURSE_ROUTE_KNOT_LENGTH \
    (BIOME_LENGTH / (float)COURSE_ROUTE_KNOTS_PER_BIOME)
#define COURSE_TRAVERSALS_PER_BIOME 2
#define COURSE_WAVES_PER_BIOME 3
#define COURSE_SIGNATURE_LOCAL_Z 2100.0f
/* Regular craft sit another COURSE_FORMATION_LEAD units beyond their beat,
   so a 320-unit beat recovery is roughly a 740-unit visual runway. */
#define COURSE_GUARDIAN_WAVE_RECOVERY 320.0f
#define COURSE_GUARDIAN_GATE_RECOVERY 900.0f
#define COURSE_FORMATION_LEAD 420.0f
#define COURSE_TRAVERSAL_SCENERY_CLEARANCE 96.0f

#define MUSIC_TRACK_COUNT    8
#define MUSIC_SECTION_COUNT  3
#define MUSIC_SAMPLE_RATE    21500
#define MUSIC_BARS           2
#define MUSIC_BEATS          (MUSIC_BARS * 4)
#define MUSIC_PHRASE_BARS    (MUSIC_BARS * MUSIC_SECTION_COUNT)
#define MUSIC_MELODY_STEPS   (MUSIC_BEATS * 2)
#define MUSIC_EDGE_SAMPLES   1024
#define MUSIC_AICA_RESERVE   (256u * 1024u)
#define MUSIC_SINE_TABLE_SIZE 2048
#define MUSIC_ASSET_HEADER_BYTES 8u
#define MUSIC_SYNTH_REVISION 0x20260831u
#define MUSIC_REST           (-127)
#define MUSIC_TIE            (-126)
#define MUSIC_TITLE_VOLUME   68
#define MUSIC_GAME_VOLUME    84
#define TEMP_WEAPON_SECONDS  15.0f
#define SPEED_BOOST_SECONDS  8.0f
#define GATE_CLEAR_RADIUS_SCALE 0.7874008f
#define VECTOR_LANE_PYLON_HALF_WIDTH 6.4f
#define VECTOR_LANE_PYLON_CLEARANCE  5.0f

#define PLAYER_Z             44.0f
#define PLAYER_AIM_Z        440.0f
#define PLAYER_MIN_X        -84.0f
#define PLAYER_MAX_X         84.0f
#define PLAYER_MIN_Y         12.0f
#define PLAYER_MAX_Y         96.0f

#define ARRAY_COUNT(a)       ((int)(sizeof(a) / sizeof((a)[0])))

typedef struct {
    float r, g, b;
} color3_t;

typedef struct {
    float x, y, z;
} vec3_t;

typedef struct {
    float x, y, z;
    bool valid;
} screen_point_t;

typedef struct {
    color3_t sky_top;
    color3_t sky_horizon;
    color3_t fog;
    color3_t ground_low;
    color3_t ground_high;
    color3_t river;
    color3_t accent;
    color3_t enemy;
    const char *name;
} palette_t;

typedef enum {
    MODE_TITLE,
    MODE_PLAYING,
    MODE_PAUSED,
    MODE_GAME_OVER
} game_mode_t;

typedef enum {
    TITLE_MENU_START,
    TITLE_MENU_SOUND_TEST,
    TITLE_MENU_EXIT,
    TITLE_MENU_ITEM_COUNT
} title_menu_item_t;

typedef struct {
    bool active;
    float x, y, z;
    float vx, vy;
    float phase;
    float radius;
    int type;
    int hp;
    int max_hp;
    int biome;
    float fire_timer;
    float hit_flash;
    bool fired;
} enemy_t;

typedef struct {
    const char *name;
    int base_hp;
    int hp_growth;
    int shot_damage;
    float radius;
    float phase_speed;
    float shot_speed;
    float fire_cadence;
    float model_scale;
} guardian_profile_t;

typedef enum {
    SHOT_PLAYER_LASER,
    SHOT_PLAYER_FAST,
    SHOT_PLAYER_PHASE,
    SHOT_ENEMY
} projectile_kind_t;

typedef struct {
    bool active;
    /* Projectiles alone use absolute world XYZ so their launch ray remains
       straight while the route bends beneath them. */
    float x, y, z;
    float vx, vy, vz;
    float life;
    float spin;
    projectile_kind_t kind;
    int damage;
    int hits_remaining;
    uint32_t hit_mask;
} projectile_t;

typedef enum {
    PARTICLE_SPRITE,
    PARTICLE_STREAK,
    PARTICLE_EXHAUST,
    PARTICLE_BOSS_IMPACT
} particle_kind_t;

typedef struct {
    bool active;
    float x, y, z;
    float vx, vy, vz;
    float life, max_life;
    float size;
    color3_t color;
    particle_kind_t kind;
} particle_t;

typedef enum {
    GATE_RESULT_PENDING,
    GATE_RESULT_CLEARED,
    GATE_RESULT_MISSED
} gate_result_t;

typedef enum {
    TRAVERSAL_GRAVITY_BLOOM,
    TRAVERSAL_SLALOM,
    TRAVERSAL_SHEAR_BARRIER,
    TRAVERSAL_KIND_COUNT
} traversal_kind_t;

typedef struct {
    bool active;
    bool faulted;
    bool announced;
    float x, y, z;
    float radius;
    float spin;
    float result_time;
    float stage_spacing;
    int variant;
    int direction;
    int stage;
    int stage_count;
    uint32_t success_mask;
    uint32_t perfect_mask;
    traversal_kind_t kind;
    gate_result_t result;
} gate_t;

typedef enum {
    PICKUP_LASER_CORE,
    PICKUP_REPAIR,
    PICKUP_NOVA,
    PICKUP_SPEED_BOOST,
    PICKUP_FAST_LASER,
    PICKUP_PHASE_WAVE,
    PICKUP_KIND_COUNT
} pickup_kind_t;

typedef enum {
    TEMP_WEAPON_NONE,
    TEMP_WEAPON_FAST_LASER,
    TEMP_WEAPON_PHASE_WAVE
} temporary_weapon_t;

typedef struct {
    bool active;
    float x, y, z;
    float spin;
    float life;
    pickup_kind_t kind;
} pickup_t;

typedef struct {
    screen_point_t screen;
    float height;
    uint32_t color;
} terrain_point_t;

typedef struct {
    uint32_t buttons;
    uint32_t pressed;
    float x, y;
    float left_trigger;
    float right_trigger;
    bool connected;
} input_t;

typedef struct {
    const char *name;
    float bpm;
    int root_midi;
    int8_t chord_offsets[MUSIC_PHRASE_BARS];
    int8_t chord_extensions[MUSIC_PHRASE_BARS];
    uint8_t minor_mask;
    int8_t bass[8 * MUSIC_SECTION_COUNT];
    uint8_t arpeggio[16];
    int8_t melody[MUSIC_MELODY_STEPS * MUSIC_SECTION_COUNT];
    uint16_t kick[MUSIC_PHRASE_BARS];
    uint16_t snare[MUSIC_PHRASE_BARS];
    uint16_t hat[MUSIC_PHRASE_BARS];
    uint16_t open_hat[MUSIC_PHRASE_BARS];
    uint16_t tom[MUSIC_PHRASE_BARS];
    uint16_t stab[MUSIC_PHRASE_BARS];
    int lead_profile;
    int bass_profile;
    int arpeggio_profile;
    int harmony_interval;
    float pulse_width;
    float lead_gate;
    float sidechain;
    float drive;
    float pad_level;
    float bass_level;
    float arpeggio_level;
    float lead_level;
    float kick_level;
    float snare_level;
    float hat_level;
    float open_hat_level;
    float tom_level;
    float stab_level;
    uint32_t noise_seed;
} soundtrack_t;

typedef struct {
    game_mode_t mode;
    float time;
    float distance;
    float speed;
    float player_x;
    float player_y;
    float player_vx;
    float player_vy;
    float camera_x;
    float camera_y;
    float bank;
    float barrel_roll;
    float roll_cooldown;
    float shield;
    float hit_cooldown;
    float scenery_hit_cooldown;
    float boost;
    float fire_cooldown;
    float exhaust_timer;
    float bomb_wave;
    int bombs;
    int score;
    float score_fraction;
    int combo;
    float combo_timer;
    int wave;
    float next_wave_z;
    float next_gate_z;
    int palette_index;
    int last_palette_index;
    char message[40];
    float message_time;
    float survival_time;
    int weapon_level;
    temporary_weapon_t temporary_weapon;
    float temporary_weapon_time;
    float speed_boost_time;
    int weapon_shot_counter;
    float camera_focal;
    float trauma;
    int guardians_destroyed;
    int music_chapter;
    int last_course_act;
    float suppressed_gate_from_z;
    float suppressed_gate_until_z;
    uint32_t previous_buttons;
    bool previous_left_trigger;
    bool previous_right_trigger;
    bool controller_notice;
} game_t;

static const palette_t palettes[] = {
    {
        {0.015f, 0.035f, 0.120f}, {0.620f, 0.190f, 0.390f},
        {0.330f, 0.210f, 0.430f}, {0.035f, 0.095f, 0.135f},
        {0.190f, 0.460f, 0.520f}, {0.060f, 0.820f, 1.000f},
        {1.000f, 0.390f, 0.640f}, {1.000f, 0.250f, 0.390f},
        "AZURE REACH"
    },
    {
        {0.005f, 0.075f, 0.095f}, {0.120f, 0.500f, 0.470f},
        {0.180f, 0.440f, 0.400f}, {0.030f, 0.120f, 0.100f},
        {0.310f, 0.570f, 0.290f}, {0.670f, 1.000f, 0.390f},
        {0.180f, 0.960f, 0.750f}, {1.000f, 0.500f, 0.130f},
        "EMERALD VEIL"
    },
    {
        {0.045f, 0.010f, 0.110f}, {0.390f, 0.100f, 0.430f},
        {0.330f, 0.160f, 0.410f}, {0.080f, 0.025f, 0.130f},
        {0.470f, 0.240f, 0.620f}, {0.970f, 0.150f, 0.850f},
        {0.430f, 0.540f, 1.000f}, {1.000f, 0.230f, 0.650f},
        "VIOLET RIFT"
    },
    {
        {0.075f, 0.010f, 0.020f}, {0.780f, 0.230f, 0.080f},
        {0.520f, 0.210f, 0.120f}, {0.120f, 0.035f, 0.025f},
        {0.600f, 0.260f, 0.090f}, {1.000f, 0.730f, 0.120f},
        {1.000f, 0.330f, 0.080f}, {0.480f, 0.820f, 1.000f},
        "EMBER CROWN"
    }
};

/* The four guardians are authored encounters rather than palette swaps. Their
 * profile data drives durability, collision, motion tempo, weapon behavior,
 * model proportions, HUD identity, and the deterministic diagnostics below. */
static const guardian_profile_t guardian_profiles[4] = {
    {
        "TIDEBREAKER AEGIS", 30, 5, 10,
        35.0f, 1.10f, 136.0f, 1.08f, 1.66f
    },
    {
        "VERDANT BASTION", 38, 6, 14,
        39.0f, 0.76f, 116.0f, 1.28f, 1.88f
    },
    {
        "PRISM SERAPH", 34, 5, 11,
        33.0f, 1.34f, 144.0f, 0.94f, 1.56f
    },
    {
        "INFERNO MANTIS", 32, 5, 12,
        34.0f, 1.52f, 154.0f, 0.70f, 1.62f
    }
};

#define GUARDIAN_SHOT_STYLE_FLAG 0x100u

/* Eight original synthwave/synthpop songs are authored by the synth below and
 * shipped as native AICA ADPCM. Each A/B/C triptych is a six-bar arcade form
 * arrangement: an establishing groove, a hook variation, and a full-power
 * final section. The runtime renderer remains as a verified fallback and as
 * the source of truth used to bake the linked album image. */
static const soundtrack_t soundtrack_defs[MUSIC_TRACK_COUNT] = {
    {
        .name = "MIDNIGHT VECTOR",
        .bpm = 164.0f,
        .root_midi = 40, /* Em, Cmaj7, Gadd9, Dadd9, Am7, B7. */
        .chord_offsets = {0,-4,3,-2,5,7},
        .chord_extensions = {10,11,14,14,10,10},
        .minor_mask = 0x11,
        .bass = {
            0,MUSIC_REST,12,0, 7,MUSIC_REST,12,MUSIC_REST,
            0,0,12,MUSIC_REST, 7,12,0,MUSIC_REST,
            0,12,7,0, 12,7,15,12
        },
        .arpeggio = {0,2,3,1, 2,4,3,2, 0,3,5,2, 4,3,2,1},
        .melody = {
             0,MUSIC_TIE,MUSIC_TIE,7, 12,MUSIC_TIE,10,7,
             5,MUSIC_TIE,7,10, 7,5,3,MUSIC_TIE,
            12,MUSIC_TIE,15,12, 10,7,5,7,
            10,MUSIC_TIE,7,5, 3,5,0,MUSIC_TIE,
            12,15,19,15, 12,10,7,10,
             7,11,14,17, 19,17,14,11
        },
        .kick = {0x1111,0x5111,0x1151,0x9511,0x5151,0xd551},
        .snare = {0x1010,0x1010,0x1010,0x5010,0x1010,0x7010},
        .hat = {0xaaaa,0xeeee,0xaaaa,0xffee,0xeeee,0xffff},
        .open_hat = {0x0000,0x2002,0x0202,0x2022,0x2222,0xaaaa},
        .tom = {0x0000,0x0000,0x0000,0x8400,0x0000,0xf000},
        .stab = {0x1010,0x1111,0x5151,0x1111,0x5555,0xd555},
        .lead_profile = 0,
        .bass_profile = 1, .arpeggio_profile = 0,
        .harmony_interval = 7, .pulse_width = 0.38f, .lead_gate = 0.82f,
        .sidechain = 0.78f, .drive = 0.50f,
        .pad_level = 0.150f, .bass_level = 0.245f,
        .arpeggio_level = 0.130f, .lead_level = 0.180f,
        .kick_level = 0.345f, .snare_level = 0.190f,
        .hat_level = 0.064f, .open_hat_level = 0.042f,
        .tom_level = 0.135f, .stab_level = 0.105f,
        .noise_seed = 0x8bd13a47u
    },
    {
        .name = "MAGENTA CIRCUIT",
        .bpm = 170.0f,
        .root_midi = 42, /* F#m, Bm, Dmaj7, C#7, Amaj7, Eadd9. */
        .chord_offsets = {0,5,-4,7,3,-2},
        .chord_extensions = {10,10,11,10,11,14},
        .minor_mask = 0x03,
        .bass = {
            0,0,MUSIC_REST,12, 0,7,12,7,
            0,MUSIC_REST,0,12, 7,12,7,MUSIC_REST,
            0,12,0,7, 12,15,12,7
        },
        .arpeggio = {0,1,3,2, 4,2,5,3, 0,2,4,6, 5,3,2,1},
        .melody = {
             0,3,7,MUSIC_REST, 8,7,5,3,
             0,MUSIC_REST, 5,7, 12,10,8,MUSIC_REST,
            12,15,12,10, 8,MUSIC_REST,7,5,
             7,11,14,17, 19,17,14,MUSIC_REST,
            12,15,19,20, 19,15,12,10,
             8,10,12,15, 17,15,12,MUSIC_REST
        },
        .kick = {0x1151,0x5151,0x1551,0xd151,0x5551,0xf551},
        .snare = {0x1010,0x1030,0x1010,0x1010,0x1030,0x3010},
        .hat = {0xeeee,0xffee,0xeeee,0xffff,0xffee,0xffff},
        .open_hat = {0x0202,0x2022,0x2222,0xaaaa,0x2222,0xaaaa},
        .tom = {0x0000,0x0400,0x0000,0xc400,0x0800,0xf400},
        .stab = {0x1111,0x5151,0x5555,0x5151,0x5555,0xd555},
        .lead_profile = 1,
        .bass_profile = 3, .arpeggio_profile = 1,
        .harmony_interval = -5, .pulse_width = 0.26f, .lead_gate = 0.72f,
        .sidechain = 0.86f, .drive = 0.58f,
        .pad_level = 0.130f, .bass_level = 0.255f,
        .arpeggio_level = 0.150f, .lead_level = 0.190f,
        .kick_level = 0.365f, .snare_level = 0.200f,
        .hat_level = 0.070f, .open_hat_level = 0.048f,
        .tom_level = 0.145f, .stab_level = 0.120f,
        .noise_seed = 0x31f2c96du
    },
    {
        .name = "GLASS HORIZON",
        .bpm = 162.0f,
        .root_midi = 36, /* Cm9, Ebmaj7, Abmaj7, Fm7, Dbmaj7, G7. */
        .chord_offsets = {0,3,-4,5,1,7},
        .chord_extensions = {14,11,11,10,11,10},
        .minor_mask = 0x09,
        .bass = {
            0,MUSIC_REST,7,12, 0,MUSIC_REST,12,7,
            0,7,MUSIC_REST,12, 0,12,7,MUSIC_REST,
            0,MUSIC_REST,12,7, 15,12,7,MUSIC_REST
        },
        .arpeggio = {0,2,4,3, 1,3,5,4, 2,4,6,5, 4,3,2,1},
        .melody = {
             0,MUSIC_TIE,MUSIC_TIE,7, 10,MUSIC_TIE,MUSIC_TIE,MUSIC_TIE,
             5,MUSIC_TIE,10,12, 10,MUSIC_TIE,7,MUSIC_TIE,
            12,MUSIC_TIE,15,19, 15,MUSIC_TIE,10,7,
             5,MUSIC_TIE,7,10, 12,MUSIC_TIE,7,MUSIC_TIE,
            19,MUSIC_TIE,12,10, 7,10,12,15,
            19,MUSIC_TIE,17,14, 11,MUSIC_TIE,7,MUSIC_TIE
        },
        .kick = {0x0941,0x4941,0x1941,0x5949,0x1151,0xd951},
        .snare = {0x0100,0x2100,0x0100,0x3100,0x0100,0x7100},
        .hat = {0xaaaa,0xeeee,0xaeee,0xffee,0xeeee,0xffff},
        .open_hat = {0x0002,0x2002,0x0202,0x2222,0x2022,0xaaaa},
        .tom = {0x0000,0x0000,0x0000,0x8800,0x0000,0xe800},
        .stab = {0x0010,0x1010,0x1111,0x5151,0x1111,0xd151},
        .lead_profile = 2,
        .bass_profile = 0, .arpeggio_profile = 2,
        .harmony_interval = 7, .pulse_width = 0.46f, .lead_gate = 0.90f,
        .sidechain = 0.62f, .drive = 0.38f,
        .pad_level = 0.175f, .bass_level = 0.220f,
        .arpeggio_level = 0.145f, .lead_level = 0.180f,
        .kick_level = 0.325f, .snare_level = 0.190f,
        .hat_level = 0.060f, .open_hat_level = 0.038f,
        .tom_level = 0.120f, .stab_level = 0.090f,
        .noise_seed = 0xde71a903u
    },
    {
        .name = "STATIC HEART",
        .bpm = 178.0f,
        .root_midi = 45, /* Am7, Gadd9, Fmaj7, Cmaj7, Dm7, E7. */
        .chord_offsets = {0,-2,-4,3,5,7},
        .chord_extensions = {10,14,11,11,10,10},
        .minor_mask = 0x11,
        .bass = {
            0,12,0,MUSIC_REST, 7,12,7,0,
            0,MUSIC_REST,12,0, 7,7,12,MUSIC_REST,
            0,12,7,12, 0,15,12,7
        },
        .arpeggio = {0,3,1,4, 2,5,3,6, 4,2,5,3, 1,4,2,5},
        .melody = {
             0,3,7,12, 10,7,5,3,
             7,MUSIC_REST,10,12, 15,12,10,MUSIC_REST,
            12,10,7,5, 7,10,12,15,
            17,MUSIC_REST,15,12, 10,7,3,0,
            12,15,17,19, 20,19,17,15,
             7,14,11,17, 19,17,14,11
        },
        .kick = {0x1551,0x5551,0x1551,0xd551,0x5751,0xfdd1},
        .snare = {0x1010,0x1810,0x1010,0x1810,0x1410,0x3010},
        .hat = {0xeeee,0xffff,0xeeee,0xffff,0xffff,0xffff},
        .open_hat = {0x2222,0xaaaa,0x2222,0xaaaa,0xaaaa,0xaaaa},
        .tom = {0x0000,0x0800,0x0000,0xc800,0x0400,0xfc00},
        .stab = {0x5151,0x5555,0x5151,0xd555,0x5555,0xf555},
        .lead_profile = 3,
        .bass_profile = 2, .arpeggio_profile = 3,
        .harmony_interval = -12, .pulse_width = 0.20f, .lead_gate = 0.66f,
        .sidechain = 0.92f, .drive = 0.68f,
        .pad_level = 0.115f, .bass_level = 0.270f,
        .arpeggio_level = 0.160f, .lead_level = 0.195f,
        .kick_level = 0.385f, .snare_level = 0.215f,
        .hat_level = 0.076f, .open_hat_level = 0.052f,
        .tom_level = 0.155f, .stab_level = 0.135f,
        .noise_seed = 0xa9417c53u
    },
    {
        .name = "AFTERIMAGE RUN",
        .bpm = 172.0f,
        .root_midi = 38, /* Dm9, Bbmaj7, Gm7, A7, Ebmaj7, A7. */
        .chord_offsets = {0,-4,5,7,1,7},
        .chord_extensions = {14,11,10,10,11,10},
        .minor_mask = 0x05,
        .bass = {
            0,MUSIC_REST,0,12, 7,0,12,MUSIC_REST,
            0,7,12,7, 0,MUSIC_REST,12,0,
            0,12,0,7, 12,15,12,7
        },
        .arpeggio = {0,2,3,5, 4,2,6,3, 5,3,4,2, 6,4,3,1},
        .melody = {
             0,MUSIC_TIE,5,7, 12,MUSIC_TIE,10,7,
             5,7,10,12, 15,MUSIC_TIE,10,MUSIC_TIE,
             5,8,12,15, 12,8,5,MUSIC_REST,
             7,11,14,17, 14,11,7,MUSIC_REST,
             8,12,17,MUSIC_TIE, 8,5,1,MUSIC_TIE,
            19,MUSIC_TIE,17,14, 11,14,7,MUSIC_TIE
        },
        .kick = {0x1111,0x5115,0x1151,0xd119,0x5551,0xf559},
        .snare = {0x1010,0x1010,0x1010,0x7010,0x1010,0x7010},
        .hat = {0xaaaa,0xfaee,0xeeee,0xffff,0xffee,0xffff},
        .open_hat = {0x0002,0x2022,0x2222,0xaaaa,0x2222,0xaaaa},
        .tom = {0x0000,0x0400,0x0000,0xe400,0x0800,0xf800},
        .stab = {0x1010,0x5151,0x1111,0xd151,0x5555,0xf555},
        .lead_profile = 4,
        .bass_profile = 1, .arpeggio_profile = 1,
        .harmony_interval = 7, .pulse_width = 0.33f, .lead_gate = 0.76f,
        .sidechain = 0.82f, .drive = 0.54f,
        .pad_level = 0.140f, .bass_level = 0.250f,
        .arpeggio_level = 0.150f, .lead_level = 0.190f,
        .kick_level = 0.360f, .snare_level = 0.205f,
        .hat_level = 0.070f, .open_hat_level = 0.048f,
        .tom_level = 0.145f, .stab_level = 0.115f,
        .noise_seed = 0x52ce8bf1u
    },
    {
        .name = "NEON AFTERBURN",
        .bpm = 176.0f,
        .root_midi = 43, /* Gm7, Cm7, Bbmaj7, Ebmaj7, Abmaj7, D7. */
        .chord_offsets = {0,5,3,-4,1,7},
        .chord_extensions = {10,10,11,11,11,10},
        .minor_mask = 0x03,
        .bass = {
            0,12,7,0, 12,MUSIC_REST,7,12,
            0,MUSIC_REST,12,7, 0,15,12,MUSIC_REST,
            0,12,0,12, 7,15,12,7
        },
        .arpeggio = {0,3,5,2, 6,4,2,5, 0,4,6,3, 5,2,4,1},
        .melody = {
             0,7,10,12, 15,12,10,7,
             5,MUSIC_REST,7,10, 12,10,7,MUSIC_REST,
            12,15,19,15, 12,10,7,5,
             7,10,12,17, 15,12,10,MUSIC_REST,
            19,17,15,12, 10,12,15,17,
             7,11,14,17, 19,14,11,MUSIC_REST
        },
        .kick = {0x1551,0x5551,0x1751,0xd751,0x5d51,0xfdd1},
        .snare = {0x1010,0x9010,0x1010,0x3010,0x1410,0x9010},
        .hat = {0xeeee,0xffff,0xffee,0xffff,0xffff,0xffff},
        .open_hat = {0x2222,0xaaaa,0x2222,0xaaaa,0xaaaa,0xaaaa},
        .tom = {0x0000,0x0c00,0x0000,0xec00,0x0800,0xfc00},
        .stab = {0x5151,0x5555,0x5555,0xd555,0x5757,0xf777},
        .lead_profile = 5,
        .bass_profile = 3, .arpeggio_profile = 3,
        .harmony_interval = -5, .pulse_width = 0.24f, .lead_gate = 0.64f,
        .sidechain = 0.90f, .drive = 0.66f,
        .pad_level = 0.120f, .bass_level = 0.275f,
        .arpeggio_level = 0.165f, .lead_level = 0.200f,
        .kick_level = 0.390f, .snare_level = 0.215f,
        .hat_level = 0.078f, .open_hat_level = 0.055f,
        .tom_level = 0.165f, .stab_level = 0.145f,
        .noise_seed = 0x697f40c1u
    },
    {
        .name = "CHROME DEVOTION",
        .bpm = 166.0f,
        .root_midi = 35, /* Bmaj7, F#add9, G#m7, Emaj7, C#m7, F#7. */
        .chord_offsets = {0,7,9,5,2,7},
        .chord_extensions = {11,14,10,11,10,10},
        .minor_mask = 0x14,
        .bass = {
            0,MUSIC_REST,12,7, 0,12,7,MUSIC_REST,
            0,7,12,MUSIC_REST, 0,12,15,12,
            0,12,7,12, 0,15,19,15
        },
        .arpeggio = {0,1,3,5, 2,4,6,3, 0,3,6,4, 5,3,2,1},
        .melody = {
             4,MUSIC_TIE,7,11, 14,MUSIC_TIE,7,MUSIC_TIE,
             2,4,7,9, 11,MUSIC_TIE,9,MUSIC_TIE,
            11,14,16,MUSIC_TIE, 11,9,7,4,
             7,MUSIC_TIE,9,11, 14,MUSIC_TIE,9,MUSIC_TIE,
            14,16,18,19, 18,16,14,11,
            19,MUSIC_TIE,14,17, 11,14,7,MUSIC_TIE
        },
        .kick = {0x1111,0x5111,0x1151,0x5151,0x5551,0xd551},
        .snare = {0x1010,0x1000,0x1010,0x1000,0x0010,0x3010},
        .hat = {0xaaaa,0xeeee,0xeeee,0xffee,0xeeee,0xffff},
        .open_hat = {0x0000,0x2002,0x0202,0x2222,0x2222,0xaaaa},
        .tom = {0x0000,0x0000,0x0000,0x8400,0x0000,0xe400},
        .stab = {0x1010,0x1111,0x5151,0x5555,0x5151,0xd555},
        .lead_profile = 6,
        .bass_profile = 0, .arpeggio_profile = 2,
        .harmony_interval = -12, .pulse_width = 0.40f, .lead_gate = 0.86f,
        .sidechain = 0.72f, .drive = 0.44f,
        .pad_level = 0.170f, .bass_level = 0.230f,
        .arpeggio_level = 0.145f, .lead_level = 0.195f,
        .kick_level = 0.340f, .snare_level = 0.195f,
        .hat_level = 0.064f, .open_hat_level = 0.042f,
        .tom_level = 0.125f, .stab_level = 0.115f,
        .noise_seed = 0x44b1d793u
    },
    {
        .name = "REDLINE PROPHECY",
        .bpm = 174.0f,
        .root_midi = 37, /* C#m9, Emaj7, Badd9, F#add9, Amaj7, G#7. */
        .chord_offsets = {0,3,-2,5,-4,7},
        .chord_extensions = {14,11,14,14,11,10},
        .minor_mask = 0x01,
        .bass = {
            0,0,12,7, 0,MUSIC_REST,12,7,
            0,12,MUSIC_REST,7, 12,15,12,MUSIC_REST,
            0,12,7,0, 12,19,15,12
        },
        .arpeggio = {0,2,5,3, 6,4,2,5, 0,3,6,4, 5,3,1,4},
        .melody = {
            12,MUSIC_REST,7,10, 15,14,10,MUSIC_REST,
            14,10,7,MUSIC_REST, 5,7,10,14,
            10,14,17,22, 17,14,12,10,
             5,7,9,12, 17,12,9,MUSIC_REST,
             8,12,15,19, 15,12,8,MUSIC_REST,
             7,11,14,19, 17,14,11,MUSIC_REST
        },
        .kick = {0x1151,0x5151,0x1551,0xd551,0x5751,0xfdd9},
        .snare = {0x1010,0x1018,0x1810,0x1018,0x1818,0x3010},
        .hat = {0xeeee,0xffee,0xeeee,0xffff,0xffff,0xffff},
        .open_hat = {0x0202,0x2022,0x2222,0xaaaa,0xaaaa,0xaaaa},
        .tom = {0x0000,0x0800,0x0000,0xe800,0x0c00,0xfc00},
        .stab = {0x1111,0x5151,0x5555,0xd555,0x5757,0xf777},
        .lead_profile = 7,
        .bass_profile = 2, .arpeggio_profile = 1,
        .harmony_interval = 7, .pulse_width = 0.29f, .lead_gate = 0.70f,
        .sidechain = 0.88f, .drive = 0.64f,
        .pad_level = 0.125f, .bass_level = 0.270f,
        .arpeggio_level = 0.160f, .lead_level = 0.202f,
        .kick_level = 0.380f, .snare_level = 0.212f,
        .hat_level = 0.075f, .open_hat_level = 0.052f,
        .tom_level = 0.160f, .stab_level = 0.140f,
        .noise_seed = 0xc9135e2bu
    }
};

/* Ten authored route beats sit inside each six-district biome chapter. The
   higher spatial cadence gives the camera real corner entries, apexes and
   releases instead of one long lateral drift per district. Cardinal-spline
   evaluation below keeps every change continuous and world anchored. A full
   four-biome lap mirrors the horizontal route for infinite reprise variety. */
static const int8_t course_center_knots[4][COURSE_ROUTE_KNOTS_PER_BIOME] = {
    /* Azure: descend through a carrier cut, then open around the wreck field. */
    { 0, -34, -78, -66,   4,  64,  82,  36, -42, -18},
    /* Emerald: two broad canopy switchbacks around the root cathedral. */
    { 0,  46,  82,  34, -46, -86, -30,  58,  72,  20},
    /* Violet: an asymmetric rift weave with a late counter-turn. */
    { 0, -32, -70, -14,  68,  26, -58, -80,  44,  16},
    /* Ember: the foundry's tightest chicanes and furnace-trench exit. */
    { 0,  60,  88,  10, -78, -42,  82,  54, -84, -20}
};

static const int8_t course_grade_knots[4][COURSE_ROUTE_KNOTS_PER_BIOME] = {
    { 0, -22, -48, -58, -24,  24,  52,  38, -16,  -8},
    { 0,  30,  62,  78,  42, -12, -56, -68, -22,   8},
    { 0, -38, -70, -16,  64,  30, -66, -28,  72, -12},
    { 0,  40,  72,  26, -60, -24,  70,  16, -64,  12}
};

/* The valley breathes with the authored route. Narrow, high-relief beats are
   placed on corner apexes and steep transitions; wider values create visual
   runways before an encounter. Width is the local-X point where canyon walls
   begin, while relief is a percentage applied to their vertical mass. */
static const uint8_t
course_corridor_knots[4][COURSE_ROUTE_KNOTS_PER_BIOME] = {
    {42, 36, 29, 27, 34, 45, 38, 29, 32, 41},
    {46, 40, 32, 28, 30, 39, 45, 34, 29, 44},
    {39, 31, 25, 34, 28, 24, 29, 38, 26, 39},
    {41, 33, 27, 31, 24, 29, 25, 34, 25, 40}
};

static const uint8_t
course_relief_knots[4][COURSE_ROUTE_KNOTS_PER_BIOME] = {
    {82, 102, 132, 145, 108, 76, 92, 136, 122, 86},
    {72,  94, 126, 148, 136, 92, 74, 118, 142, 82},
    {96, 128, 154, 112, 142,160,126, 98, 150, 92},
    {88, 116, 146, 124, 164,138,158,112, 168, 94}
};

static const float course_traversal_offsets[COURSE_TRAVERSALS_PER_BIOME] = {
    1050.0f, 2500.0f
};

static const traversal_kind_t
course_traversal_kinds[4][COURSE_TRAVERSALS_PER_BIOME] = {
    {TRAVERSAL_GRAVITY_BLOOM, TRAVERSAL_SLALOM},
    {TRAVERSAL_SLALOM, TRAVERSAL_GRAVITY_BLOOM},
    {TRAVERSAL_SHEAR_BARRIER, TRAVERSAL_GRAVITY_BLOOM},
    {TRAVERSAL_SLALOM, TRAVERSAL_SHEAR_BARRIER}
};

/* Bloom exits alternate authored inside/outside lines across the first lap;
   this keeps their staging legible without making every aperture choose the
   same side of the screen. */
static const int8_t course_bloom_polarity[4] = {1, -1, -1, 1};

static const float course_wave_offsets[COURSE_WAVES_PER_BIOME] = {
    420.0f, 1950.0f, 3300.0f
};

static const uint8_t course_wave_patterns[4][2] = {
    {0, 2}, /* Azure lines establish, then cross the river channel. */
    {1, 0}, /* Emerald orbit opens into a canopy line. */
    {1, 2}, /* Violet orbit develops into vertical flankers. */
    {2, 3}  /* Ember crossers develop into bomber pressure. */
};

/* Four-slot motifs repeat as recognizable districts inside each phrase.
   Small hashed dimensions still keep individual props from cloning exactly,
   while their silhouettes now build toward a chapter landmark. */
static const uint8_t scenery_motifs[4][COURSE_PHRASES_PER_BIOME][4] = {
    {
        {0,2,0,4}, {0,4,2,0}, {1,3,1,4},
        {2,4,0,2}, {1,0,3,0}, {0,2,0,4}
    },
    {
        {0,1,0,3}, {0,2,1,0}, {3,4,3,2},
        {4,3,4,1}, {2,0,2,3}, {0,1,3,0}
    },
    {
        {0,1,0,3}, {2,0,2,1}, {3,1,3,0},
        {4,2,4,3}, {0,3,1,2}, {1,0,2,0}
    },
    {
        {0,1,0,3}, {1,3,1,0}, {2,0,1,3},
        {1,3,0,1}, {4,0,3,1}, {0,1,0,3}
    }
};

static const char *course_landmark_names[4] = {
    "CARRIER GRAVEYARD",
    "ROOT CATHEDRAL",
    "PRISM CRUCIBLE",
    "FURNACE GAUNTLET"
};

static game_t game;
static enemy_t enemies[MAX_ENEMIES];
static projectile_t player_shots[MAX_SHOTS];
static projectile_t enemy_shots[MAX_ENEMY_SHOTS];
static particle_t particles[MAX_PARTICLES];
static gate_t gates[MAX_GATES];
static pickup_t pickups[MAX_PICKUPS];
static terrain_point_t terrain[TERRAIN_ROWS][TERRAIN_COLS];

static pvr_poly_hdr_t opaque_header;
static pvr_poly_hdr_t terrain_header;
static pvr_poly_hdr_t translucent_header;
static pvr_poly_hdr_t additive_header;
static pvr_poly_hdr_t hud_header;
static pvr_poly_hdr_t texture_headers[GRAVITY_WAVE_TEXTURE_COUNT];
static pvr_poly_hdr_t sprite_additive_headers[4];
static pvr_ptr_t texture_vram[GRAVITY_WAVE_TEXTURE_COUNT];
static const pvr_poly_hdr_t *active_poly_header;
static int hardware_fog_key = -1;

static float camera_world_x;
static float camera_world_y;
static float camera_sin_yaw;
static float camera_cos_yaw;
static float camera_sin_pitch;
static float camera_cos_pitch;
static float camera_sin_roll;
static float camera_cos_roll;
static float camera_shake_x;
static float camera_shake_y;
static float camera_route_turn;

static uint32_t random_state = 0x4ae71f03u;
static sfxhnd_t sfx_laser = SFXHND_INVALID;
static sfxhnd_t sfx_fast_laser = SFXHND_INVALID;
static sfxhnd_t sfx_phase_wave = SFXHND_INVALID;
static sfxhnd_t sfx_explosion = SFXHND_INVALID;
static sfxhnd_t sfx_hit = SFXHND_INVALID;
static sfxhnd_t sfx_gate = SFXHND_INVALID;
extern const uint8_t gravity_wave_music_assets[];
extern const uint8_t gravity_wave_music_assets_end[];
static sfxhnd_t music_sections[MUSIC_TRACK_COUNT][MUSIC_SECTION_COUNT];
static float music_section_duration[MUSIC_TRACK_COUNT][MUSIC_SECTION_COUNT];
static uint16_t music_section_samples[MUSIC_TRACK_COUNT][MUSIC_SECTION_COUNT];
static uint32_t music_random_state = 0xa17ca55eu;
static int music_shuffle_bag[MUSIC_TRACK_COUNT];
static int music_shuffle_cursor = MUSIC_TRACK_COUNT;
static int laser_channel = -1;
static int music_left_channels[2] = {-1, -1};
static int music_right_channels[2] = {-1, -1};
static int active_music_bank;
static int current_music_track = -1;
static int current_music_section;
static int current_music_volume;
static int pending_music_track = -1;
static int pending_music_volume;
static bool pending_music_shuffle;
static float music_section_time;
static bool music_playhead_armed;
static bool audio_ready;
static bool music_ready;
static float music_sine_table[MUSIC_SINE_TABLE_SIZE];
static int title_menu_item;
static bool title_sound_test;
static int sound_test_track;
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
static bool music_jukebox_complete;
static bool music_jukebox_failed;
#endif

static int course_chapter_at(float world_z);
static float course_local_z(float world_z);
static traversal_kind_t course_traversal_kind(int chapter, int event);
static float traversal_span_for_kind(traversal_kind_t kind);
static float next_course_traversal_z(float after_z);
static float next_course_wave_z(float after_z);
static bool course_wave_is_guardian(float world_z);
static bool course_world_z_reserved(float world_z, float clearance);
static enemy_t *active_guardian(void);
static vec3_t enemy_model_point(const enemy_t *enemy,
                                float lx, float ly, float lz);
static vec3_t player_model_point(float lx, float ly, float lz);
static vec3_t segment_point_at_z(vec3_t start, vec3_t end, float world_z);

static bool world_z_reserved_by_traversal(float world_z, float clearance) {
    int objective;
    if(game.mode != MODE_TITLE &&
       course_world_z_reserved(world_z, clearance))
        return true;
    for(objective = 0; objective < MAX_GATES; ++objective) {
        const gate_t *gate = &gates[objective];
        int stage;
        if(!gate->active)
            continue;
        if(gate->stage_count > 1) {
            const float end_z = gate->z + gate->stage_spacing *
                                (float)(gate->stage_count - 1);
            if(world_z > gate->z - clearance && world_z < end_z + clearance)
                return true;
            continue;
        }
        for(stage = 0; stage < gate->stage_count; ++stage) {
            const float stage_z = gate->z + gate->stage_spacing * (float)stage;
            if(fabsf(world_z - stage_z) < clearance)
                return true;
        }
    }
    return false;
}

static vec3_t player_model_point(float lx, float ly, float lz);

static float clampf(float v, float lo, float hi) {
    if(v < lo)
        return lo;
    if(v > hi)
        return hi;
    return v;
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

static float smoothstepf(float t) {
    t = clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static color3_t color_lerp(color3_t a, color3_t b, float t) {
    color3_t out;
    out.r = lerpf(a.r, b.r, t);
    out.g = lerpf(a.g, b.g, t);
    out.b = lerpf(a.b, b.b, t);
    return out;
}

static color3_t color_scale(color3_t c, float s) {
    c.r = clampf(c.r * s, 0.0f, 1.0f);
    c.g = clampf(c.g * s, 0.0f, 1.0f);
    c.b = clampf(c.b * s, 0.0f, 1.0f);
    return c;
}

static uint32_t pack_color(float a, color3_t c) {
    return PVR_PACK_COLOR(clampf(a, 0.0f, 1.0f),
                          clampf(c.r, 0.0f, 1.0f),
                          clampf(c.g, 0.0f, 1.0f),
                          clampf(c.b, 0.0f, 1.0f));
}

/*
 * Fog registers are global to the PVR, so a color write can affect a frame
 * that is still rendering. Quantize biome transitions to 5 bits per channel
 * and synchronize only when that cached color actually changes. This keeps
 * the transition smooth without serializing the CPU and PVR every frame.
 */
static void update_hardware_fog(color3_t fog) {
    const int r = (int)(clampf(fog.r, 0.0f, 1.0f) * 31.0f + 0.5f);
    const int g = (int)(clampf(fog.g, 0.0f, 1.0f) * 31.0f + 0.5f);
    const int b = (int)(clampf(fog.b, 0.0f, 1.0f) * 31.0f + 0.5f);
    const int key = (r << 10) | (g << 5) | b;

    if(key == hardware_fog_key)
        return;
    if(hardware_fog_key >= 0 && pvr_wait_render_done() < 0)
        return;

    pvr_fog_table_color(1.0f,
                        (float)r * (1.0f / 31.0f),
                        (float)g * (1.0f / 31.0f),
                        (float)b * (1.0f / 31.0f));
    hardware_fog_key = key;
}

static uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static float hash_unit(int x, int z) {
    uint32_t h = hash_u32((uint32_t)x * 0x9e3779b9u ^
                          (uint32_t)z * 0x85ebca6bu ^ 0xa341316cu);
    return (float)(h & 0xffffu) * (1.0f / 65535.0f);
}

static uint32_t random_u32(void) {
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static uint32_t music_random_u32(void) {
    music_random_state = music_random_state * 22695477u + 1u;
    return hash_u32(music_random_state);
}

static float random_unit(void) {
    return (float)((random_u32() >> 8) & 0xffffu) * (1.0f / 65535.0f);
}

static float random_signed(void) {
    return random_unit() * 2.0f - 1.0f;
}

static float value_noise(float x, float z, float cell) {
    const float gx = x / cell;
    const float gz = z / cell;
    const int ix = (int)floorf(gx);
    const int iz = (int)floorf(gz);
    const float fx = smoothstepf(gx - (float)ix);
    const float fz = smoothstepf(gz - (float)iz);
    const float n00 = hash_unit(ix, iz);
    const float n10 = hash_unit(ix + 1, iz);
    const float n01 = hash_unit(ix, iz + 1);
    const float n11 = hash_unit(ix + 1, iz + 1);
    return lerpf(lerpf(n00, n10, fx), lerpf(n01, n11, fx), fz);
}

static int course_chapter_at(float world_z) {
    return (int)floorf(world_z / BIOME_LENGTH);
}

static float course_local_z(float world_z) {
    const int chapter = course_chapter_at(world_z);
    return world_z - (float)chapter * BIOME_LENGTH;
}

static void course_knot_address(int knot, int *chapter, int *phrase) {
    int c = knot / COURSE_ROUTE_KNOTS_PER_BIOME;
    int p = knot % COURSE_ROUTE_KNOTS_PER_BIOME;
    if(p < 0) {
        p += COURSE_ROUTE_KNOTS_PER_BIOME;
        c--;
    }
    *chapter = c;
    *phrase = p;
}

static float course_knot_value(int knot, bool grade) {
    int chapter;
    int phrase;
    int lap;
    int biome;
    float scale;
    float value;

    course_knot_address(knot, &chapter, &phrase);
    biome = chapter & 3;
    lap = chapter >= 0 ? chapter / 4 : 0;
    scale = 1.0f + (float)(lap > 3 ? 3 : lap) * 0.025f;
    value = (float)(grade ? course_grade_knots[biome][phrase] :
                            course_center_knots[biome][phrase]);
    if(!grade && (lap & 1))
        value = -value;
    return value * scale;
}

static float course_cardinal_segment(float p0, float p1,
                                     float p2, float p3, float t) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    /* Slightly restrained Catmull-Rom tangents avoid overshooting the canyon
       envelope while preserving velocity through each authored apex. */
    const float m1 = (p2 - p0) * 0.42f;
    const float m2 = (p3 - p1) * 0.42f;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * p1 +
           (t3 - 2.0f * t2 + t) * m1 +
           (-2.0f * t3 + 3.0f * t2) * p2 +
           (t3 - t2) * m2;
}

static float course_profile_curve(float world_z, bool grade) {
    const float grid = world_z / COURSE_ROUTE_KNOT_LENGTH;
    const int knot = (int)floorf(grid);
    const float t = grid - (float)knot;
    return course_cardinal_segment(
        course_knot_value(knot - 1, grade),
        course_knot_value(knot, grade),
        course_knot_value(knot + 1, grade),
        course_knot_value(knot + 2, grade), t);
}

static float course_environment_knot_value(int knot, bool relief) {
    int chapter;
    int phrase;
    const uint8_t (*table)[COURSE_ROUTE_KNOTS_PER_BIOME] = relief ?
        course_relief_knots : course_corridor_knots;
    course_knot_address(knot, &chapter, &phrase);
    return (float)table[chapter & 3][phrase];
}

static float course_environment_curve(float world_z, bool relief) {
    const float grid = world_z / COURSE_ROUTE_KNOT_LENGTH;
    const int knot = (int)floorf(grid);
    const float t = smoothstepf(grid - (float)knot);
    return lerpf(course_environment_knot_value(knot, relief),
                 course_environment_knot_value(knot + 1, relief), t);
}

static float path_center(float world_z) {
    /* Authored chapter knots remain world anchored, so rolling terrain rows
       always re-evaluate to identical vertices and cannot ripple or warp. */
    return course_profile_curve(world_z, false);
}

static float path_elevation(float world_z) {
    return course_profile_curve(world_z, true);
}

static vec3_t route_position(float local_x, float local_y, float world_z) {
    return (vec3_t){path_center(world_z) + local_x,
                    path_elevation(world_z) + local_y,
                    world_z};
}

static float path_heading(float world_z) {
    return atanf((path_center(world_z + 24.0f) -
                  path_center(world_z - 24.0f)) / 48.0f);
}

static float path_grade(float world_z) {
    const float lateral = path_center(world_z + 24.0f) -
                          path_center(world_z - 24.0f);
    const float vertical = path_elevation(world_z + 24.0f) -
                           path_elevation(world_z - 24.0f);
    const float horizontal_run = fsqrt(48.0f * 48.0f + lateral * lateral);
    return atanf(vertical / horizontal_run);
}

static float route_model_pitch(float anchor_z, float yaw,
                               float pose_pitch) {
    /* Mesh local +Z may face with or against the route. Project the course
       grade onto that facing so player and enemy craft pitch in opposite,
       physically correct directions while remaining rigid. */
    return pose_pitch - path_grade(anchor_z) *
           fcos(yaw - path_heading(anchor_z));
}

static float path_turn_preview(float world_z) {
    const float near_heading = atanf(
        (path_center(world_z + 210.0f) -
         path_center(world_z + 30.0f)) / 180.0f);
    const float far_heading = atanf(
        (path_center(world_z + 430.0f) -
         path_center(world_z + 210.0f)) / 220.0f);
    return clampf(far_heading - near_heading, -0.24f, 0.24f);
}

static int traversal_stage_count_for_kind(traversal_kind_t kind) {
    return kind == TRAVERSAL_GRAVITY_BLOOM ? 1 :
           (kind == TRAVERSAL_SLALOM ? 3 : 2);
}

static float traversal_stage_spacing_for_kind(traversal_kind_t kind) {
    return kind == TRAVERSAL_SLALOM ? 250.0f :
           (kind == TRAVERSAL_SHEAR_BARRIER ? 230.0f : 0.0f);
}

static float traversal_span_for_kind(traversal_kind_t kind) {
    return traversal_stage_spacing_for_kind(kind) *
           (float)(traversal_stage_count_for_kind(kind) - 1);
}

static traversal_kind_t course_traversal_kind(int chapter, int event) {
    const int biome = chapter & 3;
    const int lap = chapter >= 0 ? chapter / 4 : 0;
    const int authored_event = (lap & 1) ?
        COURSE_TRAVERSALS_PER_BIOME - 1 - event : event;
    return course_traversal_kinds[biome][authored_event];
}

static int nearest_course_event(float world_z, const float *offsets,
                                int count) {
    const float local = course_local_z(world_z);
    float nearest_distance = 1000000.0f;
    int nearest = 0;
    int event;
    for(event = 0; event < count; ++event) {
        const float distance = fabsf(local - offsets[event]);
        if(distance < nearest_distance) {
            nearest_distance = distance;
            nearest = event;
        }
    }
    return nearest;
}

static float next_course_event_z(float after_z, const float *offsets,
                                 int count) {
    int chapter = course_chapter_at(after_z);
    const float chapter_z = (float)chapter * BIOME_LENGTH;
    const float local = after_z - chapter_z;
    int event;
    for(event = 0; event < count; ++event) {
        if(offsets[event] >= local - 0.01f)
            return chapter_z + offsets[event];
    }
    chapter++;
    return (float)chapter * BIOME_LENGTH + offsets[0];
}

static float next_course_traversal_z(float after_z) {
    return next_course_event_z(after_z, course_traversal_offsets,
                               COURSE_TRAVERSALS_PER_BIOME);
}

static float next_course_wave_z(float after_z) {
    return next_course_event_z(after_z, course_wave_offsets,
                               COURSE_WAVES_PER_BIOME);
}

static float recover_course_wave_z(float cursor_z, float recovery_z) {
    const int cursor_chapter = course_chapter_at(cursor_z);
    const int cursor_event = nearest_course_event(
        cursor_z, course_wave_offsets, COURSE_WAVES_PER_BIOME);
    if(cursor_z >= recovery_z)
        return cursor_z;
    if(course_chapter_at(recovery_z) == cursor_chapter &&
       nearest_course_event(recovery_z, course_wave_offsets,
                            COURSE_WAVES_PER_BIOME) == cursor_event)
        /* Delay this chapter beat instead of deleting its formation. */
        return recovery_z;
    return next_course_wave_z(recovery_z);
}

static bool course_wave_is_guardian(float world_z) {
    return nearest_course_event(world_z, course_wave_offsets,
                                COURSE_WAVES_PER_BIOME) ==
           COURSE_WAVES_PER_BIOME - 1;
}

static bool course_world_z_reserved(float world_z, float clearance) {
    const int center_chapter = course_chapter_at(world_z);
    int chapter;
    int event;
    for(chapter = center_chapter - 1;
        chapter <= center_chapter + 1; ++chapter) {
        const float chapter_z = (float)chapter * BIOME_LENGTH;
        for(event = 0; event < COURSE_TRAVERSALS_PER_BIOME; ++event) {
            const traversal_kind_t kind =
                course_traversal_kind(chapter, event);
            const float start_z = chapter_z +
                                  course_traversal_offsets[event];
            const float end_z = start_z + traversal_span_for_kind(kind);
            if(start_z >= game.suppressed_gate_from_z - 0.5f &&
               start_z < game.suppressed_gate_until_z - 0.5f)
                continue;
            if(world_z > start_z - clearance &&
               world_z < end_z + clearance)
                return true;
        }
    }
    return false;
}

static int course_turn_direction(float world_z) {
    const float delta = path_center(world_z + 260.0f) -
                        path_center(world_z - 120.0f);
    if(fabsf(delta) < 1.0f)
        return (course_chapter_at(world_z) & 1) ? -1 : 1;
    return delta > 0.0f ? 1 : -1;
}

static int course_grade_direction(float world_z) {
    const float delta = path_elevation(world_z + 260.0f) -
                        path_elevation(world_z - 120.0f);
    if(fabsf(delta) < 1.0f)
        return (course_chapter_at(world_z) & 1) ? 1 : -1;
    return delta > 0.0f ? 1 : -1;
}

static int course_phrase_at(float world_z) {
    int phrase = (int)(course_local_z(world_z) / COURSE_PHRASE_LENGTH);
    if(phrase < 0)
        phrase = 0;
    if(phrase >= COURSE_PHRASES_PER_BIOME)
        phrase = COURSE_PHRASES_PER_BIOME - 1;
    return phrase;
}

static int scenery_variant_for_segment(int biome, int segment,
                                       float world_z) {
    const int phrase = course_phrase_at(world_z);
    int slot = segment % 4;
    if(slot < 0)
        slot += 4;
    return (int)scenery_motifs[biome & 3][phrase][slot];
}

static float scenery_side_for_segment(int biome, int segment,
                                      float world_z) {
    const int phrase = course_phrase_at(world_z);
    return (((segment / 2) + biome + phrase) & 1) ? 1.0f : -1.0f;
}

static bool scenery_variant_spans_lane(int biome, int variant) {
    return (biome == 0 && (variant == 1 || variant == 3)) ||
           (biome == 1 && variant == 2) ||
           (biome == 2 && variant == 4) ||
           (biome == 3 && (variant == 2 || variant == 4));
}

static bool is_signature_segment(int segment) {
    const float world_z = (float)segment * SCENERY_SEGMENT;
    const int chapter = course_chapter_at(world_z);
    const float target_z = (float)chapter * BIOME_LENGTH +
                           COURSE_SIGNATURE_LOCAL_Z;
    const int target_segment =
        (int)floorf(target_z / SCENERY_SEGMENT + 0.5f);
    return segment == target_segment;
}

static float terrain_height_biome(int biome, float local_x, float world_z) {
    const float distance_from_river = fabsf(local_x);
    const float broad = value_noise(local_x + 1900.0f, world_z, 155.0f);
    const float detail = value_noise(local_x - 730.0f, world_z, 54.0f);
    const float floor_wave = fsin(world_z * 0.011f + local_x * 0.022f);
    const float corridor_width =
        clampf(course_environment_curve(world_z, false), 23.0f, 48.0f);
    const float relief =
        clampf(course_environment_curve(world_z, true) * 0.01f, 0.70f, 1.72f);
    const float route_slope =
        (path_center(world_z + 90.0f) - path_center(world_z - 90.0f)) /
        180.0f;
    float height;
    float wall_scale;
    float wall_curve;

    switch(biome & 3) {
        case 1: /* Emerald Veil: broad, humid valley and ancient terraces. */
            height = -3.0f + broad * 7.5f + detail * 2.1f +
                     fsin(world_z * 0.006f + local_x * 0.012f) * 2.8f;
            wall_scale = 18.0f;
            wall_curve = 61.0f;
            break;
        case 2: /* Violet Rift: sharp shelves over a deep fractured floor. */
            height = -9.0f + broad * 4.2f + detail * detail * 7.2f +
                     floor_wave * 2.2f;
            wall_scale = 29.0f;
            wall_curve = 96.0f;
            break;
        case 3: /* Ember Crown: stepped basalt walls surrounding a lava cut. */
            height = -6.5f + broad * 5.0f + detail * 3.3f +
                     fsin(world_z * 0.017f) * 1.5f;
            height = floorf(height * 0.28f) / 0.28f;
            wall_scale = 25.0f;
            wall_curve = 74.0f;
            break;
        default: /* Azure Reach: wet sea-cut canyon and craggy shelves. */
            height = -5.5f + broad * 6.0f + detail * 2.8f + floor_wave * 1.3f;
            wall_scale = 23.0f;
            wall_curve = 82.0f;
            break;
    }

    if(distance_from_river > corridor_width) {
        const float wall = clampf(
            (distance_from_river - corridor_width) / 175.0f,
                                  0.0f, 1.25f);
        const float ridges = 0.42f + 0.92f *
            value_noise(local_x * 1.7f + 400.0f, world_z + 810.0f, 78.0f);
        /* Bank the outside shoulder of a bend. This is authored from absolute
           route coordinates, so the silhouette remains stable as rows roll. */
        const float turn_bank = clampf(-local_x * route_slope * 0.080f,
                                       -9.0f, 9.0f);
        height += wall * wall_scale * relief +
                  wall * wall * wall_curve * ridges * relief +
                  wall * turn_bank;
    }

    return height;
}

static float terrain_height_local(float local_x, float world_z) {
    const float local = fmodf(world_z, BIOME_LENGTH);
    const int biome = ((int)(world_z / BIOME_LENGTH)) & 3;
    float height = terrain_height_biome(biome, local_x, world_z);

    if(local > BIOME_LENGTH - BIOME_BLEND_LENGTH) {
        const float blend = smoothstepf(
            (local - (BIOME_LENGTH - BIOME_BLEND_LENGTH)) /
            BIOME_BLEND_LENGTH);
        const float next = terrain_height_biome((biome + 1) & 3,
                                                local_x, world_z);
        height = lerpf(height, next, blend);
    }
    return height;
}

static const palette_t *current_palette(void) {
    return &palettes[game.palette_index % ARRAY_COUNT(palettes)];
}

static void get_blended_palette(palette_t *out) {
    const float biome_length = BIOME_LENGTH;
    const float local = fmodf(game.distance, biome_length);
    const int index = ((int)(game.distance / biome_length)) % ARRAY_COUNT(palettes);
    const int next = (index + 1) % ARRAY_COUNT(palettes);
    float t = 0.0f;

    if(local > biome_length - BIOME_BLEND_LENGTH)
        t = smoothstepf((local -
                         (biome_length - BIOME_BLEND_LENGTH)) /
                        BIOME_BLEND_LENGTH);

    out->sky_top = color_lerp(palettes[index].sky_top, palettes[next].sky_top, t);
    out->sky_horizon = color_lerp(palettes[index].sky_horizon,
                                  palettes[next].sky_horizon, t);
    out->fog = color_lerp(palettes[index].fog, palettes[next].fog, t);
    out->ground_low = color_lerp(palettes[index].ground_low,
                                 palettes[next].ground_low, t);
    out->ground_high = color_lerp(palettes[index].ground_high,
                                  palettes[next].ground_high, t);
    out->river = color_lerp(palettes[index].river, palettes[next].river, t);
    out->accent = color_lerp(palettes[index].accent, palettes[next].accent, t);
    out->enemy = color_lerp(palettes[index].enemy, palettes[next].enemy, t);
    out->name = palettes[index].name;
}

static bool project_absolute_world(vec3_t world, screen_point_t *out) {
    float dx = world.x - camera_world_x;
    float dy = world.y - camera_world_y;
    float dz = world.z - game.distance;
    const float yaw_x = dx * camera_cos_yaw - dz * camera_sin_yaw;
    const float yaw_z = dx * camera_sin_yaw + dz * camera_cos_yaw;
    const float pitch_y = dy * camera_cos_pitch - yaw_z * camera_sin_pitch;
    const float pitch_z = dy * camera_sin_pitch + yaw_z * camera_cos_pitch;
    const float roll_x = yaw_x * camera_cos_roll - pitch_y * camera_sin_roll;
    const float roll_y = yaw_x * camera_sin_roll + pitch_y * camera_cos_roll;

    if(pitch_z <= NEAR_PLANE) {
        out->valid = false;
        return false;
    }

    out->z = 1.0f / pitch_z;
    out->x = SCREEN_CX + camera_shake_x + roll_x * game.camera_focal * out->z;
    out->y = SCREEN_CY + camera_shake_y - roll_y * game.camera_focal * out->z;
    out->valid = true;
    return true;
}

static bool project_world(vec3_t world, screen_point_t *out) {
    /* Static scene geometry and route-bound entities carry absolute X/Z but
       route-relative Y. Projectiles and free particles use the explicit
       absolute-world projector above and never receive another course bend. */
    world.y += path_elevation(world.z);
    return project_absolute_world(world, out);
}

static void setup_camera(void) {
    const float lookahead = 220.0f;
    const float center_here = path_center(game.distance);
    const float center_ahead = path_center(game.distance + lookahead);
    const float elevation_here = path_elevation(game.distance);
    const float elevation_ahead =
        path_elevation(game.distance + lookahead);
    const float route_grade = atanf(
        (elevation_ahead - elevation_here) / lookahead);
    const float route_turn = path_turn_preview(game.distance);
    const float yaw = atanf((center_ahead - center_here) / lookahead) +
                      game.player_vx * 0.0008f;
    const float pitch = route_grade * 0.72f - 0.075f +
                        game.player_vy * 0.00025f;
    const float shake = game.trauma * game.trauma;
    const float roll = -game.bank * 0.090f + fsin(game.barrel_roll) * 0.025f +
                       route_turn * 0.68f +
                       fsin(game.time * 73.0f + 1.7f) * shake * 0.012f;

    camera_world_x = center_here + game.camera_x;
    camera_world_y = elevation_here + game.camera_y + 8.5f;
    camera_route_turn = route_turn;
    camera_sin_yaw = fsin(yaw);
    camera_cos_yaw = fcos(yaw);
    camera_sin_pitch = fsin(pitch);
    camera_cos_pitch = fcos(pitch);
    camera_sin_roll = fsin(roll);
    camera_cos_roll = fcos(roll);
    camera_shake_x = fsin(game.time * 91.0f + game.distance * 0.031f) *
                     shake * 4.0f;
    camera_shake_y = fsin(game.time * 77.0f + game.distance * 0.047f + 2.0f) *
                     shake * 3.0f;
}

static void make_vertex(pvr_vertex_t *v, const screen_point_t *p,
                        uint32_t color, bool end) {
    v->flags = end ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = p->x;
    v->y = p->y;
    v->z = p->z;
    v->u = 0.0f;
    v->v = 0.0f;
    v->argb = color;
    v->oargb = 0;
}

static void make_textured_vertex(pvr_vertex_t *v, const screen_point_t *p,
                                 float u, float texture_v,
                                 uint32_t color, bool end) {
    v->flags = end ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    v->x = p->x;
    v->y = p->y;
    v->z = p->z;
    v->u = u;
    v->v = texture_v;
    v->argb = color;
    v->oargb = 0;
}

/* Consecutive EOL-terminated strips can share one TA polygon header. */
static void begin_poly_list(pvr_list_t list) {
    pvr_list_begin(list);
    active_poly_header = NULL;
}

static void submit_poly_header(const pvr_poly_hdr_t *header) {
    if(active_poly_header == header)
        return;
    pvr_prim(header, sizeof(*header));
    active_poly_header = header;
}

static void submit_textured_triangle(const pvr_poly_hdr_t *header,
                                     const screen_point_t *a,
                                     const screen_point_t *b,
                                     const screen_point_t *c,
                                     float ua, float va,
                                     float ub, float vb,
                                     float uc, float vc,
                                     uint32_t ca, uint32_t cb, uint32_t cc) {
    pvr_vertex_t vertices[3];

    submit_poly_header(header);
    make_textured_vertex(&vertices[0], a, ua, va, ca, false);
    make_textured_vertex(&vertices[1], b, ub, vb, cb, false);
    make_textured_vertex(&vertices[2], c, uc, vc, cc, true);
    pvr_prim(vertices, sizeof(vertices));
}

static void submit_textured_quad(const pvr_poly_hdr_t *header,
                                 const screen_point_t *a,
                                 const screen_point_t *b,
                                 const screen_point_t *c,
                                 const screen_point_t *d,
                                 float ua, float va, float ub, float vb,
                                 float uc, float vc, float ud, float vd,
                                 uint32_t ca, uint32_t cb,
                                 uint32_t cc, uint32_t cd) {
    pvr_vertex_t vertices[4];

    submit_poly_header(header);
    make_textured_vertex(&vertices[0], a, ua, va, ca, false);
    make_textured_vertex(&vertices[1], b, ub, vb, cb, false);
    make_textured_vertex(&vertices[2], c, uc, vc, cc, false);
    make_textured_vertex(&vertices[3], d, ud, vd, cd, true);
    pvr_prim(vertices, sizeof(vertices));
}

static void submit_triangle(const pvr_poly_hdr_t *header,
                            const screen_point_t *a,
                            const screen_point_t *b,
                            const screen_point_t *c,
                            uint32_t ca, uint32_t cb, uint32_t cc) {
    pvr_vertex_t vertices[3];

    submit_poly_header(header);
    make_vertex(&vertices[0], a, ca, false);
    make_vertex(&vertices[1], b, cb, false);
    make_vertex(&vertices[2], c, cc, true);
    pvr_prim(vertices, sizeof(vertices));
}

static void submit_quad(const pvr_poly_hdr_t *header,
                        const screen_point_t *a,
                        const screen_point_t *b,
                        const screen_point_t *c,
                        const screen_point_t *d,
                        uint32_t ca, uint32_t cb, uint32_t cc, uint32_t cd) {
    pvr_vertex_t vertices[4];

    submit_poly_header(header);
    make_vertex(&vertices[0], a, ca, false);
    make_vertex(&vertices[1], b, cb, false);
    make_vertex(&vertices[2], c, cc, false);
    make_vertex(&vertices[3], d, cd, true);
    pvr_prim(vertices, sizeof(vertices));
}

static void draw_rect(const pvr_poly_hdr_t *header, float x, float y,
                      float width, float height, float z, uint32_t color) {
    const screen_point_t a = {x, y, z, true};
    const screen_point_t b = {x + width, y, z, true};
    const screen_point_t c = {x, y + height, z, true};
    const screen_point_t d = {x + width, y + height, z, true};
    submit_quad(header, &a, &b, &c, &d, color, color, color, color);
}

static void draw_line(const pvr_poly_hdr_t *header,
                      float x1, float y1, float z1,
                      float x2, float y2, float z2,
                      float width, uint32_t c1, uint32_t c2) {
    const float dx = x2 - x1;
    const float dy = y2 - y1;
    const float length_sq = dx * dx + dy * dy;
    float nx, ny;
    screen_point_t a, b, c, d;

    if(length_sq < 0.0001f)
        return;

    {
        const float scale = frsqrt(length_sq) * width * 0.5f;
        nx = -dy * scale;
        ny = dx * scale;
    }

    a = (screen_point_t){x1 + nx, y1 + ny, z1, true};
    b = (screen_point_t){x1 - nx, y1 - ny, z1, true};
    c = (screen_point_t){x2 + nx, y2 + ny, z2, true};
    d = (screen_point_t){x2 - nx, y2 - ny, z2, true};
    submit_quad(header, &a, &b, &c, &d, c1, c1, c2, c2);
}

static void draw_world_quad(const pvr_poly_hdr_t *header,
                            vec3_t a, vec3_t b, vec3_t c, vec3_t d,
                            uint32_t ca, uint32_t cb,
                            uint32_t cc, uint32_t cd) {
    screen_point_t sa, sb, sc, sd;
    if(project_world(a, &sa) && project_world(b, &sb) &&
       project_world(c, &sc) && project_world(d, &sd)) {
        submit_quad(header, &sa, &sb, &sc, &sd, ca, cb, cc, cd);
    }
}

static void draw_world_textured_triangle(const pvr_poly_hdr_t *header,
                                         vec3_t a, vec3_t b, vec3_t c,
                                         float ua, float va,
                                         float ub, float vb,
                                         float uc, float vc,
                                         uint32_t ca, uint32_t cb,
                                         uint32_t cc) {
    screen_point_t sa, sb, sc;
    if(project_world(a, &sa) && project_world(b, &sb) && project_world(c, &sc))
        submit_textured_triangle(header, &sa, &sb, &sc,
                                 ua, va, ub, vb, uc, vc, ca, cb, cc);
}

static void draw_world_textured_quad(const pvr_poly_hdr_t *header,
                                     vec3_t a, vec3_t b, vec3_t c, vec3_t d,
                                     float ua, float va, float ub, float vb,
                                     float uc, float vc, float ud, float vd,
                                     uint32_t ca, uint32_t cb,
                                     uint32_t cc, uint32_t cd) {
    screen_point_t sa, sb, sc, sd;
    if(project_world(a, &sa) && project_world(b, &sb) &&
       project_world(c, &sc) && project_world(d, &sd)) {
        submit_textured_quad(header, &sa, &sb, &sc, &sd,
                             ua, va, ub, vb, uc, vc, ud, vd,
                             ca, cb, cc, cd);
    }
}

static void draw_projected_textured_billboard(const pvr_poly_hdr_t *header,
                                              screen_point_t center,
                                              float width, float height,
                                              uint32_t color) {
    screen_point_t a, b, c, d;
    float screen_w, screen_h;

    screen_w = clampf(width * game.camera_focal * center.z, 1.0f, 220.0f);
    screen_h = clampf(height * game.camera_focal * center.z, 1.0f, 220.0f);
    if(center.x + screen_w < -8.0f || center.x - screen_w > SCREEN_W + 8.0f ||
       center.y + screen_h < -8.0f || center.y - screen_h > SCREEN_H + 8.0f)
        return;
    a = (screen_point_t){center.x - screen_w * 0.5f,
                         center.y - screen_h * 0.5f, center.z, true};
    b = (screen_point_t){center.x + screen_w * 0.5f,
                         center.y - screen_h * 0.5f, center.z, true};
    c = (screen_point_t){center.x - screen_w * 0.5f,
                         center.y + screen_h * 0.5f, center.z, true};
    d = (screen_point_t){center.x + screen_w * 0.5f,
                         center.y + screen_h * 0.5f, center.z, true};
    submit_textured_quad(header, &a, &b, &c, &d,
                         0.0f, 0.0f, 1.0f, 0.0f,
                         0.0f, 1.0f, 1.0f, 1.0f,
                         color, color, color, color);
}

static void draw_textured_billboard(const pvr_poly_hdr_t *header,
                                    vec3_t world, float width, float height,
                                    uint32_t color) {
    screen_point_t center;
    if(project_world(world, &center))
        draw_projected_textured_billboard(header, center, width, height,
                                          color);
}

static void draw_absolute_textured_billboard(const pvr_poly_hdr_t *header,
                                             vec3_t world,
                                             float width, float height,
                                             uint32_t color) {
    screen_point_t center;
    if(project_absolute_world(world, &center))
        draw_projected_textured_billboard(header, center, width, height,
                                          color);
}

static void draw_disc(const pvr_poly_hdr_t *header, float cx, float cy,
                      float radius, float z, int segments,
                      uint32_t center_color, uint32_t edge_color) {
    int i;
    const screen_point_t center = {cx, cy, z, true};
    for(i = 0; i < segments; ++i) {
        const float a0 = (float)i * TAU / (float)segments;
        const float a1 = (float)(i + 1) * TAU / (float)segments;
        const screen_point_t p0 = {
            cx + fcos(a0) * radius, cy + fsin(a0) * radius, z, true
        };
        const screen_point_t p1 = {
            cx + fcos(a1) * radius, cy + fsin(a1) * radius, z, true
        };
        submit_triangle(header, &center, &p0, &p1,
                        center_color, edge_color, edge_color);
    }
}

static const uint8_t glyph_digits[10][7] = {
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e},
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e},
    {0x0e,0x11,0x01,0x02,0x04,0x08,0x1f},
    {0x1e,0x01,0x01,0x0e,0x01,0x01,0x1e},
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02},
    {0x1f,0x10,0x10,0x1e,0x01,0x01,0x1e},
    {0x06,0x08,0x10,0x1e,0x11,0x11,0x0e},
    {0x1f,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e},
    {0x0e,0x11,0x11,0x0f,0x01,0x02,0x0c}
};

static const uint8_t glyph_letters[26][7] = {
    {0x0e,0x11,0x11,0x1f,0x11,0x11,0x11},
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e},
    {0x0f,0x10,0x10,0x10,0x10,0x10,0x0f},
    {0x1e,0x11,0x11,0x11,0x11,0x11,0x1e},
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x1f},
    {0x1f,0x10,0x10,0x1e,0x10,0x10,0x10},
    {0x0f,0x10,0x10,0x17,0x11,0x11,0x0f},
    {0x11,0x11,0x11,0x1f,0x11,0x11,0x11},
    {0x0e,0x04,0x04,0x04,0x04,0x04,0x0e},
    {0x01,0x01,0x01,0x01,0x11,0x11,0x0e},
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1f},
    {0x11,0x1b,0x15,0x15,0x11,0x11,0x11},
    {0x11,0x19,0x19,0x15,0x13,0x13,0x11},
    {0x0e,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x1e,0x11,0x11,0x1e,0x10,0x10,0x10},
    {0x0e,0x11,0x11,0x11,0x15,0x12,0x0d},
    {0x1e,0x11,0x11,0x1e,0x14,0x12,0x11},
    {0x0f,0x10,0x10,0x0e,0x01,0x01,0x1e},
    {0x1f,0x04,0x04,0x04,0x04,0x04,0x04},
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0e},
    {0x11,0x11,0x11,0x11,0x11,0x0a,0x04},
    {0x11,0x11,0x11,0x15,0x15,0x1b,0x11},
    {0x11,0x11,0x0a,0x04,0x0a,0x11,0x11},
    {0x11,0x11,0x0a,0x04,0x04,0x04,0x04},
    {0x1f,0x01,0x02,0x04,0x08,0x10,0x1f}
};

static uint8_t glyph_row(char c, int row) {
    if(c >= '0' && c <= '9')
        return glyph_digits[c - '0'][row];
    if(c >= 'A' && c <= 'Z')
        return glyph_letters[c - 'A'][row];
    if(c == '-')
        return row == 3 ? 0x0e : 0;
    if(c == ':')
        return (row == 2 || row == 5) ? 0x04 : 0;
    if(c == '.')
        return row == 6 ? 0x04 : 0;
    if(c == '/')
        return (uint8_t)(row < 5 ? (1u << (4 - row)) : 0);
    if(c == '+')
        return row == 3 ? 0x0e : ((row == 2 || row == 4) ? 0x04 : 0);
    if(c == '!')
        return row < 5 ? 0x04 : (row == 6 ? 0x04 : 0);
    return 0;
}

static int text_width(const char *text, int scale) {
    const int length = (int)strlen(text);
    return length > 0 ? length * 6 * scale - scale : 0;
}

static void draw_text(float x, float y, const char *text, int scale,
                      color3_t color, float alpha) {
    const uint32_t packed = pack_color(alpha, color);
    int character;

    for(character = 0; text[character]; ++character) {
        int row;
        for(row = 0; row < 7; ++row) {
            const uint8_t bits = glyph_row(text[character], row);
            int column;
            for(column = 0; column < 5; ++column) {
                if(bits & (1u << (4 - column))) {
                    draw_rect(&hud_header,
                              x + (float)(character * 6 + column) * scale,
                              y + (float)row * scale,
                              (float)scale, (float)scale, 9.0f, packed);
                }
            }
        }
    }
}

static void draw_text_centered(float y, const char *text, int scale,
                               color3_t color, float alpha) {
    draw_text((SCREEN_W - (float)text_width(text, scale)) * 0.5f,
              y, text, scale, color, alpha);
}

static sfxhnd_t load_generated_sound(int kind, float frequency,
                                     float duration) {
    const int rate = 22050;
    int samples = (int)(duration * rate);
    int16_t *buffer;
    uint32_t noise = 0x1983f00du ^ (uint32_t)kind;
    int i;
    sfxhnd_t handle;

    samples = (samples + 15) & ~15;
    buffer = malloc((size_t)samples * sizeof(*buffer));
    if(!buffer)
        return SFXHND_INVALID;

    for(i = 0; i < samples; ++i) {
        const float t = (float)i / (float)rate;
        const float progress = (float)i / (float)samples;
        float sample = 0.0f;
        float envelope = 1.0f - progress;

        if(kind == 0) {
            const float sweep = frequency * (1.0f - progress * 0.72f);
            sample = fsin(TAU * sweep * t) * 0.62f +
                     fsin(TAU * sweep * 2.04f * t) * 0.24f;
            envelope *= envelope;
        }
        else if(kind == 1) {
            noise = noise * 1664525u + 1013904223u;
            sample = ((float)((noise >> 16) & 0xffffu) / 32767.5f - 1.0f) *
                     0.72f + fsin(TAU * (frequency * (1.0f - progress)) * t) *
                     0.36f;
            envelope = envelope * envelope;
        }
        else if(kind == 2) {
            noise = noise * 1103515245u + 12345u;
            sample = fsin(TAU * frequency * t) * 0.34f +
                     ((float)((noise >> 17) & 0x7fffu) / 16383.5f - 1.0f) *
                     0.38f;
            envelope *= envelope * envelope;
        }
        else if(kind == 3) {
            const float shimmer = frequency * (1.0f + progress * 0.55f);
            sample = fsin(TAU * shimmer * t) * 0.54f +
                     fsin(TAU * shimmer * 1.5f * t) * 0.20f;
            envelope = (1.0f - progress) * (1.0f - progress * 0.6f);
        }
        else {
            const float sweep = frequency * (0.55f + progress * 1.85f);
            const float pulse = fsin(TAU * sweep * 1.97f * t) >= 0.0f ?
                                1.0f : -1.0f;
            sample = fsin(TAU * sweep * t) * 0.50f +
                     fsin(TAU * sweep * 0.51f * t) * 0.23f +
                     pulse * 0.12f;
            envelope = smoothstepf(progress * 9.0f) *
                       (1.0f - progress) * (1.0f - progress * 0.45f);
        }

        sample = clampf(sample * envelope, -1.0f, 1.0f);
        buffer[i] = (int16_t)(sample * 28000.0f);
    }

    handle = snd_sfx_load_raw_buf((char *)buffer,
                                  (size_t)samples * sizeof(*buffer),
                                  (uint32_t)rate, 16, 1);
    free(buffer);
    return handle;
}

static float music_note_frequency(int midi_note) {
    static const float semitone_ratios[12] = {
        1.000000f, 1.059463f, 1.122462f, 1.189207f,
        1.259921f, 1.334840f, 1.414214f, 1.498307f,
        1.587401f, 1.681793f, 1.781797f, 1.887749f
    };
    int semitones = midi_note - 69;
    float ratio = 1.0f;

    while(semitones >= 12) {
        semitones -= 12;
        ratio *= 2.0f;
    }
    while(semitones <= -12) {
        semitones += 12;
        ratio *= 0.5f;
    }
    if(semitones >= 0)
        ratio *= semitone_ratios[semitones];
    else
        ratio /= semitone_ratios[-semitones];
    return 440.0f * ratio;
}

static float music_phase(float cycles) {
    return cycles - floorf(cycles);
}

static void init_music_sine_table(void) {
    int i;
    for(i = 0; i < MUSIC_SINE_TABLE_SIZE; ++i)
        music_sine_table[i] = fsin(
            TAU * (float)i / (float)MUSIC_SINE_TABLE_SIZE);
}

static float music_sine(float cycles) {
    const float position = music_phase(cycles) *
                           (float)MUSIC_SINE_TABLE_SIZE;
    const int index = (int)position;
    const int next = (index + 1) & (MUSIC_SINE_TABLE_SIZE - 1);
    return lerpf(music_sine_table[index], music_sine_table[next],
                 position - (float)index);
}

static float music_pulse(float cycles, float duty) {
    float sample;
    float normalization;
    duty = clampf(duty, 0.05f, 0.95f);
    sample = music_phase(cycles) < duty ? 1.0f - duty : -duty;
    normalization = 1.0f / fmaxf(duty, 1.0f - duty);
    return sample * normalization;
}

static float music_triangle(float cycles) {
    return 1.0f - fabsf(music_phase(cycles) - 0.5f) * 4.0f;
}

static float music_saw(float cycles) {
    return music_phase(cycles) * 2.0f - 1.0f;
}

static float music_noise(uint32_t value) {
    const uint32_t mixed = hash_u32(value);
    return (float)((mixed >> 8) & 0xffffu) * (1.0f / 32767.5f) - 1.0f;
}

static int music_chord_semitone(int degree, bool minor) {
    switch(degree) {
        case 1: return minor ? 3 : 4;
        case 2: return 7;
        case 3: return 12;
        case 4: return 12 + (minor ? 3 : 4);
        case 5: return 19;
        case 6: return 24;
        default: return 0;
    }
}

static float music_lead_voice(const soundtrack_t *track,
                              const float *lead_frequencies,
                              const uint8_t *lead_starts,
                              const uint8_t *lead_ends,
                              float beat, float seconds_per_beat,
                              int *step_out) {
    float position;
    float note_position;
    float note_duration;
    float seconds;
    float cycles;
    float envelope;
    float gate_end;
    float voice;
    int step;

    /* A section owns its own echoes. Negative taps would otherwise wrap to
       that section's future ending and create a wrong-chord phantom note at
       the next downbeat; the outgoing buffer supplies the real decay. */
    if(beat < 0.0f || beat >= (float)MUSIC_BEATS) {
        if(step_out)
            *step_out = 0;
        return 0.0f;
    }
    position = beat * 2.0f;
    step = (int)position;
    if(step_out)
        *step_out = step;
    if(lead_frequencies[step] <= 0.0f)
        return 0.0f;
    note_position = position - (float)lead_starts[step];
    note_duration = (float)(lead_ends[step] - lead_starts[step]);
    gate_end = fmaxf(0.08f,
                     note_duration - (1.0f - track->lead_gate));
    if(note_position >= gate_end)
        return 0.0f;

    seconds = note_position * seconds_per_beat * 0.5f;
    cycles = lead_frequencies[step] * seconds;
    cycles += music_sine(
        (5.1f + (float)track->lead_profile * 0.31f) * seconds) *
        (note_duration > 1.0f ? 0.023f : 0.012f);
    envelope = smoothstepf(note_position * 5.2f) *
               smoothstepf((gate_end - note_position) * 3.8f) *
               (0.84f + 0.16f *
                (1.0f - note_position / fmaxf(gate_end, 0.08f)));
    if(track->lead_profile == 1) {
        voice = music_pulse(cycles, 0.26f) * 0.54f +
                music_sine(cycles) * 0.34f +
                music_triangle(cycles * 2.0f) * 0.12f;
    }
    else if(track->lead_profile == 2) {
        const float breath = music_noise(
            (uint32_t)(seconds * (float)MUSIC_SAMPLE_RATE) ^
            track->noise_seed ^ 0x6f73a42du);
        /* A warm breathy reed patch: a small opening scoop, rounded harmonic
           motion, a detuned fundamental, and restrained vibrato give long
           chorus notes a human contour without a sampled instrument. */
        cycles -= (1.0f - smoothstepf(note_position * 3.6f)) * 0.055f;
        voice = music_sine(cycles) * 0.50f +
                music_sine(cycles * 1.003f + 0.13f) * 0.10f +
                music_sine(cycles * 2.0f +
                           music_sine(seconds * 0.72f) * 0.018f) * 0.19f +
                music_sine(cycles * 3.0f) * 0.12f +
                music_sine(cycles * 4.0f) * 0.05f +
                breath * 0.02f;
    }
    else if(track->lead_profile == 3) {
        voice = music_pulse(cycles, track->pulse_width) * 0.42f +
                music_triangle(cycles * 2.0f) * 0.31f +
                music_sine(cycles * 3.0f) * 0.17f +
                music_sine(cycles) * 0.10f;
    }
    else if(track->lead_profile == 4) {
        const float pwm = clampf(track->pulse_width +
                                 music_sine(seconds * 3.7f) * 0.08f,
                                 0.14f, 0.62f);
        voice = music_saw(cycles) * 0.37f +
                music_pulse(cycles, pwm) * 0.36f +
                music_triangle(cycles * 0.5f) * 0.17f +
                music_sine(cycles * 2.01f) * 0.10f;
    }
    else if(track->lead_profile == 5) {
        const float pwm = clampf(track->pulse_width +
                                 music_sine(seconds * 5.3f) * 0.07f,
                                 0.12f, 0.48f);
        voice = music_saw(cycles * 0.996f) * 0.26f +
                music_saw(cycles * 1.004f) * 0.26f +
                music_pulse(cycles, pwm) * 0.30f +
                music_sine(cycles) * 0.18f;
    }
    else if(track->lead_profile == 6) {
        const float glass = music_sine(
            cycles + music_sine(cycles * 2.0f) * (1.35f / TAU));
        voice = glass * 0.48f +
                music_sine(cycles * 2.01f) * 0.22f +
                music_triangle(cycles * 0.5f) * 0.20f +
                music_pulse(cycles * 0.25f, 0.46f) * 0.10f;
    }
    else if(track->lead_profile == 7) {
        const float sync = music_phase(cycles * 0.505f) * 2.0f - 1.0f;
        voice = music_pulse(cycles, track->pulse_width) * 0.34f +
                music_saw(cycles + sync * 0.18f) * 0.36f +
                music_triangle(cycles * 2.0f) * 0.20f +
                music_sine(cycles * 3.0f) * 0.10f;
    }
    else {
        voice = music_sine(cycles) * 0.61f +
                music_triangle(cycles * 2.0f) * 0.25f +
                music_sine(cycles * 3.01f) * 0.14f;
    }
    return voice * envelope;
}

static float music_bass_voice(const soundtrack_t *track, float cycles) {
    if(track->bass_profile == 1)
        return music_pulse(cycles, track->pulse_width) * 0.36f +
               music_saw(cycles * 0.5f) * 0.24f +
               music_sine(cycles) * 0.40f;
    if(track->bass_profile == 2)
        return music_sine(
                   cycles + music_sine(cycles * 2.0f) * (0.82f / TAU)) *
                   0.50f +
               music_pulse(cycles * 0.5f, 0.34f) * 0.22f +
               music_triangle(cycles) * 0.28f;
    if(track->bass_profile == 3) {
        const float pwm = clampf(track->pulse_width +
                                 music_sine(cycles * 0.031f) * 0.08f,
                                 0.12f, 0.58f);
        return music_pulse(cycles, pwm) * 0.42f +
               music_saw(cycles * 0.5f) * 0.28f +
               music_sine(cycles) * 0.30f;
    }
    return music_pulse(cycles, track->pulse_width) * 0.38f +
           music_sine(cycles) * 0.42f +
           music_triangle(cycles * 0.5f) * 0.20f;
}

static float music_arpeggio_voice(const soundtrack_t *track, float cycles) {
    if(track->arpeggio_profile == 1)
        return music_pulse(cycles, 0.22f) * 0.38f +
               music_triangle(cycles) * 0.34f +
               music_sine(cycles * 2.0f) * 0.28f;
    if(track->arpeggio_profile == 2)
        return music_sine(
                   cycles + music_sine(cycles * 2.0f) * (0.74f / TAU)) *
                   0.45f +
               music_triangle(cycles * 0.5f) * 0.35f +
               music_saw(cycles * 0.25f) * 0.20f;
    if(track->arpeggio_profile == 3)
        return music_saw(cycles) * 0.34f +
               music_pulse(cycles * 0.5f, 0.28f) * 0.34f +
               music_triangle(cycles * 2.0f) * 0.22f +
               music_sine(cycles) * 0.10f;
    return music_triangle(cycles) * 0.44f +
           music_pulse(cycles, 0.25f) * 0.26f +
           music_sine(cycles * 2.0f) * 0.20f +
           music_saw(cycles * 0.5f) * 0.10f;
}

typedef struct {
    int history;
    int step_size;
} music_adpcm_encoder_t;

/* Public-domain YMZ/AICA encoder math used by KOS's wav2adpcm utility. AICA
 * decodes the low nibble first, so even samples occupy the low half-byte. */
static uint8_t music_encode_adpcm_nibble(music_adpcm_encoder_t *encoder,
                                         int sample) {
    static const int step_table[8] = {230,230,230,230,307,409,512,614};
    const int difference = (sample & -8) - encoder->history;
    const int absolute_difference = difference < 0 ? -difference : difference;
    int magnitude = (absolute_difference * 4) / encoder->step_size;
    int code;
    int decoded_difference;

    if(magnitude > 7)
        magnitude = 7;
    code = magnitude | (difference < 0 ? 8 : 0);
    decoded_difference = ((1 + (magnitude << 1)) *
                          encoder->step_size) >> 3;
    decoded_difference = (int)clampf((float)decoded_difference,
                                     0.0f, 32767.0f);
    encoder->history += difference < 0 ? -decoded_difference :
                                         decoded_difference;
    encoder->history = (int)clampf((float)encoder->history,
                                   -32768.0f, 32767.0f);
    encoder->step_size = (step_table[magnitude] *
                          encoder->step_size) >> 8;
    encoder->step_size = (int)clampf((float)encoder->step_size,
                                     127.0f, 24576.0f);
    return (uint8_t)code;
}

static void music_encode_adpcm_sample(uint8_t *output, int sample_index,
                                      float sample,
                                      music_adpcm_encoder_t *encoder) {
    const int pcm = (int)(clampf(sample, -1.0f, 1.0f) * 30000.0f);
    const uint8_t nibble = music_encode_adpcm_nibble(encoder, pcm);
    const int byte_index = sample_index >> 1;

    if((sample_index & 1) == 0)
        output[byte_index] = nibble;
    else
        output[byte_index] |= (uint8_t)(nibble << 4);
}

static int music_track_phrase_samples(const soundtrack_t *track) {
    const float duration = 60.0f * (float)MUSIC_BEATS / track->bpm;
    const int samples = (int)(duration * (float)MUSIC_SAMPLE_RATE + 0.5f);
    return (samples + 32) & ~63;
}

static int music_track_section_samples(const soundtrack_t *track) {
    return music_track_phrase_samples(track) + MUSIC_EDGE_SAMPLES;
}

static bool music_track_layout_valid(const soundtrack_t *track) {
    const int samples = music_track_section_samples(track);
    return samples > MUSIC_EDGE_SAMPLES && samples <= 65534 &&
           (samples & 63) == 0;
}

static size_t music_catalog_bytes_required(void) {
    size_t total = 0;
    int track;
    for(track = 0; track < MUSIC_TRACK_COUNT; ++track)
        total += (size_t)music_track_section_samples(&soundtrack_defs[track]) *
                 MUSIC_SECTION_COUNT;
    return total;
}

static uint32_t music_hash_bytes(uint32_t hash, const void *data,
                                 size_t length) {
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;
    for(i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t music_catalog_fingerprint(void) {
    uint32_t hash = 2166136261u;
    const uint32_t format[] = {
        MUSIC_SYNTH_REVISION,
        MUSIC_TRACK_COUNT,
        MUSIC_SECTION_COUNT,
        MUSIC_SAMPLE_RATE,
        MUSIC_BARS,
        MUSIC_EDGE_SAMPLES
    };
    int track;

    hash = music_hash_bytes(hash, format, sizeof(format));
    for(track = 0; track < MUSIC_TRACK_COUNT; ++track) {
        const soundtrack_t *definition = &soundtrack_defs[track];
        const uint8_t *musical_data = (const uint8_t *)&definition->bpm;
        const uint8_t *definition_end =
            (const uint8_t *)definition + sizeof(*definition);
        hash = music_hash_bytes(hash, definition->name,
                                strlen(definition->name) + 1u);
        hash = music_hash_bytes(hash, musical_data,
                                (size_t)(definition_end - musical_data));
    }
    return hash;
}

static sfxhnd_t load_embedded_music(const uint8_t *data,
                                    const soundtrack_t *track,
                                    float *duration_out) {
    const int samples = music_track_section_samples(track);
    if(!music_track_layout_valid(track))
        return SFXHND_INVALID;
    if(duration_out)
        *duration_out = (float)samples / (float)MUSIC_SAMPLE_RATE;
    return snd_sfx_load_raw_buf((char *)data, (size_t)samples,
                                MUSIC_SAMPLE_RATE, 4, 2);
}

static sfxhnd_t load_generated_music(const soundtrack_t *track, int section,
                                     float *duration_out) {
    float chord_frequencies[MUSIC_BARS][4];
    float bass_frequencies[MUSIC_BARS][8];
    float arpeggio_frequencies[MUSIC_BARS][16];
    float lead_frequencies[MUSIC_MELODY_STEPS];
    float harmony_frequencies[MUSIC_MELODY_STEPS];
    uint8_t lead_starts[MUSIC_MELODY_STEPS];
    uint8_t lead_ends[MUSIC_MELODY_STEPS];
    uint16_t kick_patterns[MUSIC_BARS];
    uint16_t snare_patterns[MUSIC_BARS];
    uint16_t hat_patterns[MUSIC_BARS];
    uint16_t open_hat_patterns[MUSIC_BARS];
    uint16_t tom_patterns[MUSIC_BARS];
    uint16_t stab_patterns[MUSIC_BARS];
    int samples = music_track_section_samples(track);
    const int phrase_samples = music_track_phrase_samples(track);
    uint8_t *buffer;
    uint8_t *left_samples;
    uint8_t *right_samples;
    music_adpcm_encoder_t left_encoder = {0,127};
    music_adpcm_encoder_t right_encoder = {0,127};
    float duration;
    float seconds_per_beat;
    float sum_squares = 0.0f;
    float peak = 0.0f;
    sfxhnd_t handle;
    int bar, step, i;

    /* AICA ADPCM stores two samples per byte, and KOS requires each planar
       channel to occupy a multiple of 32 bytes. */
    if(!music_track_layout_valid(track)) {
        printf("Gravity Wave music: %s section %c exceeds AICA sample limit.\n",
               track->name, 'A' + section);
        return SFXHND_INVALID;
    }
    buffer = malloc((size_t)samples);
    if(!buffer)
        return SFXHND_INVALID;
    memset(buffer, 0, (size_t)samples);
    left_samples = buffer;
    right_samples = buffer + samples / 2;
    duration = (float)samples / (float)MUSIC_SAMPLE_RATE;
    seconds_per_beat = (float)phrase_samples /
                       ((float)MUSIC_SAMPLE_RATE * (float)MUSIC_BEATS);
    if(duration_out)
        *duration_out = duration;

    for(bar = 0; bar < MUSIC_BARS; ++bar) {
        const int phrase_bar = section * MUSIC_BARS + bar;
        const bool minor = (track->minor_mask & (1u << phrase_bar)) != 0;
        const int root = track->root_midi + track->chord_offsets[phrase_bar];
        chord_frequencies[bar][0] = music_note_frequency(root + 12);
        chord_frequencies[bar][1] =
            music_note_frequency(root + 12 + (minor ? 3 : 4));
        chord_frequencies[bar][2] = music_note_frequency(root + 19);
        chord_frequencies[bar][3] = music_note_frequency(
            root + 12 + track->chord_extensions[phrase_bar]);
        for(step = 0; step < 8; ++step) {
            int bass_note = track->bass[section * 8 + step];
            if(section == 1 && bass_note == MUSIC_REST &&
               (step == 2 || step == 6))
                bass_note = step == 2 ? 7 : 12;
            if(bass_note == 15 && !minor)
                bass_note = 16;
            bass_frequencies[bar][step] =
                bass_note == MUSIC_REST ? 0.0f :
                music_note_frequency(root + bass_note);
        }
        for(step = 0; step < 16; ++step) {
            const int pattern_step = section == 0 ? step :
                (section == 1 ? 15 - step : (step * 5 + bar * 3) & 15);
            const int semitone = music_chord_semitone(
                track->arpeggio[pattern_step], minor);
            arpeggio_frequencies[bar][step] =
                music_note_frequency(root + 12 + semitone);
        }
        kick_patterns[bar] = track->kick[phrase_bar];
        snare_patterns[bar] = track->snare[phrase_bar];
        hat_patterns[bar] = track->hat[phrase_bar];
        open_hat_patterns[bar] = track->open_hat[phrase_bar];
        tom_patterns[bar] = track->tom[phrase_bar];
        stab_patterns[bar] = track->stab[phrase_bar];
        if(section >= 1) {
            if(bar == 1)
                kick_patterns[bar] |= 0x4040u;
            if(bar == MUSIC_BARS - 1) {
                kick_patterns[bar] |= 0x8000u;
                snare_patterns[bar] |= 0x4000u;
                if(section == MUSIC_SECTION_COUNT - 1) {
                    hat_patterns[bar] |= 0xffffu;
                    tom_patterns[bar] |= 0xe000u;
                }
            }
        }
    }
    for(step = 0; step < MUSIC_MELODY_STEPS; ++step) {
        const int melody_step = section * MUSIC_MELODY_STEPS + step;
        const int melody_note = track->melody[melody_step];
        if(melody_note == MUSIC_TIE && step > 0 &&
           lead_frequencies[step - 1] > 0.0f) {
            lead_frequencies[step] = lead_frequencies[step - 1];
            harmony_frequencies[step] = harmony_frequencies[step - 1];
            lead_starts[step] = lead_starts[step - 1];
        }
        else if(melody_note == MUSIC_REST || melody_note == MUSIC_TIE) {
            lead_frequencies[step] = 0.0f;
            harmony_frequencies[step] = 0.0f;
            lead_starts[step] = (uint8_t)step;
        }
        else {
            lead_frequencies[step] = music_note_frequency(
                track->root_midi + 24 + melody_note);
            harmony_frequencies[step] = music_note_frequency(
                track->root_midi + 24 + melody_note +
                track->harmony_interval);
            lead_starts[step] = (uint8_t)step;
        }
    }
    for(step = 0; step < MUSIC_MELODY_STEPS; ++step) {
        int end = step + 1;
        if(lead_frequencies[step] > 0.0f) {
            while(end < MUSIC_MELODY_STEPS &&
                  lead_starts[end] == lead_starts[step])
                ++end;
        }
        lead_ends[step] = (uint8_t)end;
    }

    for(i = 0; i < samples; ++i) {
        const float beat = (float)i * (float)MUSIC_BEATS /
                           (float)phrase_samples;
        const bool in_tail = i >= phrase_samples;
        const float arrangement_beat = in_tail ?
            (float)MUSIC_BEATS - 0.0001f : beat;
        const int current_bar = (int)(arrangement_beat * 0.25f);
        const float section_energy = 0.84f + (float)section * 0.13f;
        const float lead_arrangement = section == 0 ? 0.80f :
                                       (section == 1 ? 0.88f : 1.08f);
        const float beat_in_bar = arrangement_beat -
                                  (float)current_bar * 4.0f;
        const float bar_seconds = beat_in_bar * seconds_per_beat;
        const float pad_attack = smoothstepf(beat_in_bar * 4.8f);
        const float pad_release = smoothstepf((4.0f - beat_in_bar) * 3.2f);
        const float pad_envelope = pad_attack * pad_release *
            (0.92f + music_sine(beat_in_bar * 0.25f) * 0.08f);
        const float bass_position = beat_in_bar * 2.0f;
        const int bass_step = (int)bass_position;
        const float bass_phase = bass_position - (float)bass_step;
        const float arpeggio_position = beat_in_bar * 4.0f;
        const int arpeggio_step = (int)arpeggio_position;
        const float arpeggio_phase = arpeggio_position -
                                     (float)arpeggio_step;
        const int drum_step = arpeggio_step;
        const float drum_phase = arpeggio_phase;
        const float drum_seconds = drum_phase * seconds_per_beat * 0.25f;
        const uint16_t drum_bit = (uint16_t)(1u << drum_step);
        const float quarter_phase = beat_in_bar - floorf(beat_in_bar);
        const float sidechain = lerpf(
            1.0f, 0.40f + 0.60f * smoothstepf(quarter_phase * 3.4f),
            track->sidechain);
        float left = 0.0f;
        float right = 0.0f;
        float voice;
        float envelope;
        float cycles;
        int lead_step;
        int echo_step;

        left += (music_sine(chord_frequencies[current_bar][0] *
                            bar_seconds * 0.997f) * 0.34f +
                 music_sine(chord_frequencies[current_bar][1] *
                            bar_seconds * 1.002f) * 0.23f +
                 music_triangle(chord_frequencies[current_bar][2] *
                                bar_seconds * 0.999f) * 0.17f +
                 music_sine(chord_frequencies[current_bar][3] *
                            bar_seconds * 1.001f) * 0.14f +
                 music_saw(chord_frequencies[current_bar][0] *
                           bar_seconds * 0.501f) * 0.12f) *
                track->pad_level * section_energy * pad_envelope * sidechain;
        right += (music_sine(chord_frequencies[current_bar][0] *
                             bar_seconds * 1.003f) * 0.34f +
                  music_sine(chord_frequencies[current_bar][1] *
                             bar_seconds * 0.998f) * 0.23f +
                  music_triangle(chord_frequencies[current_bar][2] *
                                 bar_seconds * 1.001f) * 0.17f +
                  music_sine(chord_frequencies[current_bar][3] *
                             bar_seconds * 0.999f) * 0.14f +
                  music_saw(chord_frequencies[current_bar][1] *
                            bar_seconds * 0.499f) * 0.12f) *
                 track->pad_level * section_energy * pad_envelope * sidechain;

        if(bass_frequencies[current_bar][bass_step] > 0.0f) {
            envelope = clampf(bass_phase * 18.0f, 0.0f, 1.0f) *
                       (1.0f - bass_phase) * (1.0f - bass_phase);
            cycles = bass_frequencies[current_bar][bass_step] *
                     bass_phase * seconds_per_beat * 0.5f;
            voice = music_bass_voice(track, cycles);
            left += voice * track->bass_level * envelope * section_energy;
            right += voice * track->bass_level * envelope * section_energy;
        }

        envelope = clampf(arpeggio_phase * 24.0f, 0.0f, 1.0f) *
                   (1.0f - arpeggio_phase) *
                   (1.0f - arpeggio_phase) *
                   (1.0f - arpeggio_phase);
        cycles = arpeggio_frequencies[current_bar][arpeggio_step] *
                 arpeggio_phase * seconds_per_beat * 0.25f;
        voice = music_arpeggio_voice(track, cycles);
        if((arpeggio_step ^ current_bar) & 1) {
            left += voice * track->arpeggio_level * envelope * 0.42f *
                    section_energy;
            right += voice * track->arpeggio_level * envelope *
                     section_energy;
        }
        else {
            left += voice * track->arpeggio_level * envelope *
                    section_energy;
            right += voice * track->arpeggio_level * envelope * 0.42f *
                     section_energy;
        }

        if(stab_patterns[current_bar] & drum_bit) {
            const float stab_envelope = clampf(drum_phase * 30.0f,
                                                0.0f, 1.0f) *
                (1.0f - drum_phase) * (1.0f - drum_phase) *
                (1.0f - drum_phase);
            const float stab_seconds = drum_seconds;
            const float stab_voice =
                (music_saw(chord_frequencies[current_bar][0] *
                           stab_seconds) * 0.28f +
                 music_pulse(chord_frequencies[current_bar][1] *
                             stab_seconds, 0.42f) * 0.24f +
                 music_triangle(chord_frequencies[current_bar][2] *
                                stab_seconds) * 0.24f +
                 music_sine(chord_frequencies[current_bar][3] *
                            stab_seconds) * 0.24f) *
                track->stab_level * stab_envelope * sidechain *
                section_energy;
            left += stab_voice * (drum_step & 1 ? 0.58f : 1.0f);
            right += stab_voice * (drum_step & 1 ? 1.0f : 0.58f);
        }

        if(!in_tail) {
            voice = music_lead_voice(track, lead_frequencies,
                                     lead_starts, lead_ends, beat,
                                     seconds_per_beat, &lead_step);
            if(lead_step & 1) {
                left += voice * track->lead_level * 0.62f * section_energy *
                        lead_arrangement;
                right += voice * track->lead_level * section_energy *
                         lead_arrangement;
            }
            else {
                left += voice * track->lead_level * section_energy *
                        lead_arrangement;
                right += voice * track->lead_level * 0.62f * section_energy *
                         lead_arrangement;
            }
        }
        voice = music_lead_voice(track, lead_frequencies,
                                 lead_starts, lead_ends, beat - 0.75f,
                                 seconds_per_beat, &echo_step) * 0.30f;
        if(echo_step & 1) {
            left += voice * track->lead_level * section_energy *
                    lead_arrangement;
            right += voice * track->lead_level * 0.38f * section_energy *
                     lead_arrangement;
        }
        else {
            left += voice * track->lead_level * 0.38f * section_energy *
                    lead_arrangement;
            right += voice * track->lead_level * section_energy *
                     lead_arrangement;
        }
        voice = music_lead_voice(track, lead_frequencies,
                                 lead_starts, lead_ends, beat - 1.5f,
                                 seconds_per_beat, NULL) * 0.14f;
        left += voice * track->lead_level * 0.45f * section_energy *
                lead_arrangement;
        right += voice * track->lead_level * section_energy *
                 lead_arrangement;

        if(section == MUSIC_SECTION_COUNT - 1) {
            voice = music_lead_voice(track, harmony_frequencies,
                                     lead_starts, lead_ends, beat - 0.25f,
                                     seconds_per_beat, &echo_step) * 0.24f;
            if(echo_step & 1) {
                left += voice * track->lead_level * 0.40f;
                right += voice * track->lead_level;
            }
            else {
                left += voice * track->lead_level;
                right += voice * track->lead_level * 0.40f;
            }
        }

        if(kick_patterns[current_bar] & drum_bit) {
            const float kick_duration = seconds_per_beat * 0.25f;
            const float kick_cycles = kick_duration *
                (50.0f * drum_phase + 118.0f *
                 (drum_phase - drum_phase * drum_phase +
                  drum_phase * drum_phase * drum_phase / 3.0f));
            envelope = clampf(drum_phase * 30.0f, 0.0f, 1.0f) *
                       (1.0f - drum_phase) * (1.0f - drum_phase) *
                       (1.0f - drum_phase);
            voice = music_sine(kick_cycles) +
                    music_noise((uint32_t)i ^ track->noise_seed ^
                                (uint32_t)section * 0x9e3779b9u) *
                    clampf((0.09f - drum_phase) * 11.0f, 0.0f, 1.0f) * 0.14f;
            left += voice * track->kick_level * envelope * section_energy;
            right += voice * track->kick_level * envelope * section_energy;
        }
        if(snare_patterns[current_bar] & drum_bit) {
            const float clap_burst = 0.72f + 0.18f *
                clampf(1.0f - fabsf(drum_seconds - 0.024f) * 42.0f,
                       0.0f, 1.0f) + 0.10f *
                clampf(1.0f - fabsf(drum_seconds - 0.047f) * 48.0f,
                       0.0f, 1.0f);
            envelope = clampf(drum_phase * 34.0f, 0.0f, 1.0f) *
                       (1.0f - drum_phase) * (1.0f - drum_phase);
            left += (music_noise((uint32_t)i ^ track->noise_seed ^
                                 0x51ed270bu) * 0.78f +
                     music_sine(176.0f * drum_seconds) * 0.22f) *
                    track->snare_level * envelope * clap_burst *
                    section_energy;
            right += (music_noise((uint32_t)i ^ track->noise_seed ^
                                  0xa37c91e5u) * 0.78f +
                      music_sine(181.0f * drum_seconds) * 0.22f) *
                     track->snare_level * envelope * clap_burst *
                     section_energy;
        }
        {
            /* Short, noise-rich gated room behind each clap. It is generated
               from the same hit position, so the ambience reads as part of
               the snare rather than an unrelated wash over the phrase. */
            const float sixteenth_seconds = seconds_per_beat * 0.25f;
            const float room_duration = seconds_per_beat * 0.76f;
            int back;
            for(back = 0; back <= 3 && back <= drum_step; ++back) {
                const uint16_t room_bit =
                    (uint16_t)(1u << (drum_step - back));
                if(snare_patterns[current_bar] & room_bit) {
                    const float age = ((float)back + drum_phase) *
                                      sixteenth_seconds;
                    if(age < room_duration) {
                        const float room_envelope =
                            smoothstepf(age * 48.0f) *
                            smoothstepf((room_duration - age) * 24.0f) *
                            (1.0f - age / room_duration);
                        const uint32_t event_seed = track->noise_seed ^
                            (uint32_t)(current_bar * 16 + drum_step - back) *
                            0x9e3779b9u;
                        left += (music_noise((uint32_t)i ^ event_seed ^
                                             0x745db2a1u) * 0.92f +
                                 music_sine(132.0f * age) * 0.08f) *
                                track->snare_level * room_envelope * 0.22f *
                                section_energy;
                        right += (music_noise((uint32_t)i ^ event_seed ^
                                              0x319bc42du) * 0.92f +
                                  music_sine(139.0f * age) * 0.08f) *
                                 track->snare_level * room_envelope * 0.22f *
                                 section_energy;
                    }
                    break;
                }
            }
        }
        if(hat_patterns[current_bar] & drum_bit) {
            envelope = clampf(drum_phase * 42.0f, 0.0f, 1.0f) *
                       (1.0f - drum_phase) * (1.0f - drum_phase) *
                       (1.0f - drum_phase) * (1.0f - drum_phase);
            voice = (music_pulse(2180.0f * drum_seconds, 0.48f) *
                     music_pulse(3310.0f * drum_seconds, 0.42f) * 0.62f +
                     music_noise((uint32_t)i ^ track->noise_seed ^
                                 0xc6bc2796u) * 0.38f) *
                    track->hat_level * envelope * section_energy;
            if(drum_step & 1) {
                left += voice * 0.55f;
                right += voice;
            }
            else {
                left += voice;
                right += voice * 0.55f;
            }
        }
        if(open_hat_patterns[current_bar] & drum_bit) {
            envelope = clampf(drum_phase * 52.0f, 0.0f, 1.0f) *
                       (1.0f - drum_phase) *
                       (0.72f + (1.0f - drum_phase) * 0.28f);
            voice = (music_noise((uint32_t)i ^ track->noise_seed ^
                                 0x0f3a91d7u) * 0.54f +
                     music_pulse(2670.0f * drum_seconds, 0.47f) *
                     music_pulse(4210.0f * drum_seconds, 0.39f) * 0.46f) *
                    track->open_hat_level * envelope * section_energy;
            left += voice * (drum_step & 1 ? 0.72f : 1.0f);
            right += voice * (drum_step & 1 ? 1.0f : 0.72f);
        }
        if(tom_patterns[current_bar] & drum_bit) {
            const float tom_duration = seconds_per_beat * 0.25f;
            const float tom_cycles = tom_duration *
                (112.0f * drum_phase - 31.0f * drum_phase * drum_phase);
            envelope = clampf(drum_phase * 24.0f, 0.0f, 1.0f) *
                       (1.0f - drum_phase) * (1.0f - drum_phase);
            voice = (music_sine(tom_cycles) * 0.72f +
                     music_triangle(tom_cycles * 0.5f) * 0.28f) *
                    track->tom_level * envelope * section_energy;
            left += voice * (drum_step & 1 ? 0.46f : 1.0f);
            right += voice * (drum_step & 1 ? 1.0f : 0.46f);
        }

        {
            /* Every instrument already attacks from zero at the downbeat.
               Preserve that transient; only the appended overlap tail fades. */
            const float edge = in_tail ? smoothstepf(
                (float)(samples - 1 - i) / (float)MUSIC_EDGE_SAMPLES) : 1.0f;
            left *= 0.88f * edge;
            right *= 0.88f * edge;
        }
        left *= 1.0f + track->drive * 0.36f;
        right *= 1.0f + track->drive * 0.36f;
        left /= 1.0f + fabsf(left) * (0.46f + track->drive * 0.24f);
        right /= 1.0f + fabsf(right) * (0.46f + track->drive * 0.24f);
        left += music_noise((uint32_t)i ^ track->noise_seed ^ 0x192fc173u) *
                (1.0f / 65536.0f);
        right += music_noise((uint32_t)i ^ track->noise_seed ^ 0x71a2d8efu) *
                 (1.0f / 65536.0f);
        left = clampf(left, -1.0f, 1.0f);
        right = clampf(right, -1.0f, 1.0f);
        peak = fmaxf(peak, fmaxf(fabsf(left), fabsf(right)));
        sum_squares += left * left + right * right;
        music_encode_adpcm_sample(left_samples, i, left, &left_encoder);
        music_encode_adpcm_sample(right_samples, i, right, &right_encoder);
    }

#ifdef GRAVITY_WAVE_EXPORT_MUSIC_HEX
    {
        static const char hex_digits[] = "0123456789abcdef";
        int byte;
        printf("GW_MUSIC_SECTION %d %d %d\n",
               (int)(track - soundtrack_defs), section, samples);
        for(byte = 0; byte < samples; byte += 512) {
            char line[1025];
            int chunk;
            const int count = samples - byte < 512 ? samples - byte : 512;
            for(chunk = 0; chunk < count; ++chunk) {
                line[chunk * 2] = hex_digits[buffer[byte + chunk] >> 4];
                line[chunk * 2 + 1] =
                    hex_digits[buffer[byte + chunk] & 15];
            }
            line[count * 2] = '\0';
            printf("GWHEX %s\n", line);
        }
    }
#endif
    handle = snd_sfx_load_raw_buf((char *)buffer,
                                  (size_t)samples,
                                  MUSIC_SAMPLE_RATE, 4, 2);
    free(buffer);
    if(handle != SFXHND_INVALID) {
        printf("Gravity Wave music: rendered %s section %c "
               "(%.2fs, 21.5k ADPCM, %lu KiB, peak %.2f rms %.2f).\n",
               track->name, 'A' + section, (double)duration,
               (unsigned long)(((size_t)samples + 1023u) / 1024u),
               (double)peak,
               (double)fsqrt(sum_squares / ((float)samples * 2.0f)));
    }
    return handle;
}

static void refill_music_shuffle_bag(int avoid) {
    int i;

    for(i = 0; i < MUSIC_TRACK_COUNT; ++i)
        music_shuffle_bag[i] = i;
    for(i = MUSIC_TRACK_COUNT - 1; i > 0; --i) {
        const int swap_index = (int)(music_random_u32() % (uint32_t)(i + 1));
        const int temporary = music_shuffle_bag[i];
        music_shuffle_bag[i] = music_shuffle_bag[swap_index];
        music_shuffle_bag[swap_index] = temporary;
    }
    /* Every album pass contains all eight songs exactly once. At the seam,
       move the previous song away from the first slot so a refill can never
       create an audible repeat. */
    if(MUSIC_TRACK_COUNT > 1 && music_shuffle_bag[0] == avoid) {
        const int swap_index = 1 +
            (int)(music_random_u32() % (MUSIC_TRACK_COUNT - 1));
        const int temporary = music_shuffle_bag[0];
        music_shuffle_bag[0] = music_shuffle_bag[swap_index];
        music_shuffle_bag[swap_index] = temporary;
    }
    music_shuffle_cursor = 0;
}

static void seed_music_shuffle_after_track(int heard_track) {
    int i;

    /* Sound Test is an explicit seek, so start a fresh audible album pass
       when the player leaves it. The auditioned song counts as heard and the
       next seven automatic starts are the seven other songs exactly once. */
    refill_music_shuffle_bag(-1);
    for(i = 0; i < MUSIC_TRACK_COUNT; ++i) {
        if(music_shuffle_bag[i] == heard_track) {
            const int temporary = music_shuffle_bag[0];
            music_shuffle_bag[0] = music_shuffle_bag[i];
            music_shuffle_bag[i] = temporary;
            break;
        }
    }
    music_shuffle_cursor = MUSIC_TRACK_COUNT > 1 ? 1 : MUSIC_TRACK_COUNT;
}

static int peek_random_music_track(int avoid) {
    int i;

    if(music_shuffle_cursor >= MUSIC_TRACK_COUNT)
        refill_music_shuffle_bag(avoid);
    /* Keep the currently audible song out of the next slot. Sound Test seeds
       a fresh pass on exit, so there is always another candidate here. */
    if(music_shuffle_bag[music_shuffle_cursor] == avoid) {
        for(i = music_shuffle_cursor + 1; i < MUSIC_TRACK_COUNT; ++i) {
            if(music_shuffle_bag[i] != avoid) {
                const int temporary = music_shuffle_bag[music_shuffle_cursor];
                music_shuffle_bag[music_shuffle_cursor] = music_shuffle_bag[i];
                music_shuffle_bag[i] = temporary;
                break;
            }
        }
        if(i >= MUSIC_TRACK_COUNT) {
            /* Defensive fallback for externally forced state: the only
               remaining entry is already audible, so count it as heard. */
            music_shuffle_cursor++;
            refill_music_shuffle_bag(avoid);
        }
    }
    return music_shuffle_bag[music_shuffle_cursor];
}

static void commit_random_music_track(int track) {
    if(music_shuffle_cursor < MUSIC_TRACK_COUNT &&
       music_shuffle_bag[music_shuffle_cursor] == track)
        music_shuffle_cursor++;
}

#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_SHUFFLE
static int take_random_music_track(int avoid) {
    const int track = peek_random_music_track(avoid);
    commit_random_music_track(track);
    return track;
}

static bool run_music_shuffle_self_test(void) {
    uint32_t saved_random_state = music_random_state;
    int saved_bag[MUSIC_TRACK_COUNT];
    int saved_cursor = music_shuffle_cursor;
    int previous = -1;
    bool passed = true;
    int album;

    memcpy(saved_bag, music_shuffle_bag, sizeof(saved_bag));
    music_random_state = 0x6d2b79f5u;
    music_shuffle_cursor = MUSIC_TRACK_COUNT;
    for(album = 0; album < 4; ++album) {
        unsigned int heard_mask = 0u;
        int song;
        for(song = 0; song < MUSIC_TRACK_COUNT; ++song) {
            const int track = take_random_music_track(previous);
            unsigned int track_bit;
            if(track < 0 || track >= MUSIC_TRACK_COUNT) {
                passed = false;
                continue;
            }
            track_bit = 1u << track;
            if(track == previous || (heard_mask & track_bit) != 0u)
                passed = false;
            heard_mask |= track_bit;
            previous = track;
        }
        if(heard_mask != (1u << MUSIC_TRACK_COUNT) - 1u)
            passed = false;
    }
    printf("Gravity Wave music shuffle test: %s (four complete album bags).\n",
           passed ? "PASS" : "FAIL");
    music_random_state = saved_random_state;
    memcpy(music_shuffle_bag, saved_bag, sizeof(saved_bag));
    music_shuffle_cursor = saved_cursor;
    return passed;
}
#endif

static void stop_music(void) {
    int bank;
    for(bank = 0; bank < 2; ++bank) {
        if(music_left_channels[bank] >= 0)
            snd_sfx_stop(music_left_channels[bank]);
        if(music_right_channels[bank] >= 0)
            snd_sfx_stop(music_right_channels[bank]);
    }
    current_music_track = -1;
    current_music_section = 0;
    current_music_volume = 0;
    pending_music_track = -1;
    pending_music_volume = 0;
    pending_music_shuffle = false;
    music_section_time = 0.0f;
    music_playhead_armed = false;
}

static bool play_music_section(int track, int section, int volume) {
    sfx_play_data_t playback = {0};
    const int previous_track = current_music_track;
    const int next_bank = current_music_track < 0 ? 0 : 1 - active_music_bank;

    if(!audio_ready || !music_ready ||
       track < 0 || track >= MUSIC_TRACK_COUNT ||
       music_left_channels[next_bank] < 0 ||
       music_right_channels[next_bank] != music_left_channels[next_bank] + 1)
        return false;
    if(section < 0 || section >= MUSIC_SECTION_COUNT)
        section = 0;
    if(music_sections[track][section] == SFXHND_INVALID)
        section = 0;
    if(music_sections[track][section] == SFXHND_INVALID)
        return false;
    /* The inactive bank has been silent for almost an entire section. Start
     * the new phrase there while the active bank plays its baked fade-out. */
    snd_sfx_stop(music_left_channels[next_bank]);
    snd_sfx_stop(music_right_channels[next_bank]);
    playback.chn = music_left_channels[next_bank];
    playback.idx = music_sections[track][section];
    playback.vol = volume;
    playback.pan = 128;
    playback.loop = 0;
    playback.freq = MUSIC_SAMPLE_RATE;
    if(snd_sfx_play_ex(&playback) >= 0) {
        current_music_track = track;
        current_music_section = section;
        current_music_volume = volume;
        active_music_bank = next_bank;
        pending_music_track = -1;
        pending_music_shuffle = false;
        music_section_time = 0.0f;
        music_playhead_armed = false;
        if(previous_track != track) {
            printf("Gravity Wave music: now playing %s.\n",
                   soundtrack_defs[track].name);
        }
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
        printf("Gravity Wave music test: track=%d section=%c bank=%d "
               "samples=%u duration=%.3f.\n",
               track, 'A' + section, active_music_bank,
               (unsigned int)music_section_samples[track][section],
               (double)music_section_duration[track][section]);
#endif
        return true;
    }
    return false;
}

static void start_music_track(int track, int volume) {
    if(!audio_ready || !music_ready ||
       track < 0 || track >= MUSIC_TRACK_COUNT ||
       music_sections[track][0] == SFXHND_INVALID)
        return;
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
    /* The deterministic album test owns sequencing after its explicit 0A
       start; gameplay chapter requests must not perturb its proof. */
    if(current_music_track >= 0)
        return;
#endif
    if(current_music_track < 0) {
        play_music_section(track, 0, volume);
        return;
    }
    if(current_music_track == track && current_music_volume == volume) {
        pending_music_track = -1;
        pending_music_shuffle = false;
        return;
    }
    pending_music_track = track;
    pending_music_volume = volume;
    pending_music_shuffle = false;
}

static bool play_next_shuffled_music(int volume) {
    const int track = peek_random_music_track(current_music_track);
    if(!play_music_section(track, 0, volume))
        return false;
    commit_random_music_track(track);
    return true;
}

static void request_next_shuffled_music(int volume) {
    if(!audio_ready || !music_ready)
        return;
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
    if(current_music_track >= 0)
        return;
#endif
    if(current_music_track < 0) {
        play_next_shuffled_music(volume);
        return;
    }
    /* Reserve no concrete song yet. Repeated title/run/biome requests merely
       replace the requested volume; the bag advances only when playback at a
       phrase boundary actually succeeds. */
    pending_music_track = -1;
    pending_music_volume = volume;
    pending_music_shuffle = true;
}

static void audition_music_track(int track) {
    /* Normal score changes wait for the phrase boundary. Sound Test is a
       deliberate seek operation, so make it immediate and restart section A
       without leaving the previous section playing underneath it. */
    stop_music();
    start_music_track(track, MUSIC_TITLE_VOLUME);
}

static void update_music(float dt) {
    float duration;
    uint16_t sample_count;
    uint16_t playhead = 0;
    uint16_t transition_sample;
    bool channel_playing = false;
    int next_track;
    int next_section;
    int next_volume;
    bool commits_shuffle = false;

    if(current_music_track < 0)
        return;
    duration = music_section_duration[current_music_track]
                                     [current_music_section];
    sample_count = music_section_samples[current_music_track]
                                          [current_music_section];
    if(duration <= 0.0f || sample_count == 0)
        return;
    music_section_time += dt;
    if(music_left_channels[active_music_bank] >= 0) {
        playhead = snd_get_pos(
            (unsigned int)music_left_channels[active_music_bank]);
        channel_playing = snd_is_playing(
            (unsigned int)music_left_channels[active_music_bank]);
    }
    transition_sample = sample_count > MUSIC_EDGE_SAMPLES ?
        (uint16_t)(sample_count - MUSIC_EDGE_SAMPLES) : sample_count / 2;
    if(channel_playing && playhead < transition_sample)
        music_playhead_armed = true;
    if(music_playhead_armed) {
        if(channel_playing && playhead < transition_sample)
            return;
        if(!channel_playing && music_section_time < duration)
            return;
    }
    else if(music_section_time < duration) {
        return;
    }

    if(pending_music_track >= 0) {
        next_track = pending_music_track;
        next_section = 0;
        next_volume = pending_music_volume;
    }
    else if(pending_music_shuffle) {
        next_track = peek_random_music_track(current_music_track);
        next_section = 0;
        next_volume = pending_music_volume;
        commits_shuffle = true;
    }
    else if(current_music_section == MUSIC_SECTION_COUNT - 1) {
        next_section = 0;
        next_volume = current_music_volume;
        if(game.mode == MODE_TITLE && title_sound_test) {
            /* Sound Test is a true audition deck: the selected A/B/C form
               loops until the player chooses another song or leaves. */
            next_track = sound_test_track;
        }
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
        else {
            next_track = (current_music_track + 1) % MUSIC_TRACK_COUNT;
        }
#else
        else {
            /* Each compact A/B/C song gets one complete statement before the
               shuffle bag advances. Peek now and commit only after AICA has
               successfully begun the next song. */
            next_track = peek_random_music_track(current_music_track);
            commits_shuffle = true;
        }
#endif
    }
    else {
        next_track = current_music_track;
        next_section = (current_music_section + 1) % MUSIC_SECTION_COUNT;
        next_volume = current_music_volume;
    }
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
    {
        const int expected_track =
            current_music_section == MUSIC_SECTION_COUNT - 1 ?
            (current_music_track + 1) % MUSIC_TRACK_COUNT :
            current_music_track;
        const int expected_section =
            current_music_section == MUSIC_SECTION_COUNT - 1 ? 0 :
            current_music_section + 1;
        const bool sequence_ok = next_track == expected_track &&
                                 next_section == expected_section;
        printf("Gravity Wave music test transition: %d%c -> %d%c "
               "playhead=%u target=%u time=%.3f playing=%d armed=%d %s.\n",
               current_music_track, 'A' + current_music_section,
               next_track, 'A' + next_section,
               (unsigned int)playhead, (unsigned int)transition_sample,
               (double)music_section_time, channel_playing,
               music_playhead_armed,
               sequence_ok ? "OK" : "BAD_SEQUENCE");
        if(!sequence_ok)
            music_jukebox_failed = true;
        if(current_music_track == MUSIC_TRACK_COUNT - 1 &&
           current_music_section == MUSIC_SECTION_COUNT - 1 &&
           next_track == 0 && next_section == 0)
            music_jukebox_complete = true;
    }
#endif
    if(play_music_section(next_track, next_section, next_volume) &&
       commits_shuffle)
        commit_random_music_track(next_track);
}

static void init_audio(void) {
    int loaded_sections = 0;
    bool channel_pairs_ok = true;
    bool catalog_budget_ok;
    bool catalog_layout_ok = true;
    bool embedded_catalog_ok;
    size_t catalog_bytes;
    size_t embedded_bytes;
    size_t embedded_offset = 0;
    uint32_t aica_available;
    int i, section, bank;

    audio_ready = false;
    music_ready = false;
    laser_channel = -1;
    for(bank = 0; bank < 2; ++bank) {
        music_left_channels[bank] = -1;
        music_right_channels[bank] = -1;
    }
    active_music_bank = 0;
    current_music_track = -1;
    pending_music_track = -1;
    pending_music_shuffle = false;
    music_section_time = 0.0f;
    music_playhead_armed = false;
    for(i = 0; i < MUSIC_TRACK_COUNT; ++i) {
        for(section = 0; section < MUSIC_SECTION_COUNT; ++section) {
            music_sections[i][section] = SFXHND_INVALID;
            music_section_duration[i][section] = 0.0f;
            music_section_samples[i][section] = 0;
        }
    }

    if(snd_init() < 0) {
        printf("Gravity Wave: AICA initialization failed; continuing silently.\n");
        return;
    }
    audio_ready = true;

    laser_channel = snd_sfx_chn_alloc();
    for(bank = 0; bank < 2; ++bank) {
        music_left_channels[bank] = snd_sfx_chn_alloc();
        music_right_channels[bank] = snd_sfx_chn_alloc();
        if(music_left_channels[bank] < 0 ||
           music_right_channels[bank] != music_left_channels[bank] + 1)
            channel_pairs_ok = false;
    }
    if(!channel_pairs_ok) {
        for(bank = 0; bank < 2; ++bank) {
            if(music_left_channels[bank] >= 0)
                snd_sfx_chn_free(music_left_channels[bank]);
            if(music_right_channels[bank] >= 0)
                snd_sfx_chn_free(music_right_channels[bank]);
            music_left_channels[bank] = -1;
            music_right_channels[bank] = -1;
        }
        printf("Gravity Wave: no paired AICA channels for music.\n");
    }

    sfx_laser = load_generated_sound(0, 880.0f, 0.11f);
    sfx_fast_laser = load_generated_sound(0, 1480.0f, 0.055f);
    sfx_phase_wave = load_generated_sound(4, 235.0f, 0.22f);
    sfx_explosion = load_generated_sound(1, 95.0f, 0.42f);
    sfx_hit = load_generated_sound(2, 180.0f, 0.18f);
    sfx_gate = load_generated_sound(3, 520.0f, 0.34f);
    catalog_bytes = music_catalog_bytes_required();
    embedded_bytes = (size_t)((uintptr_t)gravity_wave_music_assets_end -
                              (uintptr_t)gravity_wave_music_assets);
    {
        const uint32_t expected_fingerprint = music_catalog_fingerprint();
        const bool embedded_size_ok =
            embedded_bytes == catalog_bytes + MUSIC_ASSET_HEADER_BYTES;
        const uint32_t embedded_fingerprint = embedded_size_ok ?
            (uint32_t)gravity_wave_music_assets[0] |
            ((uint32_t)gravity_wave_music_assets[1] << 8) |
            ((uint32_t)gravity_wave_music_assets[2] << 16) |
            ((uint32_t)gravity_wave_music_assets[3] << 24) : 0u;
        const uint32_t embedded_payload_fingerprint = embedded_size_ok ?
            (uint32_t)gravity_wave_music_assets[4] |
            ((uint32_t)gravity_wave_music_assets[5] << 8) |
            ((uint32_t)gravity_wave_music_assets[6] << 16) |
            ((uint32_t)gravity_wave_music_assets[7] << 24) : 0u;
        const uint32_t calculated_payload_fingerprint = embedded_size_ok ?
            music_hash_bytes(2166136261u,
                             gravity_wave_music_assets +
                             MUSIC_ASSET_HEADER_BYTES,
                             catalog_bytes) : 0u;
        embedded_catalog_ok =
            embedded_size_ok &&
            embedded_fingerprint == expected_fingerprint &&
            embedded_payload_fingerprint == calculated_payload_fingerprint;
        printf("Gravity Wave music: catalog fingerprint %08lx "
               "(embedded %08lx), payload %08lx/%08lx.\n",
               (unsigned long)expected_fingerprint,
               (unsigned long)embedded_fingerprint,
               (unsigned long)embedded_payload_fingerprint,
               (unsigned long)calculated_payload_fingerprint);
#ifdef GRAVITY_WAVE_EXPORT_MUSIC_HEX
        printf("GW_MUSIC_FINGERPRINT %08lx\n",
               (unsigned long)expected_fingerprint);
#endif
    }
#ifdef GRAVITY_WAVE_EXPORT_MUSIC_HEX
    embedded_catalog_ok = false;
#endif
    aica_available = snd_mem_available();
    for(i = 0; i < MUSIC_TRACK_COUNT; ++i) {
        if(!music_track_layout_valid(&soundtrack_defs[i]))
            catalog_layout_ok = false;
    }
    catalog_budget_ok = catalog_bytes + MUSIC_AICA_RESERVE <=
                        (size_t)aica_available;
    printf("Gravity Wave music: album budget %lu KiB + %lu KiB reserve "
           "inside %lu KiB available.\n",
           (unsigned long)((catalog_bytes + 1023u) / 1024u),
           (unsigned long)(MUSIC_AICA_RESERVE / 1024u),
           (unsigned long)(aica_available / 1024u));
    printf("Gravity Wave music: %s album image (%lu/%lu KiB).\n",
           embedded_catalog_ok ? "verified embedded" : "runtime fallback",
           (unsigned long)((embedded_bytes + 1023u) / 1024u),
           (unsigned long)((catalog_bytes + 1023u) / 1024u));
    if(!embedded_catalog_ok)
        init_music_sine_table();
    if(channel_pairs_ok && catalog_budget_ok && catalog_layout_ok) {
        if(embedded_catalog_ok)
            embedded_offset = MUSIC_ASSET_HEADER_BYTES;
        for(i = 0; i < MUSIC_TRACK_COUNT; ++i) {
            for(section = 0; section < MUSIC_SECTION_COUNT; ++section) {
                const int samples = music_track_section_samples(
                    &soundtrack_defs[i]);
                if(embedded_catalog_ok) {
                    music_sections[i][section] = load_embedded_music(
                        gravity_wave_music_assets + embedded_offset,
                        &soundtrack_defs[i],
                        &music_section_duration[i][section]);
                    embedded_offset += (size_t)samples;
                }
                else {
                    music_sections[i][section] = load_generated_music(
                        &soundtrack_defs[i], section,
                        &music_section_duration[i][section]);
                }
                if(music_sections[i][section] != SFXHND_INVALID) {
                    music_section_samples[i][section] = (uint16_t)(
                        music_section_duration[i][section] *
                        (float)MUSIC_SAMPLE_RATE + 0.5f);
                    loaded_sections++;
                }
            }
            if(embedded_catalog_ok)
                printf("Gravity Wave music: loaded %s A/B/C (%d KiB).\n",
                       soundtrack_defs[i].name,
                       (music_track_section_samples(&soundtrack_defs[i]) *
                        MUSIC_SECTION_COUNT + 1023) / 1024);
        }
    }
    else if(!catalog_budget_ok)
        printf("Gravity Wave music: retail AICA safety budget exceeded.\n");
    else if(!catalog_layout_ok)
        printf("Gravity Wave music: invalid AICA section layout.\n");

    if(loaded_sections == MUSIC_TRACK_COUNT * MUSIC_SECTION_COUNT &&
       channel_pairs_ok)
        music_ready = true;
    else {
        for(i = 0; i < MUSIC_TRACK_COUNT; ++i) {
            for(section = 0; section < MUSIC_SECTION_COUNT; ++section) {
                if(music_sections[i][section] != SFXHND_INVALID) {
                    snd_sfx_unload(music_sections[i][section]);
                    music_sections[i][section] = SFXHND_INVALID;
                    music_section_duration[i][section] = 0.0f;
                    music_section_samples[i][section] = 0;
                }
            }
        }
        printf("Gravity Wave music: catalog or channels incomplete; "
               "music disabled safely.\n");
    }

    printf("Gravity Wave audio: laser=%lu rapid=%lu phase=%lu "
           "explosion=%lu gate=%lu, "
           "%d/%d music sections, channels=%d/%d+%d/%d, "
           "%lu KiB AICA RAM free.\n",
           (unsigned long)sfx_laser, (unsigned long)sfx_fast_laser,
           (unsigned long)sfx_phase_wave, (unsigned long)sfx_explosion,
           (unsigned long)sfx_gate, loaded_sections,
           MUSIC_TRACK_COUNT * MUSIC_SECTION_COUNT,
           music_left_channels[0], music_right_channels[0],
           music_left_channels[1], music_right_channels[1],
           (unsigned long)(snd_mem_available() / 1024u));
}

static void play_sound(sfxhnd_t sound, int volume, int pan) {
    if(sound != SFXHND_INVALID)
        snd_sfx_play(sound, volume, pan);
}

static void play_channelled_weapon_sound(sfxhnd_t sound, int volume, float x) {
    const int pan = (int)clampf(128.0f + x * 1.35f, 16.0f, 240.0f);
    if(sound != SFXHND_INVALID) {
        if(laser_channel >= 0)
            snd_sfx_play_chn(laser_channel, sound, volume, pan);
        else
            snd_sfx_play(sound, volume, pan);
    }
}

static void play_laser(float x) {
    play_channelled_weapon_sound(sfx_laser, 92, x);
}

static void play_fast_laser(float x) {
    play_channelled_weapon_sound(sfx_fast_laser, 78, x);
}

static void play_phase_wave(float x) {
    const int pan = (int)clampf(128.0f + x * 1.2f, 20.0f, 236.0f);
    play_sound(sfx_phase_wave, 158, pan);
}

static void play_rumble(int power) {
    maple_device_t *device = maple_enum_type(0, MAPLE_FUNC_PURUPURU);
    purupuru_effect_t effect = {.raw = 0};
    if(!device)
        return;
    effect.motor = 1;
    effect.fpow = (uint32_t)clampf((float)power, 1.0f, 7.0f);
    /* Basic finite thuds work on both Sega and third-party Jump Packs. */
    effect.freq = power >= 6 ? 26 : 34;
    effect.inc = 1;
    effect.cont = false;
    purupuru_rumble(device, &effect);
}

static void shutdown_audio(void) {
    int bank;
    if(!audio_ready)
        return;
    stop_music();
    if(laser_channel >= 0) {
        snd_sfx_stop(laser_channel);
        snd_sfx_chn_free(laser_channel);
        laser_channel = -1;
    }
    for(bank = 0; bank < 2; ++bank) {
        if(music_left_channels[bank] >= 0) {
            snd_sfx_chn_free(music_left_channels[bank]);
            music_left_channels[bank] = -1;
        }
        if(music_right_channels[bank] >= 0) {
            snd_sfx_chn_free(music_right_channels[bank]);
            music_right_channels[bank] = -1;
        }
    }
    snd_sfx_unload_all();
    snd_shutdown();
    audio_ready = false;
}

static float terrain_row_z(int row) {
    /* Keep the terrain lattice fixed in world space. A camera-relative grid
       leaves every vertex at a constant screen depth while its noise sample
       advances through the world, making distant cliffs appear to ripple.
       Advancing this aligned lattice one complete row at a time preserves all
       overlapping world vertices. Keep a complete foreground guard row below
       the viewport so the retiring strip never exposes the sky; the two extra
       rows retain the original fog-distance coverage without screen-space
       bending or stretching. */
    const float first_world_z =
        floorf((game.distance + TERRAIN_NEAR_Z) / TERRAIN_SPACING_Z) *
        TERRAIN_SPACING_Z - TERRAIN_SPACING_Z;
    return first_world_z + (float)row * TERRAIN_SPACING_Z - game.distance;
}

static void prepare_terrain(const palette_t *palette) {
    int row, column;

    for(row = 0; row < TERRAIN_ROWS; ++row) {
        const float local_z = terrain_row_z(row);
        const float world_z = game.distance + local_z;
        const float center = path_center(world_z);

        for(column = 0; column < TERRAIN_COLS; ++column) {
            const float local_x =
                ((float)column - (float)(TERRAIN_COLS - 1) * 0.5f) *
                TERRAIN_SPACING_X;
            terrain[row][column].height =
                terrain_height_local(local_x, world_z);
            project_world((vec3_t){center + local_x,
                                   terrain[row][column].height,
                                   world_z},
                          &terrain[row][column].screen);
        }
    }

    for(row = 0; row < TERRAIN_ROWS; ++row) {
        const float local_z = terrain_row_z(row);
        const int world_row =
            (int)floorf((game.distance + local_z) / TERRAIN_SPACING_Z);
        for(column = 0; column < TERRAIN_COLS; ++column) {
            const float h = terrain[row][column].height;
            const float hl = terrain[row][column > 0 ? column - 1 : column].height;
            const float hr = terrain[row][column + 1 < TERRAIN_COLS ?
                                     column + 1 : column].height;
            const float slope_light = clampf(0.78f - (hr - hl) * 0.018f,
                                             0.43f, 1.18f);
            const float height_mix = clampf((h + 8.0f) / 96.0f, 0.0f, 1.0f);
            /* Base the alternating facet light on the world row as well, so
               reindexing the lattice cannot flash its checker pattern. */
            const float facet = 0.90f +
                (((world_row + column) & 1) ? 0.055f : -0.025f);
            color3_t material_tint = color_lerp(
                (color3_t){0.88f, 0.90f, 0.94f},
                color_lerp(palette->ground_low, palette->ground_high,
                           height_mix), 0.28f);
            color3_t color = material_tint;
            const float manual_fog = smoothstepf((local_z - 1050.0f) / 390.0f);
            color = color_scale(color, slope_light * facet);
            color = color_lerp(color, palette->fog, manual_fog * 0.54f);
            terrain[row][column].color = pack_color(1.0f, color);
        }
    }
}

static void draw_sky_opaque(const palette_t *palette) {
    const color3_t middle = color_lerp(palette->sky_top,
                                       palette->sky_horizon, 0.43f);
    const color3_t low = color_lerp(palette->sky_horizon,
                                    palette->fog, 0.50f);
    const float sun_x = 492.0f + fsin(game.distance * 0.00031f) * 38.0f;
    const float sun_y = 112.0f + fsin(game.time * 0.11f) * 7.0f;
    const uint32_t top_color = pack_color(1.0f, palette->sky_top);
    const uint32_t middle_color = pack_color(1.0f, middle);
    const uint32_t horizon_color = pack_color(1.0f, palette->sky_horizon);
    const uint32_t low_color = pack_color(1.0f, low);
    int i;

    {
        const screen_point_t a = {0.0f, 0.0f, 0.000001f, true};
        const screen_point_t b = {SCREEN_W, 0.0f, 0.000001f, true};
        const screen_point_t c = {0.0f, 122.0f, 0.000001f, true};
        const screen_point_t d = {SCREEN_W, 122.0f, 0.000001f, true};
        submit_quad(&opaque_header, &a, &b, &c, &d,
                    top_color, top_color, middle_color, middle_color);
    }
    {
        const screen_point_t a = {0.0f, 122.0f, 0.000001f, true};
        const screen_point_t b = {SCREEN_W, 122.0f, 0.000001f, true};
        const screen_point_t c = {0.0f, 258.0f, 0.000001f, true};
        const screen_point_t d = {SCREEN_W, 258.0f, 0.000001f, true};
        submit_quad(&opaque_header, &a, &b, &c, &d,
                    middle_color, middle_color, horizon_color, horizon_color);
    }
    draw_rect(&opaque_header, 0.0f, 258.0f, SCREEN_W, SCREEN_H - 258.0f,
              0.000001f, low_color);

    for(i = 0; i < STAR_COUNT; ++i) {
        const uint32_t h = hash_u32((uint32_t)i * 9719u + 0x51633e2du);
        float x = (float)(h & 1023u) * (SCREEN_W / 1024.0f);
        const float y = 18.0f + (float)((h >> 10) & 255u) * 0.64f;
        const float twinkle = 0.52f + 0.48f *
            fsin(game.time * (1.2f + (float)(i % 5) * 0.21f) + (float)i);
        const float size = 1.0f + (float)((h >> 19) & 3u) * 0.45f;
        color3_t star = color_lerp((color3_t){0.55f, 0.75f, 1.0f},
                                   palette->accent, (float)(i % 4) * 0.20f);
        x += fsin(game.distance * 0.0004f + (float)i) * 2.5f;
        draw_rect(&opaque_header, x, y, size, size, 0.000012f,
                  pack_color(1.0f, color_scale(star, 0.50f + twinkle * 0.50f)));
    }

    {
        const float sun_radius = 32.0f;
        const float stripe_height = 2.0f;

        draw_disc(&opaque_header, sun_x, sun_y, sun_radius, 0.000020f, 18,
              pack_color(1.0f, (color3_t){1.0f, 0.96f, 0.72f}),
              pack_color(1.0f, palette->accent));
        for(i = 0; i < 5; ++i) {
            const float stripe_y = sun_y - 20.0f + (float)i * 10.0f;
            const float offset = stripe_y + stripe_height * 0.5f - sun_y;
            const float half_chord = sqrtf(fmaxf(
                0.0f, sun_radius * sun_radius - offset * offset)) - 2.0f;

            if(half_chord > 0.0f)
                draw_rect(&opaque_header, sun_x - half_chord, stripe_y,
                          half_chord * 2.0f, stripe_height, 0.000021f,
                          pack_color(1.0f, palette->sky_horizon));
        }
    }
}

static void draw_sun_glow(const palette_t *palette) {
    const float sun_x = 492.0f + fsin(game.distance * 0.00031f) * 38.0f;
    const float sun_y = 112.0f + fsin(game.time * 0.11f) * 7.0f;
    draw_disc(&additive_header, sun_x, sun_y, 58.0f, 0.000019f, 20,
              pack_color(0.18f, palette->accent),
              pack_color(0.0f, palette->accent));
    draw_disc(&additive_header, sun_x, sun_y, 42.0f, 0.000020f, 18,
              pack_color(0.23f, (color3_t){1.0f, 0.72f, 0.32f}),
              pack_color(0.0f, palette->accent));
}

/* A restrained holographic grid gives the route a synthwave horizon without
   flattening the biome terrain or competing with combat silhouettes. */
static void draw_neon_route_grid(const palette_t *palette) {
    const float spacing = 104.0f;
    const float first_z = ceilf((game.distance + 86.0f) / spacing) * spacing;
    const color3_t electric_cyan = color_lerp(
        palette->river, (color3_t){0.03f, 0.92f, 1.0f}, 0.72f);
    const color3_t hot_magenta = color_lerp(
        palette->accent, (color3_t){1.0f, 0.04f, 0.62f}, 0.72f);
    const float mode_gain = game.mode == MODE_TITLE ? 1.12f : 0.92f;
    int row;
    int lane;

    /* Crossbars remain world-locked, so they stream naturally beneath the
       player instead of behaving like a screen-space overlay. */
    for(row = 0; row < 13; ++row) {
        const float world_z = first_z + (float)row * spacing;
        const float relative_z = world_z - game.distance;
        const float near_fade = smoothstepf((relative_z - 72.0f) / 145.0f);
        const float far_fade = 1.0f -
            smoothstepf((relative_z - 1080.0f) / 290.0f);
        const float strength = near_fade * far_fade * mode_gain;
        const float center = path_center(world_z);
        const float y = terrain_height_local(0.0f, world_z) + 1.45f;
        const color3_t color = (row & 3) == 0 ? hot_magenta : electric_cyan;
        screen_point_t left;
        screen_point_t right;

        if(strength <= 0.01f ||
           !project_world((vec3_t){center - 43.0f, y, world_z}, &left) ||
           !project_world((vec3_t){center + 43.0f, y, world_z}, &right))
            continue;
        left.z += 0.000025f;
        right.z += 0.000025f;
        draw_line(&additive_header,
                  left.x, left.y, left.z, right.x, right.y, right.z,
                  0.65f + strength * 0.95f,
                  pack_color(0.07f * strength, color),
                  pack_color(0.18f * strength, color));
    }

    /* Five curved longitudinal rails sell the perspective while staying
       inside the readable valley corridor. */
    for(lane = -2; lane <= 2; ++lane) {
        screen_point_t previous;
        float previous_strength = 0.0f;
        bool previous_valid = false;
        const float local_x = (float)lane * 14.0f;
        const color3_t color = lane == 0 ? hot_magenta : electric_cyan;

        for(row = 0; row < 13; ++row) {
            const float world_z = first_z + (float)row * spacing;
            const float relative_z = world_z - game.distance;
            const float near_fade = smoothstepf((relative_z - 72.0f) / 145.0f);
            const float far_fade = 1.0f -
                smoothstepf((relative_z - 1080.0f) / 290.0f);
            const float strength = near_fade * far_fade * mode_gain;
            const float center = path_center(world_z);
            const float y = terrain_height_local(local_x, world_z) + 1.65f;
            screen_point_t current;
            const bool valid = strength > 0.01f &&
                project_world((vec3_t){center + local_x, y, world_z}, &current);

            if(valid) {
                current.z += 0.000028f;
                if(previous_valid) {
                    draw_line(&additive_header,
                              previous.x, previous.y, previous.z,
                              current.x, current.y, current.z,
                              lane == 0 ? 1.25f : 0.88f,
                              pack_color(0.10f * previous_strength, color),
                              pack_color(0.10f * strength, color));
                }
                previous = current;
                previous_strength = strength;
            }
            previous_valid = valid;
        }
    }
}

static void draw_terrain(void) {
    const int texture_id = GRAVITY_WAVE_TEX_TERRAIN_AZURE +
                           (game.palette_index & 3);
    int row;

    submit_poly_header(&texture_headers[texture_id]);
    for(row = 0; row < TERRAIN_ROWS - 1; ++row) {
        pvr_vertex_t vertices[TERRAIN_COLS * 2];
        int column = 0;

        const float v0 = (game.distance + terrain_row_z(row)) * 0.018f;
        const float v1 = (game.distance + terrain_row_z(row + 1)) * 0.018f;
        while(column < TERRAIN_COLS) {
            int first_column;
            int strip_columns;
            int strip_column;

            /* A very tall outer ridge can rotate behind the near plane during
               a hard bank. Never feed its stale projection to the PVR: trim
               each row pair into contiguous, fully valid strip segments. */
            while(column < TERRAIN_COLS &&
                  (!terrain[row][column].screen.valid ||
                   !terrain[row + 1][column].screen.valid))
                ++column;
            first_column = column;
            while(column < TERRAIN_COLS &&
                  terrain[row][column].screen.valid &&
                  terrain[row + 1][column].screen.valid)
                ++column;
            strip_columns = column - first_column;
            if(strip_columns < 2)
                continue;

            for(strip_column = 0; strip_column < strip_columns;
                ++strip_column) {
                const int source_column = first_column + strip_column;
                const int index = strip_column * 2;
                const float u = (float)source_column * 0.42f;
                const bool end = strip_column == strip_columns - 1;
                make_textured_vertex(&vertices[index],
                                     &terrain[row][source_column].screen,
                                     u, v0,
                                     terrain[row][source_column].color,
                                     false);
                make_textured_vertex(&vertices[index + 1],
                                     &terrain[row + 1][source_column].screen,
                                     u, v1,
                                     terrain[row + 1][source_column].color,
                                     end);
            }
            pvr_prim(vertices,
                     (size_t)strip_columns * 2u * sizeof(vertices[0]));
        }
    }
}

static void draw_river(const palette_t *palette) {
    const uint32_t near_color = pack_color(1.0f, color_scale(palette->river, 0.56f));
    const uint32_t far_color = pack_color(1.0f, color_scale(palette->river, 0.88f));
    int row;

    for(row = 0; row < TERRAIN_ROWS - 1; ++row) {
        const float z0 = game.distance + terrain_row_z(row);
        const float z1 = game.distance + terrain_row_z(row + 1);
        const float width0 = 11.0f + fsin(z0 * 0.021f) * 1.8f;
        const float width1 = 11.0f + fsin(z1 * 0.021f) * 1.8f;
        const float y0 = terrain_height_local(0.0f, z0) + 0.65f;
        const float y1 = terrain_height_local(0.0f, z1) + 0.65f;
        const float c0 = path_center(z0);
        const float c1 = path_center(z1);
        const float fade = (float)row / (float)(TERRAIN_ROWS - 1);
        const uint32_t color = fade > 0.55f ? far_color : near_color;
        draw_world_textured_quad(&texture_headers[GRAVITY_WAVE_TEX_CANOPY_ENERGY],
                                 (vec3_t){c0 - width0, y0, z0},
                                 (vec3_t){c0 + width0, y0, z0},
                                 (vec3_t){c1 - width1, y1, z1},
                                 (vec3_t){c1 + width1, y1, z1},
                                 0.0f, z0 * 0.026f,
                                 1.0f, z0 * 0.026f,
                                 0.0f, z1 * 0.026f,
                                 1.0f, z1 * 0.026f,
                                 color, color, color, color);
    }
}

static void draw_spire(float local_x, float world_z, float width,
                       float height, const palette_t *palette) {
    const float center = path_center(world_z) + local_x;
    const float base = terrain_height_local(local_x, world_z);
    const vec3_t tip = {center, base + height, world_z};
    const vec3_t p0 = {center - width, base, world_z - width};
    const vec3_t p1 = {center + width, base, world_z - width};
    const vec3_t p2 = {center - width, base, world_z + width};
    const vec3_t p3 = {center + width, base, world_z + width};
    const uint32_t dark = pack_color(1.0f, color_scale(palette->ground_low, 0.72f));
    const uint32_t light = pack_color(1.0f, color_scale(palette->ground_high, 1.15f));
    const uint32_t rim = pack_color(1.0f, color_lerp(palette->ground_high,
                                                    palette->accent, 0.30f));

    const pvr_poly_hdr_t *header = &texture_headers[
        GRAVITY_WAVE_TEX_TERRAIN_AZURE + (game.palette_index & 3)];

    draw_world_textured_triangle(header, p0, p1, tip,
                                 0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f,
                                 dark, dark, light);
    draw_world_textured_triangle(header, p1, p3, tip,
                                 0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f,
                                 dark, rim, light);
    draw_world_textured_triangle(header, p3, p2, tip,
                                 0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f,
                                 rim, dark, light);
    draw_world_textured_triangle(header, p2, p0, tip,
                                 0.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f,
                                 dark, dark, light);
}

static void draw_procedural_spires(const palette_t *palette) {
    const int first_segment = (int)(game.distance / 88.0f) + 1;
    int i;

    for(i = 0; i < 17; ++i) {
        const int segment = first_segment + i;
        const uint32_t h = hash_u32((uint32_t)segment ^ 0x6b41d52bu);
        const float z = (float)segment * 88.0f;
        if((h & 7u) < 4u) {
            const float side = (h & 0x10u) ? 1.0f : -1.0f;
            /* Decorative skyline spires stay beyond the player's full lateral
               envelope; close silhouettes are reserved for collidable authored
               structures. */
            const float x = side * (112.0f + (float)((h >> 8) & 127u));
            const float width = 5.0f + (float)((h >> 16) & 7u);
            const float height = 20.0f + (float)((h >> 20) & 31u);
            draw_spire(x, z, width, height, palette);
        }
    }
}

static void draw_material_box(float local_x, float base_y, float world_z,
                              float half_width, float height, float half_depth,
                              int texture_id, color3_t tint) {
    const float center_x = path_center(world_z) + local_x;
    const float x0 = center_x - half_width;
    const float x1 = center_x + half_width;
    const float y0 = base_y;
    const float y1 = base_y + height;
    const float z0 = world_z - half_depth;
    const float z1 = world_z + half_depth;
    const float u_side = fmaxf(1.0f, half_width / 8.0f);
    const float v_side = fmaxf(1.0f, height / 10.0f);
    const uint32_t light = pack_color(1.0f, color_scale(tint, 1.08f));
    const uint32_t middle = pack_color(1.0f, color_scale(tint, 0.82f));
    const uint32_t dark = pack_color(1.0f, color_scale(tint, 0.58f));
    const pvr_poly_hdr_t *header = &texture_headers[texture_id];

    draw_world_textured_quad(header,
        (vec3_t){x0,y0,z0}, (vec3_t){x1,y0,z0},
        (vec3_t){x0,y1,z0}, (vec3_t){x1,y1,z0},
        0.0f,v_side, u_side,v_side, 0.0f,0.0f, u_side,0.0f,
        middle,middle,light,light);
    draw_world_textured_quad(header,
        (vec3_t){x1,y0,z1}, (vec3_t){x0,y0,z1},
        (vec3_t){x1,y1,z1}, (vec3_t){x0,y1,z1},
        0.0f,v_side, u_side,v_side, 0.0f,0.0f, u_side,0.0f,
        dark,dark,middle,middle);
    draw_world_textured_quad(header,
        (vec3_t){x0,y0,z1}, (vec3_t){x0,y0,z0},
        (vec3_t){x0,y1,z1}, (vec3_t){x0,y1,z0},
        0.0f,v_side, u_side,v_side, 0.0f,0.0f, u_side,0.0f,
        dark,middle,middle,light);
    draw_world_textured_quad(header,
        (vec3_t){x1,y0,z0}, (vec3_t){x1,y0,z1},
        (vec3_t){x1,y1,z0}, (vec3_t){x1,y1,z1},
        0.0f,v_side, u_side,v_side, 0.0f,0.0f, u_side,0.0f,
        middle,dark,light,middle);
    draw_world_textured_quad(header,
        (vec3_t){x0,y1,z0}, (vec3_t){x1,y1,z0},
        (vec3_t){x0,y1,z1}, (vec3_t){x1,y1,z1},
        0.0f,0.0f, u_side,0.0f, 0.0f,u_side, u_side,u_side,
        light,light,middle,middle);
}

static void draw_furnace_louvers(float local_x, float base_y, float world_z,
                                 float half_width, float front_depth) {
    const float center_x = path_center(world_z) + local_x;
    const float z = world_z - front_depth - 0.10f;
    const color3_t hot = {1.0f,0.34f,0.055f};
    const color3_t edge = {0.78f,0.075f,0.018f};
    const pvr_poly_hdr_t *header =
        &texture_headers[GRAVITY_WAVE_TEX_CANOPY_ENERGY];
    int louver;

    for(louver = 0; louver < 4; ++louver) {
        const float y0 = base_y + (float)louver * 3.25f;
        const float y1 = y0 + 1.45f;
        draw_world_textured_quad(header,
            (vec3_t){center_x - half_width,y0,z},
            (vec3_t){center_x + half_width,y0,z},
            (vec3_t){center_x - half_width,y1,z},
            (vec3_t){center_x + half_width,y1,z},
            0.0f,1.0f, 1.0f,1.0f, 0.0f,0.0f, 1.0f,0.0f,
            pack_color(1.0f, edge), pack_color(1.0f, edge),
            pack_color(1.0f, hot), pack_color(1.0f, hot));
    }
}

static void draw_material_beam(float world_z,
                               float x0, float y0, float x1, float y1,
                               float half_width, float half_depth,
                               int texture_id, color3_t tint);

static void draw_furnace_vent(float local_x, float base_y, float world_z,
                              int variant) {
    const bool tall = variant == 1;
    const float outlet_offset = tall ? 6.5f : 7.5f;
    const float housing_height = tall ? 25.0f : 28.0f;
    const float stack_base = base_y + housing_height + 5.0f;
    const float stack_top = base_y + (tall ? 67.5f : 54.5f);
    const float stack_height = stack_top - stack_base;
    const float collar_y = stack_base + stack_height * 0.52f;
    const color3_t iron = {0.40f,0.43f,0.45f};
    const color3_t dark_iron = {0.25f,0.27f,0.29f};
    const color3_t furnace = {0.63f,0.28f,0.18f};
    int side;

    /* A broad machinery plinth and armored firebox keep the assembly from
       reading as a freestanding masonry chimney. */
    draw_material_box(local_x, base_y, world_z,
                      18.0f, 6.0f, 14.0f,
                      GRAVITY_WAVE_TEX_HULL_HOSTILE, dark_iron);
    draw_material_box(local_x, base_y + 6.0f, world_z,
                      12.5f, housing_height - 6.0f, 10.5f,
                      GRAVITY_WAVE_TEX_HULL_HOSTILE, furnace);
    draw_material_box(local_x, base_y + housing_height - 5.0f, world_z,
                      16.0f, 5.0f, 12.0f,
                      GRAVITY_WAVE_TEX_HULL_HOSTILE,
                      color_scale(furnace, 0.78f));
    draw_furnace_louvers(local_x, base_y + 8.0f, world_z,
                         8.6f, 10.5f);

    /* The twin exhausts, cross-manifold, collars and flared rain caps give
       the vent a compact refinery silhouette even at Dreamcast distance. */
    draw_material_box(local_x, stack_base + 2.5f, world_z,
                      outlet_offset + 4.4f, 4.0f, 5.0f,
                      GRAVITY_WAVE_TEX_HULL_ALLIED, dark_iron);
    for(side = -1; side <= 1; side += 2) {
        const float pipe_x = local_x + (float)side * outlet_offset;
        draw_material_box(pipe_x, stack_base, world_z,
                          3.2f, stack_height, 3.4f,
                          GRAVITY_WAVE_TEX_HULL_ALLIED, iron);
        draw_material_box(pipe_x, collar_y, world_z,
                          4.5f, 2.4f, 4.7f,
                          GRAVITY_WAVE_TEX_HULL_HOSTILE, furnace);
        draw_material_box(pipe_x, stack_top - 3.2f, world_z,
                          4.9f, 3.2f, 5.1f,
                          GRAVITY_WAVE_TEX_HULL_HOSTILE,
                          color_scale(furnace, 0.84f));
        draw_material_beam(world_z,
            local_x + (float)side * 15.5f, base_y + 13.0f,
            pipe_x, stack_base + 4.0f,
            1.45f, 2.0f, GRAVITY_WAVE_TEX_HULL_ALLIED, dark_iron);
    }
}

static void draw_material_beam(float world_z,
                               float x0, float y0, float x1, float y1,
                               float half_width, float half_depth,
                               int texture_id, color3_t tint) {
    const float center_x = path_center(world_z);
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length_sq = dx * dx + dy * dy;
    const float inv_length = frsqrt(fmaxf(length_sq, 0.0001f));
    const float nx = -dy * inv_length * half_width;
    const float ny = dx * inv_length * half_width;
    const float z0 = world_z - half_depth;
    const float z1 = world_z + half_depth;
    const float texture_length = fmaxf(1.0f, sqrtf(length_sq) / 11.0f);
    const bool detailed = world_z - game.distance < 760.0f;
    const vec3_t front[4] = {
        {center_x + x0 + nx, y0 + ny, z0},
        {center_x + x0 - nx, y0 - ny, z0},
        {center_x + x1 + nx, y1 + ny, z0},
        {center_x + x1 - nx, y1 - ny, z0}
    };
    const vec3_t back[4] = {
        {center_x + x0 + nx, y0 + ny, z1},
        {center_x + x0 - nx, y0 - ny, z1},
        {center_x + x1 + nx, y1 + ny, z1},
        {center_x + x1 - nx, y1 - ny, z1}
    };
    const uint32_t light = pack_color(1.0f, color_scale(tint, 1.10f));
    const uint32_t middle = pack_color(1.0f, color_scale(tint, 0.80f));
    const uint32_t dark = pack_color(1.0f, color_scale(tint, 0.47f));
    const pvr_poly_hdr_t *header = &texture_headers[texture_id];

    draw_world_textured_quad(header,
        front[0],front[1],front[2],front[3],
        0.0f,1.0f, 1.0f,1.0f,
        0.0f,1.0f - texture_length, 1.0f,1.0f - texture_length,
        middle,dark,light,middle);
    if(!detailed)
        return;
    draw_world_textured_quad(header,
        front[0],front[2],back[0],back[2],
        0.0f,0.0f, texture_length,0.0f,
        0.0f,1.0f, texture_length,1.0f,
        light,middle,dark,dark);
    draw_world_textured_quad(header,
        front[1],front[3],back[1],back[3],
        0.0f,0.0f, texture_length,0.0f,
        0.0f,1.0f, texture_length,1.0f,
        dark,middle,dark,dark);
}

static void draw_material_arch(float world_z, float half_span,
                               float base_y, float height, float thickness,
                               int texture_id, color3_t tint) {
    const float knee_y = base_y + height * 0.58f;
    const float shoulder_y = base_y + height * 0.84f;
    const float crown_y = base_y + height;
    const float half_depth = thickness * 0.86f;
    int side;

    /* Split, swept ribs replace the old three-box doorway. The crown remains
       open in the center and each successive beam tapers into the apex. */
    for(side = -1; side <= 1; side += 2) {
        const float s = (float)side;
        draw_material_beam(world_z,
            s * half_span, base_y,
            s * half_span * 0.98f, knee_y,
            thickness, half_depth, texture_id, tint);
        draw_material_beam(world_z,
            s * half_span * 0.98f, knee_y,
            s * half_span * 0.70f, shoulder_y,
            thickness * 0.92f, half_depth, texture_id,
            color_scale(tint, 1.05f));
        draw_material_beam(world_z,
            s * half_span * 0.70f, shoulder_y,
            s * half_span * 0.10f, crown_y,
            thickness * 0.74f, half_depth * 0.88f, texture_id,
            color_scale(tint, 1.12f));
    }
}

static void draw_world_energy_ribbon(float route_z, float plane_z,
                                     float x0, float y0, float x1, float y1,
                                     float half_width,
                                     uint32_t start_color,
                                     uint32_t end_color) {
    const float center_x = path_center(route_z);
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length_sq = dx * dx + dy * dy;
    float nx;
    float ny;

    if(length_sq < 0.0001f)
        return;
    nx = -dy * frsqrt(length_sq) * half_width;
    ny = dx * frsqrt(length_sq) * half_width;
    draw_world_quad(&additive_header,
        (vec3_t){center_x + x0 + nx, y0 + ny, plane_z},
        (vec3_t){center_x + x0 - nx, y0 - ny, plane_z},
        (vec3_t){center_x + x1 + nx, y1 + ny, plane_z},
        (vec3_t){center_x + x1 - nx, y1 - ny, plane_z},
        start_color,start_color,end_color,end_color);
}

static void draw_material_arch_energy(float world_z, float half_span,
                                      float base_y, float height,
                                      float thickness, color3_t primary,
                                      color3_t accent, float distance_fade) {
    const float knee_y = base_y + height * 0.58f;
    const float shoulder_y = base_y + height * 0.84f;
    const float crown_y = base_y + height;
    const float front_z = world_z - thickness * 0.92f;
    const float pulse = 0.72f + 0.28f *
        fsin(game.time * 5.6f + world_z * 0.021f);
    const uint32_t dim = pack_color(distance_fade * 0.10f, accent);
    const uint32_t hot = pack_color(distance_fade * pulse * 0.74f, primary);
    const float diamond_w = thickness * 1.00f;
    const float diamond_h = thickness * 1.32f;
    const float center_x = path_center(world_z);
    int side;

    for(side = -1; side <= 1; side += 2) {
        const float s = (float)side;
        draw_world_energy_ribbon(world_z, front_z,
            s * half_span * 0.98f, knee_y,
            s * half_span * 0.70f, shoulder_y,
            0.42f, dim, hot);
        draw_world_energy_ribbon(world_z, front_z,
            s * half_span * 0.70f, shoulder_y,
            s * half_span * 0.10f, crown_y,
            0.56f, hot, pack_color(distance_fade * 0.96f, accent));
    }

    /* A floating diamond replaces the old flat lintel as the visual keystone. */
    draw_world_quad(&additive_header,
        (vec3_t){center_x, crown_y + diamond_h, front_z},
        (vec3_t){center_x - diamond_w, crown_y, front_z},
        (vec3_t){center_x + diamond_w, crown_y, front_z},
        (vec3_t){center_x, crown_y - diamond_h, front_z},
        pack_color(distance_fade * 0.94f, primary), dim, dim,
        pack_color(distance_fade * 0.78f, accent));
}

static float point_segment_distance_sq(float px, float py,
                                       float x0, float y0,
                                       float x1, float y1) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length_sq = dx * dx + dy * dy;
    const float t = length_sq > 0.0001f ?
        clampf(((px - x0) * dx + (py - y0) * dy) / length_sq,
               0.0f, 1.0f) : 0.0f;
    const float nearest_x = x0 + dx * t;
    const float nearest_y = y0 + dy * t;
    const float ox = px - nearest_x;
    const float oy = py - nearest_y;
    return ox * ox + oy * oy;
}

static bool player_hits_material_arch(float player_x, float player_y,
                                      float half_span, float base_y,
                                      float height, float thickness) {
    const float knee_y = base_y + height * 0.58f;
    const float shoulder_y = base_y + height * 0.84f;
    const float crown_y = base_y + height;
    const float hit_radius = thickness + 4.0f;
    const float hit_radius_sq = hit_radius * hit_radius;
    int side;

    for(side = -1; side <= 1; side += 2) {
        const float s = (float)side;
        if(point_segment_distance_sq(player_x, player_y,
            s * half_span, base_y,
            s * half_span * 0.98f, knee_y) < hit_radius_sq ||
           point_segment_distance_sq(player_x, player_y,
            s * half_span * 0.98f, knee_y,
            s * half_span * 0.70f, shoulder_y) < hit_radius_sq ||
           point_segment_distance_sq(player_x, player_y,
            s * half_span * 0.70f, shoulder_y,
            s * half_span * 0.10f, crown_y) < hit_radius_sq)
            return true;
    }
    return false;
}

static bool signature_setpiece_reserved(int biome, float world_z) {
    const int rib_count = biome == 0 ? 4 : 3;
    const float spacing = biome == 0 ? 22.0f :
                          (biome == 1 ? 28.0f : 32.0f);
    int rib;

    if(biome == 2) {
        /* The Prism Crucible's outer crystals sit 16 units down-course and
           its floating keystone extends another 49 units. */
        return world_z_reserved_by_traversal(
                   world_z, COURSE_TRAVERSAL_SCENERY_CLEARANCE) ||
               world_z_reserved_by_traversal(
                   world_z + 16.0f, COURSE_TRAVERSAL_SCENERY_CLEARANCE) ||
               world_z_reserved_by_traversal(
                   world_z + 49.0f, COURSE_TRAVERSAL_SCENERY_CLEARANCE);
    }
    for(rib = 0; rib < rib_count; ++rib) {
        if(world_z_reserved_by_traversal(
               world_z + (float)rib * spacing,
               COURSE_TRAVERSAL_SCENERY_CLEARANCE))
            return true;
    }
    return false;
}

static void prism_crystal_layout(int index, float segment_z,
                                 float *local_x, float *world_z,
                                 float *base_y, float *width,
                                 float *height) {
    const int distance = abs(index);
    *local_x = (float)index * 23.0f;
    *world_z = segment_z + (float)distance * 8.0f;
    *base_y = terrain_height_local(*local_x, segment_z);
    *width = 6.0f + (float)(index & 1) * 2.0f;
    *height = 45.0f + (float)(2 - distance) * 10.0f;
}

static bool player_hits_prism_crucible(float segment_z,
                                       vec3_t previous_player_world,
                                       vec3_t current_player_world) {
    int crystal;

    for(crystal = -2; crystal <= 2; ++crystal) {
        float crystal_x;
        float crystal_z;
        float base_y;
        float width;
        float height;
        float local_x;
        float local_y;
        float height_t;
        float tapered_radius;
        vec3_t crossing;

        prism_crystal_layout(crystal, segment_z,
                             &crystal_x, &crystal_z, &base_y,
                             &width, &height);
        crossing = segment_point_at_z(previous_player_world,
                                      current_player_world, crystal_z);
        if(fabsf(crossing.z - crystal_z) > width + 4.0f)
            continue;
        local_x = crossing.x - path_center(crystal_z) - crystal_x;
        local_y = crossing.y - path_elevation(crystal_z);
        if(local_y < base_y - 4.0f ||
           local_y > base_y + height + 4.0f)
            continue;
        height_t = clampf((local_y - base_y) / height, 0.0f, 1.0f);
        tapered_radius = width * (1.0f - height_t) + 4.0f;
        if(fabsf(local_x) < tapered_radius)
            return true;
    }

    {
        const float rock_z = segment_z + 49.0f;
        const vec3_t crossing = segment_point_at_z(
            previous_player_world, current_player_world, rock_z);
        const float dx = crossing.x - path_center(rock_z);
        const float dy = crossing.y - path_elevation(rock_z) - 82.0f;
        const float dz = crossing.z - rock_z;
        const float vertical_extent = dy >= 0.0f ? 14.4f : 21.6f;
        if(dy >= -25.6f && dy <= 18.4f) {
            const float height_t = clampf(
                fabsf(dy) / vertical_extent, 0.0f, 1.0f);
            const float tapered_radius =
                18.0f * (1.0f - height_t) + 4.0f;
            if(dx * dx + dz * dz < tapered_radius * tapered_radius)
                return true;
        }
    }
    return false;
}

static bool player_hits_signature_arch(int biome, int segment,
                                       float segment_z,
                                       vec3_t previous_player_world,
                                       vec3_t current_player_world) {
    int rib_count;
    float spacing;
    int rib;

    if(!is_signature_segment(segment))
        return false;
    if(signature_setpiece_reserved(biome, segment_z))
        return false;
    if(biome == 2)
        return player_hits_prism_crucible(
            segment_z, previous_player_world, current_player_world);
    rib_count = biome == 0 ? 4 : 3;
    spacing = biome == 0 ? 22.0f : (biome == 1 ? 28.0f : 32.0f);

    for(rib = 0; rib < rib_count; ++rib) {
        const float rib_z = segment_z + (float)rib * spacing;
        const vec3_t collision_point = segment_point_at_z(
            previous_player_world, current_player_world, rib_z);
        const float player_x = collision_point.x - path_center(rib_z);
        const float player_y = collision_point.y - path_elevation(rib_z);
        const float half_span = biome == 0 ? 43.0f - (float)rib * 2.0f :
                                (biome == 1 ?
                                 36.0f - (float)rib * 3.0f : 40.0f);
        const float base_y = biome == 3 ? -1.0f :
                             (biome == 0 ? 1.0f : 0.0f);
        const float height = biome == 0 ? 57.0f :
                             (biome == 1 ? 60.0f : 49.0f);
        const float thickness = biome == 0 ? 2.8f :
                                (biome == 1 ? 4.5f : 3.8f);

        if(fabsf(collision_point.z - rib_z) <= 18.0f &&
           player_hits_material_arch(player_x, player_y,
                                     half_span, base_y,
                                     height, thickness))
            return true;
    }
    return false;
}

static void draw_crystal(float local_x, float base_y, float world_z,
                         float width, float height, color3_t tint) {
    const float x = path_center(world_z) + local_x;
    const vec3_t tip = {x, base_y + height, world_z};
    const vec3_t a = {x - width, base_y, world_z - width};
    const vec3_t b = {x + width, base_y, world_z - width};
    const vec3_t c = {x - width, base_y, world_z + width};
    const vec3_t d = {x + width, base_y, world_z + width};
    const uint32_t bright = pack_color(1.0f, color_scale(tint, 1.18f));
    const uint32_t dim = pack_color(1.0f, color_scale(tint, 0.48f));
    const pvr_poly_hdr_t *header =
        &texture_headers[GRAVITY_WAVE_TEX_CANOPY_ENERGY];

    draw_world_textured_triangle(header, a,b,tip,
        0.0f,1.0f, 1.0f,1.0f, 0.5f,0.0f, dim,dim,bright);
    draw_world_textured_triangle(header, b,d,tip,
        0.0f,1.0f, 1.0f,1.0f, 0.5f,0.0f, dim,bright,bright);
    draw_world_textured_triangle(header, d,c,tip,
        0.0f,1.0f, 1.0f,1.0f, 0.5f,0.0f, bright,dim,bright);
    draw_world_textured_triangle(header, c,a,tip,
        0.0f,1.0f, 1.0f,1.0f, 0.5f,0.0f, dim,dim,bright);
}

static void draw_floating_rock(float local_x, float world_z,
                               float y, float radius, color3_t tint) {
    const float x = path_center(world_z) + local_x;
    const vec3_t top = {x, y + radius * 0.8f, world_z};
    const vec3_t bottom = {x, y - radius * 1.2f, world_z};
    const vec3_t a = {x - radius, y, world_z};
    const vec3_t b = {x, y, world_z - radius};
    const vec3_t c = {x + radius, y, world_z};
    const vec3_t d = {x, y, world_z + radius};
    const uint32_t light = pack_color(1.0f, color_scale(tint, 0.92f));
    const uint32_t dark = pack_color(1.0f, color_scale(tint, 0.42f));
    const pvr_poly_hdr_t *header =
        &texture_headers[GRAVITY_WAVE_TEX_TERRAIN_VIOLET];

    draw_world_textured_triangle(header, a,b,top, 0,1,1,1,.5f,0,
                                 dark,light,light);
    draw_world_textured_triangle(header, b,c,top, 0,1,1,1,.5f,0,
                                 light,dark,light);
    draw_world_textured_triangle(header, c,d,top, 0,1,1,1,.5f,0,
                                 dark,light,light);
    draw_world_textured_triangle(header, d,a,top, 0,1,1,1,.5f,0,
                                 light,dark,light);
    draw_world_textured_triangle(header, b,a,bottom, 0,0,1,0,.5f,1,
                                 light,dark,dark);
    draw_world_textured_triangle(header, c,b,bottom, 0,0,1,0,.5f,1,
                                 dark,light,dark);
    draw_world_textured_triangle(header, d,c,bottom, 0,0,1,0,.5f,1,
                                 light,dark,dark);
    draw_world_textured_triangle(header, a,d,bottom, 0,0,1,0,.5f,1,
                                 dark,light,dark);
}

static void draw_signature_setpiece(int biome, int segment, float world_z,
                                    const palette_t *palette) {
    int i;
    if(!is_signature_segment(segment))
        return;
    if(signature_setpiece_reserved(biome, world_z))
        return;

    if(biome == 0) { /* Graveyard Run carrier ribs. */
        for(i = 0; i < 4; ++i)
            draw_material_arch(world_z + (float)i * 22.0f,
                               43.0f - (float)i * 2.0f,
                               1.0f, 57.0f, 2.8f,
                               GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                               color_lerp((color3_t){0.62f,0.70f,0.75f},
                                          palette->river, 0.18f));
        draw_material_box(-57.0f, 8.0f, world_z + 34.0f,
                          17.0f, 7.0f, 47.0f,
                          GRAVITY_WAVE_TEX_HULL_ALLIED,
                          (color3_t){0.62f,0.70f,0.76f});
    }
    else if(biome == 1) { /* Root Cathedral and mossy temple lintels. */
        for(i = 0; i < 3; ++i)
            draw_material_arch(world_z + (float)i * 28.0f,
                               36.0f - (float)i * 3.0f,
                               0.0f, 60.0f, 4.5f,
                               i == 1 ? GRAVITY_WAVE_TEX_ANCIENT_MACHINE :
                                        GRAVITY_WAVE_TEX_TERRAIN_EMERALD,
                               (color3_t){0.67f,0.82f,0.57f});
    }
    else if(biome == 2) { /* Prism Crucible. */
        for(i = -2; i <= 2; ++i) {
            float crystal_x;
            float crystal_z;
            float base_y;
            float width;
            float height;
            prism_crystal_layout(i, world_z,
                                 &crystal_x, &crystal_z, &base_y,
                                 &width, &height);
            draw_crystal(crystal_x, base_y, crystal_z,
                         width, height, palette->accent);
        }
        draw_floating_rock(0.0f, world_z + 49.0f, 82.0f, 18.0f,
                           palette->ground_high);
    }
    else { /* Furnace Trench gantry. */
        for(i = 0; i < 3; ++i)
            draw_material_arch(world_z + (float)i * 32.0f,
                               40.0f, -1.0f, 49.0f, 3.8f,
                               GRAVITY_WAVE_TEX_HULL_HOSTILE,
                               (color3_t){0.82f,0.58f,0.42f});
        draw_material_box(62.0f, 0.0f, world_z + 34.0f,
                          13.0f, 66.0f, 15.0f,
                          GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                          (color3_t){0.88f,0.60f,0.34f});
    }
}

static void draw_biome_scenery_opaque(const palette_t *palette) {
    const int first_segment = (int)(game.distance / SCENERY_SEGMENT) + 1;
    int i;

#ifdef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
    (void)palette;
    return;
#endif
    draw_procedural_spires(palette);
    {
        /* Signature ribs extend as far as 66 units beyond their base segment.
           Retain the current base after the ordinary scenery cursor advances,
           so every collidable late rib remains visible until it is behind. */
        const int retained_segment = first_segment - 1;
        const float retained_z = (float)retained_segment * SCENERY_SEGMENT;
        const int retained_biome =
            ((int)(retained_z / BIOME_LENGTH)) & 3;
    if(!world_z_reserved_by_traversal(
           retained_z, COURSE_TRAVERSAL_SCENERY_CLEARANCE))
            draw_signature_setpiece(retained_biome, retained_segment,
                                    retained_z, palette);
    }
    for(i = 0; i < 16; ++i) {
        const int segment = first_segment + i;
        const float z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(z / BIOME_LENGTH)) & 3;
        const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                    (uint32_t)biome * 0x51ed270bu);
        const int variant = scenery_variant_for_segment(biome, segment, z);
        const float side = scenery_side_for_segment(biome, segment, z);
        const float x = side * (70.0f + (float)((h >> 8) & 63u));
        const float base = terrain_height_local(x, z);

        /* The hero landmark owns its block. Do not stack a routine arch or
           tower through the silhouette the player is meant to recognize. */
        if(is_signature_segment(segment)) {
            draw_signature_setpiece(biome, segment, z, palette);
            continue;
        }
        if(world_z_reserved_by_traversal(
               z, COURSE_TRAVERSAL_SCENERY_CLEARANCE) &&
           scenery_variant_spans_lane(biome, variant))
            continue;

        if(biome == 0) {
            if(variant == 0)
                draw_spire(x, z, 9.0f, 48.0f, palette);
            else if(variant == 1)
                draw_material_arch(z, 46.0f, 1.0f, 48.0f, 5.5f,
                                   GRAVITY_WAVE_TEX_TERRAIN_AZURE,
                                   (color3_t){0.77f,0.89f,0.94f});
            else if(variant == 2) {
                draw_material_box(x, base, z, 4.5f, 43.0f, 5.0f,
                                  GRAVITY_WAVE_TEX_HULL_ALLIED,
                                  (color3_t){0.72f,0.84f,0.92f});
                draw_material_box(x, base + 43.0f, z, 8.0f, 4.0f, 8.0f,
                                  GRAVITY_WAVE_TEX_CANOPY_ENERGY,
                                  (color3_t){0.72f,0.96f,1.0f});
            }
            else if(variant == 3)
                draw_material_arch(z, 39.0f, 3.0f, 52.0f, 2.5f,
                                   GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                                   (color3_t){0.65f,0.74f,0.78f});
            else {
                draw_spire(x, z, 15.0f, 62.0f, palette);
                draw_material_box(x - side * 15.0f, base + 16.0f, z,
                                  17.0f, 4.0f, 14.0f,
                                  GRAVITY_WAVE_TEX_HULL_ALLIED,
                                  (color3_t){0.54f,0.67f,0.74f});
            }
        }
        else if(biome == 1) {
            if(variant <= 1) {
                draw_material_box(x, base, z, 7.5f, 61.0f, 7.5f,
                                  GRAVITY_WAVE_TEX_TERRAIN_EMERALD,
                                  (color3_t){0.58f,0.74f,0.46f});
                draw_material_box(x, base + 27.0f, z, 13.0f, 7.0f, 12.0f,
                                  GRAVITY_WAVE_TEX_TERRAIN_EMERALD,
                                  (color3_t){0.69f,0.84f,0.52f});
            }
            else if(variant == 2)
                draw_material_arch(z, 38.0f, 0.0f, 54.0f, 5.0f,
                                   GRAVITY_WAVE_TEX_TERRAIN_EMERALD,
                                   (color3_t){0.63f,0.80f,0.51f});
            else if(variant == 3) {
                draw_material_box(x, base, z, 7.0f, 39.0f, 7.0f,
                                  GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                                  (color3_t){0.76f,0.85f,0.65f});
                draw_material_box(x, base + 39.0f, z, 11.0f, 5.0f, 10.0f,
                                  GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                                  (color3_t){0.84f,0.90f,0.68f});
            }
            else {
                int step;
                for(step = 0; step < 3; ++step)
                    draw_material_box(x, base + (float)step * 5.0f,
                                      z, 18.0f - (float)step * 3.0f,
                                      5.0f, 15.0f - (float)step * 2.0f,
                                      GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                                      (color3_t){0.69f,0.82f,0.58f});
            }
        }
        else if(biome == 2) {
            if(variant <= 1) {
                draw_crystal(x, base, z, 7.0f + (float)variant * 2.0f,
                             49.0f + (float)((h >> 19) & 15u),
                             palette->accent);
                draw_crystal(x - side * 14.0f, base, z + 7.0f,
                             4.0f, 29.0f, palette->river);
            }
            else if(variant == 2)
                draw_floating_rock(x, z, 67.0f + (float)((h >> 17) & 31u),
                                   11.0f + (float)((h >> 22) & 7u),
                                   palette->ground_high);
            else if(variant == 3) {
                draw_material_box(x, base, z, 5.0f, 57.0f, 5.0f,
                                  GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                                  (color3_t){0.76f,0.58f,0.92f});
                draw_crystal(x, base + 57.0f, z, 8.0f, 21.0f,
                             palette->accent);
            }
            else
                draw_material_arch(z, 42.0f, 3.0f, 59.0f, 3.2f,
                                   GRAVITY_WAVE_TEX_CANOPY_ENERGY,
                                   (color3_t){0.69f,0.66f,1.0f});
        }
        else {
            if(variant == 0)
                draw_material_box(x, base, z, 10.0f, 48.0f, 10.0f,
                                  GRAVITY_WAVE_TEX_TERRAIN_EMBER,
                                  (color3_t){0.80f,0.56f,0.40f});
            else if(variant == 1)
                draw_furnace_vent(x, base, z, variant);
            else if(variant == 2)
                draw_material_arch(z, 40.0f, -1.0f, 45.0f, 3.5f,
                                   GRAVITY_WAVE_TEX_HULL_HOSTILE,
                                   (color3_t){0.83f,0.54f,0.39f});
            else if(variant == 3)
                draw_furnace_vent(x, base, z, variant);
            else
                draw_material_arch(z, 49.0f, -1.0f, 57.0f, 4.2f,
                                   GRAVITY_WAVE_TEX_HULL_HOSTILE,
                                   (color3_t){0.88f,0.55f,0.34f});
        }
        draw_signature_setpiece(biome, segment, z, palette);
    }
}

static void draw_biome_scenery_translucent(const palette_t *palette) {
    const int first_segment = (int)(game.distance / SCENERY_SEGMENT) + 1;
    int i;

#ifdef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
    (void)palette;
    return;
#endif
    /* Vector Crown trims share one additive header before the textured mist,
       foliage, rift, and flame layers below. */
    for(i = 0; i < 16; ++i) {
        const int segment = first_segment + i;
        const float z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(z / BIOME_LENGTH)) & 3;
        if(is_signature_segment(segment))
            continue;
        const int variant = scenery_variant_for_segment(biome, segment, z);
        const float relative_z = z - game.distance;
        const float far_fade = 1.0f -
            smoothstepf((relative_z - 930.0f) / 360.0f);
        const float near_focus = 0.48f + 0.52f *
            smoothstepf((relative_z - 40.0f) / 120.0f);
        const float distance_fade = far_fade * near_focus * 0.82f;
        float half_span = 0.0f;
        float base_y = 0.0f;
        float height = 0.0f;
        float thickness = 0.0f;
        color3_t primary = palette->river;
        color3_t accent = palette->accent;

        if(biome == 0 && variant == 1) {
            half_span = 46.0f; base_y = 1.0f;
            height = 48.0f; thickness = 5.5f;
        }
        else if(biome == 0 && variant == 3) {
            half_span = 39.0f; base_y = 3.0f;
            height = 52.0f; thickness = 2.5f;
        }
        else if(biome == 1 && variant == 2) {
            half_span = 38.0f; base_y = 0.0f;
            height = 54.0f; thickness = 5.0f;
            primary = color_lerp(palette->river,
                                 (color3_t){0.68f,1.0f,0.46f}, 0.46f);
        }
        else if(biome == 2 && variant == 4) {
            half_span = 42.0f; base_y = 3.0f;
            height = 59.0f; thickness = 3.2f;
            accent = (color3_t){0.98f,0.32f,1.0f};
        }
        else if(biome == 3 && (variant == 2 || variant == 4)) {
            half_span = variant == 2 ? 40.0f : 49.0f;
            base_y = -1.0f;
            height = variant == 2 ? 45.0f : 57.0f;
            thickness = variant == 2 ? 3.5f : 4.2f;
            primary = (color3_t){1.0f,0.50f,0.16f};
            accent = color_lerp(palette->accent,
                                (color3_t){1.0f,0.16f,0.34f}, 0.52f);
        }

        if(half_span > 0.0f && distance_fade > 0.02f &&
           !world_z_reserved_by_traversal(
               z, COURSE_TRAVERSAL_SCENERY_CLEARANCE))
            draw_material_arch_energy(z, half_span, base_y, height,
                                      thickness, primary, accent,
                                      distance_fade);
    }

    for(i = 0; i < 16; ++i) {
        const int segment = first_segment + i;
        const float z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(z / BIOME_LENGTH)) & 3;
        if(is_signature_segment(segment))
            continue;
        const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                    (uint32_t)biome * 0x51ed270bu);
        const int variant = scenery_variant_for_segment(biome, segment, z);
        const float side = scenery_side_for_segment(biome, segment, z);
        const float x = side * (70.0f + (float)((h >> 8) & 63u));
        const float base = terrain_height_local(x, z);
        const float distance_fade = 1.0f -
            smoothstepf((z - game.distance - 950.0f) / 430.0f);

        if(distance_fade <= 0.02f)
            continue;
        if(biome == 0 && variant == 1) {
            draw_textured_billboard(
                &texture_headers[GRAVITY_WAVE_TEX_WATERFALL_MIST],
                (vec3_t){path_center(z) + x - side * 8.0f,
                         base + 31.0f, z - 2.0f},
                32.0f, 69.0f,
                pack_color(0.82f * distance_fade,
                           (color3_t){0.72f,0.91f,1.0f}));
        }
        else if(biome == 1) {
            draw_textured_billboard(
                &texture_headers[GRAVITY_WAVE_TEX_FOLIAGE],
                (vec3_t){path_center(z) + x, base + 54.0f, z},
                42.0f, 37.0f,
                pack_color(0.94f * distance_fade,
                           (color3_t){0.82f,1.0f,0.72f}));
            if((h & 0x40u) != 0u)
                draw_textured_billboard(
                    &texture_headers[GRAVITY_WAVE_TEX_FOLIAGE],
                    (vec3_t){path_center(z) + x - side * 17.0f,
                             base + 36.0f, z + 4.0f},
                    30.0f, 28.0f,
                    pack_color(0.82f * distance_fade,
                               (color3_t){0.63f,0.91f,0.52f}));
        }
        else if(biome == 2) {
            const float float_y = 35.0f + (float)((h >> 16) & 31u) +
                                  fsin(game.time * 1.8f + (float)segment) * 4.0f;
            draw_textured_billboard(
                &sprite_additive_headers[2],
                (vec3_t){path_center(z) + x * 0.72f, float_y, z},
                24.0f, 31.0f,
                pack_color(0.68f * distance_fade,
                           color_lerp((color3_t){0.72f,0.68f,1.0f},
                                      palette->accent, 0.22f)));
        }
        else if(variant == 1 || variant == 3) {
            const float vent_y = base + (variant == 1 ? 67.5f : 54.5f);
            const float outlet_offset = variant == 1 ? 6.5f : 7.5f;
            const float pulse = 0.92f + 0.08f *
                fsin(game.time * 7.0f + (float)segment * 0.71f);
            int outlet;

            for(outlet = -1; outlet <= 1; outlet += 2) {
                const float outlet_x = path_center(z) + x +
                                       (float)outlet * outlet_offset;
                screen_point_t vent;
                draw_textured_billboard(
                    &texture_headers[GRAVITY_WAVE_TEX_FIRE_SMOKE],
                    (vec3_t){outlet_x, vent_y + 13.0f, z},
                    15.0f, 29.0f,
                    pack_color(0.82f * distance_fade,
                               (color3_t){1.0f,0.78f,0.62f}));
                if(project_world((vec3_t){outlet_x, vent_y + 0.7f, z},
                                 &vent)) {
                    const float glow_size = clampf(
                        5.2f * game.camera_focal * vent.z, 1.2f, 15.0f);
                    draw_disc(&additive_header, vent.x, vent.y,
                              glow_size * pulse, vent.z + 0.00002f, 8,
                              pack_color(0.66f * distance_fade,
                                         (color3_t){1.0f,0.52f,0.12f}),
                              pack_color(0.0f,
                                         (color3_t){1.0f,0.12f,0.02f}));
                }
            }
        }
    }
}

static void set_message(const char *text, float seconds) {
    snprintf(game.message, sizeof(game.message), "%s", text);
    game.message_time = seconds;
}

static bool set_ambient_message(const char *text, float seconds) {
    if(game.message_time > 0.05f)
        return false;
    set_message(text, seconds);
    return true;
}

static particle_t *alloc_particle(void) {
    int i;
    for(i = 0; i < MAX_PARTICLES; ++i) {
        if(!particles[i].active)
            return &particles[i];
    }
    return NULL;
}

static enemy_t *alloc_enemy(void) {
    int i;
    for(i = 0; i < MAX_ENEMIES; ++i) {
        if(!enemies[i].active)
            return &enemies[i];
    }
    return NULL;
}

static projectile_t *alloc_projectile(projectile_t *pool, int count) {
    int i;
    for(i = 0; i < count; ++i) {
        if(!pool[i].active)
            return &pool[i];
    }
    return NULL;
}

static gate_t *alloc_gate(void) {
    int i;
    for(i = 0; i < MAX_GATES; ++i) {
        if(!gates[i].active)
            return &gates[i];
    }
    return NULL;
}

static pickup_t *alloc_pickup(void) {
    int i;
    for(i = 0; i < MAX_PICKUPS; ++i) {
        if(!pickups[i].active)
            return &pickups[i];
    }
    return NULL;
}

static void spawn_pickup(float x, float y, float z, pickup_kind_t kind) {
    pickup_t *pickup = alloc_pickup();
    if(!pickup)
        return;
    pickup->active = true;
    pickup->x = x;
    pickup->y = y;
    pickup->z = z;
    pickup->spin = random_unit() * TAU;
    pickup->life = 12.0f;
    pickup->kind = kind;
}

static void spawn_particle(float x, float y, float z,
                           float vx, float vy, float vz,
                           float life, float size, color3_t color,
                           particle_kind_t kind) {
    particle_t *particle = alloc_particle();
    if(!particle)
        return;
    *particle = (particle_t){
        true, x, y, z, vx, vy, vz, life, life, size, color, kind
    };
}

static void spawn_explosion(float x, float y, float z, color3_t color,
                            int count) {
    const vec3_t origin = route_position(x, y, z);
    int i;
    for(i = 0; i < count; ++i) {
        const float angle = random_unit() * TAU;
        const float lift = random_signed() * 0.75f;
        const float speed = 14.0f + random_unit() * 43.0f;
        color3_t spark = (i & 2) ? color : (color3_t){1.0f, 0.55f, 0.12f};
        spawn_particle(origin.x, origin.y, origin.z,
                       fcos(angle) * speed,
                       lift * speed,
                       fsin(angle) * speed,
                       0.42f + random_unit() * 0.72f,
                       1.4f + random_unit() * 3.5f,
                       spark, (i & 1) ? PARTICLE_STREAK : PARTICLE_SPRITE);
    }
}

static void spawn_boss_impact_world(vec3_t impact,
                                    color3_t color, int damage) {
    const int spark_count = 7 + (damage > 1 ? 3 : 0);
    int i;

    spawn_particle(impact.x, impact.y, impact.z - 1.5f,
                   0.0f, 0.0f, 0.0f,
                   0.34f, 15.0f + (float)damage * 2.0f,
                   color, PARTICLE_BOSS_IMPACT);
    for(i = 0; i < spark_count; ++i) {
        const float angle = random_unit() * TAU;
        const float speed = 20.0f + random_unit() * 38.0f;
        spawn_particle(impact.x, impact.y, impact.z - 2.0f,
                       fcos(angle) * speed,
                       fsin(angle) * speed,
                       -10.0f - random_unit() * 22.0f,
                       0.20f + random_unit() * 0.24f,
                       1.1f + random_unit() * 2.2f,
                       (i % 3) == 0 ? (color3_t){1.0f,0.98f,0.78f} : color,
                       PARTICLE_STREAK);
    }
}

static void add_score(int base) {
    const int multiplier = 1 + (game.combo / 5 > 7 ? 7 : game.combo / 5);
    game.score += base * multiplier;
    game.combo++;
    game.combo_timer = 2.35f;
}

static pickup_kind_t choose_random_pickup(void) {
    float roll = random_unit();

    /* Emergency repairs keep their own slice, then the remaining outcomes are
       normalized so drop eligibility cannot bias the pickup kind. */
    if(game.shield < 42.0f) {
        if(roll < 0.34f)
            return PICKUP_REPAIR;
        roll = (roll - 0.34f) * (1.0f / 0.66f);
    }
    if(roll < 0.22f)
        return PICKUP_SPEED_BOOST;
    if(roll < 0.44f)
        return PICKUP_FAST_LASER;
    if(roll < 0.66f)
        return PICKUP_PHASE_WAVE;
    if(roll < 0.84f && game.weapon_level < 3)
        return PICKUP_LASER_CORE;
    return game.shield < 76.0f ? PICKUP_REPAIR : PICKUP_NOVA;
}

static void destroy_enemy(enemy_t *enemy, bool award_score) {
    const palette_t *palette = enemy->type == 3 ?
        &palettes[enemy->biome & 3] : current_palette();
    const int points = enemy->type == 3 ? 4200 :
                       (enemy->type == 2 ? 500 :
                        (enemy->type == 1 ? 220 : 100));
    const float drop_roll = random_unit();
    const float drop_chance = enemy->type == 2 ? 0.42f :
                              (enemy->type == 1 ? 0.14f :
                               (game.shield < 35.0f ? 0.20f : 0.09f));
    int i;
    if(!enemy->active)
        return;
    spawn_explosion(enemy->x, enemy->y, enemy->z,
                    palette->enemy, enemy->type == 3 ? 72 :
                                    (enemy->type == 2 ? 28 : 17));
    if(award_score)
        add_score(points);
    if(award_score && enemy->type == 3) {
        game.guardians_destroyed++;
        game.trauma = 1.0f;
        for(i = 0; i < MAX_ENEMY_SHOTS; ++i)
            enemy_shots[i].active = false;
        spawn_pickup(enemy->x - 12.0f, enemy->y, enemy->z,
                     game.weapon_level < 3 ? PICKUP_LASER_CORE : PICKUP_NOVA);
        spawn_pickup(enemy->x, enemy->y + 7.0f, enemy->z + 5.0f,
                     random_unit() < 0.5f ? PICKUP_FAST_LASER :
                                            PICKUP_PHASE_WAVE);
        spawn_pickup(enemy->x + 12.0f, enemy->y, enemy->z + 10.0f,
                     game.shield < 68.0f ? PICKUP_REPAIR :
                                           PICKUP_SPEED_BOOST);
        set_message("GUARDIAN DESTROYED", 2.4f);
        /* Resolve the chapter cleanly. No queued formation or traversal may
           materialize on top of the reward field while the player collects
           the guardian's three drops. */
        {
            const float wave_recovery_z = game.distance +
                COURSE_GUARDIAN_WAVE_RECOVERY;
            const float gate_recovery_z = game.distance +
                COURSE_GUARDIAN_GATE_RECOVERY;
            const float old_wave_z = game.next_wave_z;
            const float old_gate_z = game.next_gate_z;

            game.next_wave_z = recover_course_wave_z(
                game.next_wave_z, wave_recovery_z);
            if(game.next_gate_z < gate_recovery_z) {
                game.next_gate_z = next_course_traversal_z(gate_recovery_z);
                game.suppressed_gate_from_z = old_gate_z;
                game.suppressed_gate_until_z = game.next_gate_z;
            }
            printf("Gravity Wave course: recovery wave %.0f->%.0f "
                   "traversal %.0f->%.0f.\n",
                   (double)old_wave_z, (double)game.next_wave_z,
                   (double)old_gate_z, (double)game.next_gate_z);
        }
        play_rumble(7);
    }
    else if(award_score && drop_roll < drop_chance)
        spawn_pickup(enemy->x, enemy->y, enemy->z,
                     choose_random_pickup());
    game.trauma = fmaxf(game.trauma, enemy->type == 3 ? 1.0f :
                                     (enemy->type == 2 ? 0.35f : 0.16f));
    enemy->active = false;
    play_sound(sfx_explosion, enemy->type >= 2 ? 220 : 165,
               (int)clampf(128.0f + enemy->x, 12.0f, 244.0f));
}

static void damage_player(float amount) {
    int i;
#if defined(GRAVITY_WAVE_AUTOTEST) && \
    !defined(GRAVITY_WAVE_AUTOTEST_DAMAGE)
    (void)amount;
    return;
#endif
    if(game.hit_cooldown > 0.0f || game.mode != MODE_PLAYING)
        return;

    const vec3_t player_world = route_position(
        game.player_x, game.player_y, game.distance + PLAYER_Z);
    game.shield -= amount;
    game.hit_cooldown = 0.82f;
    game.trauma = fmaxf(game.trauma, 0.72f);
    game.combo = 0;
    game.combo_timer = 0.0f;
    play_sound(sfx_hit, 210, 128);
    play_rumble(6);
    for(i = 0; i < 13; ++i) {
        spawn_particle(player_world.x, player_world.y, player_world.z,
                       random_signed() * 28.0f,
                       random_signed() * 28.0f,
                       random_signed() * 22.0f,
                       0.25f + random_unit() * 0.35f,
                       1.5f + random_unit() * 2.0f,
                       (color3_t){0.32f, 0.82f, 1.0f}, PARTICLE_STREAK);
    }

    if(game.shield <= 0.0f) {
        game.shield = 0.0f;
        spawn_explosion(game.player_x, game.player_y,
                        game.distance + PLAYER_Z,
                        (color3_t){0.25f, 0.78f, 1.0f}, 48);
        game.mode = MODE_GAME_OVER;
        set_message("FLIGHT LOST", 10.0f);
        printf("Gravity Wave run ended: score=%d distance=%.0f time=%.1f\n",
               game.score, game.distance, game.survival_time);
        play_sound(sfx_explosion, 255, 128);
    }
}

static bool spawn_wave(float world_z) {
    const float rank = clampf(game.distance / 11500.0f, 0.0f, 1.0f);
    const int chapter = course_chapter_at(world_z);
    const int biome = chapter & 3;
    const int event = nearest_course_event(
        world_z, course_wave_offsets, COURSE_WAVES_PER_BIOME);
    const uint32_t seed = hash_u32((uint32_t)chapter * 0x9e3779b9u ^
                                   (uint32_t)event * 0x85ebca6bu ^
                                   0x15c4d2a7u);
    const int pattern = event < 2 ?
        (int)course_wave_patterns[biome][event] : 0;
    const int count = pattern == 0 ? 5 : (pattern == 3 ? 3 : 4);
    bool guardian_wave = event == COURSE_WAVES_PER_BIOME - 1;
    int spawned = 0;
    int i;

#ifdef GRAVITY_WAVE_AUTOTEST_BOSS
    guardian_wave = true;
#endif
    if(guardian_wave) {
        enemy_t *guardian = alloc_enemy();
        const guardian_profile_t *profile = &guardian_profiles[biome];
        if(!guardian) {
            printf("Gravity Wave course: guardian pool full; retrying.\n");
            return false;
        }
        memset(guardian, 0, sizeof(*guardian));
        guardian->active = true;
        guardian->type = 3;
        guardian->hp = profile->base_hp +
                       game.guardians_destroyed * profile->hp_growth;
#ifdef GRAVITY_WAVE_AUTOTEST_BOSS_HITS
        guardian->hp = 1200;
#endif
        guardian->max_hp = guardian->hp;
        guardian->biome = biome;
        guardian->radius = profile->radius;
        guardian->x = 0.0f;
        guardian->y = 57.0f;
        guardian->z = world_z + 160.0f;
        guardian->phase = (float)(seed & 255u) * 0.01f;
        guardian->fire_timer = 1.35f;
        /* The chapter handoff is a clean boss arena, not a stack of stale
           formation fire sitting on top of the guardian's first telegraph. */
        memset(enemy_shots, 0, sizeof(enemy_shots));
        snprintf(game.message, sizeof(game.message),
                 "WARNING  %s", profile->name);
        game.message_time = 2.8f;
        play_sound(sfx_gate, 240, 128);
        game.wave++;
        printf("Gravity Wave course: chapter %d guardian %s deployed "
               "(hp=%d cadence=%.2f).\n",
               chapter + 1, profile->name, guardian->hp,
               (double)profile->fire_cadence);
        return true;
    }

    for(i = 0; i < count; ++i) {
        enemy_t *enemy = alloc_enemy();
        const float centered = (float)i - (float)(count - 1) * 0.5f;
        if(!enemy)
            break;

        memset(enemy, 0, sizeof(*enemy));
        enemy->active = true;
        enemy->phase = (float)i * 0.82f + (float)(seed & 255u) * 0.01f;
        enemy->radius = 5.0f;
        enemy->hp = 1;
        enemy->max_hp = 1;
        enemy->biome = biome;
        enemy->fire_timer = 0.65f + (float)i * 0.18f;
        enemy->type = 0;
        spawned++;

        if(pattern == 0) {
            enemy->x = centered * 22.0f;
            enemy->y = 38.0f + fabsf(centered) * 6.0f;
            enemy->z = world_z + COURSE_FORMATION_LEAD +
                       fabsf(centered) * 21.0f;
            enemy->vx = centered * -1.1f;
        }
        else if(pattern == 1) {
            const float angle = (float)i * TAU / (float)count;
            enemy->x = fcos(angle) * 34.0f;
            enemy->y = 43.0f + fsin(angle) * 20.0f;
            enemy->z = world_z + COURSE_FORMATION_LEAD +
                       (float)i * 28.0f;
            enemy->vx = i & 1 ? 8.0f : -8.0f;
        }
        else if(pattern == 2) {
            enemy->x = (i & 1 ? 1.0f : -1.0f) *
                       (55.0f + (float)i * 5.0f);
            enemy->y = 62.0f - (float)i * 8.0f;
            enemy->z = world_z + COURSE_FORMATION_LEAD +
                       (float)i * 24.0f;
            enemy->vx = enemy->x > 0.0f ? -13.0f : 13.0f;
            if(i == count - 1 && rank > 0.18f) {
                enemy->type = 1;
                enemy->hp = 2;
                enemy->max_hp = 2;
                enemy->radius = 6.5f;
            }
        }
        else {
            enemy->x = centered * 31.0f;
            enemy->y = 42.0f + (i & 1) * 15.0f;
            enemy->z = world_z + COURSE_FORMATION_LEAD +
                       fabsf(centered) * 18.0f;
            enemy->vx = -centered * 3.5f;
            if(i == 1) {
                enemy->type = 2;
                enemy->hp = 5;
                enemy->max_hp = 5;
                enemy->radius = 10.0f;
                enemy->y = 50.0f;
                enemy->vx = 0.0f;
            }
        }
    }
    if(spawned == 0) {
        printf("Gravity Wave course: formation pool full; retrying.\n");
        return false;
    }
    game.wave++;
    printf("Gravity Wave course: chapter %d formation %d deployed (%d craft).\n",
           chapter + 1, pattern, spawned);
    return true;
}

static float traversal_stage_z(const gate_t *gate, int stage) {
    return gate->z + gate->stage_spacing * (float)stage;
}

static int traversal_stage_direction(const gate_t *gate, int stage) {
    return (stage & 1) ? -gate->direction : gate->direction;
}

static float traversal_stage_x(const gate_t *gate, int stage) {
    if(gate->kind == TRAVERSAL_SLALOM)
        /* This is the center of a clearly marked safe lane, not the position
           of an ambiguous pole the player must guess how to pass. */
        return (float)traversal_stage_direction(gate, stage) * 36.0f;
    return gate->x;
}

static float traversal_stage_y(const gate_t *gate, int stage) {
    if(gate->kind == TRAVERSAL_SHEAR_BARRIER)
        return gate->y -
               (float)traversal_stage_direction(gate, stage) * 3.0f;
    return gate->y;
}

static bool traversal_lane_contains(const gate_t *gate, int stage,
                                    float local_x) {
    return fabsf(local_x - traversal_stage_x(gate, stage)) < gate->radius;
}

static float traversal_lane_pylon_x(const gate_t *gate, int stage, int side) {
    return traversal_stage_x(gate, stage) + (float)side *
        (gate->radius + VECTOR_LANE_PYLON_HALF_WIDTH +
         VECTOR_LANE_PYLON_CLEARANCE);
}

static bool spawn_gate(float world_z) {
    gate_t *gate = alloc_gate();
    const int chapter = course_chapter_at(world_z);
    const int biome = chapter & 3;
    const int event = nearest_course_event(
        world_z, course_traversal_offsets, COURSE_TRAVERSALS_PER_BIOME);
    const traversal_kind_t kind = course_traversal_kind(chapter, event);
    const int turn_direction = course_turn_direction(world_z);
    const int grade_direction = course_grade_direction(world_z);
    const int traversal_direction = kind == TRAVERSAL_GRAVITY_BLOOM ?
        course_turn_direction(world_z + 620.0f) *
        (int)course_bloom_polarity[biome] :
        (kind == TRAVERSAL_SHEAR_BARRIER ?
         grade_direction * (biome == 3 ? -1 : 1) : turn_direction);
    const int stage_count = traversal_stage_count_for_kind(kind);
    const float stage_spacing = traversal_stage_spacing_for_kind(kind);
    const float resolved_z = world_z;
    const uint32_t h = hash_u32((uint32_t)chapter * 0x9e3779b9u ^
                                (uint32_t)event * 0x85ebca6bu ^
                                0xd38755a1u);
    float target_x;
    float target_y;
    if(!gate)
        return false;
    memset(gate, 0, sizeof(*gate));
    /* Blooms frame the exit vector of a bend; vector lanes weave through its
       entry. Ember's counter-shear deliberately asks for a dive against the
       climb. */
    target_x = (float)traversal_direction *
               (event == 0 ? 18.0f : 28.0f);
    target_y = clampf(terrain_height_local(target_x, resolved_z) + 42.0f +
                      (float)grade_direction * 5.0f, 34.0f, 76.0f);
    gate->active = true;
    gate->x = kind == TRAVERSAL_GRAVITY_BLOOM ? target_x : 0.0f;
    gate->y = kind == TRAVERSAL_GRAVITY_BLOOM ? target_y :
              (kind == TRAVERSAL_SLALOM ? 52.0f :
               50.0f + (float)((int)((h >> 9) & 7u) - 3));
    gate->z = resolved_z;
    gate->radius = kind == TRAVERSAL_GRAVITY_BLOOM ?
                   14.0f + (float)((h >> 14) & 3u) :
                   (kind == TRAVERSAL_SLALOM ? 19.0f : 9.0f);
    gate->spin = (float)(h & 255u) * (TAU / 255.0f);
    gate->result_time = 0.0f;
    gate->stage_spacing = stage_spacing;
    gate->variant = ((chapter & 3) + event) & 3;
    gate->direction = traversal_direction;
    gate->stage = 0;
    gate->stage_count = stage_count;
    gate->success_mask = 0u;
    gate->perfect_mask = 0u;
    gate->faulted = false;
    gate->kind = kind;
    gate->result = GATE_RESULT_PENDING;
    return true;
}

#ifdef GRAVITY_WAVE_AUTOTEST_COURSE_PROFILE
static bool run_course_profile_self_test(void) {
    gate_t lane;
    bool passed = true;
    bool prism_hit;
    bool prism_miss;
    bool prism_rock_hit;
    bool prism_rock_high_miss;
    bool prism_suppressed;
    float global_max_turn = 0.0f;
    float global_max_grade = 0.0f;
    int biome;

    for(biome = 0; biome < 4; ++biome) {
        const float chapter_z = (float)biome * BIOME_LENGTH;
        float min_x = 100000.0f;
        float max_x = -100000.0f;
        float min_y = 100000.0f;
        float max_y = -100000.0f;
        float min_corridor = 100000.0f;
        float max_corridor = -100000.0f;
        float min_relief = 100000.0f;
        float max_relief = -100000.0f;
        float previous_x = path_center(chapter_z);
        float previous_y = path_elevation(chapter_z);
        float previous_z = chapter_z;
        int sample;

        for(sample = 0; sample <= 430; ++sample) {
            const float z = chapter_z + (float)sample * 10.0f;
            const float x = path_center(z);
            const float y = path_elevation(z);
            const float corridor = course_environment_curve(z, false);
            const float relief = course_environment_curve(z, true);
            if(x < min_x) min_x = x;
            if(x > max_x) max_x = x;
            if(y < min_y) min_y = y;
            if(y > max_y) max_y = y;
            if(corridor < min_corridor) min_corridor = corridor;
            if(corridor > max_corridor) max_corridor = corridor;
            if(relief < min_relief) min_relief = relief;
            if(relief > max_relief) max_relief = relief;
            if(sample > 0) {
                const float turn = fabsf((x - previous_x) /
                                         (z - previous_z));
                const float grade = fabsf((y - previous_y) /
                                          (z - previous_z));
                if(turn > global_max_turn) global_max_turn = turn;
                if(grade > global_max_grade) global_max_grade = grade;
            }
            if(path_center(z) != x || path_elevation(z) != y)
                passed = false;
            previous_x = x;
            previous_y = y;
            previous_z = z;
        }
        if(max_x - min_x < 100.0f || max_y - min_y < 70.0f ||
           min_corridor < 22.5f || max_corridor > 48.5f ||
           min_relief < 69.5f || max_relief > 172.5f)
            passed = false;
        printf("Gravity Wave course test: biome=%d lateral=%.1f vertical=%.1f "
               "corridor=%.1f..%.1f relief=%.0f..%.0f%%.\n",
               biome, (double)(max_x - min_x), (double)(max_y - min_y),
               (double)min_corridor, (double)max_corridor,
               (double)min_relief, (double)max_relief);
    }

    for(biome = 1; biome <= 4; ++biome) {
        const float seam = (float)biome * BIOME_LENGTH;
        if(fabsf(path_center(seam - 0.01f) -
                 path_center(seam + 0.01f)) > 0.05f ||
           fabsf(path_elevation(seam - 0.01f) -
                 path_elevation(seam + 0.01f)) > 0.05f)
            passed = false;
    }
    if(global_max_turn < 0.16f || global_max_turn > 0.62f ||
       global_max_grade < 0.13f || global_max_grade > 0.56f)
        passed = false;

    memset(&lane, 0, sizeof(lane));
    lane.kind = TRAVERSAL_SLALOM;
    lane.direction = 1;
    lane.radius = 19.0f;
    lane.stage_count = traversal_stage_count_for_kind(lane.kind);
    lane.stage_spacing = traversal_stage_spacing_for_kind(lane.kind);
    if(lane.stage_count != 3 || lane.stage_spacing < 240.0f ||
       traversal_stage_x(&lane, 0) <= 0.0f ||
       traversal_stage_x(&lane, 1) >= 0.0f ||
       traversal_stage_x(&lane, 2) <= 0.0f)
        passed = false;
    for(biome = 0; biome < lane.stage_count; ++biome) {
        const float center = traversal_stage_x(&lane, biome);
        int side;
        if(center - lane.radius <= PLAYER_MIN_X ||
           center + lane.radius >= PLAYER_MAX_X ||
           !traversal_lane_contains(&lane, biome, center) ||
           traversal_lane_contains(&lane, biome,
                                   center + lane.radius + 1.0f))
            passed = false;
        for(side = -1; side <= 1; side += 2) {
            const float pylon_x = traversal_lane_pylon_x(
                &lane, biome, side);
            const float inner_edge = fabsf(pylon_x - center) -
                                     VECTOR_LANE_PYLON_HALF_WIDTH;
            if(inner_edge < lane.radius +
                            VECTOR_LANE_PYLON_CLEARANCE - 0.01f)
                passed = false;
        }
    }
    for(biome = 0; biome < 4; ++biome) {
        int event;
        for(event = 0; event < COURSE_TRAVERSALS_PER_BIOME; ++event) {
            const traversal_kind_t kind = course_traversal_kind(biome, event);
            const float start_z = (float)biome * BIOME_LENGTH +
                                  course_traversal_offsets[event];
            if(event == COURSE_TRAVERSALS_PER_BIOME - 1 &&
               start_z + traversal_span_for_kind(kind) + 180.0f >=
               (float)biome * BIOME_LENGTH +
               course_wave_offsets[COURSE_WAVES_PER_BIOME - 1])
                passed = false;
            if(kind == TRAVERSAL_SLALOM) {
                int stage;
                lane.z = start_z;
                lane.direction = course_turn_direction(start_z);
                for(stage = 0; stage < lane.stage_count; ++stage) {
                    const float stage_z = traversal_stage_z(&lane, stage);
                    const float center = traversal_stage_x(&lane, stage);
                    if(terrain_height_local(center, stage_z) + 6.0f >= 52.0f)
                        passed = false;
                }
            }
        }
    }

    {
        const float target_z = 2.0f * BIOME_LENGTH +
                               COURSE_SIGNATURE_LOCAL_Z;
        const int signature_segment =
            (int)floorf(target_z / SCENERY_SEGMENT + 0.5f);
        const float signature_z =
            (float)signature_segment * SCENERY_SEGMENT;
        const vec3_t low_before = route_position(
            0.0f, 35.0f, signature_z - 24.0f);
        const vec3_t low_after = route_position(
            0.0f, 35.0f, signature_z + 24.0f);
        const vec3_t side_before = route_position(
            72.0f, 35.0f, signature_z - 24.0f);
        const vec3_t side_after = route_position(
            72.0f, 35.0f, signature_z + 24.0f);
        const vec3_t rock_before = route_position(
            0.0f, 82.0f, signature_z + 25.0f);
        const vec3_t rock_after = route_position(
            0.0f, 82.0f, signature_z + 73.0f);
        const vec3_t rock_high_before = route_position(
            14.0f, 96.0f, signature_z + 25.0f);
        const vec3_t rock_high_after = route_position(
            14.0f, 96.0f, signature_z + 73.0f);
        const gate_t saved_gate = gates[0];

        prism_hit = player_hits_signature_arch(
            2, signature_segment, signature_z, low_before, low_after);
        prism_miss = !player_hits_signature_arch(
            2, signature_segment, signature_z, side_before, side_after);
        prism_rock_hit = player_hits_signature_arch(
            2, signature_segment, signature_z, rock_before, rock_after);
        prism_rock_high_miss = !player_hits_signature_arch(
            2, signature_segment, signature_z,
            rock_high_before, rock_high_after);
        memset(&gates[0], 0, sizeof(gates[0]));
        gates[0].active = true;
        gates[0].z = signature_z;
        gates[0].stage_count = 1;
        prism_suppressed = !player_hits_signature_arch(
            2, signature_segment, signature_z, low_before, low_after);
        gates[0] = saved_gate;
        if(!prism_hit || !prism_miss || !prism_rock_hit ||
           !prism_rock_high_miss || !prism_suppressed)
            passed = false;
    }

    printf("Gravity Wave course test: max turn=%.3f max grade=%.3f "
           "vector spacing=%.0f prism=%d/%d/%d/%d/%d %s.\n",
           (double)global_max_turn, (double)global_max_grade,
           (double)lane.stage_spacing,
           prism_hit, prism_miss, prism_rock_hit,
           prism_rock_high_miss, prism_suppressed,
           passed ? "PASS" : "FAIL");
    return passed;
}
#endif

static vec3_t player_aim_target_world(void) {
    const float target_z = game.distance + PLAYER_AIM_Z;
    return route_position(game.player_x, game.player_y, target_z);
}

static void launch_player_projectile(projectile_t *shot,
                                     projectile_kind_t kind,
                                     float muzzle_x, float muzzle_y,
                                     float muzzle_z, float forward_speed,
                                     float life, float spin,
                                     int damage, int hits_remaining) {
    const vec3_t origin = player_model_point(muzzle_x, muzzle_y, muzzle_z);
    const vec3_t target = player_aim_target_world();
    const float flight_time = fmaxf(
        (target.z - origin.z) / forward_speed, 0.05f);

    *shot = (projectile_t){
        .active = true,
        .x = origin.x,
        .y = origin.y,
        .z = origin.z,
        .vx = (target.x - origin.x) / flight_time,
        .vy = (target.y - origin.y) / flight_time,
        .vz = forward_speed,
        .life = life,
        .spin = spin,
        .kind = kind,
        .damage = damage,
        .hits_remaining = hits_remaining,
        .hit_mask = 0u
    };
}

static void fire_player_weapon(void) {
    if(game.temporary_weapon == TEMP_WEAPON_PHASE_WAVE &&
       game.temporary_weapon_time > 0.0f) {
        projectile_t *shot = alloc_projectile(player_shots, MAX_SHOTS);
        if(shot)
            launch_player_projectile(
                shot, SHOT_PLAYER_PHASE, 0.0f, 0.2f, 8.0f,
                455.0f, 1.60f,
                (game.weapon_shot_counter & 1) ? -0.17f : 0.17f,
                2, 4);
        game.weapon_shot_counter++;
        game.fire_cooldown = 0.28f;
        play_phase_wave(game.player_x);
        return;
    }

    if(game.temporary_weapon == TEMP_WEAPON_FAST_LASER &&
       game.temporary_weapon_time > 0.0f) {
        projectile_t *shot = alloc_projectile(player_shots, MAX_SHOTS);
        const float offset = (game.weapon_shot_counter & 1) ? 2.9f : -2.9f;
        if(shot)
            launch_player_projectile(
                shot, SHOT_PLAYER_FAST, offset, 0.1f, 7.0f,
                690.0f, 1.35f, 0.0f, 2, 1);
        game.weapon_shot_counter++;
        game.fire_cooldown = 0.058f;
        play_fast_laser(game.player_x + offset);
        return;
    }

    const int shot_count = game.weapon_level >= 3 ? 3 :
                           (game.weapon_level == 2 ? 2 : 1);
    int index;
    for(index = 0; index < shot_count; ++index) {
        projectile_t *shot = alloc_projectile(player_shots, MAX_SHOTS);
        const float offset = shot_count == 1 ? 0.0f :
                             (shot_count == 2 ?
                              (index == 0 ? -3.7f : 3.7f) :
                              ((float)index - 1.0f) * 4.2f);
        if(!shot)
            continue;
        launch_player_projectile(
            shot, SHOT_PLAYER_LASER, offset, 0.1f, 7.0f,
            510.0f + (float)(game.weapon_level - 1) * 45.0f,
            1.75f, 0.0f, 1, 1);
    }
    game.weapon_shot_counter++;
    game.fire_cooldown = 0.155f -
                         (float)(game.weapon_level - 1) * 0.018f;
    play_laser(game.player_x);
}

static int guardian_shot_count(int biome) {
    return (biome & 3) == 3 ? 3 : 5;
}

static void guardian_shot_offsets(int biome, bool alternate, int index,
                                  float *target_x, float *target_y,
                                  float *muzzle_x, float *muzzle_y) {
    const int style = biome & 3;
    const float centered = (float)index -
        (float)(guardian_shot_count(style) - 1) * 0.5f;

    *target_x = 0.0f;
    *target_y = 0.0f;
    *muzzle_x = 0.0f;
    *muzzle_y = 0.0f;
    switch(style) {
        case 0: /* Tidebreaker: a broad wing-level strafing fan. */
            *target_x = centered * 31.0f;
            *target_y = fabsf(centered) * 3.5f;
            *muzzle_x = centered * 2.4f;
            *muzzle_y = -1.0f;
            break;
        case 1: /* Bastion: alternating cardinal and diagonal lattices. */
            if(index > 0) {
                const int spoke = index - 1;
                const float angle = (float)spoke * PI * 0.5f +
                                    (alternate ? PI * 0.25f : 0.0f);
                *target_x = fcos(angle) * 38.0f;
                *target_y = fsin(angle) * 27.0f;
                *muzzle_x = fcos(angle) * 5.5f;
                *muzzle_y = fsin(angle) * 4.0f;
            }
            break;
        case 2: /* Seraph: a climbing/falling vertical light curtain. */
            *target_x = alternate ? 10.0f : -10.0f;
            *target_y = centered * 29.0f;
            *muzzle_x = alternate ? 2.5f : -2.5f;
            *muzzle_y = centered * 2.2f;
            break;
        default: /* Mantis: fast diagonal pincers that reverse each volley. */
            *target_x = centered * 46.0f;
            *target_y = centered * (alternate ? 32.0f : -32.0f);
            *muzzle_x = centered * 7.5f;
            *muzzle_y = centered * (alternate ? 3.5f : -3.5f);
            break;
    }
}

static void fire_enemy_shot(enemy_t *enemy) {
    const bool guardian = enemy->type == 3;
    const int biome = enemy->biome & 3;
    const guardian_profile_t *profile = &guardian_profiles[biome];
    const float shot_speed = guardian ? profile->shot_speed : 105.0f;
    const float player_z = game.distance + PLAYER_Z;
    const float target_local_x = clampf(
        game.player_x + game.player_vx * 0.20f,
        PLAYER_MIN_X, PLAYER_MAX_X);
    const float target_local_y = clampf(
        game.player_y + game.player_vy * 0.12f,
        PLAYER_MIN_Y, PLAYER_MAX_Y);
    const int count = guardian ? guardian_shot_count(biome) : 1;
    int i;

    for(i = 0; i < count; ++i) {
        projectile_t *shot = alloc_projectile(enemy_shots, MAX_ENEMY_SHOTS);
        float target_offset_x = 0.0f;
        float target_offset_y = 0.0f;
        float muzzle_x = 0.0f;
        float muzzle_y = 0.0f;
        vec3_t origin;
        vec3_t target;
        float travel_time;
        if(!shot)
            break;
        if(guardian)
            guardian_shot_offsets(biome, enemy->fired, i,
                                  &target_offset_x, &target_offset_y,
                                  &muzzle_x, &muzzle_y);
        origin = enemy_model_point(
            enemy, muzzle_x, muzzle_y, guardian ? 15.0f : 8.0f);
        travel_time = clampf(
            (origin.z - player_z) / (game.speed + shot_speed), 0.12f, 2.8f);
        target = route_position(
            target_local_x, target_local_y,
            player_z + game.speed * travel_time);
        target.x += target_offset_x;
        target.y += target_offset_y;
        *shot = (projectile_t){
            .active = true,
            .x = origin.x,
            .y = origin.y,
            .z = origin.z,
            .vx = (target.x - origin.x) / travel_time,
            .vy = (target.y - origin.y) / travel_time,
            .vz = (target.z - origin.z) / travel_time,
            .life = guardian ? 4.4f : 3.2f,
            .spin = 0.0f,
            .kind = SHOT_ENEMY,
            .damage = guardian ? profile->shot_damage : 13,
            .hits_remaining = 1,
            .hit_mask = guardian ?
                GUARDIAN_SHOT_STYLE_FLAG | (uint32_t)biome : 0u
        };
    }
    if(guardian) {
        enemy->fire_timer = profile->fire_cadence;
        enemy->fired = !enemy->fired;
    }
    else {
        enemy->fire_timer = enemy->type == 2 ? 1.05f : 1.55f;
        enemy->fired = true;
    }
}

static void trigger_bomb(void) {
    const bool guardian_was_active = active_guardian() != NULL;
    int i;
    if(game.bombs <= 0 || game.bomb_wave > 0.0f)
        return;
    game.bombs--;
    game.bomb_wave = 0.001f;
    set_message("NOVA PULSE", 1.4f);
    for(i = 0; i < MAX_ENEMY_SHOTS; ++i)
        enemy_shots[i].active = false;
    for(i = 0; i < MAX_ENEMIES; ++i) {
        if(enemies[i].active && enemies[i].z < game.distance + 800.0f) {
            if(enemies[i].type == 3) {
                enemies[i].hp -= 5;
                spawn_explosion(enemies[i].x, enemies[i].y, enemies[i].z,
                                palettes[enemies[i].biome & 3].accent, 22);
                if(enemies[i].hp <= 0)
                    destroy_enemy(&enemies[i], true);
            }
            else {
                destroy_enemy(&enemies[i], true);
            }
        }
    }
    /* Keep replacement formations beyond the pulse's cleared arc. */
    if(!guardian_was_active &&
       !course_wave_is_guardian(game.next_wave_z))
        game.next_wave_z = next_course_wave_z(
            fmaxf(game.next_wave_z, game.distance + 900.0f));
    game.trauma = 0.82f;
    play_rumble(7);
    play_sound(sfx_explosion, 245, 128);
}

static void reset_game(void) {
    memset(enemies, 0, sizeof(enemies));
    memset(player_shots, 0, sizeof(player_shots));
    memset(enemy_shots, 0, sizeof(enemy_shots));
    memset(particles, 0, sizeof(particles));
    memset(gates, 0, sizeof(gates));
    memset(pickups, 0, sizeof(pickups));
    game.mode = MODE_PLAYING;
    game.time = 0.0f;
    game.distance = 0.0f;
    game.speed = 96.0f;
    game.player_x = 0.0f;
    game.player_y = 35.0f;
    game.player_vx = 0.0f;
    game.player_vy = 0.0f;
    game.camera_x = 0.0f;
    game.camera_y = 34.0f;
    game.bank = 0.0f;
    game.barrel_roll = 0.0f;
    game.roll_cooldown = 0.0f;
    game.shield = 100.0f;
    game.hit_cooldown = 0.0f;
    game.scenery_hit_cooldown = 0.0f;
    game.boost = 100.0f;
    game.fire_cooldown = 0.0f;
    game.exhaust_timer = 0.0f;
    game.bomb_wave = 0.0f;
    game.bombs = 3;
    game.score = 0;
    game.score_fraction = 0.0f;
    game.combo = 0;
    game.combo_timer = 0.0f;
    game.wave = 0;
    game.next_wave_z = next_course_wave_z(0.0f);
    game.next_gate_z = next_course_traversal_z(0.0f);
    game.palette_index = 0;
    game.last_palette_index = 0;
    game.message_time = 0.0f;
    game.survival_time = 0.0f;
    game.weapon_level = 1;
    game.temporary_weapon = TEMP_WEAPON_NONE;
    game.temporary_weapon_time = 0.0f;
    game.speed_boost_time = 0.0f;
    game.weapon_shot_counter = 0;
    game.camera_focal = FOCAL_CRUISE;
    game.trauma = 0.0f;
    game.guardians_destroyed = 0;
    game.music_chapter = 0;
    game.last_course_act = -1;
    game.suppressed_gate_from_z = -100000.0f;
    game.suppressed_gate_until_z = -100000.0f;
    request_next_shuffled_music(MUSIC_GAME_VOLUME);
    set_message("AZURE REACH", 2.4f);
    printf("Gravity Wave: new run started.\n");
}

static input_t poll_input(void) {
    input_t input;
    maple_device_t *device;
    cont_state_t *state;
    memset(&input, 0, sizeof(input));

    device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if(!device)
        return input;
    state = (cont_state_t *)maple_dev_status(device);
    if(!state)
        return input;

    input.connected = true;
    input.buttons = state->buttons;
    input.pressed = input.buttons & ~game.previous_buttons;
    input.x = fabsf((float)state->joyx) > 13.0f ?
              (float)state->joyx / 127.0f : 0.0f;
    input.y = fabsf((float)state->joyy) > 13.0f ?
              (float)state->joyy / 127.0f : 0.0f;
    if(state->buttons & CONT_DPAD_LEFT)
        input.x = -1.0f;
    if(state->buttons & CONT_DPAD_RIGHT)
        input.x = 1.0f;
    if(state->buttons & CONT_DPAD_UP)
        input.y = -1.0f;
    if(state->buttons & CONT_DPAD_DOWN)
        input.y = 1.0f;
    input.left_trigger = (float)state->ltrig / 255.0f;
    input.right_trigger = (float)state->rtrig / 255.0f;
    return input;
}

static void update_particles(float dt) {
    int i;
    for(i = 0; i < MAX_PARTICLES; ++i) {
        particle_t *particle = &particles[i];
        if(!particle->active)
            continue;
        particle->life -= dt;
        if(particle->life <= 0.0f || particle->z < game.distance - 20.0f) {
            particle->active = false;
            continue;
        }
        particle->x += particle->vx * dt;
        particle->y += particle->vy * dt;
        particle->z += particle->vz * dt;
        if(particle->kind == PARTICLE_EXHAUST) {
            particle->vx *= 1.0f - clampf(dt * 3.8f, 0.0f, 0.45f);
            particle->vy *= 1.0f - clampf(dt * 3.8f, 0.0f, 0.45f);
            particle->vz *= 1.0f - clampf(dt * 0.35f, 0.0f, 0.18f);
        }
        else if(particle->kind == PARTICLE_BOSS_IMPACT) {
            particle->vx = 0.0f;
            particle->vy = 0.0f;
            particle->vz = 0.0f;
        }
        else {
            particle->vy -= 7.0f * dt;
            particle->vx *= 1.0f - clampf(dt * 0.7f, 0.0f, 0.4f);
            particle->vz *= 1.0f - clampf(dt * 0.5f, 0.0f, 0.3f);
        }
    }
}

static vec3_t vec3_lerp(vec3_t a, vec3_t b, float t) {
    return (vec3_t){lerpf(a.x, b.x, t),
                    lerpf(a.y, b.y, t),
                    lerpf(a.z, b.z, t)};
}

static vec3_t segment_point_at_z(vec3_t start, vec3_t end, float world_z) {
    const float dz = end.z - start.z;
    const float t = fabsf(dz) > 0.0001f ?
        clampf((world_z - start.z) / dz, 0.0f, 1.0f) : 1.0f;
    return vec3_lerp(start, end, t);
}

static float vec3_dot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3_t vec3_cross(vec3_t a, vec3_t b) {
    return (vec3_t){a.y * b.z - a.z * b.y,
                    a.z * b.x - a.x * b.z,
                    a.x * b.y - a.y * b.x};
}

static bool swept_ellipsoid_hit(vec3_t start, vec3_t end, vec3_t center,
                                float side_radius, float up_radius,
                                float along_radius, float spin,
                                float *hit_t) {
    const vec3_t motion = {end.x - start.x,
                           end.y - start.y,
                           end.z - start.z};
    const float motion_length = fsqrt(vec3_dot(motion, motion));
    vec3_t forward;
    vec3_t side;
    vec3_t up;
    vec3_t spun_side;
    vec3_t spun_up;
    vec3_t relative;
    vec3_t scaled_start;
    vec3_t scaled_motion;
    float side_length;
    float a;
    float b;
    float c;
    float discriminant;
    float root;
    float cs = 1.0f;
    float sn = 0.0f;

    if(motion_length < 0.0001f) {
        const float dx = (start.x - center.x) / side_radius;
        const float dy = (start.y - center.y) / up_radius;
        const float dz = (start.z - center.z) / along_radius;
        if(dx * dx + dy * dy + dz * dz > 1.0f)
            return false;
        *hit_t = 0.0f;
        return true;
    }
    if(fabsf(spin) > 0.0001f) {
        cs = fcos(spin);
        sn = fsin(spin);
    }
    forward = (vec3_t){motion.x / motion_length,
                       motion.y / motion_length,
                       motion.z / motion_length};
    side = vec3_cross((vec3_t){0.0f,1.0f,0.0f}, forward);
    side_length = fsqrt(vec3_dot(side, side));
    if(side_length < 0.001f)
        side = (vec3_t){1.0f,0.0f,0.0f};
    else
        side = (vec3_t){side.x / side_length,
                        side.y / side_length,
                        side.z / side_length};
    up = vec3_cross(forward, side);
    spun_side = (vec3_t){side.x * cs + up.x * sn,
                         side.y * cs + up.y * sn,
                         side.z * cs + up.z * sn};
    spun_up = (vec3_t){up.x * cs - side.x * sn,
                       up.y * cs - side.y * sn,
                       up.z * cs - side.z * sn};
    relative = (vec3_t){start.x - center.x,
                        start.y - center.y,
                        start.z - center.z};
    scaled_start = (vec3_t){
        vec3_dot(relative, spun_side) / side_radius,
        vec3_dot(relative, spun_up) / up_radius,
        vec3_dot(relative, forward) / along_radius
    };
    scaled_motion = (vec3_t){
        vec3_dot(motion, spun_side) / side_radius,
        vec3_dot(motion, spun_up) / up_radius,
        vec3_dot(motion, forward) / along_radius
    };
    c = vec3_dot(scaled_start, scaled_start) - 1.0f;
    if(c <= 0.0f) {
        *hit_t = 0.0f;
        return true;
    }
    a = vec3_dot(scaled_motion, scaled_motion);
    b = 2.0f * vec3_dot(scaled_start, scaled_motion);
    discriminant = b * b - 4.0f * a * c;
    if(a < 0.000001f || discriminant < 0.0f)
        return false;
    root = (-b - fsqrt(discriminant)) / (2.0f * a);
    if(root < 0.0f || root > 1.0f)
        return false;
    *hit_t = root;
    return true;
}

static bool projectile_terrain_hit(vec3_t start, vec3_t end,
                                   float *hit_t, vec3_t *impact) {
    const vec3_t motion = {end.x - start.x,
                           end.y - start.y,
                           end.z - start.z};
    const float length = fsqrt(vec3_dot(motion, motion));
    const int steps = (int)clampf(ceilf(length / 12.0f), 1.0f, 8.0f);
    float previous_t = 0.0f;
    int step;

    for(step = 0; step <= steps; ++step) {
        const float t = (float)step / (float)steps;
        const vec3_t point = vec3_lerp(start, end, t);
        const float local_x = point.x - path_center(point.z);
        const float local_y = point.y - path_elevation(point.z);
        if(local_y <= terrain_height_local(local_x, point.z) + 0.8f) {
            float low = previous_t;
            float high = t;
            int refine;
            for(refine = 0; refine < 5; ++refine) {
                const float middle = (low + high) * 0.5f;
                const vec3_t sample = vec3_lerp(start, end, middle);
                const float sample_x =
                    sample.x - path_center(sample.z);
                const float sample_y =
                    sample.y - path_elevation(sample.z);
                if(sample_y <= terrain_height_local(sample_x, sample.z) +
                               0.8f)
                    high = middle;
                else
                    low = middle;
            }
            *hit_t = high;
            *impact = vec3_lerp(start, end, high);
            return true;
        }
        previous_t = t;
    }
    return false;
}

static void spawn_projectile_terrain_impact(vec3_t impact,
                                            color3_t color) {
    int spark;
    for(spark = 0; spark < 5; ++spark) {
        spawn_particle(impact.x, impact.y + 0.4f, impact.z,
                       random_signed() * 16.0f,
                       7.0f + random_unit() * 18.0f,
                       random_signed() * 15.0f,
                       0.16f + random_unit() * 0.20f,
                       1.0f + random_unit() * 1.7f,
                       color, PARTICLE_STREAK);
    }
}

static void resolve_player_shot_hit(projectile_t *shot, enemy_t *enemy,
                                    int enemy_index, vec3_t impact) {
    const bool phase_wave = shot->kind == SHOT_PLAYER_PHASE;
    const int damage = shot->damage > 0 ? shot->damage : 1;
    const color3_t impact_color = phase_wave ?
        (color3_t){1.0f,0.28f,0.86f} :
        (shot->kind == SHOT_PLAYER_FAST ?
         (color3_t){0.34f,1.0f,1.0f} :
         (color3_t){1.0f,0.84f,0.28f});
    const float local_impact_x = impact.x - path_center(impact.z);

    if(phase_wave) {
        shot->hit_mask |= 1u << enemy_index;
        shot->hits_remaining--;
        if(shot->hits_remaining <= 0)
            shot->active = false;
    }
    else {
        shot->active = false;
    }
    enemy->hp -= damage;
    if(enemy->type == 3) {
        enemy->hit_flash = 0.18f;
        spawn_boss_impact_world(impact, impact_color, damage);
    }
    else {
        spawn_particle(impact.x, impact.y, impact.z,
                       random_signed() * 18.0f,
                       random_signed() * 18.0f,
                       -18.0f, 0.25f, 2.5f,
                       impact_color, PARTICLE_STREAK);
    }
    if(enemy->hp <= 0)
        destroy_enemy(enemy, true);
    else
        play_sound(sfx_hit, enemy->type == 3 ? 190 : 130,
                   (int)clampf(128.0f + local_impact_x * 1.2f,
                               20.0f, 236.0f));
}

static void update_player_shots(float dt) {
    int i;
    for(i = 0; i < MAX_SHOTS; ++i) {
        projectile_t *shot = &player_shots[i];
        vec3_t previous;
        vec3_t current;
        vec3_t collision_end;
        vec3_t terrain_impact;
        float terrain_t = 2.0f;
        float segment_min_z;
        float segment_max_z;
        bool terrain_hit;
        bool phase_wave;
        int j;

        if(!shot->active)
            continue;
        previous = (vec3_t){shot->x, shot->y, shot->z};
        shot->x += shot->vx * dt;
        shot->y += shot->vy * dt;
        shot->z += shot->vz * dt;
        shot->life -= dt;
        if(shot->kind == SHOT_PLAYER_PHASE)
            shot->spin += dt * 0.55f;
        if(shot->life <= 0.0f || shot->z > game.distance + FAR_PLANE) {
            shot->active = false;
            continue;
        }
        current = (vec3_t){shot->x, shot->y, shot->z};
        terrain_hit = projectile_terrain_hit(
            previous, current, &terrain_t, &terrain_impact);
        collision_end = terrain_hit ? terrain_impact : current;
        segment_min_z = fminf(previous.z, collision_end.z);
        segment_max_z = fmaxf(previous.z, collision_end.z);
        phase_wave = shot->kind == SHOT_PLAYER_PHASE;

        if(phase_wave) {
            while(shot->active) {
                int closest_enemy = -1;
                float closest_t = 2.0f;
                for(j = 0; j < MAX_ENEMIES; ++j) {
                    enemy_t *enemy = &enemies[j];
                    vec3_t enemy_world;
                    float hit_t;
                    float broad_radius;
                    if(!enemy->active || (shot->hit_mask & (1u << j)))
                        continue;
                    broad_radius = 31.0f + enemy->radius;
                    if(enemy->z < segment_min_z - broad_radius ||
                       enemy->z > segment_max_z + broad_radius)
                        continue;
                    enemy_world = route_position(
                        enemy->x, enemy->y, enemy->z);
                    if(swept_ellipsoid_hit(
                           previous, collision_end, enemy_world,
                           31.0f + enemy->radius,
                           13.5f + enemy->radius,
                           18.0f + (enemy->type == 3 ?
                                   enemy->radius * 0.30f : 0.0f),
                           shot->spin, &hit_t) &&
                       hit_t < closest_t) {
                        closest_enemy = j;
                        closest_t = hit_t;
                    }
                }
                if(closest_enemy < 0)
                    break;
                resolve_player_shot_hit(
                    shot, &enemies[closest_enemy], closest_enemy,
                    vec3_lerp(previous, collision_end, closest_t));
            }
        }
        else {
            int closest_enemy = -1;
            float closest_t = 2.0f;
            for(j = 0; j < MAX_ENEMIES; ++j) {
                enemy_t *enemy = &enemies[j];
                vec3_t enemy_world;
                float hit_t;
                float broad_radius;
                if(!enemy->active)
                    continue;
                broad_radius = fmaxf(enemy->radius + 2.0f, 15.0f);
                if(enemy->z < segment_min_z - broad_radius ||
                   enemy->z > segment_max_z + broad_radius)
                    continue;
                enemy_world = route_position(
                    enemy->x, enemy->y, enemy->z);
                if(swept_ellipsoid_hit(
                       previous, collision_end, enemy_world,
                       enemy->radius + 2.0f, enemy->radius + 2.0f,
                       enemy->type == 3 ? enemy->radius * 0.72f : 15.0f,
                       0.0f, &hit_t) && hit_t < closest_t) {
                    closest_enemy = j;
                    closest_t = hit_t;
                }
            }
            if(closest_enemy >= 0)
                resolve_player_shot_hit(
                    shot, &enemies[closest_enemy], closest_enemy,
                    vec3_lerp(previous, collision_end, closest_t));
        }

        if(shot->active && terrain_hit) {
            const color3_t impact_color = phase_wave ?
                (color3_t){1.0f,0.28f,0.86f} :
                (shot->kind == SHOT_PLAYER_FAST ?
                 (color3_t){0.34f,1.0f,1.0f} :
                 (color3_t){1.0f,0.84f,0.28f});
            shot->active = false;
            spawn_projectile_terrain_impact(terrain_impact, impact_color);
        }
    }
}

static void update_enemy_shots(float dt,
                               vec3_t previous_player_world,
                               vec3_t current_player_world) {
    int i;
    for(i = 0; i < MAX_ENEMY_SHOTS; ++i) {
        projectile_t *shot = &enemy_shots[i];
        vec3_t previous;
        vec3_t current;
        vec3_t relative_previous;
        vec3_t relative_current;
        vec3_t terrain_impact;
        float player_hit_t = 2.0f;
        float terrain_hit_t = 2.0f;
        bool player_hit;
        bool terrain_hit;
        if(!shot->active)
            continue;
        previous = (vec3_t){shot->x, shot->y, shot->z};
        shot->x += shot->vx * dt;
        shot->y += shot->vy * dt;
        shot->z += shot->vz * dt;
        if(shot->hit_mask & GUARDIAN_SHOT_STYLE_FLAG)
            shot->spin += dt * (3.2f +
                (float)(shot->hit_mask & 3u) * 0.9f);
        shot->life -= dt;
        if(shot->life <= 0.0f || shot->z < game.distance - 10.0f) {
            shot->active = false;
            continue;
        }
        current = (vec3_t){shot->x, shot->y, shot->z};
        relative_previous = (vec3_t){
            previous.x - previous_player_world.x,
            previous.y - previous_player_world.y,
            previous.z - previous_player_world.z
        };
        relative_current = (vec3_t){
            current.x - current_player_world.x,
            current.y - current_player_world.y,
            current.z - current_player_world.z
        };
        player_hit = swept_ellipsoid_hit(
            relative_previous, relative_current,
            (vec3_t){0.0f,0.0f,0.0f},
            5.0f, 4.0f, 9.0f, 0.0f, &player_hit_t);
        terrain_hit = projectile_terrain_hit(
            previous, current, &terrain_hit_t, &terrain_impact);
        if(player_hit && player_hit_t < terrain_hit_t) {
            shot->active = false;
            damage_player((float)(shot->damage > 0 ? shot->damage : 13));
        }
        else if(terrain_hit) {
            shot->active = false;
            spawn_projectile_terrain_impact(
                terrain_impact, (color3_t){1.0f,0.34f,0.18f});
        }
    }
}

#ifdef GRAVITY_WAVE_AUTOTEST_COMBAT_GEOMETRY
static bool run_combat_geometry_self_test(void) {
    const game_t saved_game = game;
    projectile_t test_shot;
    vec3_t target;
    vec3_t intercept;
    vec3_t extended;
    vec3_t terrain_start;
    vec3_t terrain_end;
    vec3_t terrain_impact;
    vec3_t open_start;
    vec3_t open_end;
    float flight_time;
    float aim_error;
    float boundary_aim_error = 0.0f;
    float enemy_aim_error;
    float rigid_span_error = 0.0f;
    float route_separation;
    float hit_t;
    float terrain_t;
    bool crossing_hit;
    bool crossing_miss;
    bool moving_hit;
    bool stationary_hit;
    bool phase_hit;
    bool phase_rotated_miss;
    bool terrain_hit;
    bool open_miss;
    bool muzzle_forward;
    bool boundary_aim_pass = true;
    bool enemy_aim_pass;
    bool hostile_sweep_pass = true;
    bool passed;
    int boundary;

    game.distance = 14790.0f;
    game.player_x = 17.0f;
    game.player_y = 49.0f;
    game.player_vy = -11.0f;
    game.bank = 0.28f;
    game.barrel_roll = 0.0f;
    launch_player_projectile(&test_shot, SHOT_PLAYER_LASER,
                             0.0f, 0.1f, 7.0f,
                             510.0f, 1.75f, 0.0f, 1, 1);
    target = player_aim_target_world();
    flight_time = (target.z - test_shot.z) / test_shot.vz;
    intercept = (vec3_t){
        test_shot.x + test_shot.vx * flight_time,
        test_shot.y + test_shot.vy * flight_time,
        test_shot.z + test_shot.vz * flight_time
    };
    aim_error = fsqrt(
        (intercept.x - target.x) * (intercept.x - target.x) +
        (intercept.y - target.y) * (intercept.y - target.y) +
        (intercept.z - target.z) * (intercept.z - target.z));
    extended = (vec3_t){
        test_shot.x + test_shot.vx * (flight_time + 0.60f),
        test_shot.y + test_shot.vy * (flight_time + 0.60f),
        test_shot.z + test_shot.vz * (flight_time + 0.60f)
    };
    route_separation = fsqrt(
        (extended.x - (path_center(extended.z) +
                       test_shot.x - path_center(test_shot.z))) *
        (extended.x - (path_center(extended.z) +
                       test_shot.x - path_center(test_shot.z))) +
        (extended.y - (path_elevation(extended.z) +
                       test_shot.y - path_elevation(test_shot.z))) *
        (extended.y - (path_elevation(extended.z) +
                       test_shot.y - path_elevation(test_shot.z))));

    {
        static const float boundary_starts[4] = {
            3950.0f,8250.0f,12550.0f,16850.0f
        };
        for(boundary = 0; boundary < 4; ++boundary) {
            float error;
            game.distance = boundary_starts[boundary];
            game.player_x = -13.0f + (float)boundary * 8.0f;
            game.player_y = 62.0f;
            game.player_vy = 0.0f;
            game.bank = 0.0f;
            launch_player_projectile(&test_shot, SHOT_PLAYER_LASER,
                                     0.0f, 0.1f, 7.0f,
                                     510.0f, 1.75f, 0.0f, 1, 1);
            target = player_aim_target_world();
            flight_time = (target.z - test_shot.z) / test_shot.vz;
            intercept = (vec3_t){
                test_shot.x + test_shot.vx * flight_time,
                test_shot.y + test_shot.vy * flight_time,
                test_shot.z + test_shot.vz * flight_time
            };
            error = fsqrt(
                (intercept.x - target.x) * (intercept.x - target.x) +
                (intercept.y - target.y) * (intercept.y - target.y) +
                (intercept.z - target.z) * (intercept.z - target.z));
            boundary_aim_error = fmaxf(boundary_aim_error, error);
            if(error >= 0.01f)
                boundary_aim_pass = false;
        }
    }

    crossing_hit = swept_ellipsoid_hit(
        (vec3_t){0.0f,0.0f,-20.0f},
        (vec3_t){0.0f,0.0f,20.0f},
        (vec3_t){0.0f,0.0f,0.0f},
        5.0f, 4.0f, 9.0f, 0.0f, &hit_t);
    crossing_miss = !swept_ellipsoid_hit(
        (vec3_t){0.0f,4.2f,-20.0f},
        (vec3_t){0.0f,4.2f,20.0f},
        (vec3_t){0.0f,0.0f,0.0f},
        5.0f, 4.0f, 9.0f, 0.0f, &hit_t);
    moving_hit = swept_ellipsoid_hit(
        (vec3_t){0.0f,0.0f,11.0f},
        (vec3_t){0.0f,0.0f,-11.0f},
        (vec3_t){0.0f,0.0f,0.0f},
        5.0f, 4.0f, 9.0f, 0.0f, &hit_t);
    stationary_hit = swept_ellipsoid_hit(
        (vec3_t){0.0f,0.0f,0.0f},
        (vec3_t){0.0f,0.0f,0.0f},
        (vec3_t){0.0f,0.0f,0.0f},
        5.0f, 4.0f, 9.0f, 0.0f, &hit_t);
    phase_hit = swept_ellipsoid_hit(
        (vec3_t){0.0f,0.0f,0.0f},
        (vec3_t){0.0f,0.0f,20.0f},
        (vec3_t){30.0f,0.0f,10.0f},
        31.0f, 13.5f, 18.0f, 0.0f, &hit_t);
    phase_rotated_miss = !swept_ellipsoid_hit(
        (vec3_t){0.0f,0.0f,0.0f},
        (vec3_t){0.0f,0.0f,20.0f},
        (vec3_t){30.0f,0.0f,10.0f},
        31.0f, 13.5f, 18.0f, PI * 0.5f, &hit_t);

    terrain_start = route_position(
        150.0f, terrain_height_local(150.0f, 14790.0f) + 20.0f,
        14790.0f);
    terrain_end = route_position(
        150.0f, terrain_height_local(150.0f, 14850.0f) - 5.0f,
        14850.0f);
    terrain_hit = projectile_terrain_hit(
        terrain_start, terrain_end, &terrain_t, &terrain_impact);
    open_start = route_position(0.0f, 90.0f, 14790.0f);
    open_end = route_position(0.0f, 90.0f, 14850.0f);
    open_miss = !projectile_terrain_hit(
        open_start, open_end, &terrain_t, &terrain_impact);
    {
        const enemy_t test_enemy = {
            .active = true, .x = 0.0f, .y = 55.0f, .z = 15120.0f,
            .phase = 0.0f, .type = 3, .biome = 3
        };
        const vec3_t enemy_center = enemy_model_point(
            &test_enemy, 0.0f, 0.0f, 0.0f);
        const vec3_t enemy_muzzle = enemy_model_point(
            &test_enemy, 0.0f, 0.0f, 15.0f);
        muzzle_forward = enemy_muzzle.z < enemy_center.z;
    }
    {
        enemy_t rigid_guardian;
        vec3_t player_tail;
        vec3_t player_nose;
        vec3_t guardian_tail;
        vec3_t guardian_nose;
        float player_span;
        float guardian_span;

        game.distance = 11350.0f;
        game.player_vy = 0.0f;
        game.bank = 0.0f;
        player_tail = player_model_point(0.0f, 0.0f, -13.0f);
        player_nose = player_model_point(0.0f, 0.0f, 13.0f);
        player_span = fsqrt(
            (player_nose.x - player_tail.x) *
            (player_nose.x - player_tail.x) +
            (player_nose.y - player_tail.y) *
            (player_nose.y - player_tail.y) +
            (player_nose.z - player_tail.z) *
            (player_nose.z - player_tail.z));

        memset(&rigid_guardian, 0, sizeof(rigid_guardian));
        rigid_guardian.active = true;
        rigid_guardian.type = 3;
        rigid_guardian.biome = 2;
        rigid_guardian.y = 58.0f;
        rigid_guardian.z = 11798.0f;
        guardian_tail = enemy_model_point(
            &rigid_guardian, 0.0f, 0.0f, -15.0f);
        guardian_nose = enemy_model_point(
            &rigid_guardian, 0.0f, 0.0f, 15.0f);
        guardian_span = fsqrt(
            (guardian_nose.x - guardian_tail.x) *
            (guardian_nose.x - guardian_tail.x) +
            (guardian_nose.y - guardian_tail.y) *
            (guardian_nose.y - guardian_tail.y) +
            (guardian_nose.z - guardian_tail.z) *
            (guardian_nose.z - guardian_tail.z));
        rigid_span_error = fmaxf(
            fabsf(player_span - 26.0f),
            fabsf(guardian_span - 30.0f *
                   guardian_profiles[rigid_guardian.biome].model_scale));
    }
    {
        enemy_t test_enemy;
        projectile_t *enemy_shot;
        vec3_t predicted_player;
        vec3_t enemy_intercept;
        float intercept_time;
        memset(&test_enemy, 0, sizeof(test_enemy));
        memset(enemy_shots, 0, sizeof(enemy_shots));
        game.distance = 14790.0f;
        game.speed = 154.0f;
        game.player_x = -11.0f;
        game.player_y = 64.0f;
        game.player_vx = 0.0f;
        game.player_vy = 0.0f;
        test_enemy.active = true;
        test_enemy.x = 21.0f;
        test_enemy.y = 58.0f;
        test_enemy.z = game.distance + PLAYER_Z + 350.0f;
        test_enemy.type = 0;
        test_enemy.biome = 3;
        fire_enemy_shot(&test_enemy);
        enemy_shot = &enemy_shots[0];
        intercept_time = (enemy_shot->z -
                          (game.distance + PLAYER_Z)) /
                         (game.speed - enemy_shot->vz);
        enemy_intercept = (vec3_t){
            enemy_shot->x + enemy_shot->vx * intercept_time,
            enemy_shot->y + enemy_shot->vy * intercept_time,
            enemy_shot->z + enemy_shot->vz * intercept_time
        };
        predicted_player = route_position(
            game.player_x, game.player_y,
            game.distance + PLAYER_Z + game.speed * intercept_time);
        enemy_aim_error = fsqrt(
            (enemy_intercept.x - predicted_player.x) *
            (enemy_intercept.x - predicted_player.x) +
            (enemy_intercept.y - predicted_player.y) *
            (enemy_intercept.y - predicted_player.y) +
            (enemy_intercept.z - predicted_player.z) *
            (enemy_intercept.z - predicted_player.z));
        enemy_aim_pass = enemy_shot->active && enemy_shot->vz < 0.0f &&
                         enemy_aim_error < 0.05f;
    }
#ifdef GRAVITY_WAVE_AUTOTEST_DAMAGE
    {
        vec3_t previous_player;
        vec3_t current_player;
        projectile_t *enemy_shot = &enemy_shots[0];
        memset(enemy_shots, 0, sizeof(enemy_shots));
        game.mode = MODE_PLAYING;
        game.shield = 100.0f;
        game.hit_cooldown = 0.0f;
        game.player_x = 0.0f;
        game.player_y = 70.0f;
        previous_player = route_position(0.0f, 70.0f, 1000.0f);
        current_player = route_position(0.0f, 70.0f, 1012.0f);
        game.distance = current_player.z - PLAYER_Z;
        *enemy_shot = (projectile_t){
            .active = true,
            .x = previous_player.x,
            .y = previous_player.y,
            .z = previous_player.z + 8.0f,
            .vx = (current_player.x - previous_player.x) / 0.05f,
            .vy = (current_player.y - previous_player.y) / 0.05f,
            .vz = -150.0f,
            .life = 1.0f,
            .kind = SHOT_ENEMY
        };
        update_enemy_shots(0.05f, previous_player, current_player);
        hostile_sweep_pass = !enemy_shot->active &&
                             fabsf(game.shield - 87.0f) < 0.001f;
    }
#endif

    passed = aim_error < 0.01f && route_separation > 1.0f &&
             crossing_hit && crossing_miss && moving_hit && stationary_hit &&
             phase_hit && phase_rotated_miss && terrain_hit && open_miss;
    passed = passed && muzzle_forward && boundary_aim_pass &&
             enemy_aim_pass && hostile_sweep_pass &&
             rigid_span_error < 0.02f;
    printf("Gravity Wave combat geometry: %s aim=%.4f "
           "boundary=%.4f enemy=%.4f route-separation=%.2f "
           "swept=%d/%d/%d/%d phase=%d/%d terrain=%d/%d "
           "muzzle=%d hostile=%d rigid=%.4f.\n",
           passed ? "PASS" : "FAIL",
           (double)aim_error, (double)boundary_aim_error,
           (double)enemy_aim_error, (double)route_separation,
           crossing_hit, crossing_miss, moving_hit, stationary_hit,
           phase_hit, phase_rotated_miss, terrain_hit, open_miss,
           muzzle_forward, hostile_sweep_pass,
           (double)rigid_span_error);
    memset(enemy_shots, 0, sizeof(enemy_shots));
    memset(particles, 0, sizeof(particles));
    game = saved_game;
    return passed;
}
#endif

static vec3_t guardian_motion_target(int biome, float phase,
                                     float player_distance) {
    vec3_t target;
    const int style = biome & 3;

    switch(style) {
        case 0: /* A wide figure-eight over the flooded carrier channel. */
            target.x = fsin(phase) * 52.0f;
            target.y = 57.0f + fsin(phase * 2.0f + 0.35f) * 10.5f;
            target.z = player_distance + 456.0f +
                       fcos(phase * 0.55f) * 14.0f;
            break;
        case 1: /* A heavy bastion that looms and tracks in slow arcs. */
            target.x = fsin(phase * 0.72f) * 24.0f +
                       fsin(phase * 1.90f) * 8.0f;
            target.y = 51.0f + fcos(phase * 0.85f) * 14.0f;
            target.z = player_distance + 482.0f +
                       fsin(phase * 0.50f) * 8.0f;
            break;
        case 2: /* A vertical corkscrew around the rift's firing axis. */
            target.x = fcos(phase * 1.16f) * 36.0f;
            target.y = 58.0f + fsin(phase * 1.16f) * 24.0f;
            target.z = player_distance + 442.0f +
                       fsin(phase * 2.32f) * 18.0f;
            break;
        default: { /* Fast pouncing reversals across the furnace trench. */
            const float sweep = fsin(phase);
            target.x = sweep * (0.38f + fabsf(sweep) * 0.62f) * 58.0f;
            target.y = 55.0f + fsin(phase * 2.0f + 0.60f) * 15.0f;
            target.z = player_distance + 405.0f + fcos(phase) * 44.0f;
            break;
        }
    }
    return target;
}

#ifdef GRAVITY_WAVE_AUTOTEST_BOSS_PROFILES
static bool run_guardian_profile_self_test(void) {
    const game_t saved_game = game;
    bool passed = true;
    int biome;

    for(biome = 0; biome < 4; ++biome) {
        const guardian_profile_t *profile = &guardian_profiles[biome];
        enemy_t guardian;
        vec3_t motion_a;
        vec3_t motion_b;
        float minimum_x = 9999.0f;
        float maximum_x = -9999.0f;
        float minimum_y = 9999.0f;
        float maximum_y = -9999.0f;
        int active_shots = 0;
        int shot;
        int compare;

        memset(&guardian, 0, sizeof(guardian));
        memset(enemy_shots, 0, sizeof(enemy_shots));
        game.distance = 900.0f + (float)biome * BIOME_LENGTH;
        game.speed = 154.0f;
        game.player_x = -9.0f;
        game.player_y = 58.0f;
        game.player_vx = 0.0f;
        game.player_vy = 0.0f;
        guardian.active = true;
        guardian.type = 3;
        guardian.biome = biome;
        guardian.phase = 0.63f;
        guardian.x = 12.0f;
        guardian.y = 57.0f;
        guardian.z = game.distance + PLAYER_Z + 380.0f;
        fire_enemy_shot(&guardian);

        for(shot = 0; shot < MAX_ENEMY_SHOTS; ++shot) {
            const projectile_t *projectile = &enemy_shots[shot];
            if(!projectile->active)
                continue;
            active_shots++;
            if(!(projectile->hit_mask & GUARDIAN_SHOT_STYLE_FLAG) ||
               (int)(projectile->hit_mask & 3u) != biome ||
               projectile->damage != profile->shot_damage ||
               projectile->vz >= 0.0f)
                passed = false;
        }
        if(active_shots != guardian_shot_count(biome) ||
           fabsf(guardian.fire_timer - profile->fire_cadence) > 0.001f ||
           !guardian.fired)
            passed = false;

        for(shot = 0; shot < guardian_shot_count(biome); ++shot) {
            float target_x;
            float target_y;
            float muzzle_x;
            float muzzle_y;
            guardian_shot_offsets(biome, false, shot,
                                  &target_x, &target_y,
                                  &muzzle_x, &muzzle_y);
            (void)muzzle_x;
            (void)muzzle_y;
            minimum_x = fminf(minimum_x, target_x);
            maximum_x = fmaxf(maximum_x, target_x);
            minimum_y = fminf(minimum_y, target_y);
            maximum_y = fmaxf(maximum_y, target_y);
        }
        motion_a = guardian_motion_target(biome, 0.45f, game.distance);
        motion_b = guardian_motion_target(biome, 1.55f, game.distance);
        if(fabsf(motion_a.x) > 60.0f || motion_a.y < 28.0f ||
           motion_a.y > 86.0f ||
           motion_a.z - game.distance < 350.0f ||
           motion_a.z - game.distance > 510.0f ||
           fabsf(motion_b.x - motion_a.x) +
           fabsf(motion_b.y - motion_a.y) < 8.0f)
            passed = false;
        for(compare = biome + 1; compare < 4; ++compare) {
            if(strcmp(profile->name, guardian_profiles[compare].name) == 0)
                passed = false;
        }
        if((biome == 0 && maximum_x - minimum_x < 120.0f) ||
           (biome == 1 && (maximum_x - minimum_x < 70.0f ||
                           maximum_y - minimum_y < 50.0f)) ||
           (biome == 2 && maximum_y - minimum_y < 110.0f) ||
           (biome == 3 && (maximum_x - minimum_x < 90.0f ||
                           maximum_y - minimum_y < 60.0f)))
            passed = false;

        printf("Gravity Wave guardian profile %d: %s shots=%d "
               "span=(%.0f,%.0f) motion=(%.0f,%.0f,%.0f).\n",
               biome, profile->name, active_shots,
               (double)(maximum_x - minimum_x),
               (double)(maximum_y - minimum_y),
               (double)motion_a.x, (double)motion_a.y,
               (double)(motion_a.z - game.distance));
    }
    memset(enemy_shots, 0, sizeof(enemy_shots));
    game = saved_game;
    printf("Gravity Wave guardian profiles: %s.\n",
           passed ? "PASS" : "FAIL");
    return passed;
}
#endif

static void update_enemies(float dt,
                           vec3_t previous_player_world,
                           vec3_t current_player_world) {
    const float rank = clampf(game.distance / 11500.0f, 0.0f, 1.0f);
    const float player_world_z = game.distance + PLAYER_Z;
    int i;

    for(i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *enemy = &enemies[i];
        vec3_t previous_enemy_world;
        vec3_t current_enemy_world;
        vec3_t relative_previous;
        vec3_t relative_current;
        float ram_hit_t;
        float relative_z;
        if(!enemy->active)
            continue;

        previous_enemy_world = route_position(
            enemy->x, enemy->y, enemy->z);
        enemy->phase += dt * (enemy->type == 1 ? 2.4f :
                              (enemy->type == 3 ?
                               guardian_profiles[enemy->biome & 3].phase_speed :
                               1.55f));
        enemy->fire_timer -= dt;
        enemy->hit_flash = fmaxf(0.0f, enemy->hit_flash - dt);
        if(enemy->type == 3) {
            const int biome = enemy->biome & 3;
            static const float response[4] = {2.35f,1.55f,2.75f,3.35f};
            const vec3_t target = guardian_motion_target(
                biome, enemy->phase, game.distance);
            const float follow = clampf(dt * response[biome], 0.0f, 1.0f);
            enemy->x = lerpf(enemy->x, target.x, follow);
            enemy->y = lerpf(enemy->y, target.y, follow);
            enemy->z = lerpf(enemy->z, target.z, follow);
        }
        else {
            enemy->x += enemy->vx * dt;
            enemy->y += fsin(enemy->phase * 1.37f) * dt *
                        (enemy->type == 2 ? 1.8f : 5.5f);
            enemy->z -= dt * (enemy->type == 1 ? 25.0f : 10.0f);
            if(fabsf(enemy->x) > 86.0f)
                enemy->vx = -enemy->vx;
        }

        relative_z = enemy->z - player_world_z;
        if(game.bomb_wave <= 0.0f && enemy->fire_timer <= 0.0f &&
           relative_z > 105.0f &&
           relative_z < 410.0f + rank * 115.0f)
            fire_enemy_shot(enemy);

        current_enemy_world = route_position(
            enemy->x, enemy->y, enemy->z);
        relative_previous = (vec3_t){
            previous_enemy_world.x - previous_player_world.x,
            previous_enemy_world.y - previous_player_world.y,
            previous_enemy_world.z - previous_player_world.z
        };
        relative_current = (vec3_t){
            current_enemy_world.x - current_player_world.x,
            current_enemy_world.y - current_player_world.y,
            current_enemy_world.z - current_player_world.z
        };
        if(swept_ellipsoid_hit(
               relative_previous, relative_current,
               (vec3_t){0.0f,0.0f,0.0f},
               enemy->radius + 3.5f, enemy->radius + 3.0f,
               enemy->radius + 5.0f, 0.0f, &ram_hit_t)) {
            damage_player(enemy->type == 3 ? 42.0f :
                          (enemy->type == 2 ? 34.0f : 22.0f));
            if(enemy->type == 3)
                enemy->z += 75.0f;
            else
                destroy_enemy(enemy, false);
            continue;
        }

        if(enemy->type != 3 && enemy->z < game.distance - 22.0f)
            enemy->active = false;
    }
}

static void spawn_gate_clear_burst(const gate_t *gate, int stage,
                                   bool perfect,
                                   float crossing_x,
                                   float crossing_y) {
    const palette_t *palette = current_palette();
    const float clear_radius = gate->radius * GATE_CLEAR_RADIUS_SCALE;
    const int count = perfect ? 24 : 18;
    const float stage_z = traversal_stage_z(gate, stage);
    int particle;

    for(particle = 0; particle < count; ++particle) {
        const float angle = gate->spin +
            (float)particle * TAU / (float)count;
        const float cs = fcos(angle);
        const float sn = fsin(angle);
        const float speed = 32.0f + random_unit() * (perfect ? 52.0f : 38.0f);
        const color3_t color = (particle % 3) == 0 ?
            (color3_t){0.92f,1.0f,1.0f} :
            ((particle & 1) ? palette->river : palette->accent);
        const float origin_x = gate->kind == TRAVERSAL_GRAVITY_BLOOM ?
            gate->x + cs * clear_radius :
            crossing_x + cs * (gate->kind == TRAVERSAL_SLALOM ?
                               5.0f : 12.0f);
        const float origin_y = gate->kind == TRAVERSAL_GRAVITY_BLOOM ?
            gate->y + sn * clear_radius :
            crossing_y + sn * (gate->kind == TRAVERSAL_SLALOM ?
                               13.0f : 4.0f);
        const vec3_t origin = route_position(origin_x, origin_y, stage_z);
        spawn_particle(origin.x, origin.y, origin.z,
                       cs * speed, sn * speed,
                       random_signed() * 24.0f,
                       0.38f + random_unit() * 0.34f,
                       1.5f + random_unit() * 2.4f,
                       color, PARTICLE_STREAK);
    }
}

static void update_gates(float dt,
                         vec3_t previous_player_world,
                         vec3_t current_player_world) {
    const float player_world_z = current_player_world.z;
    int i;
    for(i = 0; i < MAX_GATES; ++i) {
        gate_t *gate = &gates[i];
        const float direction = (gate->variant & 1) ? -1.0f : 1.0f;
        const float end_z = traversal_stage_z(
            gate, gate->stage_count > 0 ? gate->stage_count - 1 : 0);
        if(!gate->active)
            continue;
        if(!gate->announced && gate->result == GATE_RESULT_PENDING &&
           gate->z > player_world_z && gate->z - player_world_z < 340.0f) {
            gate->announced = true;
            if(gate->kind == TRAVERSAL_GRAVITY_BLOOM)
                set_message("GRAVITY BLOOM  HOLD THE LINE", 2.1f);
            else if(gate->kind == TRAVERSAL_SLALOM)
                set_message("VECTOR LANES  CENTER IN CYAN", 2.1f);
            else
                set_message("SHEAR RUN  CHANGE ALTITUDE", 2.1f);
            play_sound(sfx_gate, 148, 128);
            printf("Gravity Wave course: chapter %d traversal %d approach.\n",
                   course_chapter_at(gate->z) + 1, (int)gate->kind);
        }
        gate->spin += dt * direction *
                      (0.72f + (float)gate->variant * 0.07f);
        gate->result_time = fmaxf(0.0f, gate->result_time - dt);
        while(gate->result == GATE_RESULT_PENDING &&
              gate->stage < gate->stage_count &&
              traversal_stage_z(gate, gate->stage) <= player_world_z) {
            const int stage = gate->stage;
            const int safe_direction = traversal_stage_direction(gate, stage);
            const float stage_z = traversal_stage_z(gate, stage);
            const vec3_t crossing = segment_point_at_z(
                previous_player_world, current_player_world, stage_z);
            const float crossing_x = crossing.x - path_center(stage_z);
            const float crossing_y = crossing.y - path_elevation(stage_z);
            const float stage_x = traversal_stage_x(gate, stage);
            const float stage_y = traversal_stage_y(gate, stage);
            const float dx = crossing_x - stage_x;
            const float dy = crossing_y - stage_y;
            const float clear_radius = gate->radius * GATE_CLEAR_RADIUS_SCALE;
            bool stage_clear;
            bool stage_perfect;

            if(gate->kind == TRAVERSAL_GRAVITY_BLOOM) {
                stage_clear = dx * dx + dy * dy <
                              clear_radius * clear_radius;
                stage_perfect = stage_clear && game.speed > 145.0f;
            }
            else if(gate->kind == TRAVERSAL_SLALOM) {
                const float lane_error = fabsf(dx);
                const float pylon_center = gate->radius +
                    VECTOR_LANE_PYLON_HALF_WIDTH +
                    VECTOR_LANE_PYLON_CLEARANCE;
                const float pylon_distance =
                    fabsf(lane_error - pylon_center);
                stage_clear = traversal_lane_contains(gate, stage, crossing_x);
                stage_perfect = stage_clear &&
                                lane_error < gate->radius * 0.42f &&
                                game.speed > 145.0f;
                if(!stage_clear) {
                    if(pylon_distance <
                       VECTOR_LANE_PYLON_HALF_WIDTH + 3.5f &&
                       crossing_y > 8.0f && crossing_y < 96.0f) {
                        const float push = dx > 0.0f ? -1.0f : 1.0f;
                        game.player_x = clampf(game.player_x + push * 7.0f,
                                               PLAYER_MIN_X, PLAYER_MAX_X);
                        game.player_vx += push * 30.0f;
                        damage_player(12.0f);
                    }
                    else {
                        /* The navigation field is a soft hazard outside its
                           cyan corridor, making a miss readable but survivable. */
                        damage_player(4.0f);
                    }
                }
            }
            else {
                const float signed_clearance = dy * (float)safe_direction;
                stage_clear = signed_clearance > gate->radius;
                stage_perfect = stage_clear && signed_clearance >
                                gate->radius + 8.0f && game.speed > 145.0f;
                if(!stage_clear)
                    damage_player(fabsf(dy) < 7.0f ? 11.0f : 6.0f);
            }

            if(stage_clear) {
                gate->success_mask |= 1u << stage;
                if(stage_perfect)
                    gate->perfect_mask |= 1u << stage;
                spawn_gate_clear_burst(
                    gate, stage, stage_perfect, crossing_x, crossing_y);
                play_sound(sfx_gate, stage_perfect ? 225 : 195, 128);
            }
            else {
                gate->faulted = true;
                game.combo = 0;
            }
            gate->stage++;

            if(gate->stage >= gate->stage_count) {
                const uint32_t stage_mask =
                    (1u << gate->stage_count) - 1u;
                const bool perfect = !gate->faulted &&
                    gate->perfect_mask == stage_mask;
                gate->result = gate->faulted ? GATE_RESULT_MISSED :
                                               GATE_RESULT_CLEARED;
                gate->result_time = gate->faulted ? 0.45f : 0.55f;
                if(!gate->faulted) {
                    const int points = gate->kind ==
                        TRAVERSAL_GRAVITY_BLOOM ? (perfect ? 420 : 280) :
                        (gate->kind == TRAVERSAL_SLALOM ?
                         (perfect ? 900 : 650) : (perfect ? 760 : 540));
                    add_score(points);
                    game.shield = clampf(
                        game.shield + (gate->stage_count > 1 ? 10.0f : 7.0f),
                        0.0f, 100.0f);
                    game.boost = clampf(
                        game.boost + (gate->stage_count > 1 ? 22.0f : 16.0f),
                        0.0f, 100.0f);
                    if(gate->kind == TRAVERSAL_GRAVITY_BLOOM)
                        set_message(perfect ? "BLOOM PERFECT" :
                                              "BLOOM CLEAR", 1.3f);
                    else if(gate->kind == TRAVERSAL_SLALOM)
                        set_message(perfect ? "VECTOR ROUTE PERFECT" :
                                              "VECTOR ROUTE CLEAR", 1.3f);
                    else
                        set_message(perfect ? "SHEAR RUN PERFECT" :
                                              "SHEAR RUN CLEAR", 1.3f);
                    game.trauma = fmaxf(
                        game.trauma, perfect ? 0.18f : 0.10f);
                }
                else if(gate->kind == TRAVERSAL_GRAVITY_BLOOM)
                    set_message("BLOOM MISSED", 0.8f);
                else if(gate->kind == TRAVERSAL_SLALOM)
                    set_message("VECTOR LANE MISSED", 0.8f);
                else
                    set_message("SHEAR ROUTE MISSED", 0.8f);
            }
#ifdef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
            printf("Gravity Wave autotest: traversal=%d stage=%d/%d %s "
                   "offset=(%.2f, %.2f) shield=%.1f.\n",
                   (int)gate->kind, stage + 1, gate->stage_count,
                   stage_clear ? "cleared" : "missed",
                   (double)dx, (double)dy, (double)game.shield);
#endif
        }
        if(end_z < game.distance - 18.0f)
            gate->active = false;
    }
}

static void collect_pickup(pickup_t *pickup) {
    switch(pickup->kind) {
        case PICKUP_LASER_CORE:
            if(game.weapon_level < 3) {
                game.weapon_level++;
                set_message(game.weapon_level == 3 ?
                            "LASER LEVEL 3  MAX" : "LASER LEVEL 2", 1.7f);
            }
            else {
                add_score(1000);
                set_message("CORE BONUS 1000", 1.3f);
            }
            break;
        case PICKUP_REPAIR:
            game.shield = clampf(game.shield + 28.0f, 0.0f, 100.0f);
            set_message("SHIELD RESTORED", 1.4f);
            break;
        case PICKUP_NOVA:
            if(game.bombs < 3) {
                game.bombs++;
                set_message("NOVA CHARGE", 1.4f);
            }
            else {
                add_score(750);
                set_message("NOVA BONUS 750", 1.3f);
            }
            break;
        case PICKUP_SPEED_BOOST:
            game.speed_boost_time = SPEED_BOOST_SECONDS;
            game.boost = 100.0f;
            set_message("SPEED BOOST 8 SEC", 1.6f);
            break;
        case PICKUP_FAST_LASER:
            game.temporary_weapon = TEMP_WEAPON_FAST_LASER;
            game.temporary_weapon_time = TEMP_WEAPON_SECONDS;
            game.fire_cooldown = 0.0f;
            set_message("FAST LASER 15 SEC", 1.7f);
            break;
        case PICKUP_PHASE_WAVE:
            game.temporary_weapon = TEMP_WEAPON_PHASE_WAVE;
            game.temporary_weapon_time = TEMP_WEAPON_SECONDS;
            game.fire_cooldown = 0.0f;
            set_message("PHASE WAVE 15 SEC", 1.7f);
            break;
        case PICKUP_KIND_COUNT:
            return;
    }
    game.trauma = fmaxf(game.trauma, 0.20f);
    pickup->active = false;
    play_rumble(pickup->kind == PICKUP_FAST_LASER ||
                pickup->kind == PICKUP_PHASE_WAVE ? 5 :
                (pickup->kind == PICKUP_LASER_CORE ? 4 : 3));
    play_sound(sfx_gate, 230, 128);
}

static void update_pickups(float dt) {
    const float player_z = game.distance + PLAYER_Z;
    int i;
    for(i = 0; i < MAX_PICKUPS; ++i) {
        pickup_t *pickup = &pickups[i];
        float dx, dy, dz;
        if(!pickup->active)
            continue;
        pickup->spin += dt * 2.8f;
        pickup->life -= dt;
        dx = game.player_x - pickup->x;
        dy = game.player_y - pickup->y;
        dz = player_z - pickup->z;
        if(dz < 0.0f && dz > -260.0f && dx * dx + dy * dy < 3600.0f) {
            const float pull = clampf(dt * (dz > -115.0f ? 5.5f : 2.0f),
                                      0.0f, 0.55f);
            pickup->x = lerpf(pickup->x, game.player_x, pull);
            pickup->y = lerpf(pickup->y, game.player_y, pull);
            pickup->z += clampf(dz, -95.0f, 95.0f) * pull * 0.72f;
        }
        if(fabsf(dz) < 12.0f && fabsf(dx) < 8.0f && fabsf(dy) < 8.0f) {
            collect_pickup(pickup);
            continue;
        }
        if(pickup->life <= 0.0f || pickup->z < game.distance - 24.0f)
            pickup->active = false;
    }
}

static enemy_t *active_guardian(void) {
    int i;
    for(i = 0; i < MAX_ENEMIES; ++i) {
        if(enemies[i].active && enemies[i].type == 3)
            return &enemies[i];
    }
    return NULL;
}

static void check_scenery_collision(vec3_t previous_player_world,
                                    vec3_t current_player_world) {
    const float player_z = current_player_world.z;
    const int center_segment = (int)floorf(player_z / SCENERY_SEGMENT);
    int offset;

    if(game.scenery_hit_cooldown > 0.0f)
        return;
    for(offset = -1; offset <= 1; ++offset) {
        const int segment = center_segment + offset;
        const float z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(z / BIOME_LENGTH)) & 3;
        const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                    (uint32_t)biome * 0x51ed270bu);
        const float side = scenery_side_for_segment(biome, segment, z);
        const float object_x = side * (70.0f + (float)((h >> 8) & 63u));
        const float base = terrain_height_local(object_x, z);
        const int variant = scenery_variant_for_segment(biome, segment, z);
        const vec3_t collision_point = segment_point_at_z(
            previous_player_world, current_player_world, z);
        const float collision_x = collision_point.x - path_center(z);
        const float collision_y = collision_point.y - path_elevation(z);
        float arch_span = 0.0f;
        float arch_base_y = 0.0f;
        float arch_height = 0.0f;
        float arch_thickness = 0.0f;
        float object_radius = 0.0f;
        float object_height = 0.0f;
        bool impact = false;
        bool signature_impact = false;
        const bool near_regular = fabsf(collision_point.z - z) <= 18.0f &&
                                  !is_signature_segment(segment);

        if(near_regular && biome == 0) {
            if(variant == 1) {
                arch_span = 46.0f; arch_base_y = 1.0f;
                arch_height = 48.0f; arch_thickness = 5.5f;
            }
            else if(variant == 3) {
                arch_span = 39.0f; arch_base_y = 3.0f;
                arch_height = 52.0f; arch_thickness = 2.5f;
            }
            else {
                object_radius = variant == 4 ? 18.0f : 10.0f;
                object_height = variant == 0 ? 48.0f :
                                (variant == 2 ? 47.0f : 62.0f);
            }
        }
        else if(near_regular && biome == 1) {
            if(variant == 2) {
                arch_span = 38.0f; arch_base_y = 0.0f;
                arch_height = 54.0f; arch_thickness = 5.0f;
            }
            else {
                object_radius = variant == 4 ? 19.0f : 13.0f;
                object_height = variant <= 1 ? 61.0f :
                                (variant == 3 ? 44.0f : 15.0f);
            }
        }
        else if(near_regular && biome == 2) {
            if(variant == 2) {
                const float rock_y = 67.0f +
                    (float)((h >> 17) & 31u);
                const float rock_radius = 11.0f +
                    (float)((h >> 22) & 7u);
                if(fabsf(collision_x - object_x) < rock_radius + 4.0f &&
                   collision_y > rock_y - rock_radius * 1.2f - 4.0f &&
                   collision_y < rock_y + rock_radius * 0.8f + 4.0f)
                    impact = true;
            }
            else if(variant == 4) {
                arch_span = 42.0f; arch_base_y = 3.0f;
                arch_height = 59.0f; arch_thickness = 3.2f;
            }
            else {
                object_radius = 11.0f;
                object_height = variant == 3 ? 78.0f :
                                49.0f + (float)((h >> 19) & 15u);
            }
        }
        else if(near_regular) {
            if(variant == 2 || variant == 4) {
                arch_span = variant == 2 ? 40.0f : 49.0f;
                arch_base_y = -1.0f;
                arch_height = variant == 2 ? 45.0f : 57.0f;
                arch_thickness = variant == 2 ? 3.5f : 4.2f;
            }
            else {
                object_radius = variant == 3 ? 17.0f : 12.0f;
                object_height = variant == 0 ? 48.0f :
                                (variant == 3 ? 52.0f : 65.0f);
            }
        }

        /* Side dressing remains part of the biome during a challenge, while
           only center-spanning collision geometry yields the readable lane. */
        if(world_z_reserved_by_traversal(
               z, COURSE_TRAVERSAL_SCENERY_CLEARANCE))
            arch_span = 0.0f;

        if(near_regular && arch_span > 0.0f &&
           player_hits_material_arch(collision_x, collision_y,
                                     arch_span, arch_base_y,
                                     arch_height, arch_thickness))
            impact = true;
        else if(near_regular && object_radius > 0.0f &&
                fabsf(collision_x - object_x) < object_radius &&
                collision_y > base - 4.0f &&
                collision_y < base + object_height)
            impact = true;

        signature_impact = player_hits_signature_arch(
            biome, segment, z,
            previous_player_world, current_player_world);
        if(!impact && signature_impact)
            impact = true;

        if(impact) {
            const float push = arch_span > 0.0f || signature_impact ?
                (collision_x >= 0.0f ? -1.0f : 1.0f) :
                (collision_x >= object_x ? 1.0f : -1.0f);
            game.player_x = clampf(game.player_x + push * 7.0f,
                                   PLAYER_MIN_X, PLAYER_MAX_X);
            game.player_vx += push * 34.0f;
            game.scenery_hit_cooldown = 0.46f;
            set_message("STRUCTURE IMPACT", 0.9f);
            damage_player(14.0f);
            return;
        }
    }
}

static void update_spawning(void) {
    if(active_guardian())
        /* The guardian owns the chapter. Cursors remain parked on the next
           authored beats; its destruction handler discards only events that
           are genuinely inside the protected reward window. */
        return;

    while(game.next_wave_z < game.distance +
          (course_wave_is_guardian(game.next_wave_z) ? 280.0f : 1120.0f)) {
        if(game.next_wave_z <= game.distance + PLAYER_Z + 80.0f) {
            game.next_wave_z = next_course_wave_z(game.next_wave_z + 1.0f);
            continue;
        }
        if(!spawn_wave(game.next_wave_z))
            break;
        game.next_wave_z = next_course_wave_z(game.next_wave_z + 1.0f);
        if(active_guardian())
            break;
    }
    if(active_guardian())
        return;
    while(game.next_gate_z < game.distance + 1260.0f) {
        if(game.next_gate_z > game.distance + PLAYER_Z + 120.0f &&
           !spawn_gate(game.next_gate_z))
            break;
        game.next_gate_z = next_course_traversal_z(game.next_gate_z + 1.0f);
    }
}

static void spawn_exhaust(float dt, bool boosting) {
    const palette_t *palette = current_palette();
    int side;

    game.exhaust_timer -= dt;
    if(game.exhaust_timer > 0.0f)
        return;
    game.exhaust_timer = boosting ? 0.018f : 0.032f;
    for(side = -1; side <= 1; side += 2) {
        const vec3_t engine = player_model_point(
            (float)side * 3.0f, -0.2f, -6.8f);
        const float back_speed =
            -18.0f - random_unit() * (boosting ? 17.0f : 9.0f);
        const float slope_x = (path_center(engine.z + 12.0f) -
                               path_center(engine.z - 12.0f)) / 24.0f;
        const float slope_y = (path_elevation(engine.z + 12.0f) -
                               path_elevation(engine.z - 12.0f)) / 24.0f;
        spawn_particle(engine.x + random_signed() * 0.22f,
                       engine.y + random_signed() * 0.18f,
                       engine.z - 0.35f,
                       slope_x * back_speed + random_signed() * 0.75f,
                       slope_y * back_speed + random_signed() * 0.55f,
                       back_speed,
                       boosting ? 0.44f : 0.30f,
                       boosting ? 1.75f : 1.12f,
                       color_lerp(palette->river,
                                  (color3_t){0.72f,0.90f,1.0f}, 0.58f),
                       PARTICLE_EXHAUST);
    }
}

static void enter_title(void) {
    const int title_biome = (int)(random_u32() >> 30);
    /* Frame each biome's authored landmark in the attract shot instead of
       dropping the camera at an arbitrary point in its course. */
    const float title_offset = COURSE_SIGNATURE_LOCAL_Z - 620.0f +
                               random_unit() * 240.0f;

    memset(enemies, 0, sizeof(enemies));
    memset(player_shots, 0, sizeof(player_shots));
    memset(enemy_shots, 0, sizeof(enemy_shots));
    memset(gates, 0, sizeof(gates));
    memset(pickups, 0, sizeof(pickups));
    game.mode = MODE_TITLE;
    game.distance = (float)title_biome * BIOME_LENGTH + title_offset;
    game.speed = 35.0f;
    game.player_x = 0.0f;
    game.player_y = 35.0f;
    game.player_vx = 0.0f;
    game.player_vy = 0.0f;
    game.camera_x = 0.0f;
    game.camera_y = 34.0f;
    game.bank = 0.0f;
    game.barrel_roll = 0.0f;
    game.bomb_wave = 0.0f;
    game.temporary_weapon = TEMP_WEAPON_NONE;
    game.temporary_weapon_time = 0.0f;
    game.speed_boost_time = 0.0f;
    game.weapon_shot_counter = 0;
    game.palette_index = title_biome;
    game.last_palette_index = title_biome;
    game.message_time = 0.0f;
    game.camera_focal = FOCAL_CRUISE;
    game.trauma = 0.0f;
    title_menu_item = TITLE_MENU_START;
    title_sound_test = false;
    request_next_shuffled_music(MUSIC_TITLE_VOLUME);
    printf("Gravity Wave: title backdrop %s.\n",
           palettes[title_biome].name);
}

static bool update_game(const input_t *input, float dt) {
    const bool left_trigger_pressed = input->left_trigger > 0.32f &&
                                      !game.previous_left_trigger;
    const bool right_trigger_pressed = input->right_trigger > 0.32f &&
                                       !game.previous_right_trigger;
    bool boosting = false;

    game.time += dt;
    game.trauma = fmaxf(0.0f, game.trauma - dt * 1.70f);
    update_music(dt);

    if(!input->connected && !game.controller_notice) {
        printf("Gravity Wave: no controller detected; waiting for hot-plug.\n");
        game.controller_notice = true;
    }
    else if(input->connected) {
        game.controller_notice = false;
    }

    if(game.mode == MODE_TITLE) {
        int title_palette;
        game.distance += 31.0f * dt;
        title_palette = ((int)(game.distance / BIOME_LENGTH)) & 3;
        if(title_palette != game.last_palette_index) {
            game.palette_index = title_palette;
            game.last_palette_index = title_palette;
        }
        game.player_x = fsin(game.time * 0.72f) * 10.0f;
        game.player_y = 35.0f + fsin(game.time * 0.49f) * 3.0f;
        game.bank = fsin(game.time * 0.72f) * -0.30f;
        game.camera_x = game.player_x * 0.78f;
        game.camera_y = 34.0f + (game.player_y - 35.0f) * 0.80f;
        game.camera_focal = lerpf(game.camera_focal, FOCAL_CRUISE,
                                  clampf(dt * 3.6f, 0.0f, 1.0f));
        spawn_exhaust(dt, false);
        update_particles(dt);
        if(title_sound_test) {
            int requested_track = sound_test_track;
            if(input->pressed & CONT_DPAD_LEFT)
                requested_track = (requested_track + MUSIC_TRACK_COUNT - 1) %
                                  MUSIC_TRACK_COUNT;
            else if(input->pressed & CONT_DPAD_RIGHT)
                requested_track = (requested_track + 1) % MUSIC_TRACK_COUNT;
            if(requested_track != sound_test_track) {
                sound_test_track = requested_track;
                audition_music_track(sound_test_track);
                play_sound(sfx_gate, 154, 128);
                printf("Gravity Wave sound test: %02d/%02d %s.\n",
                       sound_test_track + 1, MUSIC_TRACK_COUNT,
                       soundtrack_defs[sound_test_track].name);
            }
            if(input->pressed & CONT_A) {
                audition_music_track(sound_test_track);
                play_sound(sfx_gate, 154, 128);
            }
            else if(input->pressed & CONT_B) {
                seed_music_shuffle_after_track(sound_test_track);
                title_sound_test = false;
                play_sound(sfx_gate, 132, 128);
            }
        }
        else {
            if(input->pressed & CONT_DPAD_UP) {
                title_menu_item = (title_menu_item + TITLE_MENU_ITEM_COUNT - 1) %
                                  TITLE_MENU_ITEM_COUNT;
                play_sound(sfx_gate, 128, 128);
            }
            else if(input->pressed & CONT_DPAD_DOWN) {
                title_menu_item = (title_menu_item + 1) %
                                  TITLE_MENU_ITEM_COUNT;
                play_sound(sfx_gate, 128, 128);
            }
            if(input->pressed & (CONT_START | CONT_A)) {
                if(title_menu_item == TITLE_MENU_START)
                    reset_game();
                else if(title_menu_item == TITLE_MENU_SOUND_TEST) {
                    title_sound_test = true;
                    sound_test_track = current_music_track >= 0 ?
                        current_music_track : 0;
                    audition_music_track(sound_test_track);
                    play_sound(sfx_gate, 154, 128);
                }
                else
                    return false;
            }
            else if(input->pressed & CONT_B)
                return false;
        }
    }
    else if(game.mode == MODE_PAUSED) {
        if(input->pressed & CONT_START) {
            game.mode = MODE_PLAYING;
            game.message_time = 0.0f;
        }
        else if(input->pressed & CONT_B)
            enter_title();
    }
    else if(game.mode == MODE_GAME_OVER) {
        update_particles(dt);
        if(input->pressed & (CONT_START | CONT_A))
            reset_game();
        else if(input->pressed & CONT_B)
            enter_title();
    }
    else {
        const vec3_t previous_player_world = route_position(
            game.player_x, game.player_y, game.distance + PLAYER_Z);
        const float rank = clampf(game.distance / 11500.0f, 0.0f, 1.0f);
        const float target_vx = input->x * 68.0f;
        const float target_vy = -input->y * 53.0f;
        float cruise_speed = 96.0f + rank * 58.0f;
        const float response = clampf(dt * 5.8f, 0.0f, 1.0f);
        const bool speed_boost_active = game.speed_boost_time > 0.0f;
        vec3_t current_player_world;

        if(input->pressed & CONT_START) {
            game.mode = MODE_PAUSED;
            set_message("PAUSED", 999.0f);
        }
        else {
            game.player_vx = lerpf(game.player_vx, target_vx, response);
            game.player_vy = lerpf(game.player_vy, target_vy, response);
            game.player_x += game.player_vx * dt;
            game.player_y += game.player_vy * dt;
            if(game.player_x < PLAYER_MIN_X || game.player_x > PLAYER_MAX_X) {
                game.player_x = clampf(game.player_x, PLAYER_MIN_X, PLAYER_MAX_X);
                game.player_vx *= -0.22f;
            }
            if(game.player_y < PLAYER_MIN_Y || game.player_y > PLAYER_MAX_Y) {
                game.player_y = clampf(game.player_y, PLAYER_MIN_Y, PLAYER_MAX_Y);
                game.player_vy *= -0.22f;
            }

            game.bank = lerpf(game.bank,
                              clampf(-game.player_vx / 62.0f, -0.92f, 0.92f),
                              clampf(dt * 7.5f, 0.0f, 1.0f));
            game.camera_x = lerpf(game.camera_x, game.player_x * 0.86f,
                                  clampf(dt * 2.5f, 0.0f, 1.0f));
            game.camera_y = lerpf(game.camera_y,
                                  34.0f + (game.player_y - 35.0f) * 0.85f,
                                  clampf(dt * 2.2f, 0.0f, 1.0f));

            if(speed_boost_active ||
               ((input->buttons & CONT_B) && game.boost > 0.0f)) {
                boosting = true;
                cruise_speed += speed_boost_active ? 86.0f : 70.0f;
                if(!speed_boost_active)
                    game.boost -= 31.0f * dt;
            }
            else {
                game.boost += 17.0f * dt;
            }
            if(input->buttons & CONT_X)
                cruise_speed -= 38.0f;
            game.camera_focal = lerpf(
                game.camera_focal,
                boosting ? FOCAL_BOOST :
                ((input->buttons & CONT_X) ? FOCAL_BRAKE : FOCAL_CRUISE),
                clampf(dt * 4.2f, 0.0f, 1.0f));
            game.boost = clampf(game.boost, 0.0f, 100.0f);
            game.speed = lerpf(game.speed, cruise_speed,
                               clampf(dt * 2.8f, 0.0f, 1.0f));
            game.distance += game.speed * dt;
            game.survival_time += dt;
            game.score_fraction += game.speed * dt * 0.055f;
            if(game.score_fraction >= 1.0f) {
                const int whole = (int)game.score_fraction;
                game.score += whole;
                game.score_fraction -= (float)whole;
            }

            game.fire_cooldown -= dt;
            if(game.speed_boost_time > 0.0f)
                game.speed_boost_time = fmaxf(
                    0.0f, game.speed_boost_time - dt);
            if(game.temporary_weapon_time > 0.0f) {
                game.temporary_weapon_time = fmaxf(
                    0.0f, game.temporary_weapon_time - dt);
                if(game.temporary_weapon_time <= 0.0f) {
                    game.temporary_weapon = TEMP_WEAPON_NONE;
                    set_message("STANDARD LASER ONLINE", 1.25f);
                }
            }
            if((input->buttons & CONT_A) && game.fire_cooldown <= 0.0f)
                fire_player_weapon();
            if((input->pressed & CONT_Y) && game.bombs > 0)
                trigger_bomb();

            game.roll_cooldown -= dt;
            if((left_trigger_pressed || right_trigger_pressed) &&
               game.roll_cooldown <= 0.0f) {
                game.barrel_roll = left_trigger_pressed ? 0.001f : -0.001f;
                game.roll_cooldown = 0.72f;
                game.hit_cooldown = fmaxf(game.hit_cooldown, 0.38f);
            }
            if(game.barrel_roll != 0.0f) {
                const float direction = game.barrel_roll > 0.0f ? 1.0f : -1.0f;
                game.barrel_roll += direction * TAU * 1.65f * dt;
                if(fabsf(game.barrel_roll) >= TAU)
                    game.barrel_roll = 0.0f;
            }

            game.hit_cooldown -= dt;
            game.scenery_hit_cooldown = fmaxf(
                0.0f, game.scenery_hit_cooldown - dt);
            if(game.combo_timer > 0.0f) {
                game.combo_timer -= dt;
                if(game.combo_timer <= 0.0f)
                    game.combo = 0;
            }
            if(game.message_time > 0.0f)
                game.message_time -= dt;
            if(game.bomb_wave > 0.0f) {
                game.bomb_wave += dt * 1.28f;
                if(game.bomb_wave >= 1.0f)
                    game.bomb_wave = 0.0f;
            }

            spawn_exhaust(dt, boosting);
            update_spawning();
            current_player_world = route_position(
                game.player_x, game.player_y, game.distance + PLAYER_Z);
            update_player_shots(dt);
            update_enemy_shots(dt, previous_player_world,
                               current_player_world);
            if(game.mode == MODE_PLAYING)
                update_enemies(dt, previous_player_world,
                               current_player_world);
            if(game.mode == MODE_PLAYING) {
                update_gates(dt, previous_player_world,
                             current_player_world);
                update_pickups(dt);
            }
            update_particles(dt);

            if(game.mode == MODE_PLAYING) {
                const float ground = terrain_height_local(game.player_x,
                    game.distance + PLAYER_Z) + 5.0f;
                if(game.player_y < ground) {
                    game.player_y = ground;
                    game.player_vy = fmaxf(game.player_vy, 20.0f);
                    damage_player(9.0f);
                }
            }
            if(game.mode == MODE_PLAYING) {
                current_player_world = route_position(
                    game.player_x, game.player_y,
                    game.distance + PLAYER_Z);
                check_scenery_collision(previous_player_world,
                                        current_player_world);
            }

            if(game.mode == MODE_PLAYING) {
                game.palette_index = ((int)(game.distance / BIOME_LENGTH)) %
                                     ARRAY_COUNT(palettes);
                if(game.palette_index != game.last_palette_index &&
                   !active_guardian()) {
                    const int chapter = course_chapter_at(game.distance);
                    if(game.music_chapter != chapter) {
                        game.music_chapter = chapter;
                        request_next_shuffled_music(MUSIC_GAME_VOLUME);
                    }
                    if(set_ambient_message(
                           palettes[game.palette_index].name, 2.3f)) {
                        game.last_palette_index = game.palette_index;
                        play_sound(sfx_gate, 180, 128);
                    }
                }
                {
                    const int chapter = course_chapter_at(game.distance);
                    const float local_z = course_local_z(game.distance);
                    if(local_z >= COURSE_SIGNATURE_LOCAL_Z - 600.0f &&
                       local_z <= COURSE_SIGNATURE_LOCAL_Z - 350.0f &&
                       game.last_course_act != chapter &&
                       !active_guardian() &&
                       set_ambient_message(
                           course_landmark_names[chapter & 3], 2.2f)) {
                        game.last_course_act = chapter;
                        printf("Gravity Wave course: chapter %d landmark %s.\n",
                               chapter + 1,
                               course_landmark_names[chapter & 3]);
                    }
                }
            }
        }
    }

    game.previous_buttons = input->buttons;
    game.previous_left_trigger = input->left_trigger > 0.32f;
    game.previous_right_trigger = input->right_trigger > 0.32f;
    return true;
}

#ifdef GRAVITY_WAVE_AUTOTEST_SOUND_TEST
static bool force_music_phrase_boundary_for_test(input_t *input) {
    const int stopped_bank = active_music_bank;
    const int stopped_left = music_left_channels[stopped_bank];
    const int stopped_right = music_right_channels[stopped_bank];
    bool running;

    current_music_section = MUSIC_SECTION_COUNT - 1;
    music_section_time =
        music_section_duration[current_music_track][current_music_section] +
        0.25f;
    music_playhead_armed = false;
    snd_sfx_stop(stopped_left);
    snd_sfx_stop(stopped_right);
    /* AICA stop commands are asynchronous. Temporarily detach the stopped
       bank so the deterministic test exercises the boundary immediately
       instead of depending on one emulator audio-service tick. */
    music_left_channels[stopped_bank] = -1;
    music_right_channels[stopped_bank] = -1;
    input->pressed = 0;
    running = update_game(input, 1.0f / 60.0f);
    music_left_channels[stopped_bank] = stopped_left;
    music_right_channels[stopped_bank] = stopped_right;
    return running;
}

static bool run_title_sound_test_self_test(void) {
    input_t input;
    const int opening_track = current_music_track;
    int cursor_before_loop;
    int expected_shuffled_track;
    int expected_track;
    bool passed = game.mode == MODE_TITLE &&
                  title_menu_item == TITLE_MENU_START &&
                  !title_sound_test && opening_track >= 0;

    memset(&input, 0, sizeof(input));
    input.connected = true;
    input.pressed = CONT_DPAD_DOWN;
    passed = update_game(&input, 1.0f / 60.0f) && passed &&
             title_menu_item == TITLE_MENU_SOUND_TEST;

    input.pressed = CONT_A;
    passed = update_game(&input, 1.0f / 60.0f) && passed &&
             title_sound_test && sound_test_track == opening_track &&
             current_music_track == opening_track;

    /* Start is deliberately inert while browsing: it must never launch a run
       when a keyboard user is trying to restart or compare a song. */
    input.pressed = CONT_START;
    passed = update_game(&input, 1.0f / 60.0f) && passed &&
             title_sound_test && game.mode == MODE_TITLE;

    expected_track = (opening_track + 1) % MUSIC_TRACK_COUNT;
    input.pressed = CONT_DPAD_RIGHT;
    passed = update_game(&input, 1.0f / 60.0f) && passed &&
             sound_test_track == expected_track &&
             current_music_track == expected_track &&
             current_music_section == 0;

    input.pressed = CONT_A;
    passed = update_game(&input, 1.0f / 60.0f) && passed &&
             title_sound_test && current_music_track == expected_track &&
             current_music_section == 0;

    /* Run the selected song across its real C-to-A boundary. Sound Test must
       keep the UI and audio locked to the same selection without touching the
       automatic shuffle pass. */
    cursor_before_loop = music_shuffle_cursor;
    passed = force_music_phrase_boundary_for_test(&input) && passed &&
             title_sound_test && current_music_track == expected_track &&
             current_music_section == 0 &&
             music_shuffle_cursor == cursor_before_loop;

    input.pressed = CONT_B;
    passed = update_game(&input, 1.0f / 60.0f) && passed &&
             !title_sound_test && game.mode == MODE_TITLE &&
             title_menu_item == TITLE_MENU_SOUND_TEST;

    /* Leaving Sound Test seeds a fresh album pass with the auditioned track
       already heard. Overwriting a pending mode-change request cannot consume
       the next song; only a successful phrase-boundary start commits it. */
    expected_shuffled_track = music_shuffle_bag[music_shuffle_cursor];
    cursor_before_loop = music_shuffle_cursor;
    request_next_shuffled_music(MUSIC_GAME_VOLUME);
    request_next_shuffled_music(MUSIC_TITLE_VOLUME);
    passed = passed && pending_music_shuffle &&
             music_shuffle_cursor == cursor_before_loop;
    passed = force_music_phrase_boundary_for_test(&input) && passed &&
             current_music_track == expected_shuffled_track &&
             current_music_section == 0 &&
             music_shuffle_cursor == cursor_before_loop + 1;

    printf("Gravity Wave sound test menu: %s (%s -> %s, C/A loop, "
           "deferred shuffle, safe Start).\n",
           passed ? "PASS" : "FAIL",
           soundtrack_defs[opening_track >= 0 ? opening_track : 0].name,
           soundtrack_defs[expected_track].name);
    return passed;
}
#endif

static float guardian_model_roll(const enemy_t *enemy) {
    switch(enemy->biome & 3) {
        case 0:
            return fsin(enemy->phase * 1.30f) * 0.14f;
        case 1:
            return fsin(enemy->phase * 0.55f) * 0.06f;
        case 2:
            return enemy->phase * 0.42f;
        default:
            return fsin(enemy->phase * 1.85f) * 0.32f;
    }
}

static float guardian_model_pitch(const enemy_t *enemy) {
    switch(enemy->biome & 3) {
        case 0:
            return fsin(enemy->phase * 0.80f) * 0.08f;
        case 1:
            return fcos(enemy->phase * 0.60f) * 0.05f;
        case 2:
            return fsin(enemy->phase * 1.16f) * 0.18f;
        default:
            return fcos(enemy->phase * 1.45f) * 0.24f;
    }
}

static vec3_t enemy_model_point(const enemy_t *enemy,
                                float lx, float ly, float lz) {
    const float roll = enemy->type == 3 ? guardian_model_roll(enemy) :
        fsin(enemy->phase * 1.6f) * 0.33f;
    const float cr = fcos(roll);
    const float sr = fsin(roll);
    const float scale = enemy->type == 3 ?
                        guardian_profiles[enemy->biome & 3].model_scale :
                        (enemy->type == 2 ? 1.45f :
                         (enemy->type == 1 ? 1.12f : 0.92f));
    const float rx = (lx * cr - ly * sr) * scale;
    const float ry0 = (lx * sr + ly * cr) * scale;
    const float yaw = path_heading(enemy->z) + PI +
        (enemy->type == 1 ? fsin(enemy->phase) * 0.18f : 0.0f);
    const float pitch = route_model_pitch(
        enemy->z, yaw,
        enemy->type == 3 ? guardian_model_pitch(enemy) : 0.0f);
    const float cp = fcos(pitch);
    const float sp = fsin(pitch);
    const float rz0 = lz * scale;
    const float ry = ry0 * cp - rz0 * sp;
    const float rz = ry0 * sp + rz0 * cp;
    const float cy = fcos(yaw);
    const float sy = fsin(yaw);
    return (vec3_t){path_center(enemy->z) + enemy->x + rx * cy + rz * sy,
                    path_elevation(enemy->z) + enemy->y + ry,
                    enemy->z - rx * sy + rz * cy};
}

static void draw_mesh_instance(const gravity_wave_mesh_t *mesh, vec3_t origin,
                               float yaw, float pitch, float roll, float scale,
                               color3_t tint, bool flash) {
    vec3_t transformed[96];
    screen_point_t projected[96];
    const float cy = fcos(yaw);
    const float sy = fsin(yaw);
    const float rigid_pitch = route_model_pitch(origin.z, yaw, pitch);
    const float cp = fcos(rigid_pitch);
    const float sp = fsin(rigid_pitch);
    const float cr = fcos(roll);
    const float sr = fsin(roll);
    int material;
    int i;

    if(mesh->vertex_count > ARRAY_COUNT(transformed))
        return;
    if(origin.z - game.distance > FAR_PLANE + mesh->radius * scale ||
       origin.z - game.distance < NEAR_PLANE - mesh->radius * scale)
        return;

    for(i = 0; i < mesh->vertex_count; ++i) {
        const gravity_wave_mesh_vertex_t *vertex = &mesh->vertices[i];
        const float x0 = vertex->x * scale;
        const float y0 = vertex->y * scale;
        const float z0 = vertex->z * scale;
        const float xr = x0 * cr - y0 * sr;
        const float yr = x0 * sr + y0 * cr;
        const float yp = yr * cp - z0 * sp;
        const float zp = yr * sp + z0 * cp;
        transformed[i].x = origin.x + xr * cy + zp * sy;
        transformed[i].y = origin.y + yp;
        transformed[i].z = origin.z - xr * sy + zp * cy;
        /* Rigid actors use one route elevation and grade at their origin.
           Re-evaluating elevation per vertex shears long meshes on crests. */
        project_absolute_world(transformed[i], &projected[i]);
    }

    for(material = 0; material < 4; ++material) {
        const int texture_id = material == 0 ? GRAVITY_WAVE_TEX_HULL_ALLIED :
                               material == 1 ? GRAVITY_WAVE_TEX_HULL_HOSTILE :
                               material == 2 ? GRAVITY_WAVE_TEX_CANOPY_ENERGY :
                                               GRAVITY_WAVE_TEX_ANCIENT_MACHINE;
        bool header_submitted = false;

        for(i = 0; i < mesh->face_count; ++i) {
            const gravity_wave_mesh_face_t *face = &mesh->faces[i];
            const gravity_wave_mesh_vertex_t *va;
            const gravity_wave_mesh_vertex_t *vb;
            const gravity_wave_mesh_vertex_t *vc;
            vec3_t ab, ac;
            float nx, ny, nz, inv_length, diffuse;
            color3_t lit;
            uint32_t color;
            pvr_vertex_t triangle[3];

            if(face->material != material ||
               !projected[face->a].valid ||
               !projected[face->b].valid ||
               !projected[face->c].valid)
                continue;

            if(!header_submitted) {
                submit_poly_header(&texture_headers[texture_id]);
                header_submitted = true;
            }

            ab.x = transformed[face->b].x - transformed[face->a].x;
            ab.y = transformed[face->b].y - transformed[face->a].y;
            ab.z = transformed[face->b].z - transformed[face->a].z;
            ac.x = transformed[face->c].x - transformed[face->a].x;
            ac.y = transformed[face->c].y - transformed[face->a].y;
            ac.z = transformed[face->c].z - transformed[face->a].z;
            nx = ab.y * ac.z - ab.z * ac.y;
            ny = ab.z * ac.x - ab.x * ac.z;
            nz = ab.x * ac.y - ab.y * ac.x;
            inv_length = frsqrt(fmaxf(nx * nx + ny * ny + nz * nz, 0.0001f));
            diffuse = 0.47f + fmaxf(0.0f,
                nx * inv_length * -0.34f + ny * inv_length * 0.82f +
                nz * inv_length * -0.24f) * 0.62f;
            if(material == 2)
                diffuse = 1.16f;
            lit = flash ? (color3_t){1.0f,0.76f,0.68f} :
                          color_scale(tint, diffuse);
            color = pack_color(1.0f, lit);

            va = &mesh->vertices[face->a];
            vb = &mesh->vertices[face->b];
            vc = &mesh->vertices[face->c];
            make_textured_vertex(&triangle[0], &projected[face->a],
                                 va->u, va->v, color, false);
            make_textured_vertex(&triangle[1], &projected[face->b],
                                 vb->u, vb->v, color, false);
            make_textured_vertex(&triangle[2], &projected[face->c],
                                 vc->u, vc->v, color, true);
            pvr_prim(triangle, sizeof(triangle));
        }
    }
}

static void draw_guardian_model(const enemy_t *enemy) {
    const int biome = enemy->biome & 3;
    const guardian_profile_t *profile = &guardian_profiles[biome];
    const palette_t *palette = &palettes[biome];
    const float yaw = path_heading(enemy->z) + PI;
    const float pitch = guardian_model_pitch(enemy);
    const float roll = guardian_model_roll(enemy);
    const bool flash = enemy->hit_flash > 0.025f;
    const vec3_t origin = {path_center(enemy->z) + enemy->x,
                           path_elevation(enemy->z) + enemy->y,
                           enemy->z};
    const color3_t core_tint = color_lerp(
        (color3_t){0.96f,0.90f,0.82f}, palette->accent, 0.12f);
    const color3_t pod_tint = color_lerp(
        (color3_t){0.88f,0.84f,0.80f}, palette->enemy, 0.30f);
    int pod;

    draw_mesh_instance(&mesh_guardian, origin, yaw, pitch, roll,
                       profile->model_scale, core_tint, flash);
    switch(biome) {
        case 0: /* Carrier-wide outriggers create the Aegis silhouette. */
            for(pod = -1; pod <= 1; pod += 2) {
                const vec3_t pod_origin = enemy_model_point(
                    enemy, (float)pod * 11.5f, -1.0f, -2.2f);
                draw_mesh_instance(&mesh_ace, pod_origin,
                    path_heading(pod_origin.z) + PI, pitch * 0.55f,
                    roll + (float)pod * 0.18f, 0.58f,
                    pod_tint, flash);
            }
            break;
        case 1: /* Armored side keeps and a crown tower form the Bastion. */
            for(pod = -1; pod <= 1; pod += 2) {
                const vec3_t pod_origin = enemy_model_point(
                    enemy, (float)pod * 9.5f, -2.0f, -2.8f);
                draw_mesh_instance(&mesh_bomber, pod_origin,
                    path_heading(pod_origin.z) + PI, pitch * 0.35f,
                    roll + (float)pod * 0.08f, 0.56f,
                    pod_tint, flash);
            }
            {
                const vec3_t crown = enemy_model_point(
                    enemy, 0.0f, 8.0f, -1.0f);
                draw_mesh_instance(&mesh_interceptor, crown,
                    path_heading(crown.z) + PI, pitch, roll + PI * 0.5f,
                    0.50f, pod_tint, flash);
            }
            break;
        case 2: /* Three independently readable shards orbit the Seraph core. */
            for(pod = 0; pod < 3; ++pod) {
                const float angle = enemy->phase * 0.82f +
                                    (float)pod * TAU / 3.0f;
                const vec3_t shard = enemy_model_point(
                    enemy, fcos(angle) * 9.5f,
                    fsin(angle) * 9.5f, -2.0f);
                draw_mesh_instance(&mesh_interceptor, shard,
                    path_heading(shard.z) + PI, pitch * 0.5f,
                    roll + angle, 0.53f, pod_tint, flash);
            }
            break;
        default: /* Opposed blades make an unmistakable furnace mantis. */
            for(pod = -1; pod <= 1; pod += 2) {
                const vec3_t claw = enemy_model_point(
                    enemy, (float)pod * 8.5f,
                    (float)pod * 6.5f, -1.0f);
                draw_mesh_instance(&mesh_ace, claw,
                    path_heading(claw.z) + PI,
                    pitch + (float)pod * 0.12f,
                    roll + (float)pod * 0.72f, 0.66f,
                    pod_tint, flash);
            }
            break;
    }
}

static void draw_enemy_model(const enemy_t *enemy, const palette_t *palette) {
    if(enemy->type == 3) {
        draw_guardian_model(enemy);
        return;
    }
    const palette_t *model_palette = enemy->type == 3 ?
        &palettes[enemy->biome & 3] : palette;
    const gravity_wave_mesh_t *mesh = enemy->type == 3 ? &mesh_guardian :
                                enemy->type == 2 ? &mesh_bomber :
                                enemy->type == 1 ? &mesh_ace :
                                                   &mesh_interceptor;
    const float scale = enemy->type == 3 ? 1.78f :
                        (enemy->type == 2 ? 0.92f : 0.78f);
    const float roll = fsin(enemy->phase * 1.6f) *
                       (enemy->type == 3 ? 0.08f : 0.33f);
    const float yaw = path_heading(enemy->z) + PI +
        (enemy->type == 1 ? fsin(enemy->phase) * 0.18f : 0.0f);
    const vec3_t origin = {path_center(enemy->z) + enemy->x,
                           path_elevation(enemy->z) + enemy->y,
                           enemy->z};
    const color3_t tint = enemy->type == 3 ?
        color_lerp((color3_t){0.96f,0.90f,0.82f},
                   model_palette->accent, 0.12f) :
        color_lerp((color3_t){0.94f,0.88f,0.88f},
                   model_palette->enemy, 0.16f);
    draw_mesh_instance(mesh, origin, yaw, 0.0f, roll, scale, tint,
                       enemy->hit_flash > 0.025f);
}

static void draw_enemies(const palette_t *palette) {
    int i;
    for(i = 0; i < MAX_ENEMIES; ++i) {
        if(enemies[i].active)
            draw_enemy_model(&enemies[i], palette);
    }
}

static vec3_t player_model_point(float lx, float ly, float lz) {
    const float roll = game.bank * 0.62f + game.barrel_roll +
                       camera_route_turn * 1.35f;
    const float world_z = game.distance + PLAYER_Z;
    const float yaw = path_heading(world_z);
    const float pitch = route_model_pitch(
        world_z, yaw,
        clampf(-game.player_vy * 0.007f, -0.30f, 0.30f));
    const float cr = fcos(roll);
    const float sr = fsin(roll);
    const float cp = fcos(pitch);
    const float sp = fsin(pitch);
    const float rx = lx * cr - ly * sr;
    const float ry0 = lx * sr + ly * cr;
    const float ry = ry0 * cp - lz * sp;
    const float rz = ry0 * sp + lz * cp;
    const float cy = fcos(yaw);
    const float sy = fsin(yaw);
    return (vec3_t){path_center(world_z) + game.player_x + rx * cy + rz * sy,
                    path_elevation(world_z) + game.player_y + ry,
                    world_z - rx * sy + rz * cy};
}

static void draw_player_ship(const palette_t *palette) {
    const bool flashing = game.hit_cooldown > 0.0f &&
                          ((int)(game.hit_cooldown * 18.0f) & 1);
    const float roll = game.bank * 0.62f + game.barrel_roll +
                       camera_route_turn * 1.35f;
    const float pitch = clampf(-game.player_vy * 0.007f, -0.30f, 0.30f);
    const float world_z = game.distance + PLAYER_Z;
    const vec3_t origin = {path_center(world_z) + game.player_x,
                           path_elevation(world_z) + game.player_y,
                           world_z};
    const color3_t hull = color_lerp((color3_t){0.98f,0.99f,1.0f},
                                     palette->river, 0.10f);
    draw_mesh_instance(&mesh_player, origin, path_heading(world_z), pitch, roll,
                       0.88f, hull, flashing);
}

static void draw_guardian_charge_nodes(const enemy_t *enemy,
                                       const palette_t *palette,
                                       float charge) {
    const int biome = enemy->biome & 3;
    const int count = guardian_shot_count(biome);
    const float pulse = 1.0f + fsin(game.time * 18.0f) * 0.14f;
    int node;

    for(node = 0; node < count; ++node) {
        float target_x;
        float target_y;
        float muzzle_x;
        float muzzle_y;
        vec3_t muzzle;
        guardian_shot_offsets(biome, enemy->fired, node,
                              &target_x, &target_y,
                              &muzzle_x, &muzzle_y);
        (void)target_x;
        (void)target_y;
        /* Exaggerate the authored muzzle formation during the charge so the
           player can read horizontal, radial, vertical, or diagonal danger. */
        muzzle = enemy_model_point(
            enemy, muzzle_x * 1.34f, muzzle_y * 1.55f, 14.5f);
        draw_absolute_textured_billboard(
            &sprite_additive_headers[2], muzzle,
            (6.2f + charge * 5.2f) * pulse,
            (6.2f + charge * 5.2f) * pulse,
            pack_color(0.28f + charge * 0.66f,
                       node == 0 ? palette->accent : palette->enemy));
    }
}

static void draw_enemy_glows(const palette_t *palette) {
    int i;
    for(i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *enemy = &enemies[i];
        screen_point_t point;
        vec3_t engine;
        float size;
        const palette_t *glow_palette;
        if(!enemy->active)
            continue;
        glow_palette = enemy->type == 3 ?
            &palettes[enemy->biome & 3] : palette;
        engine = enemy_model_point(enemy, 0.0f, 0.0f, -6.0f);
        if(!project_absolute_world(engine, &point))
            continue;
        size = clampf(point.z * (enemy->type == 3 ? 5200.0f : 2800.0f),
                      1.5f, enemy->type == 3 ? 25.0f :
                      (enemy->type == 2 ? 16.0f : 9.0f));
        draw_disc(&additive_header, point.x, point.y, size, point.z + 0.00001f, 8,
                  pack_color(0.75f, glow_palette->enemy),
                  pack_color(0.0f, glow_palette->accent));
        {
            const float charge_window = enemy->type == 3 ?
                fminf(0.72f,
                      guardian_profiles[enemy->biome & 3].fire_cadence * 0.72f) :
                0.55f;
            if(enemy->fire_timer > 0.0f &&
               enemy->fire_timer < charge_window) {
                const float charge = 1.0f -
                                     enemy->fire_timer / charge_window;
                if(enemy->type == 3)
                    draw_guardian_charge_nodes(
                        enemy, glow_palette, charge);
                else {
                    const vec3_t muzzle = enemy_model_point(
                        enemy, 0.0f, 0.0f, 8.0f);
                    draw_absolute_textured_billboard(
                        &sprite_additive_headers[2], muzzle,
                        9.0f, 9.0f,
                        pack_color(0.30f + charge * 0.68f,
                                   glow_palette->enemy));
                }
            }
        }
    }
}

static void draw_player_glow(const palette_t *palette) {
    int side;
    for(side = -1; side <= 1; side += 2) {
        screen_point_t point;
        screen_point_t tail;
        const vec3_t engine = player_model_point((float)side * 3.0f,
                                                  -0.2f, -6.8f);
        const vec3_t plume_tail = player_model_point(
            (float)side * 3.0f, -0.2f,
            game.speed > 145.0f ? -18.0f : -12.5f);
        if(project_absolute_world(engine, &point)) {
            const float pulse = 1.0f + fsin(game.time * 26.0f) * 0.12f;
            if(project_absolute_world(plume_tail, &tail)) {
                const float outer_width = game.speed > 145.0f ? 10.0f : 6.5f;
                draw_line(&additive_header, tail.x, tail.y, tail.z,
                          point.x, point.y, point.z, outer_width * pulse,
                          pack_color(0.0f, palette->river),
                          pack_color(0.62f, palette->river));
                draw_line(&additive_header, tail.x, tail.y,
                          tail.z + 0.00001f, point.x, point.y,
                          point.z + 0.00001f, outer_width * 0.28f,
                          pack_color(0.0f, (color3_t){0.72f,0.90f,1.0f}),
                          pack_color(0.94f, (color3_t){0.86f,0.98f,1.0f}));
            }
            draw_disc(&additive_header, point.x, point.y,
                      (game.speed > 145.0f ? 13.0f : 8.5f) * pulse,
                      point.z + 0.001f, 10,
                      pack_color(0.90f, (color3_t){0.72f, 0.96f, 1.0f}),
                      pack_color(0.0f, palette->river));
        }
    }
}

static vec3_t gate_polar_point(const gate_t *gate, float radius,
                               float angle, float z_offset) {
    const float center = path_center(gate->z) + gate->x;
    return (vec3_t){center + fcos(angle) * radius,
                    gate->y + fsin(angle) * radius,
                    gate->z + z_offset};
}

static void draw_alternative_traversal_opaque(const gate_t *gate,
                                              const palette_t *palette) {
    const int biome = course_chapter_at(gate->z) & 3;
    static const int support_textures[4] = {
        GRAVITY_WAVE_TEX_HULL_ALLIED,
        GRAVITY_WAVE_TEX_TERRAIN_EMERALD,
        GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
        GRAVITY_WAVE_TEX_HULL_HOSTILE
    };
    static const int hazard_textures[4] = {
        GRAVITY_WAVE_TEX_CANOPY_ENERGY,
        GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
        GRAVITY_WAVE_TEX_RIFT_ENERGY,
        GRAVITY_WAVE_TEX_TERRAIN_EMBER
    };
    static const color3_t support_bases[4] = {
        {0.34f,0.46f,0.58f}, {0.30f,0.48f,0.31f},
        {0.40f,0.30f,0.56f}, {0.43f,0.31f,0.27f}
    };
    static const color3_t hazard_bases[4] = {
        {0.20f,0.72f,0.92f}, {0.55f,0.78f,0.30f},
        {0.73f,0.28f,0.94f}, {0.92f,0.28f,0.12f}
    };
    const int support_texture = support_textures[biome];
    const int hazard_texture = hazard_textures[biome];
    const color3_t steel = color_lerp(
        support_bases[biome], palette->river, 0.22f);
    const color3_t hazard = color_lerp(
        hazard_bases[biome], palette->enemy, 0.26f);
    const color3_t guide_frame = color_lerp(
        steel, (color3_t){0.54f,0.94f,1.0f}, 0.48f);
    int stage;

    for(stage = 0; stage < gate->stage_count; ++stage) {
        const float stage_z = traversal_stage_z(gate, stage);
        const float relative_z = stage_z - game.distance;
        if(relative_z < NEAR_PLANE - 12.0f ||
           relative_z > FAR_PLANE + 24.0f)
            continue;

        if(gate->kind == TRAVERSAL_SLALOM) {
            int side;
            /* Paired pylons visibly bracket the scoring corridor. They read as
               one lateral navigation lane rather than an unexplained obstacle. */
            for(side = -1; side <= 1; side += 2) {
                const float pylon_x = traversal_lane_pylon_x(
                    gate, stage, side);
                draw_material_box(pylon_x, 5.0f, stage_z,
                                  VECTOR_LANE_PYLON_HALF_WIDTH,
                                  10.0f, 6.5f,
                                  hazard_texture, hazard);
                draw_material_box(pylon_x, 15.0f, stage_z,
                                  3.0f, 72.0f, 3.8f,
                                  support_texture, guide_frame);
                draw_material_box(pylon_x, 43.0f, stage_z,
                                  4.7f, 3.0f, 5.2f,
                                  hazard_texture, hazard);
                draw_material_box(pylon_x, 84.0f, stage_z,
                                  5.2f, 4.0f, 5.7f,
                                  hazard_texture, hazard);
                draw_material_beam(stage_z,
                    pylon_x, 35.0f,
                    pylon_x - (float)side * 9.0f, 40.0f,
                    1.5f, 2.0f, support_texture, guide_frame);
                draw_material_beam(stage_z,
                    pylon_x, 67.0f,
                    pylon_x - (float)side * 9.0f, 62.0f,
                    1.5f, 2.0f, support_texture, guide_frame);
            }
        }
        else {
            const float bar_y = traversal_stage_y(gate, stage);
            draw_material_box(-106.0f, 6.0f, stage_z,
                              3.8f, 84.0f, 4.2f,
                              support_texture, steel);
            draw_material_box(106.0f, 6.0f, stage_z,
                              3.8f, 84.0f, 4.2f,
                              support_texture, steel);
            draw_material_box(0.0f, bar_y - 2.3f, stage_z,
                              108.0f, 4.6f, 4.4f,
                              hazard_texture, hazard);
            draw_material_box(-106.0f, bar_y - 5.5f, stage_z,
                              6.2f, 11.0f, 5.8f,
                              hazard_texture, hazard);
            draw_material_box(106.0f, bar_y - 5.5f, stage_z,
                              6.2f, 11.0f, 5.8f,
                              hazard_texture, hazard);
        }
    }
}

static void draw_gate_petals_opaque(const palette_t *palette) {
    int i;
    for(i = 0; i < MAX_GATES; ++i) {
        const gate_t *gate = &gates[i];
        const float relative_z = gate->z - game.distance;
        const float clear_radius = gate->radius * GATE_CLEAR_RADIUS_SCALE;
        const float breath = 0.5f + 0.5f *
            fsin(game.time * 2.8f + gate->spin * 1.7f);
        const float middle_radius = clear_radius +
            (6.1f + (float)gate->variant * 0.28f) *
            (1.0f + breath * 0.025f);
        const float outer_radius = clear_radius +
            (12.1f + (float)gate->variant * 0.42f) *
            (1.0f + breath * 0.040f);
        const float sweep = (gate->variant & 1) ? -1.0f : 1.0f;
        const color3_t body_tint = color_lerp(
            (color3_t){0.13f,0.17f,0.25f}, palette->accent, 0.28f);
        const color3_t edge_tint = color_lerp(
            (color3_t){0.24f,0.31f,0.43f}, palette->river, 0.34f);
        const uint32_t deep = pack_color(1.0f, color_scale(body_tint, 0.48f));
        const uint32_t body = pack_color(1.0f, color_scale(body_tint, 0.82f));
        const uint32_t edge = pack_color(1.0f, color_scale(edge_tint, 1.08f));
        const pvr_poly_hdr_t *header =
            &texture_headers[GRAVITY_WAVE_TEX_ANCIENT_MACHINE];
        int arm;

        if(!gate->active)
            continue;
        if(gate->kind != TRAVERSAL_GRAVITY_BLOOM) {
            draw_alternative_traversal_opaque(gate, palette);
            continue;
        }
        if(relative_z < NEAR_PLANE - 4.0f || relative_z > FAR_PLANE + 20.0f)
            continue;

        for(arm = 0; arm < 3; ++arm) {
            const float base = gate->spin + (float)arm * TAU / 3.0f;
            const float angles[6] = {
                base - sweep * 0.13f, base + sweep * 0.09f,
                base + sweep * 0.02f, base + sweep * 0.35f,
                base + sweep * 0.18f, base + sweep * 0.69f
            };
            const float radii[6] = {
                clear_radius + 1.9f, clear_radius + 1.9f,
                middle_radius, middle_radius,
                outer_radius, outer_radius
            };
            vec3_t front[6];
            vec3_t back[6];
            int point;

            for(point = 0; point < 6; ++point) {
                front[point] = gate_polar_point(gate, radii[point],
                                                angles[point], -1.9f);
                back[point] = gate_polar_point(gate, radii[point],
                                               angles[point], 2.3f);
            }

            draw_world_textured_quad(header,
                front[0],front[1],front[2],front[3],
                0.0f,1.0f, 1.0f,1.0f, 0.0f,0.48f, 1.0f,0.48f,
                deep,body,body,edge);
            draw_world_textured_quad(header,
                front[2],front[3],front[4],front[5],
                0.0f,0.48f, 1.0f,0.48f, 0.0f,0.0f, 1.0f,0.0f,
                body,edge,deep,body);
            draw_world_textured_quad(header,
                front[1],front[3],back[1],back[3],
                0.0f,0.0f, 1.0f,0.0f, 0.0f,0.42f, 1.0f,0.42f,
                edge,edge,deep,deep);
            draw_world_textured_quad(header,
                front[3],front[5],back[3],back[5],
                0.0f,0.0f, 1.0f,0.0f, 0.0f,0.42f, 1.0f,0.42f,
                edge,body,deep,deep);
            draw_world_textured_quad(header,
                front[4],front[5],back[4],back[5],
                0.0f,0.0f, 1.0f,0.0f, 0.0f,0.42f, 1.0f,0.42f,
                body,body,deep,deep);
        }
    }
}

static void draw_alternative_traversal(const gate_t *gate,
                                       const palette_t *palette) {
    int stage;

    for(stage = 0; stage < gate->stage_count; ++stage) {
        const float stage_z = traversal_stage_z(gate, stage);
        const float relative_z = stage_z - game.distance;
        const float far_fade = 1.0f -
            smoothstepf((relative_z - 930.0f) / 330.0f);
        const bool completed = stage < gate->stage;
        const bool cleared = (gate->success_mask & (1u << stage)) != 0u;
        const float pulse = 0.70f + 0.30f *
            fsin(game.time * 6.4f - (float)stage * 1.7f);
        float gain = far_fade * (completed ? 0.42f : 1.0f);
        color3_t guide = color_lerp(
            palette->river, (color3_t){0.88f,1.0f,1.0f}, 0.42f);
        color3_t hazard = color_lerp(
            palette->enemy, (color3_t){1.0f,0.12f,0.28f}, 0.32f);

        if(relative_z < NEAR_PLANE - 16.0f || far_fade <= 0.01f)
            continue;
        if(completed)
            guide = cleared ? (color3_t){0.76f,1.0f,0.84f} : hazard;

        if(gate->kind == TRAVERSAL_SLALOM) {
            const int safe_direction = traversal_stage_direction(gate, stage);
            const float lane_x = traversal_stage_x(gate, stage);
            const float lane_left = lane_x - gate->radius;
            const float lane_right = lane_x + gate->radius;
            const float center_x = path_center(stage_z);
            const float tail_x = lane_x - (float)safe_direction * 12.0f;
            const uint32_t curtain_edge = pack_color(gain * 0.035f, hazard);
            const uint32_t curtain_hot = pack_color(gain * 0.18f, hazard);
            int boundary;
            int arrow;

            /* Dim outer navigation fields leave one unmistakable cyan lane.
               The paired rails, arrows and scoring bounds all share exactly
               the same lane_x/radius values. */
            draw_world_quad(&additive_header,
                (vec3_t){center_x - 104.0f,12.0f,stage_z - 4.7f},
                (vec3_t){center_x + lane_left,12.0f,stage_z - 4.7f},
                (vec3_t){center_x - 104.0f,94.0f,stage_z - 4.7f},
                (vec3_t){center_x + lane_left,94.0f,stage_z - 4.7f},
                curtain_edge,curtain_edge,curtain_hot,curtain_hot);
            draw_world_quad(&additive_header,
                (vec3_t){center_x + lane_right,12.0f,stage_z - 4.7f},
                (vec3_t){center_x + 104.0f,12.0f,stage_z - 4.7f},
                (vec3_t){center_x + lane_right,94.0f,stage_z - 4.7f},
                (vec3_t){center_x + 104.0f,94.0f,stage_z - 4.7f},
                curtain_edge,curtain_edge,curtain_hot,curtain_hot);
            for(boundary = -1; boundary <= 1; boundary += 2) {
                const float rail_x = lane_x + (float)boundary * gate->radius;
                draw_world_energy_ribbon(stage_z, stage_z - 5.0f,
                    rail_x, 12.0f, rail_x, 94.0f, 1.25f,
                    pack_color(gain * 0.28f, guide),
                    pack_color(gain * (0.74f + pulse * 0.24f), guide));
            }
            for(arrow = 0; arrow < 3; ++arrow) {
                const float arrow_y = 31.0f + (float)arrow * 21.0f;
                const uint32_t tail_color = pack_color(gain * 0.16f, guide);
                const uint32_t tip_color = pack_color(
                    gain * (0.72f + pulse * 0.24f), guide);
                draw_world_energy_ribbon(stage_z, stage_z - 5.0f,
                    tail_x, arrow_y - 5.5f, lane_x, arrow_y,
                    0.90f, tail_color, tip_color);
                draw_world_energy_ribbon(stage_z, stage_z - 5.0f,
                    tail_x, arrow_y + 5.5f, lane_x, arrow_y,
                    0.90f, tail_color, tip_color);
            }
            draw_textured_billboard(
                &sprite_additive_headers[2],
                (vec3_t){path_center(stage_z) + lane_x, 52.0f,
                         stage_z - 5.2f},
                7.0f + pulse * 2.0f, 7.0f + pulse * 2.0f,
                pack_color(gain * 0.86f, guide));
        }
        else {
            const int safe_direction = traversal_stage_direction(gate, stage);
            const float bar_y = traversal_stage_y(gate, stage);
            const float center_x = path_center(stage_z);
            const float forbidden_y0 = safe_direction > 0 ?
                PLAYER_MIN_Y : bar_y + 4.5f;
            const float forbidden_y1 = safe_direction > 0 ?
                bar_y - 4.5f : PLAYER_MAX_Y;
            const uint32_t curtain_edge = pack_color(gain * 0.08f, hazard);
            const uint32_t curtain_hot = pack_color(gain * 0.28f, hazard);
            int stripe;
            int arrow;

            draw_world_quad(&additive_header,
                (vec3_t){center_x - 102.0f,forbidden_y0,stage_z - 4.7f},
                (vec3_t){center_x + 102.0f,forbidden_y0,stage_z - 4.7f},
                (vec3_t){center_x - 102.0f,forbidden_y1,stage_z - 4.7f},
                (vec3_t){center_x + 102.0f,forbidden_y1,stage_z - 4.7f},
                curtain_edge,curtain_edge,curtain_hot,curtain_hot);
            for(stripe = 1; stripe <= 4; ++stripe) {
                const float stripe_y = lerpf(
                    forbidden_y0, forbidden_y1, (float)stripe / 5.0f);
                draw_world_energy_ribbon(stage_z, stage_z - 5.0f,
                    -100.0f, stripe_y, 100.0f, stripe_y, 0.62f,
                    pack_color(gain * 0.08f, hazard),
                    pack_color(gain * (0.42f + pulse * 0.22f), hazard));
            }
            for(arrow = -1; arrow <= 1; ++arrow) {
                const float arrow_x = (float)arrow * 34.0f;
                const float tip_y = bar_y +
                    (float)safe_direction * (gate->radius + 16.0f);
                const float tail_y = tip_y - (float)safe_direction * 11.0f;
                const uint32_t tail_color = pack_color(gain * 0.15f, guide);
                const uint32_t tip_color = pack_color(
                    gain * (0.72f + pulse * 0.24f), guide);
                draw_world_energy_ribbon(stage_z, stage_z - 5.2f,
                    arrow_x - 5.5f, tail_y, arrow_x, tip_y,
                    0.90f, tail_color, tip_color);
                draw_world_energy_ribbon(stage_z, stage_z - 5.2f,
                    arrow_x + 5.5f, tail_y, arrow_x, tip_y,
                    0.90f, tail_color, tip_color);
            }
        }
    }
}

static void draw_gates(const palette_t *palette) {
    int i;

    /* Colored geometry stays grouped under one PVR header. Charge sprites are
       submitted in the second loop so every visible gate costs only one
       additional header switch. */
    for(i = 0; i < MAX_GATES; ++i) {
        const gate_t *gate = &gates[i];
        const float relative_z = gate->z - game.distance;
        const float far_fade = 1.0f -
            smoothstepf((relative_z - 930.0f) / 330.0f);
        const float approach = 1.0f -
            smoothstepf((relative_z - PLAYER_Z) / 780.0f);
        const float clear_radius = gate->radius * GATE_CLEAR_RADIUS_SCALE;
        const float stroke_scale = clampf(relative_z / 280.0f, 1.0f, 2.65f);
        const float halo_half_width = clampf(
            relative_z / game.camera_focal * 0.70f, 0.28f, 1.05f);
        const float breath = 0.5f + 0.5f *
            fsin(game.time * 2.8f + gate->spin * 1.7f);
        const float outer_radius = clear_radius +
            (12.3f + (float)gate->variant * 0.42f) *
            (1.0f + breath * 0.040f);
        const float sweep = (gate->variant & 1) ? -1.0f : 1.0f;
        const float flash = gate->result == GATE_RESULT_CLEARED ?
            clampf(gate->result_time / 0.55f, 0.0f, 1.0f) : 0.0f;
        float gain = far_fade * (0.58f + approach * 0.42f);
        color3_t guide_color = color_lerp(palette->river,
            (color3_t){0.91f,1.0f,1.0f}, 0.28f + flash * 0.58f);
        color3_t energy_color = color_lerp(palette->accent,
            (color3_t){1.0f,0.90f,1.0f}, flash * 0.80f);
        const float halo_rotation = -gate->spin * 1.48f +
                                    (float)gate->variant * 0.31f;
        int segment;
        int arm;

        if(!gate->active || far_fade <= 0.01f)
            continue;
        if(gate->kind != TRAVERSAL_GRAVITY_BLOOM) {
            draw_alternative_traversal(gate, palette);
            continue;
        }
        if(gate->result == GATE_RESULT_MISSED) {
            guide_color = palette->enemy;
            energy_color = color_lerp(palette->enemy,
                                      (color3_t){1.0f,0.12f,0.20f}, 0.45f);
            gain *= ((int)(game.time * 18.0f) & 1) ? 0.50f : 0.22f;
        }

        /* The broken guide halo is centered exactly on the scoring radius. */
        for(segment = 0; segment < 18; ++segment) {
            const float step = TAU / 18.0f;
            float a0;
            float a1;
            float pulse;
            vec3_t p0, p1, p2, p3;
            uint32_t dim;
            uint32_t hot;

            if(((segment + gate->variant) % 3) == 2)
                continue;
            a0 = halo_rotation + ((float)segment + 0.10f) * step;
            a1 = halo_rotation + ((float)segment + 0.78f) * step;
            pulse = 0.72f + 0.28f *
                fsin(game.time * 7.0f - (float)segment * 0.72f);
            p0 = gate_polar_point(gate, clear_radius + halo_half_width,
                                  a0, -2.65f);
            p1 = gate_polar_point(gate, clear_radius - halo_half_width,
                                  a0, -2.65f);
            p2 = gate_polar_point(gate, clear_radius + halo_half_width,
                                  a1, -2.65f);
            p3 = gate_polar_point(gate, clear_radius - halo_half_width,
                                  a1, -2.65f);
            dim = pack_color(gain * 0.18f, energy_color);
            hot = pack_color(gain * (0.70f + pulse * 0.28f), guide_color);
            draw_world_quad(&additive_header, p0,p1,p2,p3,
                            dim,hot,dim,hot);
        }

        /* Three hooked traces reveal the swept petal silhouette at speed. */
        for(arm = 0; arm < 3; ++arm) {
            const float base = gate->spin + (float)arm * TAU / 3.0f;
            for(segment = 0; segment < 3; ++segment) {
                const float t0 = (float)segment / 3.0f;
                const float t1 = (float)(segment + 1) / 3.0f;
                const float curve0 = smoothstepf(t0);
                const float curve1 = smoothstepf(t1);
                const float r0 = lerpf(clear_radius + 1.9f,
                                       outer_radius, curve0);
                const float r1 = lerpf(clear_radius + 1.9f,
                                       outer_radius, curve1);
                const float a0 = base + sweep * (0.09f + curve0 * 0.60f);
                const float a1 = base + sweep * (0.09f + curve1 * 0.60f);
                const float w0 = (0.42f + t0 * 0.18f) * stroke_scale;
                const float w1 = (0.42f + t1 * 0.18f) * stroke_scale;
                const vec3_t p0 = gate_polar_point(
                    gate, r0 + w0, a0, -2.35f - t0 * 0.65f);
                const vec3_t p1 = gate_polar_point(
                    gate, r0 - w0, a0, -2.35f - t0 * 0.65f);
                const vec3_t p2 = gate_polar_point(
                    gate, r1 + w1, a1, -2.35f - t1 * 0.65f);
                const vec3_t p3 = gate_polar_point(
                    gate, r1 - w1, a1, -2.35f - t1 * 0.65f);
                draw_world_quad(&additive_header, p0,p1,p2,p3,
                    pack_color(gain * (0.28f + t0 * 0.16f), energy_color),
                    pack_color(gain * (0.78f + flash * 0.20f), guide_color),
                    pack_color(gain * (0.32f + t1 * 0.20f), energy_color),
                    pack_color(gain * (0.90f + flash * 0.10f), guide_color));
            }
        }

        /* Short axial curls make the bloom read as a machine with depth rather
           than another flat billboard. Far gates use the cheaper silhouette. */
        if(relative_z < 820.0f) {
            for(arm = 0; arm < 3; ++arm) {
                const float base = gate->spin + (float)arm * TAU / 3.0f;
                for(segment = 0; segment < 3; ++segment) {
                    const float t0 = (float)segment / 3.0f;
                    const float t1 = (float)(segment + 1) / 3.0f;
                    const float a0 = base + sweep * (0.66f - t0 * 0.42f);
                    const float a1 = base + sweep * (0.66f - t1 * 0.42f);
                    const float z0 = -10.0f + t0 * 13.0f;
                    const float z1 = -10.0f + t1 * 13.0f;
                    const float helix_half_width = 0.34f * stroke_scale;
                    const vec3_t p0 = gate_polar_point(
                        gate, outer_radius + helix_half_width, a0, z0);
                    const vec3_t p1 = gate_polar_point(
                        gate, outer_radius - helix_half_width, a0, z0);
                    const vec3_t p2 = gate_polar_point(
                        gate, outer_radius + helix_half_width, a1, z1);
                    const vec3_t p3 = gate_polar_point(
                        gate, outer_radius - helix_half_width, a1, z1);
                    draw_world_quad(&additive_header, p0,p1,p2,p3,
                        pack_color(gain * 0.08f, energy_color),
                        pack_color(gain * 0.34f, guide_color),
                        pack_color(gain * 0.12f, energy_color),
                        pack_color(gain * 0.62f, guide_color));
                }
            }
        }
    }

    /* White-hot charges chase the counter-rotating safe-radius halo. */
    for(i = 0; i < MAX_GATES; ++i) {
        const gate_t *gate = &gates[i];
        const float relative_z = gate->z - game.distance;
        const float clear_radius = gate->radius * GATE_CLEAR_RADIUS_SCALE;
        const float far_fade = 1.0f -
            smoothstepf((relative_z - 560.0f) / 220.0f);
        const float rotation = -gate->spin * 1.48f + game.time * 1.85f;
        const color3_t node_color = gate->result == GATE_RESULT_MISSED ?
            palette->enemy : (color3_t){0.88f,0.99f,1.0f};
        const float node_size = clampf(
            relative_z / game.camera_focal * 10.5f, 1.5f, 3.8f);
        int node;

        if(!gate->active || gate->kind != TRAVERSAL_GRAVITY_BLOOM ||
           far_fade <= 0.01f)
            continue;
        for(node = 0; node < 3; ++node) {
            const float angle = rotation + (float)node * TAU / 3.0f;
            const float pulse = 1.0f + 0.18f *
                fsin(game.time * 11.0f + (float)node * 2.1f);
            const vec3_t world = gate_polar_point(
                gate, clear_radius + 1.15f, angle,
                -3.1f + fsin(angle * 2.0f) * 0.8f);
            draw_textured_billboard(&sprite_additive_headers[2], world,
                                    node_size * pulse, node_size * pulse,
                                    pack_color(far_fade * 0.92f, node_color));
        }
    }
}

static void draw_pickups(const palette_t *palette) {
    static const color3_t colors[PICKUP_KIND_COUNT] = {
        {0.34f,0.90f,1.00f}, {0.28f,1.00f,0.52f},
        {1.00f,0.38f,0.82f}, {1.00f,0.72f,0.18f},
        {0.12f,0.92f,1.00f}, {0.96f,0.20f,0.88f}
    };
    int i;
    for(i = 0; i < MAX_PICKUPS; ++i) {
        const pickup_t *pickup = &pickups[i];
        color3_t color;
        const vec3_t world = {path_center(pickup->z) + pickup->x,
                              pickup->y, pickup->z};
        screen_point_t point;
        float radius;
        int edge;
        if(!pickup->active || !project_world(world, &point))
            continue;
        if(pickup->kind < 0 || pickup->kind >= PICKUP_KIND_COUNT)
            continue;
        color = colors[pickup->kind];
        draw_textured_billboard(&sprite_additive_headers[2], world,
                                18.0f, 22.0f,
                                pack_color(0.82f, color));
        radius = clampf(5.0f + point.z * 950.0f, 5.0f, 15.0f);
        for(edge = 0; edge < 4; ++edge) {
            const float a0 = pickup->spin + (float)edge * PI * 0.5f;
            const float a1 = pickup->spin + (float)(edge + 1) * PI * 0.5f;
            draw_line(&additive_header,
                      point.x + fcos(a0) * radius,
                      point.y + fsin(a0) * radius,
                      point.z + 0.00002f,
                      point.x + fcos(a1) * radius,
                      point.y + fsin(a1) * radius,
                      point.z + 0.00002f,
                      2.2f, pack_color(0.18f, color),
                      pack_color(0.92f, color));
        }
        switch(pickup->kind) {
            case PICKUP_LASER_CORE:
                draw_line(&additive_header,
                          point.x - radius * 0.48f, point.y,
                          point.z + 0.00003f,
                          point.x + radius * 0.48f, point.y,
                          point.z + 0.00003f, 2.0f,
                          pack_color(0.9f, palette->river),
                          pack_color(0.9f, palette->river));
                break;
            case PICKUP_REPAIR:
                draw_line(&additive_header, point.x - radius * 0.43f, point.y,
                          point.z + 0.00003f,
                          point.x + radius * 0.43f, point.y,
                          point.z + 0.00003f, 2.2f,
                          pack_color(0.92f, color), pack_color(0.92f, color));
                draw_line(&additive_header, point.x, point.y - radius * 0.43f,
                          point.z + 0.00003f,
                          point.x, point.y + radius * 0.43f,
                          point.z + 0.00003f, 2.2f,
                          pack_color(0.92f, color), pack_color(0.92f, color));
                break;
            case PICKUP_NOVA:
                draw_line(&additive_header,
                          point.x - radius * 0.36f,
                          point.y - radius * 0.36f, point.z + 0.00003f,
                          point.x + radius * 0.36f,
                          point.y + radius * 0.36f, point.z + 0.00003f,
                          2.0f, pack_color(0.85f, color),
                          pack_color(0.85f, color));
                draw_line(&additive_header,
                          point.x + radius * 0.36f,
                          point.y - radius * 0.36f, point.z + 0.00003f,
                          point.x - radius * 0.36f,
                          point.y + radius * 0.36f, point.z + 0.00003f,
                          2.0f, pack_color(0.85f, color),
                          pack_color(0.85f, color));
                break;
            case PICKUP_SPEED_BOOST:
                for(edge = 0; edge < 2; ++edge) {
                    const float x = point.x - radius * 0.52f +
                                    (float)edge * radius * 0.48f;
                    draw_line(&additive_header,
                              x, point.y - radius * 0.38f,
                              point.z + 0.00003f,
                              x + radius * 0.38f, point.y,
                              point.z + 0.00003f, 2.0f,
                              pack_color(0.75f, color),
                              pack_color(0.98f, color));
                    draw_line(&additive_header,
                              x + radius * 0.38f, point.y,
                              point.z + 0.00003f,
                              x, point.y + radius * 0.38f,
                              point.z + 0.00003f, 2.0f,
                              pack_color(0.98f, color),
                              pack_color(0.75f, color));
                }
                break;
            case PICKUP_FAST_LASER:
                for(edge = -1; edge <= 1; ++edge)
                    draw_line(&additive_header,
                              point.x - radius * (edge == 0 ? 0.55f : 0.36f),
                              point.y + (float)edge * radius * 0.30f,
                              point.z + 0.00003f,
                              point.x + radius * 0.55f,
                              point.y + (float)edge * radius * 0.30f,
                              point.z + 0.00003f, edge == 0 ? 2.4f : 1.6f,
                              pack_color(0.55f, color),
                              pack_color(1.0f, color));
                break;
            case PICKUP_PHASE_WAVE:
                draw_line(&additive_header,
                          point.x - radius * 0.56f, point.y,
                          point.z + 0.00003f,
                          point.x - radius * 0.20f,
                          point.y + radius * 0.38f,
                          point.z + 0.00003f, 2.2f,
                          pack_color(0.70f, color), pack_color(1.0f, color));
                draw_line(&additive_header,
                          point.x - radius * 0.20f,
                          point.y + radius * 0.38f,
                          point.z + 0.00003f,
                          point.x + radius * 0.20f,
                          point.y - radius * 0.38f,
                          point.z + 0.00003f, 2.2f,
                          pack_color(1.0f, color), pack_color(1.0f, color));
                draw_line(&additive_header,
                          point.x + radius * 0.20f,
                          point.y - radius * 0.38f,
                          point.z + 0.00003f,
                          point.x + radius * 0.56f, point.y,
                          point.z + 0.00003f, 2.2f,
                          pack_color(1.0f, color), pack_color(0.70f, color));
                break;
            case PICKUP_KIND_COUNT:
                break;
        }
    }
}

static void draw_projectile_pool(const projectile_t *pool, int count,
                                 bool hostile, const palette_t *palette) {
    int i;
    for(i = 0; i < count; ++i) {
        screen_point_t head, tail;
        const projectile_t *shot = &pool[i];
        const vec3_t head_world = {shot->x, shot->y, shot->z};
        color3_t main_color;
        color3_t core_color;
        float length;
        float speed;
        float tail_time;
        vec3_t tail_world;
        float width;
        const bool guardian_shot = hostile &&
            (shot->hit_mask & GUARDIAN_SHOT_STYLE_FLAG);
        const int guardian_style = (int)(shot->hit_mask & 3u);

        if(!shot->active || !project_absolute_world(head_world, &head))
            continue;

        if(!hostile && shot->kind == SHOT_PLAYER_PHASE) {
            const color3_t wave_color = {1.0f, 0.16f, 0.82f};
            const color3_t wave_core = {0.62f, 0.94f, 1.0f};
            const float radius_x = clampf(31.0f * game.camera_focal * head.z,
                                          8.0f, 58.0f);
            const float radius_y = radius_x * 0.43f;
            const float ct = fcos(shot->spin);
            const float st = fsin(shot->spin);
            const float pulse = 0.74f + 0.22f *
                fsin(game.time * 9.0f + shot->spin * 3.0f);
            int segment;

            draw_absolute_textured_billboard(
                &sprite_additive_headers[2], head_world,
                13.0f, 13.0f, pack_color(0.52f, wave_color));
            for(segment = 0; segment < 6; ++segment) {
                const float t0 = -1.0f + (float)segment * (2.0f / 6.0f);
                const float t1 = -1.0f +
                                 (float)(segment + 1) * (2.0f / 6.0f);
                const float lx0 = t0 * radius_x;
                const float ly0 = (t0 * t0 - 0.42f) * radius_y;
                const float lx1 = t1 * radius_x;
                const float ly1 = (t1 * t1 - 0.42f) * radius_y;
                const float x0 = head.x + lx0 * ct - ly0 * st;
                const float y0 = head.y + lx0 * st + ly0 * ct;
                const float x1 = head.x + lx1 * ct - ly1 * st;
                const float y1 = head.y + lx1 * st + ly1 * ct;
                draw_line(&additive_header,
                          x0, y0, head.z + 0.00001f,
                          x1, y1, head.z + 0.00001f,
                          6.5f, pack_color(0.04f, wave_color),
                          pack_color(0.48f * pulse, wave_color));
                draw_line(&additive_header,
                          x0, y0, head.z + 0.00002f,
                          x1, y1, head.z + 0.00002f,
                          1.8f, pack_color(0.52f, wave_core),
                          pack_color(pulse, wave_core));
            }
            continue;
        }

        if(hostile) {
            if(guardian_shot) {
                static const color3_t guardian_main[4] = {
                    {0.12f,0.78f,1.00f}, {0.46f,1.00f,0.18f},
                    {1.00f,0.16f,0.86f}, {1.00f,0.34f,0.05f}
                };
                static const color3_t guardian_core[4] = {
                    {0.90f,1.00f,1.00f}, {0.96f,1.00f,0.64f},
                    {0.72f,0.88f,1.00f}, {1.00f,0.92f,0.48f}
                };
                static const float guardian_length[4] = {
                    15.0f,12.0f,18.0f,22.0f
                };
                main_color = guardian_main[guardian_style];
                core_color = guardian_core[guardian_style];
                length = guardian_length[guardian_style];
            }
            else {
                main_color = palette->enemy;
                core_color = (color3_t){1.0f, 0.68f, 0.42f};
                length = 11.0f;
            }
        }
        else if(shot->kind == SHOT_PLAYER_FAST) {
            main_color = (color3_t){0.05f, 0.88f, 1.0f};
            core_color = (color3_t){0.92f, 1.0f, 1.0f};
            length = 29.0f;
        }
        else {
            main_color = palette->river;
            core_color = (color3_t){0.86f, 1.0f, 1.0f};
            length = 23.0f;
        }
        speed = fsqrt(shot->vx * shot->vx + shot->vy * shot->vy +
                      shot->vz * shot->vz);
        tail_time = length / fmaxf(speed, 0.001f);
        tail_world = (vec3_t){shot->x - shot->vx * tail_time,
                              shot->y - shot->vy * tail_time,
                              shot->z - shot->vz * tail_time};
        if(!project_absolute_world(tail_world, &tail))
            continue;
        width = clampf(2.0f + head.z * 250.0f, 2.0f, hostile ? 8.0f : 6.0f);
        if(!hostile && shot->kind == SHOT_PLAYER_FAST)
            width *= 0.72f;
        if(guardian_shot) {
            const float pulse = 1.0f + fsin(shot->spin * 2.0f) * 0.16f;
            width *= guardian_style == 1 ? 1.25f :
                     (guardian_style == 2 ? 0.90f : 1.08f);
            draw_absolute_textured_billboard(
                &sprite_additive_headers[2], head_world,
                (4.8f + (float)guardian_style * 0.45f) * pulse,
                (4.8f + (float)guardian_style * 0.45f) * pulse,
                pack_color(0.58f, main_color));
        }
        draw_line(&additive_header, tail.x, tail.y, tail.z,
                  head.x, head.y, head.z, width * 2.8f,
                  pack_color(0.0f, main_color),
                  pack_color(0.42f, main_color));
        draw_line(&additive_header, tail.x, tail.y, tail.z + 0.00001f,
                  head.x, head.y, head.z + 0.00001f, width,
                  pack_color(0.35f, core_color),
                  pack_color(1.0f, core_color));
    }
}

static void draw_particles(void) {
    int i;
    for(i = 0; i < MAX_PARTICLES; ++i) {
        const particle_t *particle = &particles[i];
        screen_point_t point;
        float alpha, size;
        vec3_t world;
        if(!particle->active)
            continue;
        world = (vec3_t){particle->x, particle->y, particle->z};
        if(!project_absolute_world(world, &point))
            continue;
        alpha = clampf(particle->life / particle->max_life, 0.0f, 1.0f);
        size = clampf(particle->size * game.camera_focal * point.z,
                      1.0f,
                      particle->kind == PARTICLE_STREAK ? 13.0f :
                      (particle->kind == PARTICLE_BOSS_IMPACT ? 34.0f :
                                                               18.0f));
        if(particle->kind == PARTICLE_STREAK) {
            screen_point_t tail;
            vec3_t tail_world = world;
            tail_world.x -= particle->vx * 0.055f;
            tail_world.y -= particle->vy * 0.055f;
            tail_world.z -= particle->vz * 0.055f;
            if(project_absolute_world(tail_world, &tail))
                draw_line(&additive_header, tail.x, tail.y, tail.z,
                          point.x, point.y, point.z, size,
                          pack_color(0.0f, particle->color),
                          pack_color(alpha, particle->color));
        }
        else if(particle->kind == PARTICLE_EXHAUST) {
            const float age = 1.0f - alpha;
            const float visibility = smoothstepf(age / 0.14f) *
                                     smoothstepf(alpha / 0.30f);
            screen_point_t forward;
            vec3_t forward_world = world;
            const float speed = fsqrt(particle->vx * particle->vx +
                                      particle->vy * particle->vy +
                                      particle->vz * particle->vz);
            const float forward_distance = 2.0f + age * 3.0f;

            if(speed > 0.001f) {
                forward_world.x -= particle->vx / speed * forward_distance;
                forward_world.y -= particle->vy / speed * forward_distance;
                forward_world.z -= particle->vz / speed * forward_distance;
            }
            if(project_absolute_world(forward_world, &forward))
                draw_line(&additive_header, point.x, point.y, point.z,
                          forward.x, forward.y, forward.z,
                          fmaxf(1.0f, size * 0.42f),
                          pack_color(0.0f, particle->color),
                          pack_color(visibility * 0.46f, particle->color));
            draw_disc(&additive_header, point.x, point.y,
                      fmaxf(1.0f, size * lerpf(0.22f, 0.38f, age)),
                      point.z + 0.00001f, 8,
                      pack_color(visibility * 0.42f,
                                 color_lerp(particle->color,
                                            (color3_t){0.88f,0.99f,1.0f},
                                            0.34f)),
                      pack_color(0.0f, particle->color));
        }
        else if(particle->kind == PARTICLE_BOSS_IMPACT) {
            const float age = 1.0f - alpha;
            const float radius = size * (0.34f + age * 1.12f);
            const color3_t core = color_lerp(
                particle->color, (color3_t){1.0f,1.0f,0.90f}, 0.72f);
            const uint32_t ring_color = pack_color(alpha * 0.88f,
                                                    particle->color);
            int segment;

            draw_disc(&additive_header, point.x, point.y,
                      size * (0.72f - age * 0.28f),
                      point.z + 0.00003f, 10,
                      pack_color(alpha, core),
                      pack_color(0.0f, particle->color));
            for(segment = 0; segment < 16; ++segment) {
                const float a0 = (float)segment * TAU / 16.0f;
                const float a1 = (float)(segment + 1) * TAU / 16.0f;
                draw_line(&additive_header,
                          point.x + fcos(a0) * radius,
                          point.y + fsin(a0) * radius,
                          point.z + 0.00004f,
                          point.x + fcos(a1) * radius,
                          point.y + fsin(a1) * radius,
                          point.z + 0.00004f,
                          1.0f + alpha * 2.4f,
                          ring_color, ring_color);
            }
            draw_line(&additive_header, point.x, point.y,
                      point.z + 0.00005f,
                      point.x - radius * 1.35f, point.y,
                      point.z + 0.00005f, 1.0f + alpha * 1.7f,
                      pack_color(alpha * 0.72f, core),
                      pack_color(0.0f, core));
            draw_line(&additive_header, point.x, point.y,
                      point.z + 0.00005f,
                      point.x + radius * 1.35f, point.y,
                      point.z + 0.00005f, 1.0f + alpha * 1.7f,
                      pack_color(alpha * 0.72f, core),
                      pack_color(0.0f, core));
            draw_line(&additive_header, point.x, point.y,
                      point.z + 0.00005f,
                      point.x, point.y - radius * 1.35f,
                      point.z + 0.00005f, 1.0f + alpha * 1.7f,
                      pack_color(alpha * 0.72f, core),
                      pack_color(0.0f, core));
            draw_line(&additive_header, point.x, point.y,
                      point.z + 0.00005f,
                      point.x, point.y + radius * 1.35f,
                      point.z + 0.00005f, 1.0f + alpha * 1.7f,
                      pack_color(alpha * 0.72f, core),
                      pack_color(0.0f, core));
        }
        else {
            const int sprite = particle->color.r > particle->color.b * 1.18f ?
                               3 : 2;
            draw_absolute_textured_billboard(
                &sprite_additive_headers[sprite], world,
                particle->size * 2.0f, particle->size * 2.0f,
                pack_color(alpha * 0.78f, particle->color));
        }
    }
}

static void draw_speed_streaks(const palette_t *palette) {
    const int count = game.mode == MODE_PLAYING ?
                      (game.speed > 145.0f ? 34 : 18) : 12;
    const float strength = game.mode == MODE_PLAYING ?
                           clampf((game.speed - 80.0f) / 100.0f, 0.20f, 1.0f) :
                           0.22f;
    int i;
    for(i = 0; i < count; ++i) {
        const uint32_t h = hash_u32((uint32_t)i * 4153u + 0x8f1bbcd9u);
        float relative_z = fmodf((float)(h & 1023u) +
                                 game.distance * (0.82f + (float)(i % 3) * 0.11f),
                                 930.0f) + 55.0f;
        const float x = ((float)((int)((h >> 10) & 255u) - 127)) * 1.35f;
        const float y = 12.0f + (float)((h >> 18) & 63u) * 1.05f;
        const float z0 = game.distance + relative_z;
        const float z1 = z0 - (11.0f + strength * 34.0f);
        screen_point_t a, b;
        if(project_world((vec3_t){path_center(z0) + x, y, z0}, &a) &&
           project_world((vec3_t){path_center(z1) + x, y, z1}, &b)) {
            draw_line(&additive_header, a.x, a.y, a.z, b.x, b.y, b.z,
                      1.0f + strength * 1.6f,
                      pack_color(0.0f, palette->river),
                      pack_color(0.20f * strength, palette->river));
        }
    }
}

static void draw_bar(float x, float y, float width, float height,
                     float value, color3_t color) {
    const uint32_t frame = pack_color(0.72f, color_scale(color, 1.25f));
    const uint32_t back = pack_color(0.54f, (color3_t){0.008f, 0.016f, 0.034f});
    const uint32_t fill = pack_color(0.88f, color);
    value = clampf(value, 0.0f, 1.0f);
    draw_rect(&hud_header, x, y, width, height, 9.0f, back);
    draw_rect(&hud_header, x + 2.0f, y + 2.0f,
              (width - 4.0f) * value, height - 4.0f, 9.0f, fill);
    draw_rect(&hud_header, x, y, width, 1.0f, 9.0f, frame);
    draw_rect(&hud_header, x, y + height - 1.0f, width, 1.0f, 9.0f, frame);
    draw_rect(&hud_header, x, y, 1.0f, height, 9.0f, frame);
    draw_rect(&hud_header, x + width - 1.0f, y, 1.0f, height, 9.0f, frame);
}

static void draw_reticle(const palette_t *palette) {
    const vec3_t aim_target = player_aim_target_world();
    screen_point_t target;
    const uint32_t color = pack_color(0.64f, palette->river);
    const uint32_t dim = pack_color(0.22f, palette->river);
    const float r = 15.0f + fsin(game.time * 5.0f) * 1.5f;
    if(!project_absolute_world(aim_target, &target))
        return;
    draw_line(&hud_header, target.x - r, target.y - r, 9.0f,
              target.x - r * 0.35f, target.y - r, 9.0f, 1.5f, color, color);
    draw_line(&hud_header, target.x + r * 0.35f, target.y - r, 9.0f,
              target.x + r, target.y - r, 9.0f, 1.5f, color, color);
    draw_line(&hud_header, target.x - r, target.y + r, 9.0f,
              target.x - r * 0.35f, target.y + r, 9.0f, 1.5f, color, color);
    draw_line(&hud_header, target.x + r * 0.35f, target.y + r, 9.0f,
              target.x + r, target.y + r, 9.0f, 1.5f, color, color);
    draw_line(&hud_header, target.x, target.y - 5.0f, 9.0f,
              target.x, target.y + 5.0f, 9.0f, 1.0f, dim, color);
    draw_line(&hud_header, target.x - 5.0f, target.y, 9.0f,
              target.x + 5.0f, target.y, 9.0f, 1.0f, dim, color);
    draw_rect(&hud_header, target.x - 1.5f, target.y - 1.5f,
              3.0f, 3.0f, 9.0f,
              pack_color(0.72f,
                         color_lerp(palette->accent,
                                    (color3_t){1.0f, 0.04f, 0.62f}, 0.68f)));
}

static void draw_nova_wave(const palette_t *palette) {
    int i;
    const float radius = smoothstepf(game.bomb_wave) * 480.0f;
    const float alpha = (1.0f - game.bomb_wave) * 0.76f;
    const uint32_t color = pack_color(alpha, palette->river);
    if(game.bomb_wave <= 0.0f)
        return;
    for(i = 0; i < 48; ++i) {
        const float a0 = (float)i * TAU / 48.0f;
        const float a1 = (float)(i + 1) * TAU / 48.0f;
        draw_line(&hud_header,
                  SCREEN_CX + fcos(a0) * radius,
                  SCREEN_CY + fsin(a0) * radius,
                  9.0f,
                  SCREEN_CX + fcos(a1) * radius,
                  SCREEN_CY + fsin(a1) * radius,
                  9.0f, 8.0f * (1.0f - game.bomb_wave) + 1.0f,
                  color, color);
    }
}

static void draw_vignette_and_scanlines(void) {
    const uint32_t black_soft = pack_color(0.18f, (color3_t){0.0f, 0.0f, 0.0f});
    const uint32_t black_hard = pack_color(0.56f, (color3_t){0.0f, 0.0f, 0.0f});
    int y;
    draw_rect(&hud_header, 0.0f, 0.0f, 9.0f, SCREEN_H, 9.0f, black_hard);
    draw_rect(&hud_header, 9.0f, 0.0f, 16.0f, SCREEN_H, 9.0f, black_soft);
    draw_rect(&hud_header, SCREEN_W - 9.0f, 0.0f, 9.0f, SCREEN_H,
              9.0f, black_hard);
    draw_rect(&hud_header, SCREEN_W - 25.0f, 0.0f, 16.0f, SCREEN_H,
              9.0f, black_soft);
    draw_rect(&hud_header, 0.0f, 0.0f, SCREEN_W, 5.0f, 9.0f, black_hard);
    draw_rect(&hud_header, 0.0f, SCREEN_H - 5.0f, SCREEN_W, 5.0f,
              9.0f, black_hard);
    for(y = 2; y < (int)SCREEN_H; y += 5)
        draw_rect(&hud_header, 0.0f, (float)y, SCREEN_W, 1.0f, 9.0f,
                  pack_color(0.035f, (color3_t){0.0f, 0.0f, 0.0f}));
}

static void draw_game_hud(const palette_t *palette) {
    char buffer[64];
    const color3_t white = {0.78f, 0.91f, 1.0f};
    const color3_t danger = {1.0f, 0.28f, 0.24f};
    const color3_t hud_cyan = color_lerp(
        palette->river, (color3_t){0.03f, 0.92f, 1.0f}, 0.76f);
    const color3_t hud_magenta = color_lerp(
        palette->accent, (color3_t){1.0f, 0.04f, 0.62f}, 0.76f);
    const uint32_t cyan_edge = pack_color(0.70f, hud_cyan);
    const uint32_t pink_edge = pack_color(0.64f, hud_magenta);
    const uint32_t clear_edge = pack_color(0.0f, hud_cyan);

    draw_rect(&hud_header, 17.0f, 15.0f, 204.0f, 38.0f, 9.0f,
              pack_color(0.46f, (color3_t){0.005f, 0.012f, 0.030f}));
    draw_rect(&hud_header, 419.0f, 15.0f, 204.0f, 38.0f, 9.0f,
              pack_color(0.46f, (color3_t){0.005f, 0.012f, 0.030f}));
    draw_line(&hud_header, 17.0f, 14.0f, 9.0f, 126.0f, 14.0f, 9.0f,
              1.6f, cyan_edge, clear_edge);
    draw_line(&hud_header, 221.0f, 54.0f, 9.0f, 112.0f, 54.0f, 9.0f,
              1.2f, pink_edge, clear_edge);
    draw_line(&hud_header, 419.0f, 14.0f, 9.0f, 528.0f, 14.0f, 9.0f,
              1.6f, pink_edge, clear_edge);
    draw_line(&hud_header, 623.0f, 54.0f, 9.0f, 514.0f, 54.0f, 9.0f,
              1.2f, cyan_edge, clear_edge);
    snprintf(buffer, sizeof(buffer), "SCORE %07d", game.score);
    draw_text(28.0f, 26.0f, buffer, 2, white, 0.92f);
    snprintf(buffer, sizeof(buffer), "%05.1f KM", game.distance / 1000.0f);
    draw_text(455.0f, 26.0f, buffer, 2, white, 0.92f);

    draw_text(24.0f, 423.0f, "SHIELD", 2,
              game.shield < 28.0f ? danger : white, 0.92f);
    draw_bar(24.0f, 445.0f, 188.0f, 14.0f, game.shield / 100.0f,
             game.shield < 28.0f ? danger : palette->river);
    draw_text(447.0f, 423.0f, "BOOST", 2, white, 0.92f);
    draw_bar(447.0f, 445.0f, 168.0f, 14.0f, game.boost / 100.0f,
             palette->accent);
    snprintf(buffer, sizeof(buffer), "L%d  NOVA %d", game.weapon_level, game.bombs);
    draw_text(267.0f, 445.0f, buffer, 2, palette->accent, 0.92f);

    {
        enemy_t *guardian = active_guardian();
        if(guardian) {
            const int guardian_biome = guardian->biome & 3;
            const palette_t *guardian_palette = &palettes[guardian_biome];
            const float hit_feedback = smoothstepf(
                guardian->hit_flash / 0.18f);
            const color3_t guardian_bar = color_lerp(
                guardian_palette->enemy, (color3_t){1.0f,1.0f,0.72f},
                hit_feedback * 0.88f);
            draw_rect(&hud_header, 153.0f, 64.0f, 334.0f, 43.0f, 9.0f,
                      pack_color(0.58f, (color3_t){0.025f,0.004f,0.020f}));
            draw_text_centered(70.0f,
                               guardian_profiles[guardian_biome].name, 2,
                               guardian_bar, 0.96f);
            if(hit_feedback > 0.02f) {
                draw_rect(&hud_header, 174.0f, 88.0f, 292.0f, 16.0f, 9.0f,
                          pack_color(hit_feedback * 0.36f,
                                     (color3_t){1.0f,0.96f,0.62f}));
                draw_text(444.0f, 72.0f, "HIT", 1,
                          (color3_t){1.0f,0.96f,0.62f}, hit_feedback);
            }
            draw_bar(177.0f, 91.0f, 286.0f, 10.0f,
                     (float)guardian->hp /
                     (float)(guardian->max_hp > 0 ? guardian->max_hp : 1),
                     guardian_bar);
        }
    }

    if(game.combo > 1 && !active_guardian()) {
        const int multiplier = 1 + (game.combo / 5 > 7 ? 7 : game.combo / 5);
        snprintf(buffer, sizeof(buffer), "CHAIN %02d  X%d", game.combo, multiplier);
        draw_text_centered(65.0f, buffer, 2, palette->accent,
                           clampf(game.combo_timer, 0.30f, 1.0f));
    }
    if(game.message_time > 0.0f && game.mode == MODE_PLAYING)
        draw_text_centered(active_guardian() ? 116.0f : 93.0f,
                           game.message, 3, palette->river,
                           clampf(game.message_time, 0.0f, 1.0f));

    if(game.speed_boost_time > 0.0f) {
        const float y = game.temporary_weapon_time > 0.0f ? 367.0f : 392.0f;
        snprintf(buffer, sizeof(buffer), "SPEED BOOST %d",
                 (int)ceilf(game.speed_boost_time));
        draw_text_centered(y, buffer, 2,
                           (color3_t){1.0f,0.72f,0.18f}, 0.94f);
        draw_bar(240.0f, y + 16.0f, 160.0f, 5.0f,
                 game.speed_boost_time / SPEED_BOOST_SECONDS,
                 (color3_t){1.0f,0.52f,0.12f});
    }

    if(game.temporary_weapon_time > 0.0f) {
        const float y = game.speed_boost_time > 0.0f ? 394.0f : 392.0f;
        const float urgent = game.temporary_weapon_time < 3.0f ?
            0.56f + 0.44f * fabsf(fsin(game.time * 10.0f)) : 0.94f;
        const color3_t weapon_color =
            game.temporary_weapon == TEMP_WEAPON_PHASE_WAVE ?
            (color3_t){1.0f,0.24f,0.86f} :
            (color3_t){0.18f,0.94f,1.0f};
        snprintf(buffer, sizeof(buffer), "%s %d",
                 game.temporary_weapon == TEMP_WEAPON_PHASE_WAVE ?
                 "PHASE WAVE" : "FAST LASER",
                 (int)ceilf(game.temporary_weapon_time));
        draw_text_centered(y, buffer, 2, weapon_color, urgent);
        draw_bar(240.0f, y + 16.0f, 160.0f, 5.0f,
                 game.temporary_weapon_time / TEMP_WEAPON_SECONDS,
                 weapon_color);
    }

    if(game.hit_cooldown > 0.0f) {
        const uint32_t hit = pack_color(clampf(game.hit_cooldown * 0.45f,
                                               0.0f, 0.34f), danger);
        draw_rect(&hud_header, 0.0f, 0.0f, SCREEN_W, 12.0f, 9.0f, hit);
        draw_rect(&hud_header, 0.0f, SCREEN_H - 12.0f,
                  SCREEN_W, 12.0f, 9.0f, hit);
        draw_rect(&hud_header, 0.0f, 0.0f, 12.0f, SCREEN_H, 9.0f, hit);
        draw_rect(&hud_header, SCREEN_W - 12.0f, 0.0f,
                  12.0f, SCREEN_H, 9.0f, hit);
    }
    draw_reticle(palette);
}

static void draw_title_overlay(const palette_t *palette, bool controller) {
    char music_label[48];
    const float pulse = 0.78f + fsin(game.time * 3.3f) * 0.18f;
    const color3_t title_cyan = color_lerp(
        palette->river, (color3_t){0.03f, 0.92f, 1.0f}, 0.72f);
    const color3_t title_magenta = color_lerp(
        palette->accent, (color3_t){1.0f, 0.04f, 0.62f}, 0.72f);
    const uint32_t cyan = pack_color(0.82f, title_cyan);
    const uint32_t magenta = pack_color(0.76f, title_magenta);
    const uint32_t transparent = pack_color(0.0f, title_cyan);
    draw_rect(&hud_header, 84.0f, 31.0f, 472.0f, 163.0f, 9.0f,
              pack_color(0.34f, (color3_t){0.0f, 0.008f, 0.025f}));
    draw_line(&hud_header, 84.0f, 31.0f, 9.0f, 242.0f, 31.0f, 9.0f,
              2.1f, cyan, transparent);
    draw_line(&hud_header, 398.0f, 31.0f, 9.0f, 556.0f, 31.0f, 9.0f,
              2.1f, transparent, magenta);
    draw_line(&hud_header, 84.0f, 31.0f, 9.0f, 84.0f, 72.0f, 9.0f,
              1.5f, cyan, transparent);
    draw_line(&hud_header, 556.0f, 31.0f, 9.0f, 556.0f, 72.0f, 9.0f,
              1.5f, magenta, transparent);
    draw_line(&hud_header, 84.0f, 194.0f, 9.0f, 184.0f, 194.0f, 9.0f,
              1.3f, magenta, transparent);
    draw_line(&hud_header, 456.0f, 194.0f, 9.0f, 556.0f, 194.0f, 9.0f,
              1.3f, transparent, cyan);
    draw_text_centered(58.0f, "GRAVITY WAVE", 5,
                       title_magenta, 0.34f);
    draw_text_centered(55.0f, "GRAVITY WAVE", 5,
                       (color3_t){0.01f, 0.02f, 0.06f}, 0.82f);
    draw_text_centered(51.0f, "GRAVITY WAVE", 5, title_cyan, 0.99f);
    draw_text_centered(101.0f, "INFINITE 3D FLIGHT", 2,
                       (color3_t){0.82f, 0.91f, 1.0f}, 0.92f);
    draw_line(&hud_header, 145.0f, 129.0f, 9.0f, 495.0f, 129.0f, 9.0f,
              2.0f, pack_color(0.0f, palette->accent),
              pack_color(0.70f, palette->accent));
    draw_text_centered(146.0f, "ONE PILOT  ENDLESS SKY", 2,
                       palette->accent, 0.86f);
    if(current_music_track >= 0 && current_music_track < MUSIC_TRACK_COUNT) {
        snprintf(music_label, sizeof(music_label), "NOW PLAYING  %s",
                 soundtrack_defs[current_music_track].name);
        draw_text_centered(307.0f, music_label, 1,
                           color_lerp(title_cyan, title_magenta, 0.48f), 0.82f);
    }

    draw_rect(&hud_header, 95.0f, 329.0f, 450.0f, 124.0f, 9.0f,
              pack_color(0.48f, (color3_t){0.0f, 0.008f, 0.024f}));
    draw_line(&hud_header, 95.0f, 329.0f, 9.0f, 181.0f, 329.0f, 9.0f,
              1.4f, cyan, transparent);
    draw_line(&hud_header, 459.0f, 329.0f, 9.0f, 545.0f, 329.0f, 9.0f,
              1.4f, transparent, magenta);
    if(!controller) {
        draw_text_centered(344.0f, "CONNECT CONTROLLER", 2,
                           (color3_t){1.0f, 0.55f, 0.24f}, pulse);
        draw_text_centered(383.0f, "GAMEPAD OR KEYBOARD REQUIRED", 1,
                           (color3_t){0.70f, 0.82f, 0.93f}, 0.86f);
    }
    else if(title_sound_test) {
        char track_number[24];
        draw_text_centered(340.0f, "SOUND TEST", 2,
                           palette->river, 0.94f);
        draw_text_centered(370.0f, soundtrack_defs[sound_test_track].name, 2,
                           color_lerp(title_cyan, title_magenta, 0.52f), pulse);
        snprintf(track_number, sizeof(track_number), "TRACK %02d / %02d",
                 sound_test_track + 1, MUSIC_TRACK_COUNT);
        draw_text_centered(399.0f, track_number, 1,
                           (color3_t){0.78f, 0.88f, 0.97f}, 0.88f);
        draw_text_centered(418.0f, "LEFT RIGHT SELECT AND PLAY", 1,
                           (color3_t){0.70f, 0.82f, 0.93f}, 0.84f);
        draw_text_centered(438.0f, "A RESTART   B RETURN", 1,
                           color_scale(palette->accent, 0.92f), 0.84f);
    }
    else {
        const color3_t idle = {0.70f, 0.82f, 0.93f};
        draw_text_centered(340.0f,
                           title_menu_item == TITLE_MENU_START ?
                           "> START FLIGHT <" : "START FLIGHT",
                           2, title_menu_item == TITLE_MENU_START ?
                           palette->river : idle,
                           title_menu_item == TITLE_MENU_START ? pulse : 0.80f);
        draw_text_centered(372.0f,
                           title_menu_item == TITLE_MENU_SOUND_TEST ?
                           "> SOUND TEST <" : "SOUND TEST",
                           2, title_menu_item == TITLE_MENU_SOUND_TEST ?
                           palette->river : idle,
                           title_menu_item == TITLE_MENU_SOUND_TEST ?
                           pulse : 0.80f);
        draw_text_centered(404.0f,
                           title_menu_item == TITLE_MENU_EXIT ?
                           "> EXIT <" : "EXIT",
                           2, title_menu_item == TITLE_MENU_EXIT ?
                           palette->river : idle,
                           title_menu_item == TITLE_MENU_EXIT ? pulse : 0.80f);
        draw_text_centered(437.0f, "DPAD SELECT   A OR START CONFIRM", 1,
                           color_scale(palette->accent, 0.92f), 0.80f);
    }
}

static void draw_pause_overlay(const palette_t *palette) {
    draw_rect(&hud_header, 0.0f, 0.0f, SCREEN_W, SCREEN_H, 9.0f,
              pack_color(0.57f, (color3_t){0.0f, 0.005f, 0.018f}));
    draw_text_centered(181.0f, "PAUSED", 5, palette->river, 0.96f);
    draw_text_centered(249.0f, "START RESUME", 2,
                       (color3_t){0.80f, 0.90f, 1.0f}, 0.86f);
    draw_text_centered(278.0f, "B ABORT RUN", 2, palette->accent, 0.84f);
}

static void draw_game_over_overlay(const palette_t *palette) {
    char buffer[64];
    const float pulse = 0.72f + fsin(game.time * 3.2f) * 0.22f;
    draw_rect(&hud_header, 0.0f, 0.0f, SCREEN_W, SCREEN_H, 9.0f,
              pack_color(0.54f, (color3_t){0.025f, 0.0f, 0.012f}));
    draw_text_centered(124.0f, "FLIGHT LOST", 5,
                       (color3_t){1.0f, 0.25f, 0.30f}, 0.95f);
    snprintf(buffer, sizeof(buffer), "FINAL SCORE %07d", game.score);
    draw_text_centered(210.0f, buffer, 2,
                       (color3_t){0.82f, 0.92f, 1.0f}, 0.92f);
    snprintf(buffer, sizeof(buffer), "DISTANCE %05.1f KM", game.distance / 1000.0f);
    draw_text_centered(239.0f, buffer, 2,
                       (color3_t){0.82f, 0.92f, 1.0f}, 0.92f);
    draw_text_centered(301.0f, "START OR A RETRY", 2,
                       palette->river, pulse);
    draw_text_centered(332.0f, "B RETURN TO TITLE", 2,
                       palette->accent, 0.82f);
}

static void draw_hud(const palette_t *palette, bool controller_connected) {
    draw_nova_wave(palette);
    if(game.mode == MODE_TITLE)
        draw_title_overlay(palette, controller_connected);
    else {
        draw_game_hud(palette);
        if(game.mode == MODE_PAUSED)
            draw_pause_overlay(palette);
        else if(game.mode == MODE_GAME_OVER)
            draw_game_over_overlay(palette);
    }
    draw_vignette_and_scanlines();
}

static void render_frame(bool controller_connected) {
    palette_t palette;
    get_blended_palette(&palette);
    setup_camera();
    prepare_terrain(&palette);

    pvr_set_bg_color(palette.fog.r, palette.fog.g, palette.fog.b);
    update_hardware_fog(palette.fog);

    pvr_scene_begin();
    begin_poly_list(PVR_LIST_OP_POLY);
    draw_sky_opaque(&palette);
    draw_terrain();
    draw_river(&palette);
    draw_biome_scenery_opaque(&palette);
    draw_gate_petals_opaque(&palette);
    draw_enemies(&palette);
    if(game.mode != MODE_GAME_OVER)
        draw_player_ship(&palette);
    pvr_list_finish();

    begin_poly_list(PVR_LIST_TR_POLY);
    draw_sun_glow(&palette);
    draw_neon_route_grid(&palette);
    draw_biome_scenery_translucent(&palette);
    draw_speed_streaks(&palette);
    draw_gates(&palette);
    draw_pickups(&palette);
    draw_projectile_pool(player_shots, MAX_SHOTS, false, &palette);
    draw_projectile_pool(enemy_shots, MAX_ENEMY_SHOTS, true, &palette);
    draw_particles();
    draw_enemy_glows(&palette);
    if(game.mode != MODE_GAME_OVER)
        draw_player_glow(&palette);
    draw_hud(&palette, controller_connected);
    pvr_list_finish();
    pvr_scene_finish();
}

static void release_textures(void) {
    int i;
    for(i = 0; i < GRAVITY_WAVE_TEXTURE_COUNT; ++i) {
        if(texture_vram[i]) {
            pvr_mem_free(texture_vram[i]);
            texture_vram[i] = NULL;
        }
    }
}

static int load_textures(void) {
    uint32_t total_bytes = 0;
    int i;

    memset(texture_vram, 0, sizeof(texture_vram));
    for(i = 0; i < GRAVITY_WAVE_TEXTURE_COUNT; ++i) {
        const gravity_wave_texture_asset_t *asset = &gravity_wave_texture_assets[i];
        texture_vram[i] = pvr_mem_malloc(asset->byte_size);
        if(!texture_vram[i]) {
            printf("Gravity Wave: PVR texture allocation failed at asset %d.\n", i);
            release_textures();
            return -1;
        }
        pvr_txr_load_ex(asset->pixels, texture_vram[i],
                        asset->width, asset->height, PVR_TXRLOAD_16BPP);
        total_bytes += asset->byte_size;
    }
    printf("Gravity Wave: loaded %d twiddled textures (%lu KiB PVR RAM).\n",
           GRAVITY_WAVE_TEXTURE_COUNT, (unsigned long)((total_bytes + 1023u) / 1024u));
    return 0;
}

static void compile_texture_headers(void) {
    pvr_poly_cxt_t context;
    int i;

    for(i = 0; i < 8; ++i) {
        const gravity_wave_texture_asset_t *asset = &gravity_wave_texture_assets[i];
        pvr_poly_cxt_txr(&context, PVR_LIST_OP_POLY,
                         PVR_TXRFMT_RGB565,
                         asset->width, asset->height,
                         texture_vram[i], PVR_FILTER_BILINEAR);
        context.gen.shading = PVR_SHADE_GOURAUD;
        context.gen.culling = PVR_CULLING_NONE;
        context.gen.fog_type = PVR_FOG_TABLE;
        context.depth.comparison = PVR_DEPTHCMP_GREATER;
        context.depth.write = PVR_DEPTHWRITE_ENABLE;
        context.txr.env = PVR_TXRENV_MODULATE;
        context.txr.uv_clamp = PVR_UVCLAMP_NONE;
        pvr_poly_compile(&texture_headers[i], &context);
    }

    for(i = 8; i < GRAVITY_WAVE_TEXTURE_COUNT; ++i) {
        const int sprite = i - 8;
        const gravity_wave_texture_asset_t *asset = &gravity_wave_texture_assets[i];
        pvr_poly_cxt_txr(&context, PVR_LIST_TR_POLY,
                         PVR_TXRFMT_ARGB4444,
                         asset->width, asset->height,
                         texture_vram[i], PVR_FILTER_BILINEAR);
        context.gen.alpha = true;
        context.gen.shading = PVR_SHADE_GOURAUD;
        context.gen.culling = PVR_CULLING_NONE;
        context.gen.fog_type = PVR_FOG_DISABLE;
        context.depth.comparison = PVR_DEPTHCMP_GREATER;
        context.depth.write = PVR_DEPTHWRITE_DISABLE;
        context.blend.src = PVR_BLEND_SRCALPHA;
        context.blend.dst = PVR_BLEND_INVSRCALPHA;
        context.txr.alpha = PVR_TXRALPHA_ENABLE;
        context.txr.env = PVR_TXRENV_MODULATEALPHA;
        context.txr.uv_clamp = PVR_UVCLAMP_UV;
        pvr_poly_compile(&texture_headers[i], &context);

        context.blend.dst = PVR_BLEND_ONE;
        pvr_poly_compile(&sprite_additive_headers[sprite], &context);
    }
}

static int init_graphics(void) {
    pvr_poly_cxt_t context;
    pvr_init_params_t pvr_params = pvr_default_params;

    vid_set_enabled(0);
    vid_set_mode(DM_640x480, PM_RGB565);
    vid_set_dithering(true);
    pvr_params.vertex_buf_size = 768 * 1024;
    if(pvr_init(&pvr_params) < 0) {
        printf("Gravity Wave: PowerVR initialization failed.\n");
        vid_set_enabled(1);
        return -1;
    }
    if(load_textures() < 0) {
        pvr_shutdown();
        vid_set_enabled(1);
        return -1;
    }
    printf("Gravity Wave: %lu KiB PVR texture memory remains.\n",
           (unsigned long)(pvr_mem_available() / 1024u));
    /* Keep the hardware background behind our explicit sky layers. */
    pvr_set_zclip(0.0000001f);
    update_hardware_fog(palettes[0].fog);
    pvr_fog_table_linear(90.0f, FAR_PLANE);

    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    context.gen.shading = PVR_SHADE_GOURAUD;
    context.gen.culling = PVR_CULLING_NONE;
    context.gen.fog_type = PVR_FOG_DISABLE;
    context.depth.comparison = PVR_DEPTHCMP_GREATER;
    context.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(&opaque_header, &context);

    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    context.gen.shading = PVR_SHADE_GOURAUD;
    context.gen.culling = PVR_CULLING_NONE;
    context.gen.fog_type = PVR_FOG_TABLE;
    context.depth.comparison = PVR_DEPTHCMP_GREATER;
    context.depth.write = PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(&terrain_header, &context);

    pvr_poly_cxt_col(&context, PVR_LIST_TR_POLY);
    context.gen.alpha = true;
    context.gen.shading = PVR_SHADE_GOURAUD;
    context.gen.culling = PVR_CULLING_NONE;
    context.gen.fog_type = PVR_FOG_DISABLE;
    context.depth.comparison = PVR_DEPTHCMP_GREATER;
    context.depth.write = PVR_DEPTHWRITE_DISABLE;
    context.blend.src = PVR_BLEND_SRCALPHA;
    context.blend.dst = PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&translucent_header, &context);

    context.blend.src = PVR_BLEND_SRCALPHA;
    context.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&additive_header, &context);

    context.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    context.blend.src = PVR_BLEND_SRCALPHA;
    context.blend.dst = PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&hud_header, &context);
    compile_texture_headers();
    vid_set_enabled(1);
    return 0;
}

int main(int argc, char **argv) {
    uint64_t previous_time;
    bool running = true;

    (void)argc;
    (void)argv;
    printf("Gravity Wave booting.\n");
    printf("Controls: analog/D-pad fly, A fire, B boost, X brake, "
           "Y nova, L/R roll, Start pause.\n");

    if(init_graphics() < 0)
        return 1;
    init_audio();
    memset(&game, 0, sizeof(game));
    {
        const uint64_t entropy = timer_us_gettime64();
        const uint64_t rtc_entropy = (uint64_t)rtc_unix_secs();
        random_state = hash_u32(random_state ^ (uint32_t)entropy ^
                                (uint32_t)(entropy >> 32) ^
                                (uint32_t)rtc_entropy ^
                                (uint32_t)(rtc_entropy >> 32) ^
                                (uint32_t)pvr_get_vbl_count() * 0x9e3779b9u);
        music_random_state = hash_u32(
            0xa17ca55eu ^ (uint32_t)(entropy >> 7) ^
            (uint32_t)(rtc_entropy * 0x85ebca6bu) ^
            (uint32_t)pvr_get_vbl_count() * 0xc2b2ae35u);
    }
    enter_title();
#ifdef GRAVITY_WAVE_AUTOTEST
#ifdef GRAVITY_WAVE_AUTOTEST_SOUND_TEST
    if(!run_title_sound_test_self_test()) {
        shutdown_audio();
        release_textures();
        pvr_shutdown();
        return 6;
    }
#endif
    reset_game();
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_SHUFFLE
    if(!run_music_shuffle_self_test()) {
        shutdown_audio();
        release_textures();
        pvr_shutdown();
        return 5;
    }
#endif
#ifdef GRAVITY_WAVE_AUTOTEST_COURSE_PROFILE
    if(!run_course_profile_self_test()) {
        shutdown_audio();
        release_textures();
        pvr_shutdown();
        return 4;
    }
#endif
#ifdef GRAVITY_WAVE_AUTOTEST_COMBAT_GEOMETRY
    if(!run_combat_geometry_self_test()) {
        shutdown_audio();
        release_textures();
        pvr_shutdown();
        return 2;
    }
#endif
#ifdef GRAVITY_WAVE_AUTOTEST_BOSS_PROFILES
    if(!run_guardian_profile_self_test()) {
        shutdown_audio();
        release_textures();
        pvr_shutdown();
        return 3;
    }
#endif
#ifndef GRAVITY_WAVE_AUTOTEST_DISTANCE
#define GRAVITY_WAVE_AUTOTEST_DISTANCE 4120.0f
#endif
    game.distance = GRAVITY_WAVE_AUTOTEST_DISTANCE;
    game.palette_index = ((int)(game.distance / BIOME_LENGTH)) & 3;
    game.last_palette_index = game.palette_index;
    game.music_chapter = course_chapter_at(game.distance);
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
    game.music_chapter = 0;
    music_jukebox_complete = false;
    music_jukebox_failed = false;
    stop_music();
    play_music_section(0, 0, MUSIC_GAME_VOLUME);
#else
    request_next_shuffled_music(MUSIC_GAME_VOLUME);
#endif
    game.next_wave_z = next_course_wave_z(game.distance + 200.0f);
    game.next_gate_z = next_course_traversal_z(game.distance + 200.0f);
#ifdef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
    game.next_wave_z = game.distance + 2400.0f;
    game.next_gate_z = game.distance + 4000.0f;
    spawn_gate(game.distance + 1050.0f);
    gates[0].x = 0.0f;
    gates[0].y = 35.0f;
    gates[0].radius = 16.0f;
#if defined(GRAVITY_WAVE_AUTOTEST_TRAVERSAL_SLALOM)
    gates[0].kind = TRAVERSAL_SLALOM;
#ifndef GRAVITY_WAVE_AUTOTEST_DIRECTION
#define GRAVITY_WAVE_AUTOTEST_DIRECTION 1
#endif
    gates[0].direction = GRAVITY_WAVE_AUTOTEST_DIRECTION;
    gates[0].stage_count = 3;
    gates[0].stage_spacing = traversal_stage_spacing_for_kind(
        TRAVERSAL_SLALOM);
    gates[0].radius = 19.0f;
    printf("Gravity Wave autotest: three-stage Vector Lane route enabled.\n");
#elif defined(GRAVITY_WAVE_AUTOTEST_TRAVERSAL_SHEAR)
    gates[0].kind = TRAVERSAL_SHEAR_BARRIER;
#ifndef GRAVITY_WAVE_AUTOTEST_DIRECTION
#define GRAVITY_WAVE_AUTOTEST_DIRECTION 1
#endif
    gates[0].direction = GRAVITY_WAVE_AUTOTEST_DIRECTION;
    gates[0].stage_count = 2;
    gates[0].stage_spacing = 230.0f;
    gates[0].y = 50.0f;
    gates[0].radius = 9.0f;
    printf("Gravity Wave autotest: two-stage Shear Run enabled.\n");
#else
    gates[0].kind = TRAVERSAL_GRAVITY_BLOOM;
    gates[0].stage_count = 1;
    gates[0].stage_spacing = 0.0f;
    printf("Gravity Wave autotest: centered Gravity Bloom enabled.\n");
#endif
#endif
#ifdef GRAVITY_WAVE_AUTOTEST_POWERUPS
    {
        pickup_t test_pickup = {.active = true, .kind = PICKUP_SPEED_BOOST};
        collect_pickup(&test_pickup);
        test_pickup.active = true;
#ifdef GRAVITY_WAVE_AUTOTEST_PHASE_WAVE
        test_pickup.kind = PICKUP_PHASE_WAVE;
#else
        test_pickup.kind = PICKUP_FAST_LASER;
#endif
        collect_pickup(&test_pickup);
        printf("Gravity Wave autotest: speed boost plus %s enabled.\n",
               game.temporary_weapon == TEMP_WEAPON_PHASE_WAVE ?
               "phase wave" : "fast laser");
    }
#endif
#ifdef GRAVITY_WAVE_AUTOTEST_SOUND_TEST_VIEW
    enter_title();
    title_menu_item = TITLE_MENU_SOUND_TEST;
    title_sound_test = true;
    sound_test_track = current_music_track >= 0 ? current_music_track : 0;
    audition_music_track(sound_test_track);
    game.time = 0.0f;
#endif
#ifdef GRAVITY_WAVE_AUTOTEST_BOSS
    game.wave = 7;
    game.next_wave_z = game.distance + 310.0f;
#endif
#endif
    previous_time = timer_us_gettime64();

    while(running) {
        const uint64_t now = timer_us_gettime64();
        float dt = (float)(now - previous_time) * 0.000001f;
        input_t input;
        previous_time = now;
        dt = clampf(dt, 0.001f, 0.050f);
#ifdef GRAVITY_WAVE_AUTOTEST_FIXED_DT
        dt = GRAVITY_WAVE_AUTOTEST_FIXED_DT;
#endif
        input = poll_input();
#ifdef GRAVITY_WAVE_AUTOTEST
        input.connected = true;
#ifdef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
#if defined(GRAVITY_WAVE_AUTOTEST_TRAVERSAL_SLALOM)
        {
            const gate_t *gate = &gates[0];
            const int stage = gate->stage < gate->stage_count ?
                              gate->stage : gate->stage_count - 1;
#ifdef GRAVITY_WAVE_AUTOTEST_TRAVERSAL_COLLISION
            const float target_x = traversal_stage_x(gate, stage) +
                                   gate->radius + 1.0f;
#else
            const float target_x = traversal_stage_x(gate, stage);
#endif
            input.x = clampf((target_x - game.player_x) * 0.10f,
                             -1.0f, 1.0f);
            input.y = 0.0f;
        }
#elif defined(GRAVITY_WAVE_AUTOTEST_TRAVERSAL_SHEAR)
        {
            const gate_t *gate = &gates[0];
            const int stage = gate->stage < gate->stage_count ?
                              gate->stage : gate->stage_count - 1;
#ifdef GRAVITY_WAVE_AUTOTEST_TRAVERSAL_COLLISION
            const float target_y = traversal_stage_y(gate, stage);
#else
            const float target_y = traversal_stage_y(gate, stage) +
                (float)traversal_stage_direction(gate, stage) * 22.0f;
#endif
            input.x = 0.0f;
            input.y = clampf((game.player_y - target_y) * 0.10f,
                             -1.0f, 1.0f);
        }
#else
        input.x = 0.0f;
        input.y = 0.0f;
#endif
#elif defined(GRAVITY_WAVE_AUTOTEST_EXTENTS)
        input.x = fsin(game.time * 0.58f);
        input.y = fsin(game.time * 0.47f + 0.7f);
#elif defined(GRAVITY_WAVE_AUTOTEST_BOSS_HITS) || \
      defined(GRAVITY_WAVE_AUTOTEST_BOSS_AIM)
        {
            const enemy_t *guardian = active_guardian();
            if(guardian) {
                input.x = clampf((guardian->x - game.player_x) * 0.09f,
                                 -1.0f, 1.0f);
                input.y = clampf((game.player_y - guardian->y) * 0.09f,
                                 -1.0f, 1.0f);
            }
            else {
                input.x = 0.0f;
                input.y = 0.0f;
            }
        }
#else
        input.x = fsin(game.time * 0.73f) * 0.72f;
        input.y = fsin(game.time * 0.51f + 0.7f) * 0.48f;
#endif
        input.buttons |= CONT_A;
#ifndef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
        if(fmodf(game.time, 7.0f) < 2.0f)
            input.buttons |= CONT_B;
#endif
#endif
        running = update_game(&input, dt);
        render_frame(input.connected);
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
        if(music_jukebox_complete && music_section_time >= 0.75f)
            running = false;
#endif
#ifdef GRAVITY_WAVE_AUTOTEST_EXIT_SECONDS
        if(game.time >= GRAVITY_WAVE_AUTOTEST_EXIT_SECONDS)
            running = false;
#endif
#ifdef GRAVITY_WAVE_AUTOTEST
        {
            static int last_stats_second = -1;
            const int stats_second = (int)game.time;
            if(stats_second != last_stats_second && stats_second > 0 &&
               (stats_second % 5) == 0) {
                pvr_stats_t stats;
                int active_shots = 0;
                int shot;
                for(shot = 0; shot < MAX_SHOTS; ++shot) {
                    if(player_shots[shot].active)
                        active_shots++;
                }
                if(pvr_get_stats(&stats) == 0) {
                    printf("Gravity Wave perf: %.1f fps frame=%.2fms "
                           "reg=%.2fms render=%.2fms vbuf=%lu/%lu KiB "
                           "shots=%d weapon=%.1fs boost=%.1fs velocity=%.1f "
                           "pos=(%.1f,%.1f)\n",
                           stats.frame_rate,
                           (double)stats.frame_last_time / 1000000.0,
                           (double)stats.reg_last_time / 1000000.0,
                           (double)stats.rnd_last_time / 1000000.0,
                           (unsigned long)(stats.vtx_buffer_used / 1024u),
                           (unsigned long)(stats.vtx_buffer_used_max / 1024u),
                           active_shots,
                           (double)game.temporary_weapon_time,
                           (double)game.speed_boost_time,
                           (double)game.speed,
                           (double)game.player_x,
                           (double)game.player_y);
                }
                last_stats_second = stats_second;
            }
        }
#endif
    }

#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
    printf("Gravity Wave music test: %s full 8-track A/B/C cycle.\n",
           music_jukebox_complete && !music_jukebox_failed ?
           "PASS" : "FAIL");
#endif
    printf("Gravity Wave: shutting down cleanly.\n");
    shutdown_audio();
    release_textures();
    pvr_shutdown();
#ifdef GRAVITY_WAVE_AUTOTEST_MUSIC_JUKEBOX
    if(!music_jukebox_complete || music_jukebox_failed)
        return 3;
#endif
    return 0;
}
