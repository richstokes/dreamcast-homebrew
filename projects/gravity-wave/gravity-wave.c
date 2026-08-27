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
#define SCENERY_SEGMENT      96.0f

#define MUSIC_TRACK_COUNT    5
#define MUSIC_SECTION_COUNT  2
#define MUSIC_SAMPLE_RATE    13000
#define MUSIC_BARS           2
#define MUSIC_BEATS          (MUSIC_BARS * 4)
#define MUSIC_PHRASE_BARS    (MUSIC_BARS * MUSIC_SECTION_COUNT)
#define MUSIC_MELODY_STEPS   (MUSIC_BEATS * 2)
#define MUSIC_EDGE_SAMPLES   1024
#define MUSIC_REST           (-127)
#define MUSIC_TITLE_VOLUME   68
#define MUSIC_GAME_VOLUME    84
#define TEMP_WEAPON_SECONDS  15.0f
#define SPEED_BOOST_SECONDS  8.0f
#define GATE_CLEAR_RADIUS_SCALE 0.7874008f

#define PLAYER_Z             44.0f
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

typedef struct {
    bool active;
    float x, y, z;
    float vx, vy;
    float phase;
    float radius;
    int type;
    int hp;
    int max_hp;
    float fire_timer;
    bool fired;
} enemy_t;

typedef enum {
    SHOT_PLAYER_LASER,
    SHOT_PLAYER_FAST,
    SHOT_PLAYER_PHASE,
    SHOT_ENEMY
} projectile_kind_t;

typedef struct {
    bool active;
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
    PARTICLE_EXHAUST
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

typedef struct {
    bool active;
    float x, y, z;
    float radius;
    float spin;
    float result_time;
    int variant;
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
    uint8_t minor_mask;
    int8_t bass[8 * MUSIC_SECTION_COUNT];
    uint8_t arpeggio[16];
    int8_t melody[MUSIC_MELODY_STEPS * MUSIC_SECTION_COUNT];
    uint16_t kick[MUSIC_PHRASE_BARS];
    uint16_t snare[MUSIC_PHRASE_BARS];
    uint16_t hat[MUSIC_PHRASE_BARS];
    int lead_profile;
    float pulse_width;
    float sidechain;
    float drive;
    float pad_level;
    float bass_level;
    float arpeggio_level;
    float lead_level;
    float kick_level;
    float snare_level;
    float hat_level;
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

/* Five original, high-energy synthwave/synthpop songs are mixed into seamless
 * stereo PCM at boot. Each A/B pair forms a four-bar composition with its own
 * hook, bass motion, drum programming, synth voicing, and chord sequence. */
static const soundtrack_t soundtrack_defs[MUSIC_TRACK_COUNT] = {
    {
        .name = "MIDNIGHT VECTOR",
        .bpm = 154.0f,
        .root_midi = 40, /* E minor: Em, C, G, D. */
        .chord_offsets = {0, -4, 3, -2},
        .minor_mask = 0x01,
        .bass = {
            0,MUSIC_REST,12,0, 7,MUSIC_REST,12,MUSIC_REST,
            0,0,12,MUSIC_REST, 7,12,0,MUSIC_REST
        },
        .arpeggio = {0,2,3,1, 2,4,3,2, 0,3,5,2, 4,3,2,1},
        .melody = {
             0,MUSIC_REST, 3,7, 12,MUSIC_REST,10,7,
             5,MUSIC_REST, 7,10, 7,5,3,MUSIC_REST,
            12,MUSIC_REST,15,12, 10,7,5,7,
            10,MUSIC_REST, 7,5, 3,5,0,MUSIC_REST
        },
        .kick = {0x1111,0x5111,0x1111,0x9511},
        .snare = {0x1010,0x1010,0x1010,0x5010},
        .hat = {0xaaaa,0xeeee,0xaaaa,0xffff},
        .lead_profile = 0,
        .pulse_width = 0.42f, .sidechain = 0.72f, .drive = 0.46f,
        .pad_level = 0.145f, .bass_level = 0.235f,
        .arpeggio_level = 0.125f, .lead_level = 0.175f,
        .kick_level = 0.335f, .snare_level = 0.185f,
        .hat_level = 0.068f, .noise_seed = 0x8bd13a47u
    },
    {
        .name = "MAGENTA CIRCUIT",
        .bpm = 160.0f,
        .root_midi = 42, /* F-sharp minor: F#m, D, A, E. */
        .chord_offsets = {0, -4, 3, -2},
        .minor_mask = 0x01,
        .bass = {
            0,0,MUSIC_REST,12, 0,7,12,7,
            0,MUSIC_REST,0,12, 7,12,7,MUSIC_REST
        },
        .arpeggio = {0,1,3,2, 4,2,5,3, 0,2,4,6, 5,3,2,1},
        .melody = {
             0,3,7,MUSIC_REST, 8,7,5,3,
             0,MUSIC_REST, 5,7, 12,10,8,MUSIC_REST,
            12,15,12,10, 8,MUSIC_REST,7,5,
             7,8,10,12, 10,7,3,MUSIC_REST
        },
        .kick = {0x1151,0x5151,0x1151,0xd151},
        .snare = {0x1010,0x1010,0x1010,0x5010},
        .hat = {0xeeee,0xffee,0xeeee,0xffff},
        .lead_profile = 1,
        .pulse_width = 0.28f, .sidechain = 0.82f, .drive = 0.54f,
        .pad_level = 0.125f, .bass_level = 0.245f,
        .arpeggio_level = 0.145f, .lead_level = 0.185f,
        .kick_level = 0.350f, .snare_level = 0.195f,
        .hat_level = 0.074f, .noise_seed = 0x31f2c96du
    },
    {
        .name = "GLASS HORIZON",
        .bpm = 156.0f,
        .root_midi = 36, /* C minor: Cm, Ab, Fm, Gm. */
        .chord_offsets = {0,-4,5,7},
        .minor_mask = 0x0d,
        .bass = {
            0,MUSIC_REST,7,12, 0,MUSIC_REST,12,7,
            0,7,MUSIC_REST,12, 0,12,7,MUSIC_REST
        },
        .arpeggio = {0,2,4,3, 1,3,5,4, 2,4,6,5, 4,3,2,1},
        .melody = {
             0,MUSIC_REST, 3,7, 10,7,3,MUSIC_REST,
             5,7,10,12, 10,MUSIC_REST,7,5,
            12,MUSIC_REST,15,19, 15,12,10,7,
             5,MUSIC_REST, 7,10, 12,10,7,MUSIC_REST
        },
        .kick = {0x0941,0x4941,0x1941,0x5949},
        .snare = {0x1010,0x1010,0x1010,0x3010},
        .hat = {0xaaaa,0xeeee,0xaeee,0xffee},
        .lead_profile = 2,
        .pulse_width = 0.47f, .sidechain = 0.58f, .drive = 0.34f,
        .pad_level = 0.165f, .bass_level = 0.215f,
        .arpeggio_level = 0.135f, .lead_level = 0.180f,
        .kick_level = 0.315f, .snare_level = 0.190f,
        .hat_level = 0.066f, .noise_seed = 0xde71a903u
    },
    {
        .name = "STATIC HEART",
        .bpm = 166.0f,
        .root_midi = 45, /* A minor: Am, F, C, G. */
        .chord_offsets = {0,-4,3,-2},
        .minor_mask = 0x01,
        .bass = {
            0,12,0,MUSIC_REST, 7,12,7,0,
            0,MUSIC_REST,12,0, 7,7,12,MUSIC_REST
        },
        .arpeggio = {0,3,1,4, 2,5,3,6, 4,2,5,3, 1,4,2,5},
        .melody = {
             0,3,7,12, 10,7,5,3,
             7,MUSIC_REST,10,12, 15,12,10,MUSIC_REST,
            12,10,7,5, 7,10,12,15,
            17,MUSIC_REST,15,12, 10,7,3,0
        },
        .kick = {0x1551,0x5551,0x1551,0xd551},
        .snare = {0x1010,0x1010,0x1010,0x5010},
        .hat = {0xeeee,0xffff,0xeeee,0xffff},
        .lead_profile = 3,
        .pulse_width = 0.22f, .sidechain = 0.88f, .drive = 0.62f,
        .pad_level = 0.115f, .bass_level = 0.255f,
        .arpeggio_level = 0.155f, .lead_level = 0.190f,
        .kick_level = 0.365f, .snare_level = 0.205f,
        .hat_level = 0.078f, .noise_seed = 0xa9417c53u
    },
    {
        .name = "AFTERIMAGE RUN",
        .bpm = 158.0f,
        .root_midi = 38, /* D minor: Dm, Bb, F, C. */
        .chord_offsets = {0,-4,3,-2},
        .minor_mask = 0x01,
        .bass = {
            0,MUSIC_REST,0,12, 7,0,12,MUSIC_REST,
            0,7,12,7, 0,MUSIC_REST,12,0
        },
        .arpeggio = {0,2,3,5, 4,2,6,3, 5,3,4,2, 6,4,3,1},
        .melody = {
             0,3,5,7, 12,MUSIC_REST,10,7,
             5,7,10,12, 15,12,10,MUSIC_REST,
            12,15,17,15, 12,10,7,5,
             7,MUSIC_REST,10,7, 5,3,0,MUSIC_REST
        },
        .kick = {0x1111,0x5115,0x1111,0xd119},
        .snare = {0x1010,0x1010,0x1010,0x7010},
        .hat = {0xaaaa,0xfaee,0xaaaa,0xffff},
        .lead_profile = 4,
        .pulse_width = 0.35f, .sidechain = 0.76f, .drive = 0.50f,
        .pad_level = 0.135f, .bass_level = 0.240f,
        .arpeggio_level = 0.145f, .lead_level = 0.188f,
        .kick_level = 0.345f, .snare_level = 0.198f,
        .hat_level = 0.072f, .noise_seed = 0x52ce8bf1u
    }
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
static sfxhnd_t music_sections[MUSIC_TRACK_COUNT][MUSIC_SECTION_COUNT];
static float music_section_duration[MUSIC_TRACK_COUNT][MUSIC_SECTION_COUNT];
static int laser_channel = -1;
static int music_left_channels[2] = {-1, -1};
static int music_right_channels[2] = {-1, -1};
static int active_music_bank;
static int current_music_track = -1;
static int current_music_section;
static int current_music_volume;
static int pending_music_track = -1;
static int pending_music_volume;
static float music_section_time;
static int music_song_loops;
static bool audio_ready;

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

static float path_center(float world_z) {
    const float broad_amplitude = 0.78f +
        fsin(world_z * 0.00031f + 1.7f) * 0.22f;

    /* Three incommensurate waves create broad turns, S-bends, and tightening
       apexes without discontinuities or expensive per-vertex noise hashes. */
    return fsin(world_z * 0.00092f + 0.4f) * 82.0f * broad_amplitude +
           fsin(world_z * 0.00235f + 2.1f) * 34.0f +
           fsin(world_z * 0.0048f + 0.8f) * 11.0f;
}

static float path_elevation(float world_z) {
    return fsin(world_z * 0.00083f + 1.1f) * 31.0f +
           fsin(world_z * 0.0027f + 2.8f) * 15.0f +
           fsin(world_z * 0.0049f + 0.2f) * 5.0f;
}

static float path_turn_preview(float world_z) {
    const float near_heading = atanf(
        (path_center(world_z + 210.0f) -
         path_center(world_z + 30.0f)) / 180.0f);
    const float far_heading = atanf(
        (path_center(world_z + 430.0f) -
         path_center(world_z + 210.0f)) / 220.0f);
    return clampf(far_heading - near_heading, -0.16f, 0.16f);
}

static float terrain_height_biome(int biome, float local_x, float world_z) {
    const float distance_from_river = fabsf(local_x);
    const float broad = value_noise(local_x + 1900.0f, world_z, 155.0f);
    const float detail = value_noise(local_x - 730.0f, world_z, 54.0f);
    const float floor_wave = fsin(world_z * 0.011f + local_x * 0.022f);
    float height;
    float wall_start;
    float wall_scale;
    float wall_curve;

    switch(biome & 3) {
        case 1: /* Emerald Veil: broad, humid valley and ancient terraces. */
            height = -3.0f + broad * 7.5f + detail * 2.1f +
                     fsin(world_z * 0.006f + local_x * 0.012f) * 2.8f;
            wall_start = 42.0f;
            wall_scale = 18.0f;
            wall_curve = 61.0f;
            break;
        case 2: /* Violet Rift: sharp shelves over a deep fractured floor. */
            height = -9.0f + broad * 4.2f + detail * detail * 7.2f +
                     floor_wave * 2.2f;
            wall_start = 30.0f;
            wall_scale = 29.0f;
            wall_curve = 96.0f;
            break;
        case 3: /* Ember Crown: stepped basalt walls surrounding a lava cut. */
            height = -6.5f + broad * 5.0f + detail * 3.3f +
                     fsin(world_z * 0.017f) * 1.5f;
            height = floorf(height * 0.28f) / 0.28f;
            wall_start = 36.0f;
            wall_scale = 25.0f;
            wall_curve = 74.0f;
            break;
        default: /* Azure Reach: wet sea-cut canyon and craggy shelves. */
            height = -5.5f + broad * 6.0f + detail * 2.8f + floor_wave * 1.3f;
            wall_start = 34.0f;
            wall_scale = 23.0f;
            wall_curve = 82.0f;
            break;
    }

    if(distance_from_river > wall_start) {
        const float wall = clampf((distance_from_river - wall_start) / 190.0f,
                                  0.0f, 1.25f);
        const float ridges = 0.42f + 0.92f *
            value_noise(local_x * 1.7f + 400.0f, world_z + 810.0f, 78.0f);
        height += wall * wall_scale + wall * wall * wall_curve * ridges;
    }

    return height;
}

static float terrain_height_local(float local_x, float world_z) {
    const float local = fmodf(world_z, BIOME_LENGTH);
    const int biome = ((int)(world_z / BIOME_LENGTH)) & 3;
    float height = terrain_height_biome(biome, local_x, world_z);

    if(local > BIOME_LENGTH - 650.0f) {
        const float blend = smoothstepf(
            (local - (BIOME_LENGTH - 650.0f)) / 650.0f);
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

    if(local > biome_length - 650.0f)
        t = smoothstepf((local - (biome_length - 650.0f)) / 650.0f);

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

static bool project_world(vec3_t world, screen_point_t *out) {
    float dx = world.x - camera_world_x;
    /* Combat and collision remain in route-local Y. Bend them into world
       space here so every gate, projectile, landmark, and terrain vertex
       follows the same climb or dive without desynchronizing gameplay. */
    float dy = world.y + path_elevation(world.z) - camera_world_y;
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

static void setup_camera(void) {
    const float lookahead = 260.0f;
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
    const float pitch = route_grade - 0.075f +
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

static void draw_textured_billboard(const pvr_poly_hdr_t *header,
                                    vec3_t world, float width, float height,
                                    uint32_t color) {
    screen_point_t center;
    screen_point_t a, b, c, d;
    float screen_w, screen_h;

    if(!project_world(world, &center))
        return;
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

static float music_pulse(float cycles, float duty) {
    return music_phase(cycles) < duty ? 1.0f : -1.0f;
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
                              float beat, float seconds_per_beat,
                              int *step_out) {
    float position;
    float phase;
    float seconds;
    float cycles;
    float envelope;
    float voice;
    int step;

    while(beat < 0.0f)
        beat += (float)MUSIC_BEATS;
    while(beat >= (float)MUSIC_BEATS)
        beat -= (float)MUSIC_BEATS;
    position = beat * 2.0f;
    step = (int)position;
    phase = position - (float)step;
    if(step_out)
        *step_out = step;
    if(lead_frequencies[step] <= 0.0f)
        return 0.0f;

    seconds = phase * seconds_per_beat * 0.5f;
    cycles = lead_frequencies[step] * seconds;
    cycles += fsin(TAU * (5.1f + (float)track->lead_profile * 0.31f) *
                   seconds) * 0.012f;
    envelope = clampf(phase * 15.0f, 0.0f, 1.0f) *
               (1.0f - phase) * (1.0f - phase);
    if(track->lead_profile == 1) {
        voice = music_pulse(cycles, 0.26f) * 0.54f +
                fsin(TAU * cycles) * 0.34f +
                music_triangle(cycles * 2.0f) * 0.12f;
    }
    else if(track->lead_profile == 2) {
        voice = music_saw(cycles) * 0.44f +
                fsin(TAU * cycles) * 0.33f +
                music_pulse(cycles * 0.5f, 0.48f) * 0.18f;
    }
    else if(track->lead_profile == 3) {
        voice = music_pulse(cycles, track->pulse_width) * 0.42f +
                music_triangle(cycles * 2.0f) * 0.31f +
                fsin(TAU * cycles * 3.0f) * 0.17f +
                fsin(TAU * cycles) * 0.10f;
    }
    else if(track->lead_profile == 4) {
        const float pwm = clampf(track->pulse_width +
                                 fsin(TAU * seconds * 3.7f) * 0.08f,
                                 0.14f, 0.62f);
        voice = music_saw(cycles) * 0.37f +
                music_pulse(cycles, pwm) * 0.36f +
                music_triangle(cycles * 0.5f) * 0.17f +
                fsin(TAU * cycles * 2.01f) * 0.10f;
    }
    else {
        voice = fsin(TAU * cycles) * 0.61f +
                music_triangle(cycles * 2.0f) * 0.25f +
                fsin(TAU * cycles * 3.01f) * 0.14f;
    }
    return voice * envelope;
}

static sfxhnd_t load_generated_music(const soundtrack_t *track, int section,
                                     float *duration_out) {
    float chord_frequencies[MUSIC_BARS][3];
    float bass_frequencies[MUSIC_BARS][8];
    float arpeggio_frequencies[MUSIC_BARS][16];
    float lead_frequencies[MUSIC_MELODY_STEPS];
    uint16_t kick_patterns[MUSIC_BARS];
    uint16_t snare_patterns[MUSIC_BARS];
    uint16_t hat_patterns[MUSIC_BARS];
    const float requested_duration = 60.0f * (float)MUSIC_BEATS / track->bpm;
    int samples = (int)(requested_duration * (float)MUSIC_SAMPLE_RATE + 0.5f);
    int16_t *buffer;
    int16_t *left_samples;
    int16_t *right_samples;
    float duration;
    float seconds_per_beat;
    sfxhnd_t handle;
    int bar, step, i;

    samples = (samples + 15) & ~15;
    if(samples > 65520)
        samples = 65520;
    buffer = malloc((size_t)samples * 2u * sizeof(*buffer));
    if(!buffer)
        return SFXHND_INVALID;
    left_samples = buffer;
    right_samples = buffer + samples;
    duration = (float)samples / (float)MUSIC_SAMPLE_RATE;
    seconds_per_beat = duration / (float)MUSIC_BEATS;
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
        for(step = 0; step < 8; ++step) {
            int bass_note = track->bass[section * 8 + step];
            if(section == 1 && bass_note == MUSIC_REST &&
               (step == 2 || step == 6))
                bass_note = step == 2 ? 7 : 12;
            bass_frequencies[bar][step] =
                bass_note == MUSIC_REST ? 0.0f :
                music_note_frequency(root + bass_note);
        }
        for(step = 0; step < 16; ++step) {
            const int pattern_step = section == 0 ? step : 15 - step;
            const int semitone = music_chord_semitone(
                track->arpeggio[pattern_step], minor);
            arpeggio_frequencies[bar][step] =
                music_note_frequency(root + 12 + semitone);
        }
        kick_patterns[bar] = track->kick[phrase_bar];
        snare_patterns[bar] = track->snare[phrase_bar];
        hat_patterns[bar] = track->hat[phrase_bar];
        if(section == 1) {
            if(bar == 1)
                kick_patterns[bar] |= 0x4040u;
            if(bar == MUSIC_BARS - 1) {
                kick_patterns[bar] |= 0x8000u;
                snare_patterns[bar] |= 0x4000u;
                hat_patterns[bar] |= 0xffffu;
            }
        }
    }
    for(step = 0; step < MUSIC_MELODY_STEPS; ++step) {
        const int melody_step = section * MUSIC_MELODY_STEPS + step;
        const int melody_note = track->melody[melody_step];
        lead_frequencies[step] = melody_note == MUSIC_REST ? 0.0f :
            music_note_frequency(track->root_midi + 24 + melody_note);
    }

    for(i = 0; i < samples; ++i) {
        const float beat = (float)i * (float)MUSIC_BEATS / (float)samples;
        const int current_bar = (int)(beat * 0.25f);
        const float beat_in_bar = beat - (float)current_bar * 4.0f;
        const float bar_seconds = beat_in_bar * seconds_per_beat;
        const float pad_attack = smoothstepf(beat_in_bar * 4.8f);
        const float pad_release = smoothstepf((4.0f - beat_in_bar) * 3.2f);
        const float pad_envelope = pad_attack * pad_release *
            (0.92f + fsin(TAU * beat_in_bar * 0.25f) * 0.08f);
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

        left += (fsin(TAU * chord_frequencies[current_bar][0] *
                      bar_seconds * 0.997f) * 0.40f +
                 fsin(TAU * chord_frequencies[current_bar][1] *
                      bar_seconds * 1.002f) * 0.27f +
                 music_triangle(chord_frequencies[current_bar][2] *
                                bar_seconds * 0.999f) * 0.20f +
                 music_saw(chord_frequencies[current_bar][0] *
                           bar_seconds * 0.501f) * 0.13f) *
                track->pad_level * pad_envelope * sidechain;
        right += (fsin(TAU * chord_frequencies[current_bar][0] *
                       bar_seconds * 1.003f) * 0.40f +
                  fsin(TAU * chord_frequencies[current_bar][1] *
                       bar_seconds * 0.998f) * 0.27f +
                  music_triangle(chord_frequencies[current_bar][2] *
                                 bar_seconds * 1.001f) * 0.20f +
                  music_saw(chord_frequencies[current_bar][1] *
                            bar_seconds * 0.499f) * 0.13f) *
                 track->pad_level * pad_envelope * sidechain;

        if(bass_frequencies[current_bar][bass_step] > 0.0f) {
            envelope = clampf(bass_phase * 18.0f, 0.0f, 1.0f) *
                       (1.0f - bass_phase) * (1.0f - bass_phase);
            cycles = bass_frequencies[current_bar][bass_step] *
                     bass_phase * seconds_per_beat * 0.5f;
            voice = music_pulse(cycles, track->pulse_width) * 0.43f +
                    fsin(TAU * cycles) * 0.42f +
                    music_triangle(cycles * 0.5f) * 0.15f;
            left += voice * track->bass_level * envelope;
            right += voice * track->bass_level * envelope;
        }

        envelope = clampf(arpeggio_phase * 24.0f, 0.0f, 1.0f) *
                   (1.0f - arpeggio_phase) *
                   (1.0f - arpeggio_phase) *
                   (1.0f - arpeggio_phase);
        cycles = arpeggio_frequencies[current_bar][arpeggio_step] *
                 arpeggio_phase * seconds_per_beat * 0.25f;
        voice = music_triangle(cycles) * 0.45f +
                music_pulse(cycles, 0.25f) * 0.27f +
                fsin(TAU * cycles * 2.0f) * 0.18f +
                music_saw(cycles * 0.5f) * 0.10f;
        if((arpeggio_step ^ current_bar) & 1) {
            left += voice * track->arpeggio_level * envelope * 0.42f;
            right += voice * track->arpeggio_level * envelope;
        }
        else {
            left += voice * track->arpeggio_level * envelope;
            right += voice * track->arpeggio_level * envelope * 0.42f;
        }

        voice = music_lead_voice(track, lead_frequencies, beat,
                                 seconds_per_beat, &lead_step);
        if(lead_step & 1) {
            left += voice * track->lead_level * 0.62f;
            right += voice * track->lead_level;
        }
        else {
            left += voice * track->lead_level;
            right += voice * track->lead_level * 0.62f;
        }
        voice = music_lead_voice(track, lead_frequencies, beat - 0.75f,
                                 seconds_per_beat, &echo_step) * 0.30f;
        if(echo_step & 1) {
            left += voice * track->lead_level;
            right += voice * track->lead_level * 0.38f;
        }
        else {
            left += voice * track->lead_level * 0.38f;
            right += voice * track->lead_level;
        }
        voice = music_lead_voice(track, lead_frequencies, beat - 1.5f,
                                 seconds_per_beat, NULL) * 0.14f;
        left += voice * track->lead_level * 0.45f;
        right += voice * track->lead_level;

        if(kick_patterns[current_bar] & drum_bit) {
            const float kick_frequency = 50.0f +
                118.0f * (1.0f - drum_phase) * (1.0f - drum_phase);
            envelope = clampf(drum_phase * 30.0f, 0.0f, 1.0f) *
                       (1.0f - drum_phase) * (1.0f - drum_phase) *
                       (1.0f - drum_phase);
            voice = fsin(TAU * kick_frequency * drum_seconds) +
                    music_noise((uint32_t)i ^ track->noise_seed ^
                                (uint32_t)section * 0x9e3779b9u) *
                    clampf((0.09f - drum_phase) * 11.0f, 0.0f, 1.0f) * 0.14f;
            left += voice * track->kick_level * envelope;
            right += voice * track->kick_level * envelope;
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
                     fsin(TAU * 176.0f * drum_seconds) * 0.22f) *
                    track->snare_level * envelope * clap_burst;
            right += (music_noise((uint32_t)i ^ track->noise_seed ^
                                  0xa37c91e5u) * 0.78f +
                      fsin(TAU * 181.0f * drum_seconds) * 0.22f) *
                     track->snare_level * envelope * clap_burst;
        }
        if(hat_patterns[current_bar] & drum_bit) {
            envelope = clampf(drum_phase * 42.0f, 0.0f, 1.0f) *
                       (1.0f - drum_phase) * (1.0f - drum_phase) *
                       (1.0f - drum_phase) * (1.0f - drum_phase);
            voice = (music_pulse(2180.0f * drum_seconds, 0.48f) *
                     music_pulse(3310.0f * drum_seconds, 0.42f) * 0.62f +
                     music_noise((uint32_t)i ^ track->noise_seed ^
                                 0xc6bc2796u) * 0.38f) *
                    track->hat_level * envelope;
            if(drum_step & 1) {
                left += voice * 0.55f;
                right += voice;
            }
            else {
                left += voice;
                right += voice * 0.55f;
            }
        }

        {
            const float edge_in = smoothstepf(
                (float)i / (float)MUSIC_EDGE_SAMPLES);
            const float edge_out = smoothstepf(
                (float)(samples - 1 - i) / (float)MUSIC_EDGE_SAMPLES);
            const float edge = fminf(edge_in, edge_out);
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
        left_samples[i] = (int16_t)(clampf(left, -1.0f, 1.0f) * 30000.0f);
        right_samples[i] = (int16_t)(clampf(right, -1.0f, 1.0f) * 30000.0f);
    }

    handle = snd_sfx_load_raw_buf((char *)buffer,
                                  (size_t)samples * 2u * sizeof(*buffer),
                                  MUSIC_SAMPLE_RATE, 16, 2);
    free(buffer);
    if(handle != SFXHND_INVALID) {
        printf("Gravity Wave music: rendered %s section %c "
               "(%.2fs stereo, %lu KiB).\n",
               track->name, section == 0 ? 'A' : 'B', (double)duration,
               (unsigned long)(((size_t)samples * 4u + 1023u) / 1024u));
    }
    return handle;
}

static int choose_random_music_track(int avoid) {
    int track = (int)(random_u32() % MUSIC_TRACK_COUNT);
    if(MUSIC_TRACK_COUNT > 1 && track == avoid)
        track = (track + 1 + (int)(random_u32() %
                                  (MUSIC_TRACK_COUNT - 1))) %
                MUSIC_TRACK_COUNT;
    return track;
}

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
    music_section_time = 0.0f;
    music_song_loops = 0;
}

static void play_music_section(int track, int section, int volume) {
    sfx_play_data_t playback = {0};
    const int previous_track = current_music_track;
    const int next_bank = current_music_track < 0 ? 0 : 1 - active_music_bank;

    if(!audio_ready || track < 0 || track >= MUSIC_TRACK_COUNT ||
       music_left_channels[next_bank] < 0 ||
       music_right_channels[next_bank] != music_left_channels[next_bank] + 1)
        return;
    if(section < 0 || section >= MUSIC_SECTION_COUNT)
        section = 0;
    if(music_sections[track][section] == SFXHND_INVALID)
        section = 0;
    if(music_sections[track][section] == SFXHND_INVALID)
        return;
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
        music_section_time = 0.0f;
        if(previous_track != track) {
            music_song_loops = 0;
            printf("Gravity Wave music: now playing %s.\n",
                   soundtrack_defs[track].name);
        }
    }
}

static void start_music_track(int track, int volume) {
    if(!audio_ready || track < 0 || track >= MUSIC_TRACK_COUNT ||
       music_sections[track][0] == SFXHND_INVALID)
        return;
    if(current_music_track < 0) {
        play_music_section(track, 0, volume);
        return;
    }
    if(current_music_track == track && current_music_volume == volume) {
        pending_music_track = -1;
        return;
    }
    pending_music_track = track;
    pending_music_volume = volume;
}

static void update_music(float dt) {
    const float crossfade_time =
        (float)MUSIC_EDGE_SAMPLES / (float)MUSIC_SAMPLE_RATE;
    float duration;
    int next_track;
    int next_section;
    int next_volume;

    if(current_music_track < 0)
        return;
    duration = music_section_duration[current_music_track]
                                     [current_music_section];
    if(duration <= 0.0f)
        return;
    music_section_time += dt;
    if(music_section_time < duration - crossfade_time)
        return;

    if(pending_music_track >= 0) {
        next_track = pending_music_track;
        next_section = 0;
        next_volume = pending_music_volume;
    }
    else if(current_music_section == MUSIC_SECTION_COUNT - 1) {
        music_song_loops++;
        if(music_song_loops >= 2) {
            next_track = choose_random_music_track(current_music_track);
            next_section = 0;
            next_volume = current_music_volume;
            music_song_loops = 0;
        }
        else {
            next_track = current_music_track;
            next_section = 0;
            next_volume = current_music_volume;
        }
    }
    else {
        next_track = current_music_track;
        next_section = (current_music_section + 1) % MUSIC_SECTION_COUNT;
        next_volume = current_music_volume;
    }
    play_music_section(next_track, next_section, next_volume);
}

static void init_audio(void) {
    int loaded_sections = 0;
    bool channel_pairs_ok = true;
    int i, section, bank;

    audio_ready = false;
    laser_channel = -1;
    for(bank = 0; bank < 2; ++bank) {
        music_left_channels[bank] = -1;
        music_right_channels[bank] = -1;
    }
    active_music_bank = 0;
    current_music_track = -1;
    pending_music_track = -1;
    music_section_time = 0.0f;
    music_song_loops = 0;
    for(i = 0; i < MUSIC_TRACK_COUNT; ++i) {
        for(section = 0; section < MUSIC_SECTION_COUNT; ++section) {
            music_sections[i][section] = SFXHND_INVALID;
            music_section_duration[i][section] = 0.0f;
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
    for(i = 0; i < MUSIC_TRACK_COUNT; ++i) {
        for(section = 0; section < MUSIC_SECTION_COUNT; ++section) {
            music_sections[i][section] = load_generated_music(
                &soundtrack_defs[i], section,
                &music_section_duration[i][section]);
            if(music_sections[i][section] != SFXHND_INVALID)
                loaded_sections++;
        }
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
            const float x = side * (78.0f + (float)((h >> 8) & 127u));
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

static bool player_hits_signature_arch(int biome, int segment,
                                       float segment_z, float player_z,
                                       float player_x, float player_y) {
    int rib_count;
    float spacing;
    int rib;

    if(((segment % 37 + 37) % 37) != 11 || biome == 2)
        return false;
    rib_count = biome == 0 ? 4 : 3;
    spacing = biome == 0 ? 22.0f : (biome == 1 ? 28.0f : 32.0f);

    for(rib = 0; rib < rib_count; ++rib) {
        const float rib_z = segment_z + (float)rib * spacing;
        const float half_span = biome == 0 ? 43.0f - (float)rib * 2.0f :
                                (biome == 1 ?
                                 36.0f - (float)rib * 3.0f : 40.0f);
        const float base_y = biome == 3 ? -1.0f :
                             (biome == 0 ? 1.0f : 0.0f);
        const float height = biome == 0 ? 57.0f :
                             (biome == 1 ? 60.0f : 49.0f);
        const float thickness = biome == 0 ? 2.8f :
                                (biome == 1 ? 4.5f : 3.8f);

        if(fabsf(player_z - rib_z) <= 18.0f &&
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
    if((segment % 37 + 37) % 37 != 11)
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
        for(i = -2; i <= 2; ++i)
            draw_crystal((float)i * 23.0f,
                         terrain_height_local((float)i * 23.0f, world_z),
                         world_z + fabsf((float)i) * 8.0f,
                         6.0f + (float)(i & 1) * 2.0f,
                         45.0f + (float)(2 - abs(i)) * 10.0f,
                         palette->accent);
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
        draw_signature_setpiece(retained_biome, retained_segment,
                                retained_z, palette);
    }
    for(i = 0; i < 16; ++i) {
        const int segment = first_segment + i;
        const float z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(z / BIOME_LENGTH)) & 3;
        const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                    (uint32_t)biome * 0x51ed270bu);
        const int variant = (int)((h >> 3) % 5u);
        const float side = (h & 1u) ? 1.0f : -1.0f;
        const float x = side * (70.0f + (float)((h >> 8) & 63u));
        const float base = terrain_height_local(x, z);

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
            else if(variant == 1) {
                draw_material_box(x, base, z, 8.0f, 58.0f, 8.0f,
                                  GRAVITY_WAVE_TEX_HULL_HOSTILE,
                                  (color3_t){0.77f,0.52f,0.39f});
                draw_material_box(x, base + 58.0f, z, 11.0f, 7.0f, 11.0f,
                                  GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                                  (color3_t){0.92f,0.68f,0.44f});
                draw_material_box(x, base + 65.0f, z, 6.5f, 2.5f, 6.5f,
                                  GRAVITY_WAVE_TEX_CANOPY_ENERGY,
                                  (color3_t){1.0f,0.38f,0.10f});
            }
            else if(variant == 2)
                draw_material_arch(z, 40.0f, -1.0f, 45.0f, 3.5f,
                                   GRAVITY_WAVE_TEX_HULL_HOSTILE,
                                   (color3_t){0.83f,0.54f,0.39f});
            else if(variant == 3) {
                draw_material_box(x, base, z, 15.0f, 35.0f, 13.0f,
                                  GRAVITY_WAVE_TEX_ANCIENT_MACHINE,
                                  (color3_t){0.86f,0.60f,0.38f});
                draw_material_box(x, base + 35.0f, z, 10.0f, 17.0f, 9.0f,
                                  GRAVITY_WAVE_TEX_HULL_HOSTILE,
                                  (color3_t){0.76f,0.40f,0.29f});
                draw_material_box(x, base + 52.0f, z, 6.5f, 2.5f, 6.5f,
                                  GRAVITY_WAVE_TEX_CANOPY_ENERGY,
                                  (color3_t){1.0f,0.38f,0.10f});
            }
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
        const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                    (uint32_t)biome * 0x51ed270bu);
        const int variant = (int)((h >> 3) % 5u);
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

        if(half_span > 0.0f && distance_fade > 0.02f)
            draw_material_arch_energy(z, half_span, base_y, height,
                                      thickness, primary, accent,
                                      distance_fade);
    }

    for(i = 0; i < 16; ++i) {
        const int segment = first_segment + i;
        const float z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(z / BIOME_LENGTH)) & 3;
        const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                    (uint32_t)biome * 0x51ed270bu);
        const int variant = (int)((h >> 3) % 5u);
        const float side = (h & 1u) ? 1.0f : -1.0f;
        const float x = side * (70.0f + (float)((h >> 8) & 63u));
        const float base = terrain_height_local(x, z);
        const float distance_fade = 1.0f -
            smoothstepf((z - game.distance - 950.0f) / 430.0f);

        if(distance_fade <= 0.02f)
            continue;
        if(biome == 0 && ((h >> 3) % 5u) == 1u) {
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
            const float pulse = 0.92f + 0.08f *
                fsin(game.time * 7.0f + (float)segment * 0.71f);
            screen_point_t vent;

            draw_textured_billboard(
                &texture_headers[GRAVITY_WAVE_TEX_FIRE_SMOKE],
                (vec3_t){path_center(z) + x, vent_y + 17.0f, z},
                25.0f, 38.0f,
                pack_color(0.86f * distance_fade,
                           (color3_t){1.0f,0.83f,0.69f}));
            if(project_world((vec3_t){path_center(z) + x,
                                      vent_y + 1.0f, z}, &vent)) {
                const float glow_size = clampf(
                    7.0f * game.camera_focal * vent.z, 1.5f, 18.0f);
                draw_disc(&additive_header, vent.x, vent.y,
                          glow_size * pulse, vent.z + 0.00002f, 8,
                          pack_color(0.68f * distance_fade,
                                     (color3_t){1.0f,0.52f,0.12f}),
                          pack_color(0.0f, (color3_t){1.0f,0.12f,0.02f}));
            }
        }
    }
}

static void set_message(const char *text, float seconds) {
    snprintf(game.message, sizeof(game.message), "%s", text);
    game.message_time = seconds;
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
    int i;
    for(i = 0; i < count; ++i) {
        const float angle = random_unit() * TAU;
        const float lift = random_signed() * 0.75f;
        const float speed = 14.0f + random_unit() * 43.0f;
        color3_t spark = (i & 2) ? color : (color3_t){1.0f, 0.55f, 0.12f};
        spawn_particle(x, y, z,
                       fcos(angle) * speed,
                       lift * speed,
                       fsin(angle) * speed,
                       0.42f + random_unit() * 0.72f,
                       1.4f + random_unit() * 3.5f,
                       spark, (i & 1) ? PARTICLE_STREAK : PARTICLE_SPRITE);
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
    const palette_t *palette = current_palette();
    const int points = enemy->type == 3 ? 4200 :
                       (enemy->type == 2 ? 500 :
                        (enemy->type == 1 ? 220 : 100));
    const float drop_roll = random_unit();
    const float drop_chance = enemy->type == 2 ? 0.42f :
                              (enemy->type == 1 ? 0.14f :
                               (game.shield < 35.0f ? 0.20f : 0.09f));
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
        spawn_pickup(enemy->x - 12.0f, enemy->y, enemy->z,
                     game.weapon_level < 3 ? PICKUP_LASER_CORE : PICKUP_NOVA);
        spawn_pickup(enemy->x, enemy->y + 7.0f, enemy->z + 5.0f,
                     random_unit() < 0.5f ? PICKUP_FAST_LASER :
                                            PICKUP_PHASE_WAVE);
        spawn_pickup(enemy->x + 12.0f, enemy->y, enemy->z + 10.0f,
                     game.shield < 68.0f ? PICKUP_REPAIR :
                                           PICKUP_SPEED_BOOST);
        set_message("GUARDIAN DESTROYED", 2.4f);
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
#ifdef GRAVITY_WAVE_AUTOTEST
    (void)amount;
    return;
#endif
    if(game.hit_cooldown > 0.0f || game.mode != MODE_PLAYING)
        return;

    game.shield -= amount;
    game.hit_cooldown = 0.82f;
    game.trauma = fmaxf(game.trauma, 0.72f);
    game.combo = 0;
    game.combo_timer = 0.0f;
    play_sound(sfx_hit, 210, 128);
    play_rumble(6);
    for(i = 0; i < 13; ++i) {
        spawn_particle(game.player_x, game.player_y,
                       game.distance + PLAYER_Z,
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

static void spawn_wave(float world_z) {
    const float rank = clampf(game.distance / 11500.0f, 0.0f, 1.0f);
    const uint32_t seed = hash_u32((uint32_t)(world_z / 73.0f) ^
                                   (uint32_t)game.wave * 0x9e3779b9u);
    const int pattern = (int)(seed % (rank > 0.42f ? 4u : 3u));
    const int count = pattern == 0 ? 5 : (pattern == 3 ? 3 : 4);
    int i;

    if(game.wave > 0 && (game.wave & 7) == 7) {
        enemy_t *guardian = alloc_enemy();
        if(guardian) {
            memset(guardian, 0, sizeof(*guardian));
            guardian->active = true;
            guardian->type = 3;
            guardian->hp = 28 + game.guardians_destroyed * 6;
            guardian->max_hp = guardian->hp;
            guardian->radius = 24.0f;
            guardian->x = 0.0f;
            guardian->y = 57.0f;
            guardian->z = world_z + 160.0f;
            guardian->phase = (float)(seed & 255u) * 0.01f;
            guardian->fire_timer = 1.2f;
            set_message("WARNING  BIOME GUARDIAN", 2.6f);
            play_sound(sfx_gate, 240, 128);
        }
        game.wave++;
        return;
    }

    game.wave++;
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
        enemy->fire_timer = 0.65f + (float)i * 0.18f;
        enemy->type = 0;

        if(pattern == 0) {
            enemy->x = centered * 22.0f;
            enemy->y = 38.0f + fabsf(centered) * 6.0f;
            enemy->z = world_z + fabsf(centered) * 21.0f;
            enemy->vx = centered * -1.1f;
        }
        else if(pattern == 1) {
            const float angle = (float)i * TAU / (float)count;
            enemy->x = fcos(angle) * 34.0f;
            enemy->y = 43.0f + fsin(angle) * 20.0f;
            enemy->z = world_z + (float)i * 28.0f;
            enemy->vx = i & 1 ? 8.0f : -8.0f;
        }
        else if(pattern == 2) {
            enemy->x = (i & 1 ? 1.0f : -1.0f) *
                       (55.0f + (float)i * 5.0f);
            enemy->y = 62.0f - (float)i * 8.0f;
            enemy->z = world_z + (float)i * 24.0f;
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
            enemy->z = world_z + fabsf(centered) * 18.0f;
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
}

static bool scenery_segment_has_regular_arch(int segment) {
    const float z = (float)segment * SCENERY_SEGMENT;
    const int biome = ((int)(z / BIOME_LENGTH)) & 3;
    const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                (uint32_t)biome * 0x51ed270bu);
    const int variant = (int)((h >> 3) % 5u);
    return (biome == 0 && (variant == 1 || variant == 3)) ||
           (biome == 1 && variant == 2) ||
           (biome == 2 && variant == 4) ||
           (biome == 3 && (variant == 2 || variant == 4));
}

static bool world_z_near_scenery_arch(float world_z, float clearance) {
    const int center_segment = (int)floorf(world_z / SCENERY_SEGMENT);
    int offset;

    for(offset = -2; offset <= 2; ++offset) {
        const int segment = center_segment + offset;
        const float segment_z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(segment_z / BIOME_LENGTH)) & 3;

        if(scenery_segment_has_regular_arch(segment) &&
           fabsf(world_z - segment_z) < clearance)
            return true;

        if(((segment % 37 + 37) % 37) == 11 && biome != 2) {
            const int rib_count = biome == 0 ? 4 : 3;
            const float spacing = biome == 0 ? 22.0f :
                                  (biome == 1 ? 28.0f : 32.0f);
            int rib;
            for(rib = 0; rib < rib_count; ++rib) {
                if(fabsf(world_z -
                         (segment_z + (float)rib * spacing)) < clearance)
                    return true;
            }
        }
    }
    return false;
}

static float resolve_gate_world_z(float requested_z) {
    const uint32_t h = hash_u32((uint32_t)(requested_z / 17.0f) ^
                                0x61e93a7du);
    const float preferred = (h & 1u) ? 1.0f : -1.0f;
    int step;

    if(!world_z_near_scenery_arch(requested_z, 34.0f))
        return requested_z;
    for(step = 1; step <= 16; ++step) {
        const float distance = (float)step * 24.0f;
        const float first = requested_z + preferred * distance;
        const float second = requested_z - preferred * distance;
        if(!world_z_near_scenery_arch(first, 34.0f))
            return first;
        if(!world_z_near_scenery_arch(second, 34.0f))
            return second;
    }

    /* Procedural arch runs are much shorter than this search window. Keep a
       deterministic fallback rather than ever superimposing both challenges. */
    return requested_z + preferred * 408.0f;
}

static void spawn_gate(float world_z) {
    gate_t *gate = alloc_gate();
    const float resolved_z = resolve_gate_world_z(world_z);
    const uint32_t h = hash_u32((uint32_t)(resolved_z / 31.0f) ^ 0xd38755a1u);
    if(!gate)
        return;
    if(world_z_near_scenery_arch(resolved_z, 34.0f)) {
        printf("Gravity Wave: skipped obstructed gate near %.0f.\n",
               (double)world_z);
        return;
    }
    if(fabsf(resolved_z - world_z) > 0.5f)
        printf("Gravity Wave: shifted gate %.0f units clear of scenery.\n",
               (double)(resolved_z - world_z));
    gate->active = true;
    gate->x = ((float)((int)(h & 127u) - 63)) * 0.72f;
    gate->y = 31.0f + (float)((h >> 8) & 31u);
    gate->z = resolved_z;
    gate->radius = 14.0f + (float)((h >> 14) & 3u);
    gate->spin = (float)(h & 255u) * (TAU / 255.0f);
    gate->result_time = 0.0f;
    gate->variant = (int)((h >> 18) & 3u);
    gate->result = GATE_RESULT_PENDING;
}

static void fire_player_weapon(void) {
    if(game.temporary_weapon == TEMP_WEAPON_PHASE_WAVE &&
       game.temporary_weapon_time > 0.0f) {
        projectile_t *shot = alloc_projectile(player_shots, MAX_SHOTS);
        if(shot) {
            *shot = (projectile_t){
                .active = true,
                .x = game.player_x,
                .y = game.player_y + 0.2f,
                .z = game.distance + PLAYER_Z + 8.0f,
                .vx = game.player_vx * 0.08f,
                .vy = game.player_vy * 0.05f,
                .vz = 455.0f,
                .life = 1.60f,
                .spin = (game.weapon_shot_counter & 1) ? -0.17f : 0.17f,
                .kind = SHOT_PLAYER_PHASE,
                .damage = 2,
                .hits_remaining = 4,
                .hit_mask = 0u
            };
        }
        game.weapon_shot_counter++;
        game.fire_cooldown = 0.28f;
        play_phase_wave(game.player_x);
        return;
    }

    if(game.temporary_weapon == TEMP_WEAPON_FAST_LASER &&
       game.temporary_weapon_time > 0.0f) {
        projectile_t *shot = alloc_projectile(player_shots, MAX_SHOTS);
        const float offset = (game.weapon_shot_counter & 1) ? 2.9f : -2.9f;
        if(shot) {
            *shot = (projectile_t){
                .active = true,
                .x = game.player_x + offset,
                .y = game.player_y + 0.1f,
                .z = game.distance + PLAYER_Z + 7.0f,
                .vx = game.player_vx * 0.11f + offset * 0.30f,
                .vy = game.player_vy * 0.06f,
                .vz = 690.0f,
                .life = 1.35f,
                .spin = 0.0f,
                .kind = SHOT_PLAYER_FAST,
                .damage = 2,
                .hits_remaining = 1,
                .hit_mask = 0u
            };
        }
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
        *shot = (projectile_t){
            .active = true,
            .x = game.player_x + offset,
            .y = game.player_y + 0.1f,
            .z = game.distance + PLAYER_Z + 7.0f,
            .vx = game.player_vx * 0.15f + offset * 0.48f,
            .vy = game.player_vy * 0.08f,
            .vz = 510.0f + (float)(game.weapon_level - 1) * 45.0f,
            .life = 1.75f,
            .spin = 0.0f,
            .kind = SHOT_PLAYER_LASER,
            .damage = 1,
            .hits_remaining = 1,
            .hit_mask = 0u
        };
    }
    game.weapon_shot_counter++;
    game.fire_cooldown = 0.155f -
                         (float)(game.weapon_level - 1) * 0.018f;
    play_laser(game.player_x);
}

static void fire_enemy_shot(enemy_t *enemy) {
    const float relative_z = enemy->z - (game.distance + PLAYER_Z);
    const float shot_speed = enemy->type == 3 ? 138.0f : 105.0f;
    const float travel_time = clampf(relative_z / (game.speed + shot_speed),
                                     0.35f, 2.5f);
    const int count = enemy->type == 3 ? 3 : 1;
    int i;

    for(i = 0; i < count; ++i) {
        projectile_t *shot = alloc_projectile(enemy_shots, MAX_ENEMY_SHOTS);
        const float spread = ((float)i - (float)(count - 1) * 0.5f) * 23.0f;
        if(!shot)
            break;
        *shot = (projectile_t){
            .active = true,
            .x = enemy->x,
            .y = enemy->y,
            .z = enemy->z - 3.0f,
            .vx = (game.player_x + game.player_vx * 0.20f - enemy->x) /
                  travel_time + spread,
            .vy = (game.player_y + game.player_vy * 0.12f - enemy->y) /
                  travel_time +
                  (enemy->type == 3 ? fabsf(spread) * 0.10f : 0.0f),
            .vz = -shot_speed,
            .life = enemy->type == 3 ? 4.2f : 3.2f,
            .spin = 0.0f,
            .kind = SHOT_ENEMY,
            .damage = 1,
            .hits_remaining = 1,
            .hit_mask = 0u
        };
    }
    enemy->fire_timer = enemy->type == 3 ? 0.72f :
                        (enemy->type == 2 ? 1.05f : 1.55f);
    enemy->fired = true;
}

static void trigger_bomb(void) {
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
                                current_palette()->accent, 22);
                if(enemies[i].hp <= 0)
                    destroy_enemy(&enemies[i], true);
            }
            else {
                destroy_enemy(&enemies[i], true);
            }
        }
    }
    /* Keep replacement formations beyond the pulse's cleared arc. */
    game.next_wave_z = fmaxf(game.next_wave_z, game.distance + 900.0f);
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
    game.next_wave_z = 520.0f;
    game.next_gate_z = 820.0f;
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
    start_music_track(choose_random_music_track(current_music_track),
                      MUSIC_GAME_VOLUME);
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
        else {
            particle->vy -= 7.0f * dt;
            particle->vx *= 1.0f - clampf(dt * 0.7f, 0.0f, 0.4f);
            particle->vz *= 1.0f - clampf(dt * 0.5f, 0.0f, 0.3f);
        }
    }
}

static void update_player_shots(float dt) {
    int i, j;
    for(i = 0; i < MAX_SHOTS; ++i) {
        projectile_t *shot = &player_shots[i];
        float previous_x;
        float previous_y;
        float previous_z;
        bool phase_wave;
        float z_radius;
        float sweep_min_z;
        float sweep_max_z;
        float sweep_dz;
        float phase_cos = 1.0f;
        float phase_sin = 0.0f;
        if(!shot->active)
            continue;
        previous_x = shot->x;
        previous_y = shot->y;
        previous_z = shot->z;
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

        phase_wave = shot->kind == SHOT_PLAYER_PHASE;
        z_radius = phase_wave ? 18.0f : 15.0f;
        sweep_min_z = fminf(previous_z, shot->z) - z_radius;
        sweep_max_z = fmaxf(previous_z, shot->z) + z_radius;
        sweep_dz = shot->z - previous_z;
        if(phase_wave) {
            phase_cos = fcos(shot->spin);
            phase_sin = fsin(shot->spin);
        }

        for(j = 0; j < MAX_ENEMIES; ++j) {
            enemy_t *enemy = &enemies[j];
            float sweep_t;
            float hit_x;
            float hit_y;
            bool lateral_hit;
            if(!enemy->active)
                continue;
            if(phase_wave && (shot->hit_mask & (1u << j)))
                continue;
            if(enemy->z < sweep_min_z || enemy->z > sweep_max_z)
                continue;

            sweep_t = fabsf(sweep_dz) > 0.0001f ?
                clampf((enemy->z - previous_z) / sweep_dz, 0.0f, 1.0f) :
                1.0f;
            hit_x = lerpf(previous_x, shot->x, sweep_t);
            hit_y = lerpf(previous_y, shot->y, sweep_t);
            if(phase_wave) {
                const float dx = hit_x - enemy->x;
                const float dy = hit_y - enemy->y;
                const float local_x = dx * phase_cos + dy * phase_sin;
                const float local_y = -dx * phase_sin + dy * phase_cos;
                const float radius_x = 31.0f + enemy->radius;
                const float radius_y = 13.5f + enemy->radius;
                lateral_hit =
                    (local_x * local_x) / (radius_x * radius_x) +
                    (local_y * local_y) / (radius_y * radius_y) < 1.0f;
            }
            else {
                lateral_hit =
                    fabsf(hit_x - enemy->x) < enemy->radius + 2.0f &&
                    fabsf(hit_y - enemy->y) < enemy->radius + 2.0f;
            }

            if(lateral_hit) {
                if(phase_wave) {
                    shot->hit_mask |= 1u << j;
                    shot->hits_remaining--;
                    if(shot->hits_remaining <= 0)
                        shot->active = false;
                }
                else {
                    shot->active = false;
                }
                enemy->hp -= shot->damage > 0 ? shot->damage : 1;
                spawn_particle(enemy->x, enemy->y, enemy->z,
                               random_signed() * 18.0f,
                               random_signed() * 18.0f,
                               -18.0f,
                               0.25f, 2.5f,
                               phase_wave ?
                               (color3_t){1.0f, 0.28f, 0.86f} :
                               (color3_t){0.80f, 0.95f, 1.0f},
                               PARTICLE_STREAK);
                if(enemy->hp <= 0)
                    destroy_enemy(enemy, true);
                else
                    play_sound(sfx_hit, 130, 128);
                if(!shot->active)
                    break;
            }
        }
    }
}

static void update_enemy_shots(float dt) {
    int i;
    const float player_world_z = game.distance + PLAYER_Z;
    for(i = 0; i < MAX_ENEMY_SHOTS; ++i) {
        projectile_t *shot = &enemy_shots[i];
        if(!shot->active)
            continue;
        shot->x += shot->vx * dt;
        shot->y += shot->vy * dt;
        shot->z += shot->vz * dt;
        shot->life -= dt;
        if(shot->life <= 0.0f || shot->z < game.distance - 10.0f) {
            shot->active = false;
            continue;
        }
        if(fabsf(shot->z - player_world_z) < 9.0f &&
           fabsf(shot->x - game.player_x) < 5.0f &&
           fabsf(shot->y - game.player_y) < 4.0f) {
            shot->active = false;
            damage_player(13.0f);
        }
    }
}

static void update_enemies(float dt) {
    const float rank = clampf(game.distance / 11500.0f, 0.0f, 1.0f);
    const float player_world_z = game.distance + PLAYER_Z;
    int i;

    for(i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *enemy = &enemies[i];
        float relative_z;
        if(!enemy->active)
            continue;

        enemy->phase += dt * (enemy->type == 1 ? 2.4f :
                              (enemy->type == 3 ? 1.08f : 1.55f));
        enemy->fire_timer -= dt;
        if(enemy->type == 3) {
            const float target_z = game.distance + 455.0f;
            enemy->x = fsin(enemy->phase * 0.83f) * 43.0f;
            enemy->y = 57.0f + fsin(enemy->phase * 1.21f) * 11.0f;
            enemy->z = lerpf(enemy->z, target_z, clampf(dt * 1.35f, 0.0f, 1.0f));
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

        if(fabsf(relative_z) < enemy->radius + 5.0f &&
           fabsf(enemy->x - game.player_x) < enemy->radius + 3.5f &&
           fabsf(enemy->y - game.player_y) < enemy->radius + 3.0f) {
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

static void spawn_gate_clear_burst(const gate_t *gate, bool perfect) {
    const palette_t *palette = current_palette();
    const float clear_radius = gate->radius * GATE_CLEAR_RADIUS_SCALE;
    const int count = perfect ? 24 : 18;
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
        spawn_particle(gate->x + cs * clear_radius,
                       gate->y + sn * clear_radius, gate->z,
                       cs * speed, sn * speed,
                       random_signed() * 24.0f,
                       0.38f + random_unit() * 0.34f,
                       1.5f + random_unit() * 2.4f,
                       color, PARTICLE_STREAK);
    }
}

static void update_gates(float dt) {
    const float player_world_z = game.distance + PLAYER_Z;
    int i;
    for(i = 0; i < MAX_GATES; ++i) {
        gate_t *gate = &gates[i];
        const float direction = (gate->variant & 1) ? -1.0f : 1.0f;
        if(!gate->active)
            continue;
        gate->spin += dt * direction *
                      (0.72f + (float)gate->variant * 0.07f);
        gate->result_time = fmaxf(0.0f, gate->result_time - dt);
        if(gate->result == GATE_RESULT_PENDING &&
           gate->z <= player_world_z + 2.0f) {
            const float dx = gate->x - game.player_x;
            const float dy = gate->y - game.player_y;
            const float clear_radius = gate->radius * GATE_CLEAR_RADIUS_SCALE;
            const bool perfect = game.speed > 145.0f;
            if(dx * dx + dy * dy < clear_radius * clear_radius) {
                gate->result = GATE_RESULT_CLEARED;
                gate->result_time = 0.55f;
                add_score(perfect ? 420 : 280);
                game.shield = clampf(game.shield + 7.0f, 0.0f, 100.0f);
                game.boost = clampf(game.boost + 16.0f, 0.0f, 100.0f);
                set_message(perfect ?
                            "BOOST GATE PERFECT" : "GATE CLEAR", 1.3f);
                spawn_gate_clear_burst(gate, perfect);
                game.trauma = fmaxf(game.trauma, perfect ? 0.18f : 0.10f);
                play_sound(sfx_gate, 225, 128);
            }
            else {
                gate->result = GATE_RESULT_MISSED;
                gate->result_time = 0.45f;
                game.combo = 0;
                set_message("GATE MISSED", 0.8f);
            }
#ifdef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
            printf("Gravity Wave autotest: Gravity Bloom %s at "
                   "offset=(%.2f, %.2f), aperture=%.2f.\n",
                   gate->result == GATE_RESULT_CLEARED ? "cleared" : "missed",
                   (double)dx, (double)dy, (double)clear_radius);
#endif
        }
        if(gate->z < game.distance - 18.0f)
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

static void check_scenery_collision(void) {
    const float player_z = game.distance + PLAYER_Z;
    const int center_segment = (int)floorf(player_z / SCENERY_SEGMENT);
    int offset;

    if(game.hit_cooldown > 0.0f)
        return;
    for(offset = -1; offset <= 1; ++offset) {
        const int segment = center_segment + offset;
        const float z = (float)segment * SCENERY_SEGMENT;
        const int biome = ((int)(z / BIOME_LENGTH)) & 3;
        const uint32_t h = hash_u32((uint32_t)segment * 0x9e3779b9u ^
                                    (uint32_t)biome * 0x51ed270bu);
        const float side = (h & 1u) ? 1.0f : -1.0f;
        const float object_x = side * (70.0f + (float)((h >> 8) & 63u));
        const float base = terrain_height_local(object_x, z);
        const int variant = (int)((h >> 3) % 5u);
        float arch_span = 0.0f;
        float arch_base_y = 0.0f;
        float arch_height = 0.0f;
        float arch_thickness = 0.0f;
        float object_radius = 0.0f;
        float object_height = 0.0f;
        bool impact = false;
        bool signature_impact = false;
        const bool near_regular = fabsf(player_z - z) <= 18.0f;

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
                if(fabsf(game.player_x - object_x) < rock_radius + 4.0f &&
                   game.player_y > rock_y - rock_radius * 1.2f - 4.0f &&
                   game.player_y < rock_y + rock_radius * 0.8f + 4.0f)
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

        if(near_regular && arch_span > 0.0f &&
           player_hits_material_arch(game.player_x, game.player_y,
                                     arch_span, arch_base_y,
                                     arch_height, arch_thickness))
            impact = true;
        else if(near_regular && object_radius > 0.0f &&
                fabsf(game.player_x - object_x) < object_radius &&
                game.player_y > base - 4.0f &&
                game.player_y < base + object_height)
            impact = true;

        signature_impact = player_hits_signature_arch(
            biome, segment, z, player_z, game.player_x, game.player_y);
        if(!impact && signature_impact)
            impact = true;

        if(impact) {
            const float push = arch_span > 0.0f || signature_impact ?
                (game.player_x >= 0.0f ? -1.0f : 1.0f) :
                (game.player_x >= object_x ? 1.0f : -1.0f);
            game.player_x = clampf(game.player_x + push * 7.0f,
                                   PLAYER_MIN_X, PLAYER_MAX_X);
            game.player_vx += push * 34.0f;
            set_message("STRUCTURE IMPACT", 0.9f);
            damage_player(14.0f);
            return;
        }
    }
}

static void update_spawning(void) {
    const float rank = clampf(game.distance / 11500.0f, 0.0f, 1.0f);
    if(active_guardian()) {
        if(game.next_wave_z < game.distance + 760.0f)
            game.next_wave_z = game.distance + 760.0f;
        return;
    }
    while(game.next_wave_z < game.distance + 1120.0f) {
        spawn_wave(game.next_wave_z);
        game.next_wave_z += 620.0f - rank * 155.0f;
        if(active_guardian())
            break;
    }
    while(game.next_gate_z < game.distance + 1260.0f) {
        spawn_gate(game.next_gate_z);
        game.next_gate_z += 900.0f +
                            (float)(hash_u32((uint32_t)game.next_gate_z) & 127u);
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
        const vec3_t engine = player_model_point((float)side * 3.0f,
                                                  -0.2f, -6.8f);
        const float engine_local_x = engine.x - path_center(engine.z);
        spawn_particle(engine_local_x + random_signed() * 0.22f,
                       engine.y + random_signed() * 0.18f,
                       engine.z - 0.35f,
                       random_signed() * 0.75f,
                       random_signed() * 0.55f,
                       -18.0f - random_unit() * (boosting ? 17.0f : 9.0f),
                       boosting ? 0.44f : 0.30f,
                       boosting ? 1.75f : 1.12f,
                       color_lerp(palette->river,
                                  (color3_t){0.72f,0.90f,1.0f}, 0.58f),
                       PARTICLE_EXHAUST);
    }
}

static void enter_title(void) {
    const int title_biome = (int)(random_u32() >> 30);
    const float title_offset = 620.0f + random_unit() * 1280.0f;

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
    start_music_track(choose_random_music_track(current_music_track),
                      MUSIC_TITLE_VOLUME);
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
            start_music_track(choose_random_music_track(current_music_track),
                              MUSIC_TITLE_VOLUME);
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
        if(input->pressed & (CONT_START | CONT_A))
            reset_game();
        else if(input->pressed & CONT_B)
            return false;
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
        const float rank = clampf(game.distance / 11500.0f, 0.0f, 1.0f);
        const float target_vx = input->x * 68.0f;
        const float target_vy = -input->y * 53.0f;
        float cruise_speed = 96.0f + rank * 58.0f;
        const float response = clampf(dt * 5.8f, 0.0f, 1.0f);
        const bool speed_boost_active = game.speed_boost_time > 0.0f;

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
            update_player_shots(dt);
            update_enemy_shots(dt);
            if(game.mode == MODE_PLAYING)
                update_enemies(dt);
            if(game.mode == MODE_PLAYING) {
                update_gates(dt);
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
            if(game.mode == MODE_PLAYING)
                check_scenery_collision();

            if(game.mode == MODE_PLAYING) {
                game.palette_index = ((int)(game.distance / BIOME_LENGTH)) %
                                     ARRAY_COUNT(palettes);
                if(game.palette_index != game.last_palette_index) {
                    set_message(palettes[game.palette_index].name, 2.3f);
                    game.last_palette_index = game.palette_index;
                    start_music_track(
                        choose_random_music_track(current_music_track),
                        MUSIC_GAME_VOLUME);
                    play_sound(sfx_gate, 180, 128);
                }
            }
        }
    }

    game.previous_buttons = input->buttons;
    game.previous_left_trigger = input->left_trigger > 0.32f;
    game.previous_right_trigger = input->right_trigger > 0.32f;
    return true;
}

static vec3_t enemy_model_point(const enemy_t *enemy,
                                float lx, float ly, float lz) {
    const float roll = fsin(enemy->phase * 1.6f) * 0.33f;
    const float cr = fcos(roll);
    const float sr = fsin(roll);
    const float scale = enemy->type == 3 ? 2.15f :
                        (enemy->type == 2 ? 1.45f :
                         (enemy->type == 1 ? 1.12f : 0.92f));
    const float rx = (lx * cr - ly * sr) * scale;
    const float ry = (lx * sr + ly * cr) * scale;
    return (vec3_t){path_center(enemy->z) + enemy->x + rx,
                    enemy->y + ry, enemy->z + lz * scale};
}

static void draw_mesh_instance(const gravity_wave_mesh_t *mesh, vec3_t origin,
                               float yaw, float pitch, float roll, float scale,
                               color3_t tint, bool flash) {
    vec3_t transformed[96];
    screen_point_t projected[96];
    const float cy = fcos(yaw);
    const float sy = fsin(yaw);
    const float cp = fcos(pitch);
    const float sp = fsin(pitch);
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
        project_world(transformed[i], &projected[i]);
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

static void draw_enemy_model(const enemy_t *enemy, const palette_t *palette) {
    const gravity_wave_mesh_t *mesh = enemy->type == 3 ? &mesh_guardian :
                                enemy->type == 2 ? &mesh_bomber :
                                enemy->type == 1 ? &mesh_ace :
                                                   &mesh_interceptor;
    const float scale = enemy->type == 3 ? 1.78f :
                        (enemy->type == 2 ? 0.92f : 0.78f);
    const float roll = fsin(enemy->phase * 1.6f) *
                       (enemy->type == 3 ? 0.08f : 0.33f);
    const float yaw = PI + (enemy->type == 1 ? fsin(enemy->phase) * 0.18f : 0.0f);
    const vec3_t origin = {path_center(enemy->z) + enemy->x,
                           enemy->y, enemy->z};
    const color3_t tint = enemy->type == 3 ?
        color_lerp((color3_t){0.96f,0.90f,0.82f}, palette->accent, 0.12f) :
        color_lerp((color3_t){0.94f,0.88f,0.88f}, palette->enemy, 0.16f);
    draw_mesh_instance(mesh, origin, yaw, 0.0f, roll, scale, tint, false);
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
    const float pitch = clampf(-game.player_vy * 0.007f, -0.30f, 0.30f);
    const float cr = fcos(roll);
    const float sr = fsin(roll);
    const float cp = fcos(pitch);
    const float sp = fsin(pitch);
    const float rx = lx * cr - ly * sr;
    const float ry0 = lx * sr + ly * cr;
    const float ry = ry0 * cp - lz * sp;
    const float rz = ry0 * sp + lz * cp;
    const float world_z = game.distance + PLAYER_Z;
    return (vec3_t){path_center(world_z) + game.player_x + rx,
                    game.player_y + ry, world_z + rz};
}

static void draw_player_ship(const palette_t *palette) {
    const bool flashing = game.hit_cooldown > 0.0f &&
                          ((int)(game.hit_cooldown * 18.0f) & 1);
    const float roll = game.bank * 0.62f + game.barrel_roll +
                       camera_route_turn * 1.35f;
    const float pitch = clampf(-game.player_vy * 0.007f, -0.30f, 0.30f);
    const float world_z = game.distance + PLAYER_Z;
    const vec3_t origin = {path_center(world_z) + game.player_x,
                           game.player_y, world_z};
    const color3_t hull = color_lerp((color3_t){0.98f,0.99f,1.0f},
                                     palette->river, 0.10f);
    draw_mesh_instance(&mesh_player, origin, 0.0f, pitch, roll,
                       0.88f, hull, flashing);
}

static void draw_enemy_glows(const palette_t *palette) {
    int i;
    for(i = 0; i < MAX_ENEMIES; ++i) {
        enemy_t *enemy = &enemies[i];
        screen_point_t point;
        vec3_t engine;
        float size;
        if(!enemy->active)
            continue;
        engine = enemy_model_point(enemy, 0.0f, 0.0f, 6.0f);
        if(!project_world(engine, &point))
            continue;
        size = clampf(point.z * (enemy->type == 3 ? 5200.0f : 2800.0f),
                      1.5f, enemy->type == 3 ? 25.0f :
                      (enemy->type == 2 ? 16.0f : 9.0f));
        draw_disc(&additive_header, point.x, point.y, size, point.z + 0.00001f, 8,
                  pack_color(0.75f, palette->enemy),
                  pack_color(0.0f, palette->accent));
        if(enemy->fire_timer > 0.0f && enemy->fire_timer < 0.55f) {
            const float charge = 1.0f - enemy->fire_timer / 0.55f;
            const vec3_t muzzle = enemy_model_point(
                enemy, 0.0f, 0.0f, enemy->type == 3 ? -15.0f : -8.0f);
            draw_textured_billboard(&sprite_additive_headers[2], muzzle,
                                    enemy->type == 3 ? 20.0f : 9.0f,
                                    enemy->type == 3 ? 20.0f : 9.0f,
                                    pack_color(0.30f + charge * 0.68f,
                                               palette->enemy));
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
        if(project_world(engine, &point)) {
            const float pulse = 1.0f + fsin(game.time * 26.0f) * 0.12f;
            if(project_world(plume_tail, &tail)) {
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

        if(!gate->active || far_fade <= 0.01f)
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
        const vec3_t head_world = {path_center(shot->z) + shot->x,
                                   shot->y, shot->z};
        color3_t main_color;
        color3_t core_color;
        float length;
        float tail_z;
        vec3_t tail_world;
        float width;

        if(!shot->active || !project_world(head_world, &head))
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

            draw_textured_billboard(&sprite_additive_headers[2], head_world,
                                    13.0f, 13.0f,
                                    pack_color(0.52f, wave_color));
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
            main_color = palette->enemy;
            core_color = (color3_t){1.0f, 0.68f, 0.42f};
            length = 11.0f;
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
        tail_z = shot->z - (hostile ? -length : length);
        tail_world = (vec3_t){path_center(tail_z) + shot->x,
                              shot->y, tail_z};
        if(!project_world(tail_world, &tail))
            continue;
        width = clampf(2.0f + head.z * 250.0f, 2.0f, hostile ? 8.0f : 6.0f);
        if(!hostile && shot->kind == SHOT_PLAYER_FAST)
            width *= 0.72f;
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
        world = (vec3_t){path_center(particle->z) + particle->x,
                         particle->y, particle->z};
        if(!project_world(world, &point))
            continue;
        alpha = clampf(particle->life / particle->max_life, 0.0f, 1.0f);
        size = clampf(particle->size * game.camera_focal * point.z,
                      1.0f,
                      particle->kind == PARTICLE_STREAK ? 13.0f : 18.0f);
        if(particle->kind == PARTICLE_STREAK) {
            screen_point_t tail;
            vec3_t tail_world = world;
            tail_world.x -= particle->vx * 0.055f;
            tail_world.y -= particle->vy * 0.055f;
            tail_world.z -= particle->vz * 0.055f;
            if(project_world(tail_world, &tail))
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

            forward_world.z += 2.0f + age * 3.0f;
            if(project_world(forward_world, &forward))
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
        else {
            const int sprite = particle->color.r > particle->color.b * 1.18f ?
                               3 : 2;
            draw_textured_billboard(&sprite_additive_headers[sprite], world,
                                    particle->size * 2.0f,
                                    particle->size * 2.0f,
                                    pack_color(alpha * 0.78f,
                                               particle->color));
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
    const float target_z = game.distance + 440.0f;
    screen_point_t target;
    const uint32_t color = pack_color(0.64f, palette->river);
    const uint32_t dim = pack_color(0.22f, palette->river);
    const float r = 15.0f + fsin(game.time * 5.0f) * 1.5f;
    if(!project_world((vec3_t){path_center(target_z) + game.player_x,
                               game.player_y, target_z}, &target))
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
            draw_rect(&hud_header, 153.0f, 64.0f, 334.0f, 43.0f, 9.0f,
                      pack_color(0.58f, (color3_t){0.025f,0.004f,0.020f}));
            draw_text_centered(70.0f, "BIOME GUARDIAN", 2,
                               palette->enemy, 0.96f);
            draw_bar(177.0f, 91.0f, 286.0f, 10.0f,
                     (float)guardian->hp /
                     (float)(guardian->max_hp > 0 ? guardian->max_hp : 1),
                     palette->enemy);
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
    if(controller)
        draw_text_centered(344.0f, "PRESS START", 3, palette->river, pulse);
    else
        draw_text_centered(344.0f, "CONNECT CONTROLLER", 2,
                           (color3_t){1.0f, 0.55f, 0.24f}, pulse);
    draw_text_centered(383.0f, "ANALOG OR DPAD TO FLY", 2,
                       (color3_t){0.70f, 0.82f, 0.93f}, 0.86f);
    draw_text_centered(405.0f, "A FIRE  B BOOST  X BRAKE  Y NOVA", 1,
                       (color3_t){0.70f, 0.82f, 0.93f}, 0.86f);
    draw_text_centered(421.0f, "L R BARREL ROLL   START PAUSE", 1,
                       (color3_t){0.70f, 0.82f, 0.93f}, 0.86f);
    draw_text_centered(439.0f, "B ON TITLE TO EXIT", 1,
                       color_scale(palette->accent, 0.85f), 0.76f);
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
    }
    enter_title();
#ifdef GRAVITY_WAVE_AUTOTEST
    reset_game();
#ifndef GRAVITY_WAVE_AUTOTEST_DISTANCE
#define GRAVITY_WAVE_AUTOTEST_DISTANCE 4120.0f
#endif
    game.distance = GRAVITY_WAVE_AUTOTEST_DISTANCE;
    game.palette_index = ((int)(game.distance / BIOME_LENGTH)) & 3;
    game.last_palette_index = game.palette_index;
    start_music_track(choose_random_music_track(current_music_track),
                      MUSIC_GAME_VOLUME);
    game.next_wave_z = game.distance + 500.0f;
    game.next_gate_z = game.distance + 680.0f;
#ifdef GRAVITY_WAVE_AUTOTEST_GATE_VIEW
    game.next_wave_z = game.distance + 2400.0f;
    game.next_gate_z = game.distance + 4000.0f;
    spawn_gate(game.distance + 2200.0f);
    gates[0].x = 0.0f;
    gates[0].y = 35.0f;
    gates[0].radius = 16.0f;
    printf("Gravity Wave autotest: centered Gravity Bloom enabled.\n");
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
        input.x = 0.0f;
        input.y = 0.0f;
#elif defined(GRAVITY_WAVE_AUTOTEST_EXTENTS)
        input.x = fsin(game.time * 0.58f);
        input.y = fsin(game.time * 0.47f + 0.7f);
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

    printf("Gravity Wave: shutting down cleanly.\n");
    shutdown_audio();
    release_textures();
    pvr_shutdown();
    return 0;
}
