/*
 * Demon Bazooka -- a procedural 3D arcade game for Sega Dreamcast.
 *
 * Everything visible is submitted as untextured colored geometry through the
 * low-level KallistiOS PowerVR API. Even the UI font is a tiny polygon font.
 * Sound effects are synthesized at startup; there are no external assets.
 */

#include <kos.h>

#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

#define SCREEN_W 640.0f
#define SCREEN_H 480.0f
#define PI 3.14159265358979323846f
#define TAU (2.0f * PI)

#define MAX_ENEMIES 24
#define MAX_BOLTS 40
#define MAX_POWERUPS 10
#define MAX_PARTICLES 224
#define MAX_SHOCKWAVES 10
#define MAX_LIGHTNING 24
#define MAX_LIVES 6
#define WAVE_TRANSITION_SECONDS 3.6f

/* Raised once per captured visual-development pass. */
#define VISUAL_REVISION 20

#define COL_BLACK       0xff050007u
#define COL_GROUND      0xff19050au
#define COL_GROUND_ALT  0xff28070du
#define COL_CRIMSON     0xffa20d24u
#define COL_RED         0xfff12b19u
#define COL_ORANGE      0xffff6a08u
#define COL_GOLD        0xffffc928u
#define COL_YELLOW      0xfffff06au
#define COL_BONE        0xffffefd0u
#define COL_WHITE       0xffffffffu
#define COL_PURPLE      0xff6d16a8u
#define COL_MAGENTA     0xffff2aa8u
#define COL_CYAN        0xff35e7ffu
#define COL_BLUE        0xff3266ffu
#define COL_DARK_BLUE   0xff101c55u
#define COL_GREEN       0xff36e178u
#define COL_GREY        0xff635766u

typedef struct vec3 {
    float x;
    float y;
    float z;
} vec3_t;

typedef struct screen_point {
    float x;
    float y;
    float z;
} screen_point_t;

typedef struct camera {
    vec3_t pos;
    vec3_t right;
    vec3_t up;
    vec3_t forward;
    float focal;
} camera_t;

typedef struct enemy {
    int active;
    int kind;
    int hp;
    vec3_t pos;
    vec3_t vel;
    float phase;
    float yaw;
    float radius;
} enemy_t;

typedef struct bolt {
    int active;
    vec3_t pos;
    vec3_t prev;
    vec3_t vel;
    float life;
} bolt_t;

typedef enum powerup_kind {
    POWERUP_SPEED,
    POWERUP_RAPID_FIRE,
    POWERUP_TRIPLE_SHOT
} powerup_kind_t;

typedef struct powerup {
    int active;
    powerup_kind_t kind;
    vec3_t pos;
    float age;
    float lifetime;
    float phase;
} powerup_t;

typedef struct particle {
    int active;
    vec3_t pos;
    vec3_t vel;
    float life;
    float max_life;
    float size;
    uint32_t color;
} particle_t;

typedef struct shockwave {
    int active;
    vec3_t pos;
    float age;
    float max_age;
    float max_radius;
    uint32_t color;
} shockwave_t;

typedef struct lightning {
    int active;
    vec3_t target;
    float age;
    float max_age;
    float height;
    uint32_t seed;
    uint32_t color;
} lightning_t;

typedef enum game_mode {
    MODE_TITLE,
    MODE_PLAY,
    MODE_GAME_OVER
} game_mode_t;

typedef enum speech_kind {
    SPEECH_SILENCE,
    SPEECH_VOWEL,
    SPEECH_VOICED,
    SPEECH_FRICATIVE,
    SPEECH_PLOSIVE
} speech_kind_t;

typedef enum phoneme_id {
    PH_SIL,
    PH_AA,
    PH_AE,
    PH_AH,
    PH_EH,
    PH_IH,
    PH_IY,
    PH_OH,
    PH_UW,
    PH_ER,
    PH_B,
    PH_D,
    PH_F,
    PH_G,
    PH_H,
    PH_K,
    PH_L,
    PH_M,
    PH_N,
    PH_R,
    PH_S,
    PH_SH,
    PH_T,
    PH_TH,
    PH_V,
    PH_W,
    PH_Z,
    PH_COUNT,
    PH_END = -1
} phoneme_id_t;

typedef struct speech_phoneme {
    speech_kind_t kind;
    float formant[3];
    float bandwidth;
    float voiced;
    float noise;
    float amplitude;
} speech_phoneme_t;

typedef struct speech_unit {
    phoneme_id_t phoneme;
    int duration_ms;
    int pitch_offset;
} speech_unit_t;

typedef struct resonator {
    float previous_1;
    float previous_2;
    float coefficient;
    float radius_squared;
    float gain;
} resonator_t;

static camera_t camera_view;
static pvr_poly_hdr_t opaque_header;
static pvr_poly_hdr_t translucent_header;
static pvr_poly_hdr_t additive_header;
static pvr_poly_hdr_t ui_header;
static const pvr_poly_hdr_t *active_header;

static enemy_t enemies[MAX_ENEMIES];
static bolt_t bolts[MAX_BOLTS];
static powerup_t powerups[MAX_POWERUPS];
static particle_t particles[MAX_PARTICLES];
static shockwave_t shockwaves[MAX_SHOCKWAVES];
static lightning_t lightning_bolts[MAX_LIGHTNING];

static game_mode_t game_mode = MODE_TITLE;
static vec3_t player_pos;
static vec3_t player_dir;
static vec3_t player_vel;
static int player_lives;
static int score;
static int high_score;
static int combo;
static int wave_number;
static int wave_kills;
static int wave_target;
static int wave_spawned;
static int kills;
static float barrage_charge;
static float fire_cooldown;
static float dash_cooldown;
static float invulnerability;
static float speed_boost_time;
static float rapid_fire_time;
static float triple_shot_time;
static float powerup_banner_time;
static powerup_kind_t powerup_banner_kind;
static float spawn_timer;
static float wave_transition_time;
static float game_time;
static float mode_time;
static float screen_shake;
static float flash_amount;
static uint32_t previous_buttons;
static uint32_t rng_state = 0x5a7a4e13u;
static int welcome_spoken;
static float voice_cooldown;

static void begin_wave_transition(void);

static sfxhnd_t sfx_fire = SFXHND_INVALID;
static sfxhnd_t sfx_boom = SFXHND_INVALID;
static sfxhnd_t sfx_dash = SFXHND_INVALID;
static sfxhnd_t sfx_barrage = SFXHND_INVALID;
static sfxhnd_t sfx_music = SFXHND_INVALID;
static sfxhnd_t sfx_music_gameplay = SFXHND_INVALID;
static int music_channel = -1;
static sfxhnd_t speech_welcome = SFXHND_INVALID;
static sfxhnd_t speech_blast = SFXHND_INVALID;
static sfxhnd_t speech_bazooka = SFXHND_INVALID;
static sfxhnd_t speech_game_over = SFXHND_INVALID;
static sfxhnd_t speech_more_demons = SFXHND_INVALID;
static sfxhnd_t speech_burn_in_hell = SFXHND_INVALID;

/*
 * Formant targets are intentionally exaggerated. At 11.025 kHz they produce
 * intelligible, crunchy speech while leaving plenty of AICA RAM for gameplay.
 */
static const speech_phoneme_t speech_phonemes[PH_COUNT] = {
    {SPEECH_SILENCE,   {   0.0f,    0.0f,    0.0f}, 100.0f, 0.00f, 0.00f, 0.00f},
    {SPEECH_VOWEL,     { 730.0f, 1090.0f, 2440.0f},  85.0f, 1.00f, 0.04f, 0.90f}, /* AA */
    {SPEECH_VOWEL,     { 660.0f, 1720.0f, 2410.0f},  90.0f, 1.00f, 0.04f, 0.92f}, /* AE */
    {SPEECH_VOWEL,     { 640.0f, 1190.0f, 2390.0f},  90.0f, 1.00f, 0.05f, 0.88f}, /* AH */
    {SPEECH_VOWEL,     { 530.0f, 1840.0f, 2480.0f},  85.0f, 1.00f, 0.04f, 0.90f}, /* EH */
    {SPEECH_VOWEL,     { 390.0f, 1990.0f, 2550.0f},  80.0f, 1.00f, 0.04f, 0.86f}, /* IH */
    {SPEECH_VOWEL,     { 270.0f, 2290.0f, 3010.0f},  75.0f, 1.00f, 0.03f, 0.84f}, /* IY */
    {SPEECH_VOWEL,     { 570.0f,  840.0f, 2410.0f},  90.0f, 1.00f, 0.04f, 0.92f}, /* OH */
    {SPEECH_VOWEL,     { 300.0f,  870.0f, 2240.0f},  80.0f, 1.00f, 0.03f, 0.88f}, /* UW */
    {SPEECH_VOWEL,     { 490.0f, 1350.0f, 1690.0f},  85.0f, 1.00f, 0.04f, 0.90f}, /* ER */
    {SPEECH_PLOSIVE,   { 500.0f, 1100.0f, 2200.0f}, 220.0f, 0.18f, 0.75f, 0.82f}, /* B */
    {SPEECH_PLOSIVE,   { 700.0f, 1900.0f, 3000.0f}, 260.0f, 0.12f, 0.82f, 0.82f}, /* D */
    {SPEECH_FRICATIVE, {1200.0f, 2800.0f, 4200.0f}, 360.0f, 0.00f, 1.00f, 0.72f}, /* F */
    {SPEECH_PLOSIVE,   { 450.0f, 1400.0f, 2500.0f}, 230.0f, 0.18f, 0.78f, 0.84f}, /* G */
    {SPEECH_FRICATIVE, { 650.0f, 1450.0f, 2600.0f}, 400.0f, 0.00f, 0.72f, 0.52f}, /* H */
    {SPEECH_PLOSIVE,   {1200.0f, 2600.0f, 3900.0f}, 300.0f, 0.00f, 0.95f, 0.84f}, /* K */
    {SPEECH_VOICED,    { 400.0f, 1200.0f, 2600.0f}, 110.0f, 0.82f, 0.06f, 0.72f}, /* L */
    {SPEECH_VOICED,    { 250.0f, 1000.0f, 2100.0f}, 120.0f, 0.88f, 0.04f, 0.68f}, /* M */
    {SPEECH_VOICED,    { 300.0f, 1700.0f, 2500.0f}, 120.0f, 0.84f, 0.05f, 0.68f}, /* N */
    {SPEECH_VOICED,    { 350.0f, 1300.0f, 1700.0f}, 100.0f, 0.88f, 0.04f, 0.74f}, /* R */
    {SPEECH_FRICATIVE, {2400.0f, 3600.0f, 4700.0f}, 310.0f, 0.00f, 1.00f, 0.68f}, /* S */
    {SPEECH_FRICATIVE, {1700.0f, 2700.0f, 3900.0f}, 360.0f, 0.00f, 1.00f, 0.72f}, /* SH */
    {SPEECH_PLOSIVE,   {1800.0f, 3200.0f, 4600.0f}, 330.0f, 0.00f, 1.00f, 0.80f}, /* T */
    {SPEECH_FRICATIVE, { 650.0f, 2300.0f, 3400.0f}, 330.0f, 0.38f, 0.62f, 0.67f}, /* TH */
    {SPEECH_FRICATIVE, { 500.0f, 1500.0f, 3000.0f}, 250.0f, 0.48f, 0.48f, 0.70f}, /* V */
    {SPEECH_VOICED,    { 300.0f,  700.0f, 2100.0f}, 100.0f, 0.90f, 0.03f, 0.72f}, /* W */
    {SPEECH_FRICATIVE, { 500.0f, 3400.0f, 4600.0f}, 300.0f, 0.45f, 0.70f, 0.72f}  /* Z */
};

static const speech_unit_t phrase_welcome[] = {
    {PH_D, 70, 2}, {PH_IY, 155, 1}, {PH_M, 95, 0}, {PH_AH, 145, -1},
    {PH_N, 80, -2}, {PH_Z, 100, -3}, {PH_SIL, 90, 0},
    {PH_AH, 105, 2}, {PH_W, 70, 1}, {PH_EH, 185, -1}, {PH_T, 75, -4},
    {PH_SIL, 130, 0}, {PH_END, 0, 0}
};

static const speech_unit_t phrase_blast[] = {
    {PH_B, 70, 2}, {PH_L, 75, 1}, {PH_AE, 165, 0}, {PH_S, 80, -1},
    {PH_T, 65, -2}, {PH_SIL, 80, 0}, {PH_B, 70, 1}, {PH_IH, 135, 2}, {PH_G, 65, 0},
    {PH_IH, 145, -1}, {PH_N, 80, -3}, {PH_Z, 105, -5},
    {PH_SIL, 130, 0}, {PH_END, 0, 0}
};

static const speech_unit_t phrase_bazooka[] = {
    {PH_B, 70, 4}, {PH_AH, 145, 2}, {PH_Z, 85, 1}, {PH_UW, 210, 0},
    {PH_K, 70, -1}, {PH_AH, 195, -6},
    {PH_SIL, 160, 0}, {PH_END, 0, 0}
};

static const speech_unit_t phrase_game_over[] = {
    {PH_H, 75, 2}, {PH_AH, 160, 1}, {PH_N, 85, 0}, {PH_T, 65, -1},
    {PH_ER, 175, -2}, {PH_SIL, 85, 0},
    {PH_D, 70, 0}, {PH_AA, 145, -1}, {PH_W, 70, -2}, {PH_N, 150, -6},
    {PH_SIL, 160, 0}, {PH_END, 0, 0}
};

static const speech_unit_t phrase_more_demons[] = {
    {PH_M, 95, 2}, {PH_OH, 145, 3}, {PH_R, 105, 0}, {PH_SIL, 70, 0},
    {PH_D, 70, 1}, {PH_IY, 150, 1}, {PH_M, 90, 0}, {PH_AH, 130, -1},
    {PH_N, 80, -3}, {PH_Z, 105, -5}, {PH_SIL, 130, 0}, {PH_END, 0, 0}
};

static const speech_unit_t phrase_burn_in_hell[] = {
    {PH_B, 70, 2}, {PH_ER, 180, 1}, {PH_N, 110, -2}, {PH_SIL, 80, 0},
    {PH_IH, 145, 1}, {PH_N, 85, -1}, {PH_SIL, 70, 0},
    {PH_H, 75, -1}, {PH_EH, 195, -3}, {PH_L, 135, -6},
    {PH_SIL, 150, 0}, {PH_END, 0, 0}
};

/* Four bars of an original A-minor tracker melody; -1 denotes a rest. */
static const int8_t music_lead_notes[64] = {
    76, -1, 76, 79, 81, -1, 84, 83, 81, 79, 76, -1, 72, 76, 79, -1,
    81, -1, 81, 79, 77, -1, 84, 81, 79, 77, 76, 72, 77, -1, 81, 84,
    83, -1, 86, 83, 79, 81, 83, -1, 86, 88, 86, 83, 79, -1, 74, 79,
    80, -1, 83, 80, 76, 78, 80, -1, 83, 85, 83, 80, 76, 74, 71, -1
};

/* Palm-muted power-chord roots under the chip lead. */
static const int8_t music_riff_notes[64] = {
    45, -1, 45, 45, 48, 45, -1, 43, 45, 45, 52, 50, 48, 45, 43, -1,
    41, -1, 41, 41, 48, 41, 43, 45, 41, 41, 53, 48, 46, 45, 43, -1,
    43, -1, 43, 43, 50, 43, 45, 46, 43, 43, 55, 53, 50, 48, 46, -1,
    40, 40, 47, 40, 43, 40, 47, 48, 40, 40, 52, 50, 48, 47, 43, -1
};

/* Five-by-seven polygon glyphs: A-Z, 0-9, then !, :, -, /, and ?. */
static const uint8_t font_rows[41][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {31,4,4,4,4,4,31},      {7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30},   {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4},     {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31},     {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2},     {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14},{31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14},{14,17,17,15,1,1,14},
    {4,4,4,4,4,0,4},       {0,4,4,0,4,4,0},
    {0,0,0,31,0,0,0},      {1,2,4,8,16,0,0},
    {14,17,1,2,4,0,4}
};

static vec3_t v3(float x, float y, float z) {
    vec3_t value = {x, y, z};
    return value;
}

static vec3_t vadd(vec3_t a, vec3_t b) {
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static vec3_t vsub(vec3_t a, vec3_t b) {
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static vec3_t vscale(vec3_t a, float scale) {
    return v3(a.x * scale, a.y * scale, a.z * scale);
}

static float vdot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static vec3_t vcross(vec3_t a, vec3_t b) {
    return v3(a.y * b.z - a.z * b.y,
              a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}

static float vlength(vec3_t value) {
    return sqrtf(vdot(value, value));
}

static vec3_t vnormalize(vec3_t value) {
    float length = vlength(value);
    if(length < 0.0001f)
        return v3(0.0f, 0.0f, -1.0f);
    return vscale(value, 1.0f / length);
}

static float clampf(float value, float minimum, float maximum) {
    if(value < minimum)
        return minimum;
    if(value > maximum)
        return maximum;
    return value;
}

static uint32_t rng_next(void) {
    uint32_t value = rng_state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    rng_state = value;
    return value;
}

static float rng_float(void) {
    return (float)(rng_next() & 0x00ffffffu) / 16777215.0f;
}

static float rng_signed(void) {
    return rng_float() * 2.0f - 1.0f;
}

static uint32_t color_scale(uint32_t color, float amount) {
    unsigned a = (color >> 24) & 255u;
    unsigned r = (color >> 16) & 255u;
    unsigned g = (color >> 8) & 255u;
    unsigned b = color & 255u;
    r = (unsigned)clampf((float)r * amount, 0.0f, 255.0f);
    g = (unsigned)clampf((float)g * amount, 0.0f, 255.0f);
    b = (unsigned)clampf((float)b * amount, 0.0f, 255.0f);
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t color_alpha(uint32_t color, float alpha) {
    unsigned a = (unsigned)clampf(alpha * 255.0f, 0.0f, 255.0f);
    return (color & 0x00ffffffu) | (a << 24);
}

static void setup_camera(float time) {
    vec3_t target = v3(0.0f, 0.4f, -0.5f);
    float shake_x = 0.0f;
    float shake_y = 0.0f;

    if(screen_shake > 0.0f) {
        shake_x = fsin(time * 77.0f) * screen_shake;
        shake_y = fcos(time * 91.0f) * screen_shake * 0.55f;
    }

    if(game_mode == MODE_TITLE && VISUAL_REVISION >= 1) {
        float drift = fsin(time * 0.16f);
        target = v3(drift * 0.65f, 2.7f, -7.0f);
        camera_view.pos = v3(drift * 1.6f + shake_x,
                             6.15f + fcos(time * 0.13f) * 0.24f + shake_y,
                             17.2f);
    }
    else {
        camera_view.pos = v3(shake_x, 13.0f + shake_y, 21.5f);
    }
    camera_view.forward = vnormalize(vsub(target, camera_view.pos));
    camera_view.right = vnormalize(vcross(camera_view.forward, v3(0.0f, 1.0f, 0.0f)));
    camera_view.up = vnormalize(vcross(camera_view.right, camera_view.forward));
    if(screen_shake > 0.0f) {
        float roll = fsin(time * 63.0f) * screen_shake * 0.055f;
        float sine = fsin(roll);
        float cosine = fcos(roll);
        vec3_t old_right = camera_view.right;
        camera_view.right = vadd(vscale(camera_view.right, cosine),
                                 vscale(camera_view.up, sine));
        camera_view.up = vadd(vscale(camera_view.up, cosine),
                              vscale(old_right, -sine));
    }
    camera_view.focal = game_mode == MODE_TITLE && VISUAL_REVISION >= 1 ?
                        505.0f : 455.0f;
}

static int project_point(vec3_t point, screen_point_t *screen) {
    vec3_t relative = vsub(point, camera_view.pos);
    float depth = vdot(relative, camera_view.forward);
    float inverse_depth;

    if(depth <= 0.35f)
        return 0;

    inverse_depth = 1.0f / depth;
    screen->x = SCREEN_W * 0.5f + vdot(relative, camera_view.right) * camera_view.focal * inverse_depth;
    screen->y = SCREEN_H * 0.5f - vdot(relative, camera_view.up) * camera_view.focal * inverse_depth;
    screen->z = inverse_depth;
    return 1;
}

static void submit_vertex(float x, float y, float z, uint32_t color, int end) {
    pvr_vertex_t vertex;
    vertex.flags = end ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vertex.x = x;
    vertex.y = y;
    vertex.z = z;
    vertex.u = 0.0f;
    vertex.v = 0.0f;
    vertex.argb = color;
    vertex.oargb = 0;
    pvr_prim(&vertex, sizeof(vertex));
}

static void draw_tri_3d(vec3_t a, vec3_t b, vec3_t c,
                        uint32_t ca, uint32_t cb, uint32_t cc) {
    screen_point_t pa;
    screen_point_t pb;
    screen_point_t pc;
    if(!project_point(a, &pa) || !project_point(b, &pb) || !project_point(c, &pc))
        return;
    pvr_prim(active_header, sizeof(*active_header));
    submit_vertex(pa.x, pa.y, pa.z, ca, 0);
    submit_vertex(pb.x, pb.y, pb.z, cb, 0);
    submit_vertex(pc.x, pc.y, pc.z, cc, 1);
}

/* Vertex order is a,b,d,c so this is one four-vertex triangle strip. */
static void draw_quad_3d(vec3_t a, vec3_t b, vec3_t c, vec3_t d,
                         uint32_t ca, uint32_t cb, uint32_t cc, uint32_t cd) {
    screen_point_t point[4];
    if(!project_point(a, &point[0]) || !project_point(b, &point[1]) ||
       !project_point(c, &point[2]) || !project_point(d, &point[3]))
        return;
    pvr_prim(active_header, sizeof(*active_header));
    submit_vertex(point[0].x, point[0].y, point[0].z, ca, 0);
    submit_vertex(point[1].x, point[1].y, point[1].z, cb, 0);
    submit_vertex(point[3].x, point[3].y, point[3].z, cd, 0);
    submit_vertex(point[2].x, point[2].y, point[2].z, cc, 1);
}

static void draw_rect_2d(float x, float y, float width, float height,
                         float depth, uint32_t color) {
    pvr_prim(active_header, sizeof(*active_header));
    submit_vertex(x, y, depth, color, 0);
    submit_vertex(x + width, y, depth, color, 0);
    submit_vertex(x, y + height, depth, color, 0);
    submit_vertex(x + width, y + height, depth, color, 1);
}

static void draw_line_2d(float x1, float y1, float x2, float y2,
                         float width, float depth, uint32_t color) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float length = sqrtf(dx * dx + dy * dy);
    float nx;
    float ny;
    if(length < 0.01f)
        return;
    nx = -dy * width * 0.5f / length;
    ny = dx * width * 0.5f / length;
    pvr_prim(active_header, sizeof(*active_header));
    submit_vertex(x1 + nx, y1 + ny, depth, color, 0);
    submit_vertex(x1 - nx, y1 - ny, depth, color, 0);
    submit_vertex(x2 + nx, y2 + ny, depth, color, 0);
    submit_vertex(x2 - nx, y2 - ny, depth, color, 1);
}

static void draw_line_3d(vec3_t a, vec3_t b, float width, uint32_t color) {
    screen_point_t pa;
    screen_point_t pb;
    if(!project_point(a, &pa) || !project_point(b, &pb))
        return;
    draw_line_2d(pa.x, pa.y, pb.x, pb.y, width,
                 (pa.z + pb.z) * 0.5f, color);
}

static vec3_t yaw_point(vec3_t point, float yaw) {
    float sine = fsin(yaw);
    float cosine = fcos(yaw);
    return v3(point.x * cosine + point.z * sine,
              point.y,
              -point.x * sine + point.z * cosine);
}

static vec3_t rotate_xyz(vec3_t point, float pitch, float yaw, float roll) {
    float sx = fsin(pitch);
    float cx = fcos(pitch);
    float sy = fsin(yaw);
    float cy = fcos(yaw);
    float sz = fsin(roll);
    float cz = fcos(roll);
    vec3_t rotated;
    float x;
    float y;

    rotated.x = point.x;
    rotated.y = point.y * cx - point.z * sx;
    rotated.z = point.y * sx + point.z * cx;
    x = rotated.x * cy + rotated.z * sy;
    rotated.z = -rotated.x * sy + rotated.z * cy;
    rotated.x = x;
    x = rotated.x * cz - rotated.y * sz;
    y = rotated.x * sz + rotated.y * cz;
    rotated.x = x;
    rotated.y = y;
    return rotated;
}

static vec3_t plane_point(vec3_t center, float radius, float angle,
                          float pitch, float yaw, float roll) {
    return vadd(center, rotate_xyz(v3(fcos(angle) * radius,
                                       fsin(angle) * radius, 0.0f),
                                   pitch, yaw, roll));
}

static void draw_oriented_ring(vec3_t center, float radius, float width,
                               int segments, float pitch, float yaw, float roll,
                               uint32_t color) {
    int index;
    for(index = 0; index < segments; ++index) {
        float a0 = (float)index * TAU / (float)segments;
        float a1 = (float)(index + 1) * TAU / (float)segments;
        vec3_t p0 = plane_point(center, radius - width * 0.5f, a0,
                                pitch, yaw, roll);
        vec3_t p1 = plane_point(center, radius - width * 0.5f, a1,
                                pitch, yaw, roll);
        vec3_t p2 = plane_point(center, radius + width * 0.5f, a1,
                                pitch, yaw, roll);
        vec3_t p3 = plane_point(center, radius + width * 0.5f, a0,
                                pitch, yaw, roll);
        draw_quad_3d(p0, p1, p2, p3, color, color,
                     color_scale(color, 0.72f), color_scale(color, 0.72f));
    }
}

static void draw_box(vec3_t center, vec3_t half, float yaw, uint32_t color) {
    vec3_t points[8];
    static const uint8_t faces[6][4] = {
        {0,1,2,3}, {5,4,7,6}, {4,0,2,6},
        {1,5,7,3}, {3,2,6,7}, {4,5,1,0}
    };
    static const float light[6] = {0.85f,0.48f,0.64f,0.95f,1.08f,0.55f};
    int index;
    int face;

    for(index = 0; index < 8; ++index) {
        vec3_t local = v3((index & 1) ? half.x : -half.x,
                          (index & 4) ? half.y : -half.y,
                          (index & 2) ? half.z : -half.z);
        points[index] = vadd(center, yaw_point(local, yaw));
    }
    for(face = 0; face < 6; ++face) {
        uint32_t shaded = color_scale(color, light[face]);
        draw_quad_3d(points[faces[face][0]], points[faces[face][1]],
                     points[faces[face][2]], points[faces[face][3]],
                     shaded, shaded, shaded, shaded);
    }
}

static void draw_octahedron(vec3_t center, float radius, float height,
                            uint32_t color) {
    vec3_t top = vadd(center, v3(0.0f, height, 0.0f));
    vec3_t bottom = vadd(center, v3(0.0f, -height, 0.0f));
    vec3_t ring[4];
    int index;
    for(index = 0; index < 4; ++index) {
        float angle = (float)index * PI * 0.5f;
        ring[index] = vadd(center, v3(fcos(angle) * radius, 0.0f,
                                      fsin(angle) * radius));
    }
    for(index = 0; index < 4; ++index) {
        int next = (index + 1) & 3;
        draw_tri_3d(top, ring[index], ring[next],
                    color_scale(color, 1.25f), color,
                    color_scale(color, 0.72f));
        draw_tri_3d(bottom, ring[next], ring[index],
                    color_scale(color, 0.45f), color_scale(color, 0.70f), color);
    }
}

static void draw_cone(vec3_t base, float height, float radius, int sides,
                      uint32_t color) {
    vec3_t top = vadd(base, v3(0.0f, height, 0.0f));
    int index;
    for(index = 0; index < sides; ++index) {
        float a0 = (float)index * TAU / (float)sides;
        float a1 = (float)(index + 1) * TAU / (float)sides;
        vec3_t p0 = vadd(base, v3(fcos(a0) * radius, 0.0f, fsin(a0) * radius));
        vec3_t p1 = vadd(base, v3(fcos(a1) * radius, 0.0f, fsin(a1) * radius));
        float shade = 0.58f + 0.45f * ((float)(index & 3) / 3.0f);
        draw_tri_3d(top, p0, p1, color_scale(color, 1.18f),
                    color_scale(color, shade), color_scale(color, shade * 0.82f));
    }
}

static void draw_ring(vec3_t center, float radius, float width, int segments,
                      uint32_t color) {
    int index;
    for(index = 0; index < segments; ++index) {
        float a0 = (float)index * TAU / (float)segments;
        float a1 = (float)(index + 1) * TAU / (float)segments;
        float inner = radius - width * 0.5f;
        float outer = radius + width * 0.5f;
        vec3_t p0 = vadd(center, v3(fcos(a0) * inner, 0.0f, fsin(a0) * inner));
        vec3_t p1 = vadd(center, v3(fcos(a1) * inner, 0.0f, fsin(a1) * inner));
        vec3_t p2 = vadd(center, v3(fcos(a1) * outer, 0.0f, fsin(a1) * outer));
        vec3_t p3 = vadd(center, v3(fcos(a0) * outer, 0.0f, fsin(a0) * outer));
        draw_quad_3d(p0, p1, p2, p3, color, color, color, color);
    }
}

static void draw_ground_strip(vec3_t a, vec3_t b, float width, uint32_t color) {
    vec3_t direction = vnormalize(vsub(b, a));
    vec3_t side = v3(-direction.z * width * 0.5f, 0.0f,
                     direction.x * width * 0.5f);
    draw_quad_3d(vadd(a, side), vsub(a, side), vsub(b, side), vadd(b, side),
                 color, color, color, color);
}

static int glyph_index(char character) {
    if(character >= 'A' && character <= 'Z')
        return character - 'A';
    if(character >= '0' && character <= '9')
        return 26 + character - '0';
    if(character == '!') return 36;
    if(character == ':') return 37;
    if(character == '-') return 38;
    if(character == '/') return 39;
    if(character == '?') return 40;
    return -1;
}

static float text_width(const char *text, float scale) {
    size_t length = strlen(text);
    if(length == 0)
        return 0.0f;
    return ((float)length * 6.0f - 1.0f) * scale;
}

static void draw_text(float x, float y, float scale, const char *text,
                      uint32_t color) {
    size_t character;
    for(character = 0; text[character]; ++character) {
        int glyph = glyph_index(text[character]);
        int row;
        if(glyph < 0)
            continue;
        for(row = 0; row < 7; ++row) {
            int column;
            for(column = 0; column < 5; ++column) {
                if(font_rows[glyph][row] & (1 << (4 - column))) {
                    draw_rect_2d(x + ((float)character * 6.0f + (float)column) * scale,
                                 y + (float)row * scale,
                                 scale + 0.25f, scale + 0.25f,
                                 80.0f, color);
                }
            }
        }
    }
}

static void draw_text_centered(float y, float scale, const char *text,
                               uint32_t color) {
    draw_text((SCREEN_W - text_width(text, scale)) * 0.5f,
              y, scale, text, color);
}

static void spawn_particle(vec3_t position, vec3_t velocity, float life,
                           float size, uint32_t color) {
    int index;
    for(index = 0; index < MAX_PARTICLES; ++index) {
        if(!particles[index].active) {
            particles[index].active = 1;
            particles[index].pos = position;
            particles[index].vel = velocity;
            particles[index].life = life;
            particles[index].max_life = life;
            particles[index].size = size;
            particles[index].color = color;
            return;
        }
    }
}

static void particle_burst(vec3_t position, int count, float speed,
                           uint32_t color) {
    int index;
    for(index = 0; index < count; ++index) {
        vec3_t velocity = v3(rng_signed() * speed,
                             (0.25f + rng_float()) * speed,
                             rng_signed() * speed);
        spawn_particle(position, velocity, 0.35f + rng_float() * 0.65f,
                       0.12f + rng_float() * 0.20f,
                       (index & 1) ? color : COL_GOLD);
    }
}

static void spawn_shockwave(vec3_t position, float radius, float lifetime,
                            uint32_t color) {
    int index;
    for(index = 0; index < MAX_SHOCKWAVES; ++index) {
        if(!shockwaves[index].active) {
            shockwaves[index].active = 1;
            shockwaves[index].pos = position;
            shockwaves[index].pos.y = 0.09f;
            shockwaves[index].age = 0.0f;
            shockwaves[index].max_age = lifetime;
            shockwaves[index].max_radius = radius;
            shockwaves[index].color = color;
            return;
        }
    }
}

static float hash_signed(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return (float)(value & 0xffffu) / 32767.5f - 1.0f;
}

static void spawn_lightning(vec3_t target, float height, float lifetime,
                            uint32_t color) {
    int index;
    for(index = 0; index < MAX_LIGHTNING; ++index) {
        if(!lightning_bolts[index].active) {
            lightning_bolts[index].active = 1;
            lightning_bolts[index].target = target;
            lightning_bolts[index].age = 0.0f;
            lightning_bolts[index].max_age = lifetime;
            lightning_bolts[index].height = height;
            lightning_bolts[index].seed = rng_next();
            lightning_bolts[index].color = color;
            return;
        }
    }
}

static void play_sfx(sfxhnd_t sound, int volume, float world_x) {
    int pan;
    if(sound == SFXHND_INVALID)
        return;
    pan = (int)clampf(128.0f + world_x * 6.0f, 16.0f, 240.0f);
    snd_sfx_play(sound, volume, pan);
}

static void speak_phrase(sfxhnd_t speech) {
    if(speech != SFXHND_INVALID) {
        snd_sfx_play(speech, 245, 128);
        voice_cooldown = 1.45f;
    }
}

static void try_speak_phrase(sfxhnd_t speech) {
    if(voice_cooldown <= 0.0f)
        speak_phrase(speech);
}

static void resonator_configure(resonator_t *resonator, float frequency,
                                float bandwidth, float sample_rate) {
    float radius = expf(-PI * bandwidth / sample_rate);
    resonator->coefficient = 2.0f * radius *
                             fcos(TAU * frequency / sample_rate);
    resonator->radius_squared = radius * radius;
    resonator->gain = (1.0f - radius) * 0.82f;
}

static float resonator_process(resonator_t *resonator, float input) {
    float output = input * resonator->gain +
                   resonator->coefficient * resonator->previous_1 -
                   resonator->radius_squared * resonator->previous_2;
    resonator->previous_2 = resonator->previous_1;
    resonator->previous_1 = output;
    return output;
}

static float triangle_wave(float phase) {
    return 1.0f - 4.0f * fabsf(phase - 0.5f);
}

static sfxhnd_t synth_speech(const char *name,
                             const speech_unit_t *phrase) {
    const int sample_rate = 11025;
    const int echo_delay = 276; /* 25 ms: a cheap infernal double-track. */
    resonator_t resonators[3] = {{0}};
    int total_samples = 0;
    int padded_samples;
    int sample_cursor = 0;
    int unit_index;
    int16_t *buffer;
    float voice_phase = 0.0f;
    float sub_phase = 0.0f;
    float wobble_phase = 0.0f;
    float growl_phase = 0.0f;
    float previous_noise = 0.0f;
    float peak = 0.0f;
    sfxhnd_t handle;

    for(unit_index = 0; phrase[unit_index].phoneme != PH_END; ++unit_index) {
        int unit_samples = phrase[unit_index].duration_ms * sample_rate / 1000;
        if(unit_samples > 65520 - total_samples) {
            printf("Demon Bazooka: speech phrase '%s' exceeds AICA SFX limit\n",
                   name);
            return SFXHND_INVALID;
        }
        total_samples += unit_samples;
    }

    padded_samples = (total_samples + 15) & ~15;
    buffer = calloc((size_t)padded_samples, sizeof(*buffer));
    if(!buffer)
        return SFXHND_INVALID;

    for(unit_index = 0; phrase[unit_index].phoneme != PH_END; ++unit_index) {
        const speech_unit_t *unit = &phrase[unit_index];
        const speech_phoneme_t *phoneme = &speech_phonemes[unit->phoneme];
        int unit_samples = unit->duration_ms * sample_rate / 1000;
        int local_sample;
        int formant;

        for(formant = 0; formant < 3; ++formant) {
            resonator_configure(&resonators[formant],
                                phoneme->formant[formant],
                                phoneme->bandwidth * (1.0f + (float)formant * 0.62f),
                                (float)sample_rate);
        }

        for(local_sample = 0; local_sample < unit_samples; ++local_sample) {
            float progress = (float)local_sample / (float)unit_samples;
            float phrase_progress = (float)sample_cursor / (float)total_samples;
            float white_noise = rng_signed();
            float high_noise = white_noise - previous_noise * 0.86f;
            float pitch_wobble;
            float pitch;
            float glottal;
            float excitation;
            float envelope = 1.0f;
            float filtered;
            float sample;
            float delayed = 0.0f;
            float absolute_sample;
            int quantized;

            previous_noise = white_noise;
            wobble_phase += 4.6f / (float)sample_rate;
            if(wobble_phase >= 1.0f)
                wobble_phase -= 1.0f;
            growl_phase += 29.0f / (float)sample_rate;
            if(growl_phase >= 1.0f)
                growl_phase -= 1.0f;
            pitch_wobble = triangle_wave(wobble_phase) * 1.8f;
            pitch = 71.0f + (float)unit->pitch_offset + pitch_wobble -
                    phrase_progress * 2.5f;

            voice_phase += pitch / (float)sample_rate;
            sub_phase += pitch * 0.501f / (float)sample_rate;
            if(voice_phase >= 1.0f)
                voice_phase -= 1.0f;
            if(sub_phase >= 1.0f)
                sub_phase -= 1.0f;

            glottal = (1.0f - voice_phase * 2.0f) * 0.72f +
                      triangle_wave(sub_phase) * 0.28f;
            excitation = glottal * phoneme->voiced +
                         high_noise * phoneme->noise;

            if(phoneme->kind == SPEECH_PLOSIVE) {
                if(progress < 0.48f) {
                    excitation = 0.0f;
                }
                else {
                    float burst = 1.0f - (progress - 0.48f) / 0.52f;
                    excitation = (high_noise * phoneme->noise +
                                  glottal * phoneme->voiced) * burst * burst;
                }
            }
            else if(phoneme->kind == SPEECH_SILENCE) {
                excitation = 0.0f;
            }

            if(progress < 0.055f)
                envelope = progress / 0.055f;
            else if(progress > 0.93f)
                envelope = (1.0f - progress) / 0.07f;

            filtered = resonator_process(&resonators[0], excitation) * 1.00f +
                       resonator_process(&resonators[1], excitation) * 0.58f +
                       resonator_process(&resonators[2], excitation) * 0.34f;
            if(phoneme->kind == SPEECH_FRICATIVE ||
               phoneme->kind == SPEECH_PLOSIVE) {
                filtered += high_noise * phoneme->noise * 0.16f;
            }

            sample = filtered * phoneme->amplitude * envelope * 1.72f;
            sample += triangle_wave(growl_phase) * phoneme->voiced *
                      envelope * 0.045f;
            sample *= 0.88f + (growl_phase < 0.48f ? 0.14f : -0.06f);
            if(sample_cursor < 96)
                sample *= (float)sample_cursor / 96.0f;
            if(total_samples - sample_cursor < 128)
                sample *= (float)(total_samples - sample_cursor) / 128.0f;

            /* Soft saturation and coarse quantization make it sound arcadey. */
            sample = sample / (1.0f + fabsf(sample) * 0.72f);
            if(sample_cursor >= echo_delay)
                delayed = (float)buffer[sample_cursor - echo_delay] / 32768.0f;
            sample = sample * 0.86f + delayed * 0.22f;
            quantized = (int)(clampf(sample, -1.0f, 1.0f) * 112.0f);
            sample = (float)quantized / 112.0f;
            sample = clampf(sample, -1.0f, 1.0f);
            absolute_sample = fabsf(sample);
            if(absolute_sample > peak)
                peak = absolute_sample;
            buffer[sample_cursor++] = (int16_t)(sample * 30000.0f);
        }
    }

    handle = snd_sfx_load_raw_buf((char *)buffer,
                                  (size_t)padded_samples * sizeof(*buffer),
                                  (uint32_t)sample_rate, 16, 1);
    printf("Demon Bazooka: synthesized speech '%s' (%d samples, peak %.2f)\n",
           name, total_samples, (double)peak);
    free(buffer);
    return handle;
}

static sfxhnd_t synth_sound(int kind, float seconds) {
    const int rate = 22050;
    int samples = (int)(seconds * (float)rate);
    int16_t *buffer;
    int index;
    sfxhnd_t handle;

    samples = (samples + 15) & ~15;
    buffer = malloc((size_t)samples * sizeof(*buffer));
    if(!buffer)
        return SFXHND_INVALID;

    for(index = 0; index < samples; ++index) {
        float t = (float)index / (float)rate;
        float progress = (float)index / (float)samples;
        float envelope = 1.0f - progress;
        float sample = 0.0f;
        rng_state = rng_state * 1664525u + 1013904223u;
        if(kind == 0) {
            float frequency = 360.0f - progress * 250.0f;
            sample = fsin(t * TAU * frequency) * 0.60f + rng_signed() * 0.18f;
            sample *= envelope * envelope;
        }
        else if(kind == 1) {
            sample = rng_signed() * 0.62f + fsin(t * TAU * 58.0f) * 0.38f;
            sample *= envelope * envelope;
        }
        else if(kind == 2) {
            float frequency = 80.0f + progress * 760.0f;
            sample = fsin(t * TAU * frequency) * envelope;
        }
        else {
            float grow = progress < 0.18f ? progress / 0.18f : envelope;
            sample = (fsin(t * TAU * 43.0f) * 0.52f +
                      fsin(t * TAU * 67.0f) * 0.30f +
                      rng_signed() * 0.16f) * grow;
        }
        sample = clampf(sample, -1.0f, 1.0f);
        buffer[index] = (int16_t)(sample * 28000.0f);
    }

    handle = snd_sfx_load_raw_buf((char *)buffer,
                                  (size_t)samples * sizeof(*buffer),
                                  rate, 16, 1);
    free(buffer);
    return handle;
}

static float midi_frequency(int note) {
    return 440.0f * powf(2.0f, ((float)note - 69.0f) / 12.0f);
}

static float pulse_wave(float phase, float duty) {
    return phase < duty ? 1.0f : -1.0f;
}

static void advance_phase(float *phase, float frequency, float sample_rate) {
    *phase += frequency / sample_rate;
    if(*phase >= 1.0f)
        *phase -= (float)(int)*phase;
}

static float tracker_envelope(float progress, float gate) {
    const float attack = 0.035f;
    const float release = 0.14f;
    if(progress >= gate)
        return 0.0f;
    if(progress < attack)
        return progress / attack;
    if(progress > gate - release)
        return (gate - progress) / release;
    return 1.0f;
}

static sfxhnd_t synth_music(int gameplay) {
    const int sample_rate = 11025;
    const int step_samples = gameplay ? 899 : 1020;
    const int total_steps = 64;
    const int total_samples = step_samples * total_steps;
    const int chord_roots[4] = {57, 53, 55, 52}; /* Am, F, G, E */
    const int minor_intervals[4] = {0, 3, 7, 12};
    const int major_intervals[4] = {0, 4, 7, 12};
    float note_frequencies[128];
    int16_t *buffer = malloc((size_t)total_samples * sizeof(*buffer));
    float lead_phase = 0.0f;
    float arpeggio_phase = 0.0f;
    float bass_phase = 0.0f;
    float riff_root_phase = 0.0f;
    float riff_fifth_phase = 0.0f;
    float riff_octave_phase = 0.0f;
    float kick_phase = 0.0f;
    float vibrato_phase = 0.0f;
    float previous_noise = 0.0f;
    float peak = 0.0f;
    int sample_index;
    int note_index;
    sfxhnd_t handle;

    if(!buffer)
        return SFXHND_INVALID;

    for(note_index = 0; note_index < 128; ++note_index)
        note_frequencies[note_index] = midi_frequency(note_index);

    for(sample_index = 0; sample_index < total_samples; ++sample_index) {
        int step = sample_index / step_samples;
        int local_sample = sample_index - step * step_samples;
        int bar = step / 16;
        int bar_step = step & 15;
        int lead_note = (int)music_lead_notes[step];
        int riff_note = (int)music_riff_notes[step];
        const int *intervals = bar == 0 ? minor_intervals : major_intervals;
        int arpeggio_note = chord_roots[bar] + intervals[step & 3];
        int bass_note = chord_roots[bar] - 12;
        float progress = (float)local_sample / (float)step_samples;
        float white_noise = rng_signed();
        float high_noise = white_noise - previous_noise * 0.82f;
        float lead = 0.0f;
        float arpeggio;
        float bass;
        float guitar = 0.0f;
        float kick = 0.0f;
        float snare = 0.0f;
        float hat = 0.0f;
        float crash = 0.0f;
        float sample;
        float absolute_sample;
        int quantized;
        int kick_trigger = gameplay ?
                           (((bar_step & 1) == 0) || (bar_step & 3) == 3) :
                           (((bar_step & 1) == 0) || bar_step == 3 ||
                            bar_step == 7 || bar_step == 11);
        int snare_trigger = bar_step == 4 || bar_step == 12;
        int hat_trigger = 1;

        previous_noise = white_noise;
        if((bar_step & 7) == 6)
            bass_note += 12;

        if(local_sample == 0) {
            arpeggio_phase = 0.0f;
            if(lead_note >= 0)
                lead_phase = 0.0f;
            if(riff_note >= 0) {
                riff_root_phase = 0.0f;
                riff_fifth_phase = 0.0f;
                riff_octave_phase = 0.0f;
            }
            if((bar_step & 3) == 0)
                bass_phase = 0.0f;
            if(kick_trigger)
                kick_phase = 0.0f;
        }

        advance_phase(&vibrato_phase, 5.8f, (float)sample_rate);
        if(lead_note >= 0) {
            float vibrato = 1.0f + triangle_wave(vibrato_phase) * 0.0065f;
            float lead_frequency = note_frequencies[lead_note] * vibrato;
            advance_phase(&lead_phase, lead_frequency, (float)sample_rate);
            lead = pulse_wave(lead_phase, (bar & 1) ? 0.25f : 0.18f) *
                   tracker_envelope(progress, 0.80f) *
                   (gameplay ? 0.24f : 0.27f);
        }

        advance_phase(&arpeggio_phase, note_frequencies[arpeggio_note],
                      (float)sample_rate);
        arpeggio = pulse_wave(arpeggio_phase, 0.125f) *
                     tracker_envelope(progress, 0.58f) *
                     (gameplay ? 0.13f : 0.16f);

        advance_phase(&bass_phase, note_frequencies[bass_note],
                      (float)sample_rate);
        bass = triangle_wave(bass_phase) *
               tracker_envelope(progress, 0.92f) *
               (gameplay ? 0.31f : 0.25f);

        if(riff_note >= 0) {
            float raw_guitar;
            float riff_envelope = tracker_envelope(progress,
                                                    (bar_step & 1) ? 0.48f : 0.66f);
            advance_phase(&riff_root_phase, note_frequencies[riff_note],
                          (float)sample_rate);
            advance_phase(&riff_fifth_phase, note_frequencies[riff_note + 7],
                          (float)sample_rate);
            advance_phase(&riff_octave_phase, note_frequencies[riff_note + 12],
                          (float)sample_rate);
            raw_guitar = pulse_wave(riff_root_phase, 0.46f) * 0.52f +
                         pulse_wave(riff_fifth_phase, 0.43f) * 0.31f +
                         pulse_wave(riff_octave_phase, 0.49f) * 0.17f;
            guitar = clampf(raw_guitar * (gameplay ? 4.6f : 3.4f),
                             -1.0f, 1.0f) * riff_envelope *
                     (gameplay ? 0.43f : 0.31f);
        }

        if(kick_trigger) {
            float kick_envelope = 1.0f - progress;
            float kick_frequency = 145.0f - progress * 104.0f;
            advance_phase(&kick_phase, kick_frequency, (float)sample_rate);
            kick = fsin(kick_phase * TAU) * kick_envelope * kick_envelope *
                   (gameplay ? 0.50f : 0.40f);
        }

        if(snare_trigger) {
            float snare_envelope = 1.0f - progress;
            snare = (high_noise * 0.78f +
                     pulse_wave(bass_phase, 0.5f) * 0.22f) *
                    snare_envelope * snare_envelope *
                    (gameplay ? 0.34f : 0.27f);
        }

        if(hat_trigger && progress < 0.18f) {
            float hat_envelope = 1.0f - progress / 0.18f;
            hat = high_noise * hat_envelope * hat_envelope *
                  ((bar_step & 1) ? 0.055f :
                   (bar_step == 14 ? 0.17f : 0.10f));
        }

        if(bar_step == 0) {
            float crash_envelope = 1.0f - progress;
            crash = high_noise * crash_envelope * crash_envelope * 0.16f;
        }

        sample = lead + arpeggio + bass + guitar + kick + snare + hat + crash;
        sample = clampf(sample * (gameplay ? 1.62f : 1.45f), -1.3f, 1.3f);
        sample = sample / (1.0f + fabsf(sample) * 0.38f);
        quantized = (int)(clampf(sample, -1.0f, 1.0f) * 88.0f);
        sample = (float)quantized / 88.0f;
        absolute_sample = fabsf(sample);
        if(absolute_sample > peak)
            peak = absolute_sample;
        buffer[sample_index] = (int16_t)(sample * 28500.0f);
    }

    handle = snd_sfx_load_raw_buf((char *)buffer,
                                  (size_t)total_samples * sizeof(*buffer),
                                  (uint32_t)sample_rate, 16, 1);
    printf("Demon Bazooka: synthesized %s chip-metal "
           "(%d samples, %d BPM, peak %.2f)\n",
           gameplay ? "gameplay" : "title",
           total_samples, gameplay ? 184 : 162, (double)peak);
    free(buffer);
    return handle;
}

static void start_music(sfxhnd_t track, int volume, const char *label) {
    sfx_play_data_t playback = {
        .chn = -1,
        .idx = track,
        .vol = volume,
        .pan = 128,
        .loop = 1,
        .freq = 11025,
        .loopstart = 0,
        .loopend = 0
    };
    if(track == SFXHND_INVALID)
        return;
    if(music_channel >= 0)
        snd_sfx_stop(music_channel);
    music_channel = snd_sfx_play_ex(&playback);
    if(music_channel < 0)
        printf("Demon Bazooka: unable to start %s music\n", label);
    else
        printf("Demon Bazooka: %s music looping on AICA channel %d\n",
               label, music_channel);
}

static void init_sound(void) {
    if(snd_init() < 0) {
        printf("Demon Bazooka: sound initialization failed; continuing silently\n");
        return;
    }
    sfx_fire = synth_sound(0, 0.16f);
    sfx_boom = synth_sound(1, 0.36f);
    sfx_dash = synth_sound(2, 0.20f);
    sfx_barrage = synth_sound(3, 1.15f);
    sfx_music = synth_music(0);
    sfx_music_gameplay = synth_music(1);
    speech_welcome = synth_speech("DEMONS AWAIT", phrase_welcome);
    speech_blast = synth_speech("BLAST BEGINS", phrase_blast);
    speech_bazooka = synth_speech("BAZOOKA", phrase_bazooka);
    speech_game_over = synth_speech("HUNTER DOWN", phrase_game_over);
    speech_more_demons = synth_speech("MORE DEMONS", phrase_more_demons);
    speech_burn_in_hell = synth_speech("BURN IN HELL", phrase_burn_in_hell);
    printf("Demon Bazooka: procedural PCM ready (%s)\n",
           (sfx_fire && sfx_boom && sfx_dash && sfx_barrage && sfx_music &&
            sfx_music_gameplay &&
            speech_welcome && speech_blast && speech_bazooka &&
            speech_game_over && speech_more_demons && speech_burn_in_hell) ?
           "four effects, six speech lines, and two music loops" :
           "partial audio");
    start_music(sfx_music, 108, "title");
}

static void shutdown_sound(void) {
    snd_sfx_stop_all();
    if(sfx_fire) snd_sfx_unload(sfx_fire);
    if(sfx_boom) snd_sfx_unload(sfx_boom);
    if(sfx_dash) snd_sfx_unload(sfx_dash);
    if(sfx_barrage) snd_sfx_unload(sfx_barrage);
    if(sfx_music) snd_sfx_unload(sfx_music);
    if(sfx_music_gameplay) snd_sfx_unload(sfx_music_gameplay);
    if(speech_welcome) snd_sfx_unload(speech_welcome);
    if(speech_blast) snd_sfx_unload(speech_blast);
    if(speech_bazooka) snd_sfx_unload(speech_bazooka);
    if(speech_game_over) snd_sfx_unload(speech_game_over);
    if(speech_more_demons) snd_sfx_unload(speech_more_demons);
    if(speech_burn_in_hell) snd_sfx_unload(speech_burn_in_hell);
}

static void init_headers(void) {
    pvr_poly_cxt_t context;

    pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    context.gen.shading = PVR_SHADE_GOURAUD;
    context.depth.comparison = PVR_DEPTHCMP_GREATER;
    context.depth.write = true;
    pvr_poly_compile(&opaque_header, &context);

    pvr_poly_cxt_col(&context, PVR_LIST_TR_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    context.gen.shading = PVR_SHADE_GOURAUD;
    context.depth.comparison = PVR_DEPTHCMP_GREATER;
    context.depth.write = false;
    context.blend.src = PVR_BLEND_SRCALPHA;
    context.blend.dst = PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&translucent_header, &context);

    context.blend.src = PVR_BLEND_SRCALPHA;
    context.blend.dst = PVR_BLEND_ONE;
    pvr_poly_compile(&additive_header, &context);

    /* UI lives in the translucent list but always wins the depth test. This
       keeps lightning, particles, and volumetric geometry behind readable
       text without giving up their intensity elsewhere on screen. */
    pvr_poly_cxt_col(&context, PVR_LIST_TR_POLY);
    context.gen.culling = PVR_CULLING_NONE;
    context.gen.shading = PVR_SHADE_GOURAUD;
    context.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    context.depth.write = false;
    context.blend.src = PVR_BLEND_SRCALPHA;
    context.blend.dst = PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&ui_header, &context);
}

static int wave_target_for(int wave) {
    int target = 8 + wave * 3;
    if((wave % 5) == 0)
        target += 6;
    return target > 34 ? 34 : target;
}

static void reset_game(void) {
    memset(enemies, 0, sizeof(enemies));
    memset(bolts, 0, sizeof(bolts));
    memset(powerups, 0, sizeof(powerups));
    memset(particles, 0, sizeof(particles));
    memset(shockwaves, 0, sizeof(shockwaves));
    memset(lightning_bolts, 0, sizeof(lightning_bolts));
    player_pos = v3(0.0f, 0.0f, 4.0f);
    player_dir = v3(0.0f, 0.0f, -1.0f);
    player_vel = v3(0.0f, 0.0f, 0.0f);
    player_lives = MAX_LIVES;
    score = 0;
    combo = 1;
    wave_number = 1;
    wave_kills = 0;
    wave_target = wave_target_for(wave_number);
    wave_spawned = 0;
    kills = 0;
    barrage_charge = 0.0f;
    fire_cooldown = 0.0f;
    dash_cooldown = 0.0f;
    invulnerability = 0.0f;
    speed_boost_time = 0.0f;
    rapid_fire_time = 0.0f;
    triple_shot_time = 0.0f;
    powerup_banner_time = 0.0f;
    spawn_timer = 0.5f;
    wave_transition_time = 0.0f;
    game_time = 0.0f;
    mode_time = 0.0f;
    screen_shake = 0.0f;
    flash_amount = 0.0f;
    game_mode = MODE_PLAY;
    spawn_shockwave(player_pos, 4.0f, 0.65f, COL_RED);
    spawn_lightning(player_pos, 16.0f, 0.55f, COL_MAGENTA);
    start_music(sfx_music_gameplay, 158, "gameplay");
    speak_phrase(speech_blast);
    printf("Demon Bazooka: hunt initiated\n");
}

static void spawn_enemy(void) {
    int index;
    for(index = 0; index < MAX_ENEMIES; ++index) {
        if(!enemies[index].active) {
            float angle = rng_float() * TAU;
            unsigned roll = rng_next() % 100u;
            int kind = 0;
            int elite_chance = 19 + wave_number * 4;
            int boss_chance = wave_number >= 4 ? 3 + wave_number * 2 : 0;
            if(elite_chance > 64)
                elite_chance = 64;
            if(boss_chance > 30)
                boss_chance = 30;
            if((wave_number % 5) == 0 && wave_spawned == 0)
                kind = 2;
            else if((int)roll < boss_chance)
                kind = 2;
            else if(wave_number >= 2 && (int)roll < elite_chance)
                kind = 1;
            enemies[index].active = 1;
            enemies[index].kind = kind;
            enemies[index].hp = (kind == 0 ? 1 : (kind == 1 ? 3 : 6)) +
                                (kind > 0 ? wave_number / 5 : wave_number / 9);
            enemies[index].radius = kind == 0 ? 0.72f : (kind == 1 ? 1.0f : 1.35f);
            /* Spawn fully inside the arena dressing. Enemies previously began
               beyond the rim, so pylons and the raised edge could cover their
               silhouettes during the most important first second. */
            enemies[index].pos = v3(fcos(angle) * 11.15f, 0.0f,
                                    fsin(angle) * 11.15f);
            if(vlength(vsub(enemies[index].pos, player_pos)) < 6.0f) {
                angle += PI;
                enemies[index].pos = v3(fcos(angle) * 11.15f, 0.0f,
                                        fsin(angle) * 11.15f);
            }
            enemies[index].vel = v3(0.0f, 0.0f, 0.0f);
            enemies[index].phase = rng_float() * TAU;
            enemies[index].yaw = angle + PI;
            wave_spawned++;
            return;
        }
    }
}

static int launch_rocket(vec3_t direction) {
    int index;
    for(index = 0; index < MAX_BOLTS; ++index) {
        if(!bolts[index].active) {
            vec3_t muzzle = vadd(player_pos, vscale(direction, 1.28f));
            muzzle.y = 1.34f;
            bolts[index].active = 1;
            bolts[index].pos = muzzle;
            bolts[index].prev = vsub(muzzle, vscale(direction, 0.5f));
            bolts[index].vel = vscale(direction, 13.5f);
            bolts[index].life = 1.55f;
            spawn_particle(muzzle, v3(0.0f, 1.9f, 0.0f), 0.24f, 0.48f, COL_YELLOW);
            return 1;
        }
    }
    return 0;
}

static void fire_rocket(void) {
    int fired = launch_rocket(player_dir);

    if(fired && triple_shot_time > 0.0f) {
        vec3_t tangent = v3(-player_dir.z, 0.0f, player_dir.x);
        launch_rocket(vnormalize(vadd(player_dir, vscale(tangent, -0.27f))));
        launch_rocket(vnormalize(vadd(player_dir, vscale(tangent, 0.27f))));
    }
    if(fired) {
        play_sfx(sfx_fire, 205, player_pos.x);
        fire_cooldown = rapid_fire_time > 0.0f ? 0.115f : 0.265f;
    }
}

static void spawn_powerup(vec3_t position, int guaranteed) {
    int index;
    unsigned drop_roll = rng_next() % 100u;

    if(!guaranteed && drop_roll >= 12u)
        return;
    for(index = 0; index < MAX_POWERUPS; ++index) {
        if(!powerups[index].active) {
            powerups[index].active = 1;
            powerups[index].kind = (powerup_kind_t)(rng_next() % 3u);
            powerups[index].pos = position;
            powerups[index].pos.y = 0.18f;
            powerups[index].age = 0.0f;
            powerups[index].lifetime = 12.0f;
            powerups[index].phase = rng_float() * TAU;
            printf("Demon Bazooka: dropped powerup %d at %.1f, %.1f\n",
                   (int)powerups[index].kind,
                   (double)position.x, (double)position.z);
            return;
        }
    }
}

static void kill_enemy(enemy_t *enemy, int barrage) {
    int reward = enemy->kind == 0 ? 100 : (enemy->kind == 1 ? 350 : 900);
    vec3_t death_position = enemy->pos;
    particle_burst(vadd(enemy->pos, v3(0.0f, 1.0f, 0.0f)),
                   enemy->kind == 2 ? 42 : 18,
                   enemy->kind == 2 ? 5.8f : 4.0f,
                   enemy->kind == 0 ? COL_ORANGE : COL_MAGENTA);
    spawn_shockwave(enemy->pos, enemy->kind == 2 ? 3.5f : 1.7f,
                    0.42f, enemy->kind == 0 ? COL_RED : COL_MAGENTA);
    spawn_lightning(enemy->pos, enemy->kind == 2 ? 19.0f : 13.0f,
                    enemy->kind == 2 ? 0.65f : 0.34f,
                    enemy->kind == 0 ? COL_ORANGE : COL_MAGENTA);
    screen_shake = clampf(screen_shake + (enemy->kind == 2 ? 0.42f : 0.10f),
                          0.0f, 1.0f);
    if(!barrage)
        play_sfx(sfx_boom, enemy->kind == 2 ? 220 : 140, enemy->pos.x);
    score += reward * combo;
    if(score > high_score)
        high_score = score;
    combo++;
    if(combo > 13)
        combo = 13;
    if(!barrage) {
        barrage_charge = clampf(barrage_charge +
                                 (enemy->kind == 0 ? 12.0f : 22.0f),
                                 0.0f, 100.0f);
    }
    kills++;
    wave_kills++;
    if(enemy->kind == 2)
        try_speak_phrase(speech_burn_in_hell);
    enemy->active = 0;
    if(!barrage)
        spawn_powerup(death_position, enemy->kind == 2);
    if(wave_kills >= wave_target)
        begin_wave_transition();
}

static void explode_rocket(vec3_t impact) {
    int index;

    particle_burst(impact, 26, 6.2f, COL_ORANGE);
    spawn_shockwave(impact, 3.2f, 0.42f, COL_YELLOW);
    spawn_lightning(impact, 8.0f, 0.24f, COL_ORANGE);
    screen_shake = clampf(screen_shake + 0.24f, 0.0f, 1.0f);
    flash_amount = clampf(flash_amount + 0.16f, 0.0f, 1.0f);

    for(index = 0; index < MAX_ENEMIES; ++index) {
        enemy_t *enemy = &enemies[index];
        vec3_t target;
        vec3_t difference;
        float blast_radius;
        if(!enemy->active)
            continue;
        target = vadd(enemy->pos, v3(0.0f, 0.9f, 0.0f));
        difference = vsub(target, impact);
        blast_radius = 2.55f + enemy->radius;
        if(vdot(difference, difference) < blast_radius * blast_radius) {
            enemy->hp -= vdot(difference, difference) < 1.35f ? 3 : 1;
            if(enemy->hp <= 0)
                kill_enemy(enemy, 0);
            else
                particle_burst(target, 6, 3.0f, COL_RED);
        }
    }
}

static void unleash_bazooka_barrage(void) {
    int index;
    if(barrage_charge < 100.0f)
        return;
    barrage_charge = 0.0f;
    invulnerability = 1.4f;
    flash_amount = 1.0f;
    screen_shake = 0.85f;
    spawn_shockwave(player_pos, 21.0f, 1.15f, COL_MAGENTA);
    play_sfx(sfx_barrage, 255, 0.0f);
    speak_phrase(speech_bazooka);
    for(index = 0; index < MAX_ENEMIES; ++index) {
        if(enemies[index].active)
            kill_enemy(&enemies[index], 1);
    }
    for(index = 0; index < 14; ++index) {
        float angle = (float)index * TAU / 14.0f;
        float radius = 3.0f + (float)(index % 4) * 2.3f;
        spawn_lightning(vadd(player_pos,
                             v3(fcos(angle) * radius, 0.0f,
                                fsin(angle) * radius)),
                        15.0f + (float)(index & 3), 0.72f,
                        (index & 1) ? COL_MAGENTA : COL_ORANGE);
    }
    for(index = 0; index < 82; ++index) {
        float angle = rng_float() * TAU;
        float speed = 4.0f + rng_float() * 8.0f;
        spawn_particle(vadd(player_pos, v3(0.0f, 1.0f, 0.0f)),
                       v3(fcos(angle) * speed, 1.0f + rng_float() * 7.0f,
                          fsin(angle) * speed),
                       0.65f + rng_float() * 0.8f,
                       0.2f + rng_float() * 0.42f,
                       (index & 1) ? COL_MAGENTA : COL_ORANGE);
    }
    printf("Demon Bazooka: BAZOOKA BARRAGE at score %d\n", score);
}

static void update_particles(float dt) {
    int index;
    for(index = 0; index < MAX_PARTICLES; ++index) {
        if(particles[index].active) {
            particles[index].life -= dt;
            if(particles[index].life <= 0.0f) {
                particles[index].active = 0;
                continue;
            }
            particles[index].pos = vadd(particles[index].pos,
                                        vscale(particles[index].vel, dt));
            particles[index].vel.y -= 4.5f * dt;
            particles[index].vel = vscale(particles[index].vel, 1.0f - dt * 0.45f);
        }
    }
    for(index = 0; index < MAX_SHOCKWAVES; ++index) {
        if(shockwaves[index].active) {
            shockwaves[index].age += dt;
            if(shockwaves[index].age >= shockwaves[index].max_age)
                shockwaves[index].active = 0;
        }
    }
    for(index = 0; index < MAX_LIGHTNING; ++index) {
        if(lightning_bolts[index].active) {
            lightning_bolts[index].age += dt;
            if(lightning_bolts[index].age >= lightning_bolts[index].max_age)
                lightning_bolts[index].active = 0;
        }
    }
}

static void begin_wave_transition(void) {
    int index;
    if(wave_transition_time > 0.0f || game_mode != MODE_PLAY)
        return;

    wave_transition_time = WAVE_TRANSITION_SECONDS;
    player_vel = v3(0.0f, 0.0f, 0.0f);
    invulnerability = WAVE_TRANSITION_SECONDS + 0.8f;
    screen_shake = 0.92f;
    flash_amount = 1.0f;
    memset(bolts, 0, sizeof(bolts));

    for(index = 0; index < MAX_ENEMIES; ++index) {
        if(enemies[index].active) {
            particle_burst(vadd(enemies[index].pos, v3(0.0f, 1.0f, 0.0f)),
                           8, 4.2f, COL_MAGENTA);
            enemies[index].active = 0;
        }
    }
    for(index = 0; index < 12; ++index) {
        float angle = (float)index * TAU / 12.0f;
        vec3_t impact = v3(fcos(angle) * (4.0f + (float)(index & 3) * 2.1f),
                           0.0f,
                           fsin(angle) * (4.0f + (float)(index & 3) * 2.1f));
        spawn_lightning(impact, 15.0f + (float)(index % 4),
                        0.8f + (float)(index & 1) * 0.25f,
                        (index & 1) ? COL_MAGENTA : COL_ORANGE);
    }
    spawn_shockwave(player_pos, 24.0f, 1.45f, COL_GOLD);
    play_sfx(sfx_barrage, 205, 0.0f);
    printf("Demon Bazooka: wave %d cleared; opening wave %d\n",
           wave_number, wave_number + 1);
}

static void update_wave_transition(float dt) {
    int index;
#if defined(WAVE_TRANSITION_QA)
    /* Hold the ritual at peak aperture for screenshot-based visual QA. */
    wave_transition_time = WAVE_TRANSITION_SECONDS * 0.5f;
    update_particles(dt);
    return;
#endif
    wave_transition_time -= dt;
    screen_shake = clampf(screen_shake - dt * 0.22f, 0.18f, 1.0f);
    flash_amount = clampf(flash_amount - dt * 0.32f, 0.0f, 1.0f);

    if((rng_next() % 3u) == 0u) {
        float angle = rng_float() * TAU;
        float radius = 1.5f + rng_float() * 10.5f;
        spawn_particle(v3(fcos(angle) * radius, 0.2f,
                          fsin(angle) * radius),
                       v3(-fcos(angle) * 1.4f, 2.5f + rng_float() * 5.0f,
                          -fsin(angle) * 1.4f),
                       0.6f + rng_float() * 0.9f,
                       0.12f + rng_float() * 0.28f,
                       (rng_next() & 1u) ? COL_GOLD : COL_MAGENTA);
    }
    update_particles(dt);

    if(wave_transition_time > 0.0f)
        return;

    wave_transition_time = 0.0f;
    wave_number++;
    wave_kills = 0;
    wave_target = wave_target_for(wave_number);
    wave_spawned = 0;
    spawn_timer = 0.18f;
    mode_time = 0.0f;
    invulnerability = 1.25f;
    barrage_charge = clampf(barrage_charge + 18.0f, 0.0f, 100.0f);
    if((wave_number % 3) == 0 && player_lives < MAX_LIVES)
        player_lives++;
    screen_shake = 0.48f;
    flash_amount = 0.82f;
    spawn_shockwave(player_pos, 8.0f, 0.72f, COL_MAGENTA);
    for(index = 0; index < 4 + wave_number / 3 && index < 10; ++index)
        spawn_enemy();
    try_speak_phrase(speech_more_demons);
    play_sfx(sfx_dash, 215, 0.0f);
    printf("Demon Bazooka: wave %d begins; quota %d, opening pack %d\n",
           wave_number, wave_target, index);
}

static void damage_player(enemy_t *enemy) {
    vec3_t away;
    if(invulnerability > 0.0f)
        return;
    player_lives--;
    combo = 1;
    barrage_charge = clampf(barrage_charge - 18.0f, 0.0f, 100.0f);
    invulnerability = 1.45f;
    screen_shake = 0.62f;
    flash_amount = 0.75f;
    away = vnormalize(vsub(player_pos, enemy->pos));
    player_vel = vscale(away, 7.0f);
    particle_burst(vadd(player_pos, v3(0.0f, 1.0f, 0.0f)), 18, 5.5f, COL_RED);
    spawn_shockwave(player_pos, 3.4f, 0.48f, COL_RED);
    spawn_lightning(player_pos, 11.0f, 0.40f, COL_RED);
    play_sfx(sfx_boom, 230, player_pos.x);
    enemy->active = 0;
    if(player_lives <= 0) {
        game_mode = MODE_GAME_OVER;
        mode_time = 0.0f;
        start_music(sfx_music, 108, "title");
        speak_phrase(speech_game_over);
        if(score > high_score)
            high_score = score;
        printf("Demon Bazooka: hunter down, score %d, wave %d\n",
               score, wave_number);
    }
}

static cont_state_t *read_controller(void) {
    maple_device_t *controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if(!controller)
        return NULL;
    return (cont_state_t *)maple_dev_status(controller);
}

static void collect_powerup(powerup_t *powerup) {
    static const char *names[3] = {
        "SPEED BOOST", "RAPID FIRE", "TRIPLE BAZOOKA"
    };

    if(powerup->kind == POWERUP_SPEED)
        speed_boost_time = clampf(speed_boost_time + 8.0f, 8.0f, 16.0f);
    else if(powerup->kind == POWERUP_RAPID_FIRE)
        rapid_fire_time = clampf(rapid_fire_time + 8.0f, 8.0f, 16.0f);
    else
        triple_shot_time = clampf(triple_shot_time + 10.0f, 10.0f, 18.0f);

    powerup_banner_kind = powerup->kind;
    powerup_banner_time = 1.65f;
    score += 250;
    particle_burst(vadd(powerup->pos, v3(0.0f, 0.8f, 0.0f)),
                   24, 5.2f,
                   powerup->kind == POWERUP_SPEED ? COL_CYAN :
                   (powerup->kind == POWERUP_RAPID_FIRE ? COL_GOLD : COL_MAGENTA));
    spawn_shockwave(powerup->pos, 3.2f, 0.46f,
                    powerup->kind == POWERUP_SPEED ? COL_CYAN :
                    (powerup->kind == POWERUP_RAPID_FIRE ? COL_GOLD : COL_MAGENTA));
    play_sfx(sfx_dash, 220, player_pos.x);
    printf("Demon Bazooka: collected %s\n", names[powerup->kind]);
    powerup->active = 0;
}

static void update_powerups(float dt) {
    int index;
    for(index = 0; index < MAX_POWERUPS; ++index) {
        powerup_t *powerup = &powerups[index];
        vec3_t difference;
        if(!powerup->active)
            continue;
        powerup->age += dt;
        powerup->lifetime -= dt;
        powerup->phase += dt * 2.8f;
        if(powerup->lifetime <= 0.0f) {
            powerup->active = 0;
            continue;
        }
        difference = vsub(player_pos, powerup->pos);
        difference.y = 0.0f;
        if(vdot(difference, difference) < 1.44f)
            collect_powerup(powerup);
    }
}

static void update_game(float dt, const cont_state_t *state,
                        uint32_t pressed) {
    vec3_t input = v3(0.0f, 0.0f, 0.0f);
    float input_length;
    float arena_distance;
    int index;

    game_time += dt;
    fire_cooldown -= dt;
    dash_cooldown -= dt;
    invulnerability -= dt;
    speed_boost_time = clampf(speed_boost_time - dt, 0.0f, 18.0f);
    rapid_fire_time = clampf(rapid_fire_time - dt, 0.0f, 18.0f);
    triple_shot_time = clampf(triple_shot_time - dt, 0.0f, 20.0f);
    powerup_banner_time = clampf(powerup_banner_time - dt, 0.0f, 2.0f);
    spawn_timer -= dt;
    screen_shake = clampf(screen_shake - dt * 1.8f, 0.0f, 1.0f);
    flash_amount = clampf(flash_amount - dt * 1.5f, 0.0f, 1.0f);

    if(state) {
        input.x = (float)state->joyx / 128.0f;
        input.z = (float)state->joyy / 128.0f;
        if(state->buttons & CONT_DPAD_LEFT) input.x = -1.0f;
        if(state->buttons & CONT_DPAD_RIGHT) input.x = 1.0f;
        if(state->buttons & CONT_DPAD_UP) input.z = -1.0f;
        if(state->buttons & CONT_DPAD_DOWN) input.z = 1.0f;
    }
    input_length = sqrtf(input.x * input.x + input.z * input.z);
    if(input_length > 0.18f) {
        if(input_length > 1.0f)
            input = vscale(input, 1.0f / input_length);
        player_dir = vnormalize(input);
        player_dir.y = 0.0f;
        player_vel = vadd(vscale(player_vel, 0.80f),
                          vscale(input, speed_boost_time > 0.0f ? 1.48f : 1.10f));
    }
    else {
        player_vel = vscale(player_vel, 1.0f - dt * 7.5f);
    }

    if((pressed & CONT_B) && dash_cooldown <= 0.0f) {
        player_vel = vscale(player_dir, 15.0f);
        dash_cooldown = 0.92f;
        invulnerability = 0.33f;
        screen_shake = 0.22f;
        spawn_shockwave(player_pos, 2.1f, 0.36f, COL_PURPLE);
        play_sfx(sfx_dash, 185, player_pos.x);
    }
    if(state && (state->buttons & CONT_A) && fire_cooldown <= 0.0f)
        fire_rocket();
    if(pressed & CONT_X)
        unleash_bazooka_barrage();

    player_pos = vadd(player_pos, vscale(player_vel, dt));
    arena_distance = sqrtf(player_pos.x * player_pos.x + player_pos.z * player_pos.z);
    if(arena_distance > 11.4f) {
        float scale = 11.4f / arena_distance;
        player_pos.x *= scale;
        player_pos.z *= scale;
        player_vel = vscale(player_vel, 0.5f);
    }
    update_powerups(dt);

    if(vlength(player_vel) > 7.0f && (rng_next() & 1u)) {
        spawn_particle(vadd(player_pos, v3(rng_signed() * 0.3f, 0.45f,
                                           rng_signed() * 0.3f)),
                       v3(-player_vel.x * 0.18f, 1.2f, -player_vel.z * 0.18f),
                       0.28f, 0.20f, COL_MAGENTA);
    }

    if(spawn_timer <= 0.0f) {
        float cadence = 1.04f - (float)(wave_number - 1) * 0.075f -
                        (float)wave_kills * 0.006f;
        spawn_enemy();
        spawn_timer = clampf(cadence, 0.20f, 1.04f);
        if(wave_number >= 3 && (rng_next() % 6u) < (unsigned)(wave_number / 3))
            spawn_enemy();
        if(wave_number >= 7 && (rng_next() % 5u) == 0u)
            spawn_enemy();
    }
    if((rng_next() % 155u) == 0u) {
        float angle = rng_float() * TAU;
        float radius = 4.0f + rng_float() * 8.5f;
        spawn_lightning(v3(fcos(angle) * radius, 0.0f,
                           fsin(angle) * radius),
                        12.0f + rng_float() * 8.0f,
                        0.22f + rng_float() * 0.24f,
                        (rng_next() & 1u) ? COL_PURPLE : COL_RED);
    }

    for(index = 0; index < MAX_ENEMIES; ++index) {
        enemy_t *enemy = &enemies[index];
        vec3_t difference;
        float distance;
        float speed;
        if(!enemy->active)
            continue;
        enemy->phase += dt * (enemy->kind == 2 ? 2.1f : 4.0f);
        difference = vsub(player_pos, enemy->pos);
        distance = vlength(difference);
        speed = enemy->kind == 0 ? 1.85f : (enemy->kind == 1 ? 1.35f : 0.92f);
        speed += clampf((float)(wave_number - 1) * 0.11f +
                        game_time * 0.0015f, 0.0f, 1.85f);
        enemy->vel = vscale(vnormalize(difference), speed);
        if(enemy->kind == 1) {
            vec3_t tangent = v3(-enemy->vel.z, 0.0f, enemy->vel.x);
            enemy->vel = vadd(enemy->vel, vscale(tangent, fsin(enemy->phase) * 0.65f));
        }
        enemy->pos = vadd(enemy->pos, vscale(enemy->vel, dt));
        enemy->yaw = atan2f(enemy->vel.x, -enemy->vel.z);
        if(distance < enemy->radius + 0.62f)
            damage_player(enemy);
    }

    for(index = 0; index < MAX_BOLTS; ++index) {
        bolt_t *bolt = &bolts[index];
        int enemy_index;
        if(!bolt->active)
            continue;
        bolt->life -= dt;
        bolt->prev = bolt->pos;
        bolt->pos = vadd(bolt->pos, vscale(bolt->vel, dt));
        if(bolt->life <= 0.0f ||
           bolt->pos.x * bolt->pos.x + bolt->pos.z * bolt->pos.z > 280.0f) {
            bolt->active = 0;
            continue;
        }
        spawn_particle(bolt->prev,
                       v3(rng_signed() * 0.4f, 0.7f, rng_signed() * 0.4f),
                       0.18f, 0.13f, COL_ORANGE);
        for(enemy_index = 0; enemy_index < MAX_ENEMIES; ++enemy_index) {
            enemy_t *enemy = &enemies[enemy_index];
            vec3_t difference;
            float hit_radius;
            if(!enemy->active)
                continue;
            difference = vsub(bolt->pos,
                              vadd(enemy->pos, v3(0.0f, 0.9f, 0.0f)));
            hit_radius = enemy->radius + 0.34f;
            if(vdot(difference, difference) < hit_radius * hit_radius) {
                bolt->active = 0;
                explode_rocket(bolt->pos);
                break;
            }
        }
    }
    update_particles(dt);
}

static void update_title(float dt) {
    int index;
    screen_shake = 0.0f;
    if(game_mode == MODE_TITLE && !welcome_spoken && mode_time >= 0.45f) {
        speak_phrase(speech_welcome);
        welcome_spoken = 1;
        printf("Demon Bazooka: voice: DEMONS AWAIT\n");
    }
    for(index = 0; index < MAX_PARTICLES; ++index) {
        if(!particles[index].active && (rng_next() % 20u) == 0u) {
            float angle = rng_float() * TAU;
            float radius = 2.0f + rng_float() * 9.0f;
            spawn_particle(v3(fcos(angle) * radius, 0.2f,
                              fsin(angle) * radius),
                           v3(rng_signed() * 0.2f, 1.0f + rng_float() * 1.8f,
                              rng_signed() * 0.2f),
                           0.7f + rng_float() * 1.1f,
                           0.1f + rng_float() * 0.23f, COL_ORANGE);
            break;
        }
    }
    if((rng_next() % 92u) == 0u) {
        float angle = rng_float() * TAU;
        float radius = 3.0f + rng_float() * 9.0f;
        spawn_lightning(v3(fcos(angle) * radius, 0.0f,
                           fsin(angle) * radius),
                        14.0f + rng_float() * 7.0f,
                        0.32f + rng_float() * 0.22f,
                        (rng_next() & 1u) ? COL_MAGENTA : COL_ORANGE);
    }
    update_particles(dt);
}

static void draw_hell_engine(float time) {
    vec3_t center = v3(0.0f, 8.2f, -21.0f);
    float pulse = 1.0f + fsin(time * 2.7f) * 0.07f;
    float roll = time * 0.19f;
    vec3_t star[5];
    int index;

    draw_oriented_ring(center, 8.2f * pulse, 0.48f, 36,
                       0.10f, fsin(time * 0.23f) * 0.24f,
                       roll, 0xff570713u);
    draw_oriented_ring(center, 6.75f, 0.21f, 32,
                       0.42f, fcos(time * 0.17f) * 0.34f,
                       -roll * 1.35f, COL_CRIMSON);
    draw_oriented_ring(center, 4.75f * pulse, 0.17f, 28,
                       -0.38f, 0.48f, roll * 1.8f, COL_GOLD);

    for(index = 0; index < 18; ++index) {
        float angle = (float)index * TAU / 18.0f + roll;
        vec3_t left = plane_point(center, 8.05f, angle - 0.055f,
                                  0.10f, 0.0f, 0.0f);
        vec3_t right = plane_point(center, 8.05f, angle + 0.055f,
                                   0.10f, 0.0f, 0.0f);
        vec3_t tip = plane_point(center, 9.75f + (float)(index & 1) * 0.65f,
                                 angle, 0.10f, 0.0f, 0.0f);
        draw_tri_3d(left, right, tip, COL_CRIMSON,
                    color_scale(COL_RED, 0.65f), COL_GOLD);
    }

    for(index = 0; index < 5; ++index) {
        float angle = -PI * 0.5f + (float)index * TAU / 5.0f - time * 0.11f;
        star[index] = plane_point(center, 5.65f, angle,
                                  0.18f, -0.16f, 0.0f);
    }
    for(index = 0; index < 5; ++index)
        draw_line_3d(star[index], star[(index + 2) % 5], 4.5f, COL_RED);

    /* The eye at the center watches the whole arena. */
    draw_octahedron(center, 2.15f * pulse, 1.42f, COL_RED);
    draw_octahedron(vadd(center, v3(0.0f, 0.0f, 1.72f)),
                    0.62f, 0.82f, COL_BLACK);
    draw_octahedron(vadd(center, v3(0.0f, 0.0f, 2.22f)),
                    0.20f, 0.28f, COL_YELLOW);

    /* Orbiting reliquaries and energy spokes. */
    for(index = 0; index < 9; ++index) {
        float angle = (float)index * TAU / 9.0f - time * 0.14f;
        vec3_t relic = vadd(center,
                            v3(fcos(angle) * 11.2f,
                               fsin(angle * 2.0f + time * 0.3f) * 3.3f,
                               fsin(angle) * 2.7f));
        float size = 0.55f + (float)(index % 3) * 0.15f;
        draw_line_3d(center, relic, 2.0f,
                     color_scale(COL_PURPLE, 0.58f));
        draw_octahedron(relic, size, size * 1.8f,
                        (index & 1) ? COL_PURPLE : COL_CRIMSON);
        draw_cone(vadd(relic, v3(0.0f, size, 0.0f)),
                  1.4f + size, size * 0.45f, 5, COL_BONE);
        draw_cone(vadd(relic, v3(0.0f, -size, 0.0f)),
                  -1.4f - size, size * 0.45f, 5, COL_BONE);
    }

    /* Inverted floating cathedrals along the horizon. */
    for(index = 0; index < 7; ++index) {
        float x = ((float)index - 3.0f) * 6.6f;
        float hover = fsin(time * 0.6f + (float)index * 1.9f) * 0.55f;
        vec3_t base = v3(x, 11.5f + hover,
                         -16.0f - fabsf((float)index - 3.0f) * 1.2f);
        draw_box(base, v3(0.75f, 1.5f, 0.75f), time * 0.08f, 0xff26062eu);
        draw_cone(vadd(base, v3(0.0f, -1.35f, 0.0f)),
                  -4.5f, 1.35f, 6, COL_DARK_BLUE);
        draw_cone(vadd(base, v3(0.0f, 1.35f, 0.0f)),
                  2.1f, 0.72f, 6, COL_CRIMSON);
    }
}

static void draw_lightning_effects(float time) {
    int bolt_index;
    active_header = &additive_header;

    for(bolt_index = 0; bolt_index < MAX_LIGHTNING; ++bolt_index) {
        lightning_t *bolt = &lightning_bolts[bolt_index];
        if(bolt->active) {
            const int segments = 10;
            float fade = 1.0f - bolt->age / bolt->max_age;
            uint32_t frame_seed = bolt->seed ^
                                  (uint32_t)(bolt->age * 90.0f) * 0x9e3779b9u;
            vec3_t previous = vadd(bolt->target,
                                   v3(hash_signed(frame_seed) * 1.6f,
                                      bolt->height, hash_signed(frame_seed + 7u)));
            int segment;
            for(segment = 1; segment <= segments; ++segment) {
                float t = (float)segment / (float)segments;
                float spread = fsin(t * PI) * (1.35f + bolt->height * 0.025f);
                uint32_t segment_seed = frame_seed +
                                        (uint32_t)segment * 0x85ebca6bu;
                vec3_t current = vadd(bolt->target,
                                      v3(hash_signed(segment_seed) * spread,
                                         bolt->height * (1.0f - t),
                                         hash_signed(segment_seed + 19u) * spread));
                uint32_t color = color_alpha(bolt->color, fade * 0.86f);
                draw_line_3d(previous, current, 2.5f + fade * 5.0f, color);
                if(VISUAL_REVISION >= 11)
                    draw_line_3d(previous, current, 1.1f + fade * 1.7f,
                                 color_alpha(COL_WHITE, fade * 0.90f));
                if(segment == 4 || segment == 7) {
                    vec3_t branch = vadd(current,
                                         v3(hash_signed(segment_seed + 31u) * 2.2f,
                                            -1.3f,
                                            hash_signed(segment_seed + 47u) * 2.2f));
                    draw_line_3d(current, branch, 1.5f + fade * 2.0f,
                                 color_alpha(bolt->color, fade * 0.45f));
                }
                previous = current;
            }
        }
    }
    (void)time;
}

static void draw_cinematic_translucent(float time) {
    int cinematic_chaos = game_mode != MODE_PLAY || wave_transition_time > 0.0f;
    int index;
    active_header = &additive_header;

    if(VISUAL_REVISION >= 9 && cinematic_chaos) {
        vec3_t source = v3(0.0f, 8.2f, -19.2f);
        for(index = 0; index < 9; ++index) {
            float angle = (float)index * TAU / 9.0f + time * 0.09f;
            float radius = 7.0f + (float)(index & 1) * 5.5f;
            vec3_t left = v3(fcos(angle - 0.12f) * radius, -0.2f,
                             fsin(angle - 0.12f) * radius);
            vec3_t right = v3(fcos(angle + 0.12f) * radius, -0.2f,
                              fsin(angle + 0.12f) * radius);
            uint32_t apex = color_alpha((index & 1) ? COL_MAGENTA : COL_ORANGE,
                                        0.28f);
            uint32_t foot = color_alpha(COL_RED, 0.015f);
            draw_tri_3d(source, left, right, apex, foot, foot);
        }
    }

    if(VISUAL_REVISION >= 10) {
        for(index = 0; index < 72; ++index) {
            uint32_t seed = (uint32_t)index * 0x9e3779b9u;
            float cycle = fmodf(time * (0.055f + (float)(index % 7) * 0.008f) +
                                hash_signed(seed + 13u) * 0.5f + 0.5f, 1.0f);
            vec3_t center = v3(hash_signed(seed) * 21.0f,
                               -0.4f + cycle * 15.0f,
                               -18.0f + (hash_signed(seed + 71u) * 0.5f + 0.5f) * 31.0f);
            if(!cinematic_chaos &&
               center.x * center.x + center.z * center.z < 145.0f)
                continue;
            float size = 0.045f + (float)(index % 5) * 0.022f;
            vec3_t side = vscale(camera_view.right, size);
            vec3_t up = vscale(camera_view.up, size * (1.5f + cycle));
            uint32_t color = color_alpha((index % 3) ? COL_ORANGE : COL_MAGENTA,
                                         0.18f + (1.0f - cycle) * 0.52f);
            draw_quad_3d(vadd(vadd(center, side), up),
                         vadd(vsub(center, side), up),
                         vsub(vsub(center, side), up),
                         vadd(vsub(center, up), side),
                         color, color, color_alpha(color, 0.05f),
                         color_alpha(color, 0.05f));
        }
    }

    if(VISUAL_REVISION >= 12 && cinematic_chaos) {
        vec3_t previous[3];
        int strand;
        for(strand = 0; strand < 3; ++strand)
            previous[strand] = v3(0.0f, 0.1f, 1.8f);
        for(index = 1; index <= 42; ++index) {
            float t = (float)index / 42.0f;
            for(strand = 0; strand < 3; ++strand) {
                float angle = t * TAU * 3.25f + time * 0.85f +
                              (float)strand * TAU / 3.0f;
                float radius = 0.45f + t * 5.8f;
                vec3_t current = v3(fcos(angle) * radius,
                                    0.18f + t * 9.4f,
                                    1.8f + fsin(angle) * radius);
                draw_line_3d(previous[strand], current,
                             1.0f + (1.0f - t) * 2.0f,
                             color_alpha(strand == 0 ? COL_ORANGE :
                                         (strand == 1 ? COL_MAGENTA : COL_CYAN),
                                         0.46f * (1.0f - t * 0.55f)));
                previous[strand] = current;
            }
        }
    }

    if(VISUAL_REVISION >= 17 && cinematic_chaos) {
        for(index = 0; index < 15; ++index) {
            float cycle = fmodf(time * (0.10f + (float)(index % 4) * 0.012f) +
                                (float)index * 0.137f, 1.0f);
            float lane = hash_signed((uint32_t)index * 733u) * 24.0f;
            vec3_t head = v3(lane + cycle * 9.0f, 19.0f - cycle * 31.0f,
                             -5.0f - (float)(index % 5) * 3.5f);
            draw_octahedron(head, 0.20f + (float)(index % 3) * 0.11f,
                            0.42f, (index & 1) ? COL_YELLOW : COL_MAGENTA);
            draw_oriented_ring(head, 0.42f + (float)(index & 1) * 0.18f,
                               0.10f, 10, time, time * 0.7f,
                               time * 1.3f, color_alpha(COL_ORANGE, 0.45f));
        }
    }

    if(VISUAL_REVISION >= 18) {
        for(index = 0; index < 20; ++index) {
            float angle = (float)index * TAU / 20.0f;
            float blast = 1.2f + (fsin(time * 7.0f + (float)index * 2.1f) *
                                  0.5f + 0.5f) *
                                  (cinematic_chaos ? 5.9f : 2.3f);
            vec3_t foot = v3(fcos(angle) * 13.0f, 0.15f,
                             fsin(angle) * 13.0f);
            vec3_t side = vscale(camera_view.right, 0.34f);
            draw_tri_3d(vsub(foot, side), vadd(foot, side),
                        vadd(foot, v3(0.0f, blast, 0.0f)),
                        color_alpha(COL_RED, cinematic_chaos ? 0.68f : 0.28f),
                        color_alpha(COL_ORANGE, cinematic_chaos ? 0.68f : 0.28f),
                        color_alpha(COL_YELLOW, 0.02f));
        }
    }

    if(VISUAL_REVISION >= 20 && cinematic_chaos) {
        vec3_t engine = v3(0.0f, 8.2f, -19.0f);
        for(index = 0; index < 16; ++index) {
            float angle = (float)index * TAU / 16.0f - time * 0.18f;
            vec3_t target = v3(fcos(angle) * 14.0f, 0.0f,
                               fsin(angle) * 14.0f);
            draw_line_3d(engine, target,
                         2.0f + fsin(time * 8.0f + (float)index) * 0.8f,
                         color_alpha((index & 1) ? COL_GOLD : COL_MAGENTA,
                                     0.36f));
        }
    }
}

static void draw_sky_effects(float time) {
    vec3_t engine = v3(0.0f, 8.2f, -21.0f);
    int cinematic_chaos = game_mode != MODE_PLAY || wave_transition_time > 0.0f;
    int index;
    active_header = &additive_header;

    draw_oriented_ring(engine, 9.0f + fsin(time * 2.0f) * 0.45f,
                       0.65f, 36, 0.08f, 0.0f, -time * 0.25f,
                       color_alpha(COL_MAGENTA, 0.18f));
    draw_oriented_ring(engine, 3.1f + fsin(time * 4.4f) * 0.25f,
                       0.42f, 24, -0.2f, 0.3f, time * 0.44f,
                       color_alpha(COL_ORANGE, 0.32f));

    /* Meteor rain is cinematic dressing; active combat keeps a clean sky. */
    if(cinematic_chaos) {
        for(index = 0; index < 15; ++index) {
            float cycle = fmodf(time * (0.10f + (float)(index % 4) * 0.012f) +
                                (float)index * 0.137f, 1.0f);
            float lane = hash_signed((uint32_t)index * 733u) * 24.0f;
            float depth = -5.0f - (float)(index % 5) * 3.5f;
            vec3_t head = v3(lane + cycle * 9.0f,
                             19.0f - cycle * 31.0f, depth);
            vec3_t tail = vadd(head, v3(-2.4f, 5.2f, -0.8f));
            draw_line_3d(tail, head, 2.5f + (float)(index & 1) * 2.0f,
                         color_alpha((index & 1) ? COL_ORANGE : COL_MAGENTA,
                                     0.36f + (1.0f - cycle) * 0.28f));
        }
    }

    /* Rim-height additive pipes are reserved for non-interactive spectacle. */
    if(cinematic_chaos) {
        for(index = 0; index < 16; ++index) {
            float angle = (float)index * TAU / 16.0f;
            float beat = fsin(time * 5.4f + (float)index * 1.7f) * 0.5f + 0.5f;
            vec3_t base = v3(fcos(angle) * 12.9f, 0.2f,
                             fsin(angle) * 12.9f);
            draw_line_3d(base, vadd(base, v3(0.0f, 1.2f + beat * 4.8f, 0.0f)),
                         3.0f + beat * 4.0f,
                         color_alpha((index & 1) ? COL_RED : COL_ORANGE,
                                     0.22f + beat * 0.25f));
        }
    }
    draw_lightning_effects(time);
}

static void draw_screen_effects(float time) {
    int y;
    active_header = &translucent_header;

    /* CRT scanlines and a heavy theatrical vignette, all polygons. */
    for(y = 0; y < 480; y += 9)
        draw_rect_2d(0.0f, (float)y, SCREEN_W, 1.0f, 92.0f,
                     color_alpha(COL_BLACK, 0.055f));
    draw_rect_2d(0.0f, 0.0f, 42.0f, SCREEN_H, 93.0f,
                 color_alpha(COL_BLACK, 0.48f));
    draw_rect_2d(SCREEN_W - 42.0f, 0.0f, 42.0f, SCREEN_H, 93.0f,
                 color_alpha(COL_BLACK, 0.48f));
    draw_rect_2d(0.0f, 0.0f, SCREEN_W, 25.0f, 93.0f,
                 color_alpha(COL_BLACK, 0.32f));
    draw_rect_2d(0.0f, SCREEN_H - 24.0f, SCREEN_W, 24.0f, 93.0f,
                 color_alpha(COL_BLACK, 0.30f));

    if(flash_amount > 0.0f) {
        active_header = &additive_header;
        draw_rect_2d(0.0f, 0.0f, SCREEN_W, SCREEN_H, 96.0f,
                     color_alpha(COL_RED, flash_amount * 0.28f));
    }
    if(game_mode == MODE_PLAY && barrage_charge >= 100.0f) {
        float glow = 0.35f + fsin(time * 11.0f) * 0.18f;
        active_header = &additive_header;
        draw_rect_2d(0.0f, 0.0f, SCREEN_W, 9.0f, 97.0f,
                     color_alpha(COL_MAGENTA, glow));
        draw_rect_2d(0.0f, SCREEN_H - 9.0f, SCREEN_W, 9.0f, 97.0f,
                     color_alpha(COL_MAGENTA, glow));
        draw_rect_2d(0.0f, 0.0f, 9.0f, SCREEN_H, 97.0f,
                     color_alpha(COL_MAGENTA, glow));
        draw_rect_2d(SCREEN_W - 9.0f, 0.0f, 9.0f, SCREEN_H, 97.0f,
                     color_alpha(COL_MAGENTA, glow));
    }
    if(game_mode == MODE_TITLE && VISUAL_REVISION >= 19) {
        float pulse = fsin(time * 1.8f) * 0.5f + 0.5f;
        active_header = &translucent_header;
        draw_rect_2d(0.0f, 0.0f, SCREEN_W, 16.0f, 98.0f,
                     color_alpha(COL_BLACK, 0.76f));
        draw_rect_2d(0.0f, SCREEN_H - 17.0f, SCREEN_W, 17.0f, 98.0f,
                     color_alpha(COL_BLACK, 0.76f));
        draw_rect_2d(0.0f, 16.0f, SCREEN_W, 2.0f, 99.0f,
                     color_alpha(COL_CRIMSON, 0.45f + pulse * 0.25f));
        draw_rect_2d(0.0f, SCREEN_H - 19.0f, SCREEN_W, 2.0f, 99.0f,
                     color_alpha(COL_GOLD, 0.35f + pulse * 0.20f));
    }
}

static void draw_arena(float time) {
    const int segments = 32;
    int cinematic_chaos = game_mode != MODE_PLAY || wave_transition_time > 0.0f;
    vec3_t center = v3(0.0f, -0.12f, 0.0f);
    int index;

    for(index = 0; index < segments; ++index) {
        float a0 = (float)index * TAU / (float)segments;
        float a1 = (float)(index + 1) * TAU / (float)segments;
        vec3_t p0 = v3(fcos(a0) * 13.0f, -0.1f, fsin(a0) * 13.0f);
        vec3_t p1 = v3(fcos(a1) * 13.0f, -0.1f, fsin(a1) * 13.0f);
        uint32_t color = (index & 1) ? COL_GROUND : COL_GROUND_ALT;
        draw_tri_3d(center, p0, p1, color_scale(color, 1.15f), color,
                    color_scale(color, 0.72f));
    }

    draw_ring(v3(0.0f, 0.015f, 0.0f), 12.85f, 0.32f, 32, COL_CRIMSON);
    draw_ring(v3(0.0f, 0.02f, 0.0f), 8.0f, 0.10f, 28,
              color_scale(COL_RED, 0.55f));
    draw_ring(v3(0.0f, 0.025f, 0.0f), 4.2f, 0.08f, 24,
              color_scale(COL_PURPLE, 0.75f));

    for(index = 0; index < 14; ++index) {
        float angle = (float)index * TAU / 14.0f + time * 0.035f;
        float inner = 4.45f + (float)(index & 1) * 0.45f;
        float outer = 11.9f - (float)(index % 3) * 0.38f;
        vec3_t p0 = v3(fcos(angle) * inner, 0.032f,
                       fsin(angle) * inner);
        vec3_t p1 = v3(fcos(angle) * outer, 0.032f,
                       fsin(angle) * outer);
        draw_ground_strip(p0, p1, 0.055f,
                          color_scale((index & 1) ? COL_PURPLE : COL_CRIMSON,
                                      0.42f));
    }

    for(index = 0; index < 12; ++index) {
        float angle = (float)index * TAU / 12.0f - time * 0.09f;
        vec3_t rune = v3(fcos(angle) * 10.4f, 0.052f,
                         fsin(angle) * 10.4f);
        vec3_t tangent = v3(-fsin(angle) * 0.55f, 0.0f,
                            fcos(angle) * 0.55f);
        vec3_t outward = v3(fcos(angle) * 0.75f, 0.0f,
                            fsin(angle) * 0.75f);
        draw_ground_strip(vsub(rune, tangent), vadd(rune, outward), 0.09f,
                          color_scale(COL_ORANGE, 0.58f));
        draw_ground_strip(vadd(rune, tangent), vadd(rune, outward), 0.09f,
                          color_scale(COL_ORANGE, 0.58f));
    }

    /* Central pentagram and its animated infernal pulse. */
    for(index = 0; index < 5; ++index) {
        float a0 = -PI * 0.5f + (float)(index * 2) * TAU / 5.0f;
        float a1 = -PI * 0.5f + (float)((index * 2 + 2) % 5) * TAU / 5.0f;
        vec3_t p0 = v3(fcos(a0) * 3.65f, 0.045f, fsin(a0) * 3.65f);
        vec3_t p1 = v3(fcos(a1) * 3.65f, 0.045f, fsin(a1) * 3.65f);
        draw_ground_strip(p0, p1, 0.16f,
                          color_scale(COL_RED, 0.75f + fsin(time * 3.0f) * 0.18f));
    }
    draw_ring(v3(0.0f, 0.04f, 0.0f), 3.75f, 0.12f, 30,
              color_scale(COL_RED, 0.92f));

    /* Deterministic code-built basalt skyline. */
    for(index = 0; index < 22; ++index) {
        float angle = (float)index * TAU / 22.0f;
        float ripple = fsin((float)index * 9.17f) * 0.5f + 0.5f;
        float radius = 17.0f + ripple * 4.0f;
        float height = 3.5f + ripple * 7.5f;
        vec3_t base = v3(fcos(angle) * radius, -0.45f,
                         fsin(angle) * radius);
        uint32_t color = (index & 1) ? 0xff21050du : 0xff310810u;
        draw_cone(base, height, 1.5f + ripple * 1.4f, 7, color);
        if((index % 3) == 0)
            draw_cone(vadd(base, v3(0.0f, height * 0.76f, 0.0f)),
                      2.1f + ripple, 0.42f, 6, COL_CRIMSON);
    }

    /* Tall gate markers are cinematic-only; even at the rim their projected
       silhouettes could merge with incoming demons during an active wave. */
    if(cinematic_chaos) {
        for(index = 0; index < 8; ++index) {
            float angle = (float)index * TAU / 8.0f;
            vec3_t base = v3(fcos(angle) * 12.35f, 0.0f,
                             fsin(angle) * 12.35f);
            draw_box(vadd(base, v3(0.0f, 0.65f, 0.0f)),
                     v3(0.24f, 0.65f, 0.24f), -angle, COL_GREY);
            draw_cone(vadd(base, v3(0.0f, 1.25f, 0.0f)), 1.1f, 0.43f, 6,
                      COL_BONE);
        }
    }
}

static void draw_ring_wall(vec3_t center, float radius, float bottom,
                           float top, int segments, uint32_t dark,
                           uint32_t light) {
    int index;
    for(index = 0; index < segments; ++index) {
        float a0 = (float)index * TAU / (float)segments;
        float a1 = (float)(index + 1) * TAU / (float)segments;
        vec3_t p0 = vadd(center, v3(fcos(a0) * radius, bottom,
                                    fsin(a0) * radius));
        vec3_t p1 = vadd(center, v3(fcos(a1) * radius, bottom,
                                    fsin(a1) * radius));
        vec3_t q0 = vadd(center, v3(fcos(a0) * radius, top,
                                    fsin(a0) * radius));
        vec3_t q1 = vadd(center, v3(fcos(a1) * radius, top,
                                    fsin(a1) * radius));
        draw_quad_3d(p0, p1, q1, q0,
                     color_scale(dark, (index & 1) ? 0.72f : 0.92f),
                     dark, light, color_scale(light, 0.72f));
    }
}

static void draw_lava_expanse(float time) {
    const int segments = 48;
    int index;
    for(index = 0; index < segments; ++index) {
        float a0 = (float)index * TAU / (float)segments;
        float a1 = (float)(index + 1) * TAU / (float)segments;
        float heat = fsin(time * 1.7f + (float)index * 2.39f) * 0.5f + 0.5f;
        vec3_t i0 = v3(fcos(a0) * 13.25f, -0.72f, fsin(a0) * 13.25f);
        vec3_t i1 = v3(fcos(a1) * 13.25f, -0.72f, fsin(a1) * 13.25f);
        vec3_t o0 = v3(fcos(a0) * 34.0f, -1.10f, fsin(a0) * 34.0f);
        vec3_t o1 = v3(fcos(a1) * 34.0f, -1.10f, fsin(a1) * 34.0f);
        uint32_t inner = color_scale(COL_RED, 0.40f + heat * 0.28f);
        uint32_t outer = color_scale(COL_ORANGE, 0.20f + heat * 0.16f);
        draw_quad_3d(i0, i1, o1, o0, inner, inner, outer, outer);
        if((index & 3) == 0) {
            float angle = (a0 + a1) * 0.5f + fsin(time * 0.3f) * 0.02f;
            draw_ground_strip(v3(fcos(angle) * 13.4f, -0.68f,
                                 fsin(angle) * 13.4f),
                              v3(fcos(angle) * 31.0f, -1.02f,
                                 fsin(angle) * 31.0f),
                              0.16f, color_scale(COL_GOLD, 0.65f));
        }
    }
}

static void draw_titan_gate(float time) {
    vec3_t skull = v3(0.0f, 6.3f, -17.0f);
    float pulse = 1.0f + fsin(time * 2.5f) * 0.045f;
    int side;

    draw_octahedron(skull, 3.2f * pulse, 3.45f, 0xff4d1118u);
    draw_box(vadd(skull, v3(0.0f, -2.15f, 0.25f)),
             v3(1.75f, 1.35f, 1.5f), 0.0f, 0xff25050au);
    for(side = -1; side <= 1; side += 2) {
        vec3_t eye = vadd(skull, v3((float)side * 1.10f, 0.35f, 2.62f));
        vec3_t horn = vadd(skull, v3((float)side * 2.15f, 1.65f, 0.0f));
        draw_octahedron(eye, 0.48f, 0.68f,
                        (side < 0) ? COL_MAGENTA : COL_ORANGE);
        draw_cone(horn, 4.8f, 1.15f, 7, COL_BONE);
        draw_cone(vadd(horn, v3((float)side * 0.85f, 3.75f, 0.0f)),
                  2.7f, 0.62f, 6, COL_GOLD);
    }
    for(side = -2; side <= 2; ++side)
        draw_box(vadd(skull, v3((float)side * 0.58f, -3.05f, 2.20f)),
                 v3(0.20f, 0.72f + (float)(side & 1) * 0.18f, 0.26f),
                 0.0f, COL_BONE);
}

static void draw_mega_chains(float time) {
    int side;
    for(side = -1; side <= 1; side += 2) {
        vec3_t previous = v3((float)side * 24.0f, 15.5f, -9.0f);
        int link;
        for(link = 1; link <= 18; ++link) {
            float t = (float)link / 18.0f;
            float sag = fsin(t * PI) * 8.0f;
            vec3_t current = v3((float)side * (24.0f * (1.0f - t)),
                                15.5f - t * 6.4f - sag,
                                -9.0f - t * 11.5f);
            draw_line_3d(previous, current, 5.5f,
                         (link & 1) ? COL_GREY : COL_GOLD);
            if((link & 1) == 0)
                draw_oriented_ring(current, 0.48f, 0.13f, 8,
                                   0.2f, time * 0.2f + (float)link,
                                   (float)link * 0.7f, COL_BONE);
            previous = current;
        }
    }
}

static void draw_engine_overdrive(float time) {
    vec3_t center = v3(0.0f, 8.2f, -21.0f);
    int index;
    for(index = 0; index < 3; ++index) {
        float radius = 9.8f + (float)index * 1.25f;
        float roll = time * ((index & 1) ? -0.32f : 0.24f) + (float)index;
        draw_oriented_ring(center, radius, 0.18f + (float)index * 0.05f,
                           40, 0.16f * (float)(index - 1),
                           0.22f * (float)index, roll,
                           (index == 1) ? COL_GOLD : COL_PURPLE);
    }
    for(index = 0; index < 24; ++index) {
        float angle = (float)index * TAU / 24.0f + time * 0.21f;
        vec3_t inner = plane_point(center, 8.7f, angle, 0.1f, 0.0f, 0.0f);
        vec3_t outer = plane_point(center, 12.4f + (float)(index & 1) * 0.9f,
                                   angle, 0.1f, 0.0f, 0.0f);
        draw_line_3d(inner, outer, (index % 3) ? 2.0f : 4.0f,
                     (index & 1) ? COL_CRIMSON : COL_GOLD);
    }
}

static void draw_hell_megacity(float time) {
    int index;
    for(index = 0; index < 18; ++index) {
        float side = index < 9 ? -1.0f : 1.0f;
        int lane = index % 9;
        float x = side * (9.0f + (float)lane * 2.15f);
        float z = -12.5f - (float)(lane % 3) * 3.8f;
        float height = 4.5f + (float)((lane * 7) % 5) * 1.7f;
        vec3_t base = v3(x, -0.2f, z);
        draw_box(vadd(base, v3(0.0f, height * 0.5f, 0.0f)),
                 v3(0.75f + (float)(lane & 1) * 0.3f,
                    height * 0.5f, 0.85f),
                 side * 0.08f, (lane & 1) ? 0xff26042eu : 0xff32070du);
        draw_cone(vadd(base, v3(0.0f, height, 0.0f)),
                  2.0f + (float)(lane % 3), 0.95f, 6,
                  (lane % 3) ? COL_CRIMSON : COL_DARK_BLUE);
        if((lane & 1) == 0)
            draw_ring(vadd(base, v3(0.0f, height * 0.72f, 0.0f)),
                      0.48f, 0.10f, 10,
                      color_scale(COL_ORANGE,
                                  0.65f + fsin(time + (float)lane) * 0.2f));
    }
}

static void draw_title_stage(float time) {
    vec3_t base = v3(0.0f, -0.05f, -1.0f);
    int index;
    draw_ring_wall(base, 2.7f, -0.75f, 0.0f, 24,
                   0xff24030du, COL_CRIMSON);
    draw_ring(vadd(base, v3(0.0f, 0.05f, 0.0f)), 2.55f, 0.18f, 24,
              COL_GOLD);
    draw_ring(vadd(base, v3(0.0f, 0.08f, 0.0f)),
              1.25f + fsin(time * 3.4f) * 0.08f,
              0.11f, 20, COL_MAGENTA);
    for(index = 0; index < 7; ++index) {
        float angle = (float)index * TAU / 7.0f + time * 0.08f;
        vec3_t spike = vadd(base, v3(fcos(angle) * 2.45f, 0.0f,
                                     fsin(angle) * 2.45f));
        draw_cone(spike, 1.35f + (float)(index & 1) * 0.55f,
                  0.24f, 5, (index & 1) ? COL_BONE : COL_GOLD);
    }
}

static void draw_title_ordnance(float time) {
    vec3_t root = v3(0.0f, 1.7f, -1.05f);
    int index;

    /* A radial rack of shell tracers frames the hunter on the title screen. */
    for(index = 0; index < 18; ++index) {
        float angle = (float)index * TAU / 18.0f + time * 0.16f;
        float inner_radius = 1.2f + (float)(index & 1) * 0.35f;
        float outer_radius = 3.0f + (float)(index % 3) * 0.34f;
        vec3_t inner = vadd(root, v3(fcos(angle) * inner_radius,
                                     fsin(angle) * inner_radius, 0.35f));
        vec3_t outer = vadd(root, v3(fcos(angle) * outer_radius,
                                     fsin(angle) * outer_radius, 0.58f));
        draw_line_3d(inner, outer, (index % 3) == 0 ? 4.0f : 2.0f,
                     (index & 1) ? COL_CYAN : COL_GOLD);
        if((index % 3) == 0)
            draw_octahedron(outer, 0.16f, 0.28f, COL_ORANGE);
    }
}

static void draw_cinematic_opaque(float time) {
    int cinematic_chaos = game_mode != MODE_PLAY || wave_transition_time > 0.0f;

    if(VISUAL_REVISION >= 2) {
        draw_ring_wall(v3(0.0f, 0.0f, 0.0f), 13.05f, -1.7f, -0.05f,
                       32, 0xff160207u, COL_CRIMSON);
        draw_ring_wall(v3(0.0f, 0.0f, 0.0f), 8.05f, -0.55f, -0.08f,
                       28, 0xff21030bu, COL_PURPLE);
    }
    if(VISUAL_REVISION >= 3)
        draw_lava_expanse(time);
    if(VISUAL_REVISION >= 4 && game_mode == MODE_TITLE)
        draw_title_stage(time);
    if(VISUAL_REVISION >= 5)
        draw_titan_gate(time);
    if(VISUAL_REVISION >= 6 && cinematic_chaos)
        draw_mega_chains(time);
    if(VISUAL_REVISION >= 7)
        draw_engine_overdrive(time);
    if(VISUAL_REVISION >= 8)
        draw_hell_megacity(time);
    if(VISUAL_REVISION >= 15 && game_mode == MODE_TITLE)
        draw_title_ordnance(time);
    if(VISUAL_REVISION >= 20 && cinematic_chaos) {
        int index;
        for(index = 0; index < 28; ++index) {
            float angle = (float)index * TAU / 28.0f + time * 0.04f;
            vec3_t base = v3(fcos(angle) * 14.6f, -0.4f,
                             fsin(angle) * 14.6f);
            draw_cone(base, 2.2f + (float)(index % 5) * 0.55f,
                      0.20f + (float)(index & 1) * 0.08f, 5,
                      (index % 3) ? COL_CRIMSON : COL_GOLD);
        }
    }
}

static void draw_horn(vec3_t base, float yaw, float side) {
    vec3_t middle = vadd(base, yaw_point(v3(side * 0.42f, 0.62f, 0.02f), yaw));
    vec3_t tip = vadd(base, yaw_point(v3(side * 0.70f, 1.03f, -0.08f), yaw));
    vec3_t width = yaw_point(v3(0.13f, 0.0f, 0.0f), yaw);
    draw_tri_3d(vadd(base, width), vsub(base, width), middle,
                COL_BONE, color_scale(COL_BONE, 0.66f), COL_GOLD);
    draw_tri_3d(vadd(middle, vscale(width, 0.58f)),
                vsub(middle, vscale(width, 0.58f)), tip,
                COL_GOLD, color_scale(COL_BONE, 0.55f), COL_WHITE);
}

static void draw_hunter(float time, int title_pose) {
    vec3_t base = title_pose ? v3(0.0f, 0.0f, -1.0f) : player_pos;
    float yaw = title_pose ? fsin(time * 0.7f) * 0.35f :
                atan2f(player_dir.x, -player_dir.z);
    float bob = fsin(time * 7.0f) * 0.055f;
    vec3_t body = vadd(base, v3(0.0f, 1.05f + bob, 0.0f));
    vec3_t head = vadd(base, v3(0.0f, 2.05f + bob, 0.0f));
    vec3_t forward = yaw_point(v3(0.0f, 0.0f, -1.0f), yaw);
    vec3_t right = yaw_point(v3(1.0f, 0.0f, 0.0f), yaw);
    vec3_t shoulder = vadd(body, vadd(vscale(right, 0.28f),
                                      v3(0.0f, 0.38f, 0.0f)));
    vec3_t tube = vadd(shoulder, vscale(forward, 0.62f));
    vec3_t muzzle = vadd(tube, vscale(forward, 1.18f));
    uint32_t suit = invulnerability > 0.0f && ((int)(game_time * 18.0f) & 1) ?
                    COL_WHITE : COL_DARK_BLUE;

    /* Human demon-hunter: armored silhouette, helmet, backpack, and an
       intentionally absurd shoulder-fired bazooka readable from game camera. */
    draw_box(vadd(body, vscale(forward, -0.34f)),
             v3(0.46f, 0.54f, 0.24f), yaw, COL_GREY);
    draw_box(body, v3(0.48f, 0.62f, 0.34f), yaw, suit);
    draw_octahedron(vadd(body, vscale(right, -0.55f)), 0.22f, 0.30f, COL_CYAN);
    draw_octahedron(vadd(body, vscale(right, 0.55f)), 0.22f, 0.30f, COL_CYAN);
    draw_box(vadd(base, yaw_point(v3(-0.28f, 0.34f, 0.0f), yaw)),
             v3(0.18f, 0.34f, 0.22f), yaw, COL_GREY);
    draw_box(vadd(base, yaw_point(v3(0.28f, 0.34f, 0.0f), yaw)),
             v3(0.18f, 0.34f, 0.22f), yaw, COL_GREY);
    draw_octahedron(head, 0.43f, 0.48f, COL_BONE);
    draw_box(vadd(head, v3(0.0f, 0.17f, 0.0f)),
             v3(0.46f, 0.26f, 0.38f), yaw, COL_GREY);
    draw_box(vadd(head, vscale(forward, 0.40f)),
             v3(0.31f, 0.10f, 0.07f), yaw, COL_CYAN);

    draw_box(tube, v3(0.29f, 0.27f, 1.16f), yaw, 0xff49613au);
    draw_box(vadd(tube, v3(0.0f, 0.31f, 0.0f)),
             v3(0.14f, 0.09f, 0.34f), yaw, COL_CYAN);
    draw_octahedron(muzzle, 0.39f, 0.34f, COL_GREY);
    draw_octahedron(vadd(muzzle, vscale(forward, 0.18f)),
                    0.22f, 0.18f, COL_BLACK);
    draw_box(vadd(tube, vscale(forward, -0.98f)),
             v3(0.25f, 0.25f, 0.34f), yaw, COL_GROUND_ALT);
    draw_octahedron(vadd(shoulder, vscale(right, -0.34f)),
                    0.16f, 0.20f, COL_BONE);
    draw_octahedron(vadd(tube, vadd(vscale(forward, 0.32f),
                                    vscale(right, -0.34f))),
                    0.16f, 0.20f, COL_BONE);

    if(!title_pose && barrage_charge > 45.0f) {
        int spike;
        float strength = (barrage_charge - 45.0f) / 55.0f;
        float radius = 1.15f + strength * 0.75f + fsin(time * 8.0f) * 0.10f;
        draw_ring(vadd(base, v3(0.0f, 0.08f, 0.0f)), radius, 0.08f, 20,
                  strength > 0.95f ? COL_YELLOW : COL_CYAN);
        for(spike = 0; spike < 8; ++spike) {
            float angle = (float)spike * TAU / 8.0f + time * 1.7f;
            vec3_t foot = vadd(base, v3(fcos(angle) * radius, 0.08f,
                                        fsin(angle) * radius));
            vec3_t tip = vadd(foot, v3(0.0f,
                                       0.35f + strength * (0.7f +
                                       fsin(time * 9.0f + (float)spike) * 0.25f),
                                       0.0f));
            draw_line_3d(foot, tip, 2.0f + strength * 2.5f,
                         strength > 0.95f ? COL_GOLD : COL_CYAN);
        }
    }
}

static void draw_demon_wing(vec3_t root, float yaw, float side, float flap,
                            float scale, uint32_t color) {
    vec3_t p0 = root;
    vec3_t p1 = vadd(root, yaw_point(v3(side * 0.95f * scale,
                                        0.42f * scale + flap,
                                        0.1f), yaw));
    vec3_t p2 = vadd(root, yaw_point(v3(side * 1.42f * scale,
                                        0.02f * scale + flap * 0.4f,
                                        0.18f), yaw));
    vec3_t p3 = vadd(root, yaw_point(v3(side * 0.64f * scale,
                                        -0.40f * scale,
                                        0.12f), yaw));
    draw_tri_3d(p0, p1, p3, color_scale(color, 0.42f), color, COL_BLACK);
    draw_tri_3d(p1, p2, p3, color, color_scale(color, 0.54f), COL_BLACK);
}

static void draw_enemy(const enemy_t *enemy, float time) {
    float bob = 0.86f + fsin(enemy->phase) * 0.18f;
    float scale = enemy->kind == 0 ? 1.0f : (enemy->kind == 1 ? 1.28f : 1.65f);
    vec3_t center = vadd(enemy->pos, v3(0.0f, bob + scale * 0.28f, 0.0f));
    vec3_t head = vadd(center, v3(0.0f, 0.82f * scale, 0.0f));
    vec3_t forward = yaw_point(v3(0.0f, 0.0f, -1.0f), enemy->yaw);
    vec3_t right = yaw_point(v3(1.0f, 0.0f, 0.0f), enemy->yaw);
    vec3_t tail_root = vadd(center, vscale(forward, -0.25f * scale));
    vec3_t tail_mid = vadd(tail_root,
                           vadd(vscale(forward, -0.75f * scale),
                                v3(0.0f, -0.35f * scale, 0.0f)));
    vec3_t tail_tip = vadd(tail_mid,
                           vadd(vscale(right, fsin(time * 5.0f + enemy->phase) *
                                                   0.55f * scale),
                                v3(0.0f, 0.22f * scale, 0.0f)));
    uint32_t hide = enemy->kind == 0 ? COL_CRIMSON :
                    (enemy->kind == 1 ? COL_PURPLE : COL_BLACK);
    uint32_t hot = enemy->kind == 2 ? COL_MAGENTA : COL_ORANGE;

    draw_cone(vadd(center, v3(0.0f, 0.28f * scale, 0.0f)),
              -1.10f * scale, 0.62f * scale, 7, color_scale(hide, 0.72f));
    draw_octahedron(center, 0.50f * scale, 0.65f * scale, hide);
    draw_octahedron(head, 0.36f * scale, 0.40f * scale,
                    enemy->kind == 2 ? COL_CRIMSON : COL_RED);
    draw_horn(vadd(head, vadd(vscale(right, -0.20f * scale),
                              v3(0.0f, 0.20f * scale, 0.0f))),
              enemy->yaw, -1.0f);
    draw_horn(vadd(head, vadd(vscale(right, 0.20f * scale),
                              v3(0.0f, 0.20f * scale, 0.0f))),
              enemy->yaw, 1.0f);
    if(enemy->kind > 0) {
        draw_demon_wing(center, enemy->yaw, -1.0f,
                        fsin(enemy->phase) * 0.25f, scale, hide);
        draw_demon_wing(center, enemy->yaw, 1.0f,
                        fsin(enemy->phase + PI) * 0.25f, scale, hide);
    }

    draw_line_3d(tail_root, tail_mid, 3.0f * scale, hide);
    draw_line_3d(tail_mid, tail_tip, 2.2f * scale, hot);
    draw_octahedron(tail_tip, 0.13f * scale, 0.18f * scale, hot);
    draw_line_3d(vadd(center, vscale(right, -0.32f * scale)),
                 vadd(center, vadd(vscale(right, -0.80f * scale),
                                   vscale(forward, 0.28f * scale))),
                 3.0f * scale, hide);
    draw_line_3d(vadd(center, vscale(right, 0.32f * scale)),
                 vadd(center, vadd(vscale(right, 0.80f * scale),
                                   vscale(forward, 0.28f * scale))),
                 3.0f * scale, hide);

    draw_octahedron(vadd(head, vadd(vscale(forward, 0.34f * scale),
                                    vscale(right, -0.12f * scale))),
                    0.065f * scale, 0.065f * scale, COL_YELLOW);
    draw_octahedron(vadd(head, vadd(vscale(forward, 0.34f * scale),
                                    vscale(right, 0.12f * scale))),
                    0.065f * scale, 0.065f * scale, COL_YELLOW);

    if(enemy->kind == 2)
        draw_ring(vadd(enemy->pos, v3(0.0f, 0.06f, 0.0f)),
                  1.20f * scale + fsin(time * 5.0f) * 0.12f,
                  0.12f, 20, COL_MAGENTA);

    if(enemy->kind > 0) {
        int pip;
        for(pip = 0; pip < enemy->hp; ++pip) {
            vec3_t pip_pos = vadd(head, v3(((float)pip - (float)(enemy->hp - 1) * 0.5f) * 0.18f,
                                            0.88f * scale, 0.0f));
            draw_octahedron(pip_pos, 0.055f, 0.08f,
                            enemy->kind == 2 ? COL_MAGENTA : COL_GOLD);
        }
    }
}

static void draw_bolts(void) {
    int index;
    for(index = 0; index < MAX_BOLTS; ++index) {
        if(bolts[index].active) {
            vec3_t direction = vnormalize(bolts[index].vel);
            vec3_t tail = vsub(bolts[index].pos, vscale(direction, 0.68f));
            float yaw = atan2f(direction.x, -direction.z);
            draw_box(bolts[index].pos, v3(0.16f, 0.16f, 0.36f),
                     yaw, COL_GREY);
            draw_octahedron(vadd(bolts[index].pos, vscale(direction, 0.36f)),
                            0.20f, 0.24f, COL_YELLOW);
            draw_line_3d(tail, bolts[index].pos, 7.0f, COL_ORANGE);
        }
    }
}

static void draw_powerups(float time) {
    int index;
    for(index = 0; index < MAX_POWERUPS; ++index) {
        const powerup_t *powerup = &powerups[index];
        float bob;
        float pulse;
        vec3_t center;
        uint32_t color;
        if(!powerup->active)
            continue;
        if(powerup->lifetime < 3.0f && ((int)(powerup->age * 10.0f) & 1))
            continue;

        bob = 0.78f + fsin(time * 4.6f + powerup->phase) * 0.18f;
        pulse = 0.55f + fsin(time * 7.0f + powerup->phase) * 0.10f;
        center = vadd(powerup->pos, v3(0.0f, bob, 0.0f));
        color = powerup->kind == POWERUP_SPEED ? COL_CYAN :
                (powerup->kind == POWERUP_RAPID_FIRE ? COL_GOLD : COL_MAGENTA);

        draw_ring(vadd(powerup->pos, v3(0.0f, 0.04f, 0.0f)),
                  0.70f + pulse * 0.18f, 0.12f, 16, color);
        draw_oriented_ring(center, 0.52f, 0.10f, 12,
                           time * 0.8f, powerup->phase,
                           -time * 1.4f, color);

        if(powerup->kind == POWERUP_SPEED) {
            vec3_t left = vadd(center, v3(-0.48f, -0.22f, 0.0f));
            vec3_t right = vadd(center, v3(0.48f, -0.22f, 0.0f));
            vec3_t tip = vadd(center, v3(0.0f, 0.58f, 0.0f));
            draw_tri_3d(left, right, tip, COL_BLUE, COL_CYAN, COL_WHITE);
            draw_octahedron(center, 0.20f, 0.36f, COL_WHITE);
        }
        else if(powerup->kind == POWERUP_RAPID_FIRE) {
            int round;
            for(round = -1; round <= 1; ++round) {
                vec3_t round_pos = vadd(center, v3((float)round * 0.28f,
                                                   (float)(round & 1) * 0.12f,
                                                   0.0f));
                draw_box(round_pos, v3(0.10f, 0.35f, 0.10f),
                         time * 1.5f, COL_GOLD);
                draw_cone(vadd(round_pos, v3(0.0f, 0.35f, 0.0f)),
                          0.24f, 0.10f, 5, COL_YELLOW);
            }
        }
        else {
            int rocket;
            for(rocket = -1; rocket <= 1; ++rocket) {
                vec3_t rocket_pos = vadd(center,
                                         v3((float)rocket * 0.32f,
                                            rocket == 0 ? 0.20f : -0.08f,
                                            0.0f));
                draw_octahedron(rocket_pos, 0.16f, 0.42f,
                                rocket == 0 ? COL_WHITE : COL_MAGENTA);
            }
        }
    }
}

static void draw_particles(void) {
    int index;
    active_header = &additive_header;
    for(index = 0; index < MAX_PARTICLES; ++index) {
        particle_t *particle = &particles[index];
        if(particle->active) {
            float fade = clampf(particle->life / particle->max_life, 0.0f, 1.0f);
            float size = particle->size * (0.55f + fade * 0.7f);
            vec3_t side = vscale(camera_view.right, size);
            vec3_t up = vscale(camera_view.up, size);
            uint32_t color = color_alpha(particle->color, fade * 0.78f);
            draw_quad_3d(vadd(vadd(particle->pos, side), up),
                         vadd(vsub(particle->pos, side), up),
                         vsub(vsub(particle->pos, side), up),
                         vadd(vsub(particle->pos, up), side),
                         color, color, color_alpha(color, 0.15f), color_alpha(color, 0.15f));
        }
    }
    for(index = 0; index < MAX_SHOCKWAVES; ++index) {
        shockwave_t *shock = &shockwaves[index];
        if(shock->active) {
            float progress = shock->age / shock->max_age;
            uint32_t color = color_alpha(shock->color, (1.0f - progress) * 0.72f);
            draw_ring(shock->pos, shock->max_radius * progress,
                      0.18f + progress * 0.42f, 32, color);
        }
    }
}

static void draw_flames(float time) {
    int index;

    if(game_mode == MODE_PLAY && wave_transition_time <= 0.0f)
        return;

    active_header = &additive_header;
    for(index = 0; index < 18; ++index) {
        float angle = (float)index * TAU / 18.0f;
        float wave = fsin(time * 3.0f + (float)index * 2.7f);
        vec3_t position = v3(fcos(angle) * 12.9f,
                             0.55f + wave * 0.28f,
                             fsin(angle) * 12.9f);
        vec3_t side = vscale(camera_view.right, 0.25f + wave * 0.05f);
        vec3_t up = vscale(camera_view.up, 0.7f + wave * 0.18f);
        uint32_t bottom = color_alpha(COL_RED, 0.72f);
        uint32_t top = color_alpha(COL_YELLOW, 0.10f);
        draw_tri_3d(vsub(position, side), vadd(position, side),
                    vadd(position, up), bottom, bottom, top);
    }
}

static void draw_hud(void) {
    char buffer[64];
    char boost_buffer[64];
    int index;
    float pulse = 0.7f + fsin(game_time * 8.0f) * 0.3f;

    active_header = &ui_header;
    draw_rect_2d(12.0f, 10.0f, 616.0f, 38.0f, 70.0f,
                 color_alpha(COL_BLACK, 0.84f));
    draw_rect_2d(12.0f, 10.0f, 616.0f, 2.0f, 74.0f, COL_CRIMSON);
    draw_rect_2d(12.0f, 46.0f, 616.0f, 2.0f, 74.0f, COL_GOLD);
    draw_text(21.0f, 19.0f, 2.1f, "LIFE", COL_BONE);
    for(index = 0; index < MAX_LIVES; ++index) {
        uint32_t color = index < player_lives ? COL_RED : 0xff35121au;
        draw_rect_2d(89.0f + (float)index * 19.0f, 19.0f, 14.0f, 17.0f,
                     78.0f, color);
        if(index < player_lives)
            draw_rect_2d(93.0f + (float)index * 19.0f, 15.0f, 6.0f, 6.0f,
                         79.0f, COL_ORANGE);
    }

    snprintf(buffer, sizeof(buffer), "SCORE %07d", score);
    draw_text(232.0f, 19.0f, 2.1f, buffer, COL_GOLD);
    snprintf(buffer, sizeof(buffer), "W%02d %02d/%02d",
             wave_number, wave_kills, wave_target);
    draw_text(474.0f, 21.0f, 1.55f, buffer, COL_BONE);

    draw_text(18.0f, 445.0f, 2.0f, "ROCKET",
              barrage_charge >= 100.0f ? COL_MAGENTA : COL_BONE);
    draw_rect_2d(102.0f, 446.0f, 300.0f, 15.0f, 70.0f, 0xff2b0a20u);
    draw_rect_2d(105.0f, 449.0f, 2.94f * barrage_charge, 9.0f, 75.0f,
                 barrage_charge >= 100.0f ?
                 color_scale(COL_MAGENTA, pulse + 0.5f) : COL_CRIMSON);
    if(barrage_charge >= 100.0f)
        draw_text(416.0f, 445.0f, 2.0f, "X BARRAGE", COL_YELLOW);
    else {
        snprintf(buffer, sizeof(buffer), "COMBO X%02d", combo);
        draw_text(461.0f, 445.0f, 2.0f, buffer, combo > 5 ? COL_ORANGE : COL_BONE);
    }

    if(dash_cooldown <= 0.0f)
        draw_text(18.0f, 416.0f, 1.4f, "B DASH READY", COL_CYAN);
    if(speed_boost_time > 0.0f || rapid_fire_time > 0.0f ||
       triple_shot_time > 0.0f) {
        snprintf(boost_buffer, sizeof(boost_buffer), "S%02d R%02d T%02d",
                 (int)ceilf(speed_boost_time),
                 (int)ceilf(rapid_fire_time),
                 (int)ceilf(triple_shot_time));
        draw_text(242.0f, 416.0f, 1.35f, boost_buffer, COL_GOLD);
    }
    if(powerup_banner_time > 0.0f) {
        static const char *names[3] = {
            "SPEED BOOST", "RAPID FIRE", "TRIPLE BAZOOKA"
        };
        uint32_t banner_color = powerup_banner_kind == POWERUP_SPEED ? COL_CYAN :
                                (powerup_banner_kind == POWERUP_RAPID_FIRE ?
                                 COL_GOLD : COL_MAGENTA);
        draw_rect_2d(178.0f, 352.0f, 284.0f, 34.0f, 70.0f,
                     color_alpha(COL_BLACK, 0.84f));
        draw_text_centered(361.0f, 2.0f,
                           names[powerup_banner_kind], banner_color);
    }
    if(mode_time < 4.5f)
        draw_text_centered(392.0f, 1.55f,
                           "ANALOG MOVE  A BAZOOKA  B DASH", COL_BONE);
}

static void draw_title(float time) {
    uint32_t title_color = color_scale(COL_RED, 0.88f + fsin(time * 4.0f) * 0.12f);
    float prompt_pulse = fsin(time * 5.0f) * 0.5f + 0.5f;
    active_header = &ui_header;

    /* Purpose-built negative space: the spectacle remains visible around the
       logo and through the alpha, but never destroys the letter silhouettes. */
    draw_rect_2d(166.0f, 31.0f, 308.0f, 119.0f, 70.0f,
                 color_alpha(COL_BLACK, 0.82f));
    draw_rect_2d(174.0f, 37.0f, 292.0f, 3.0f, 74.0f,
                 color_alpha(COL_MAGENTA, 0.88f));
    draw_rect_2d(174.0f, 145.0f, 292.0f, 3.0f, 74.0f,
                 color_alpha(COL_GOLD, 0.88f));
    draw_rect_2d(166.0f, 31.0f, 3.0f, 119.0f, 74.0f,
                 color_alpha(COL_CRIMSON, 0.92f));
    draw_rect_2d(471.0f, 31.0f, 3.0f, 119.0f, 74.0f,
                 color_alpha(COL_ORANGE, 0.92f));
    draw_line_2d(166.0f, 31.0f, 184.0f, 13.0f, 3.0f, 75.0f, COL_CRIMSON);
    draw_line_2d(474.0f, 31.0f, 456.0f, 13.0f, 3.0f, 75.0f, COL_ORANGE);
    draw_line_2d(166.0f, 150.0f, 184.0f, 168.0f, 3.0f, 75.0f, COL_GOLD);
    draw_line_2d(474.0f, 150.0f, 456.0f, 168.0f, 3.0f, 75.0f, COL_GOLD);
    if(VISUAL_REVISION < 13) {
        draw_rect_2d(28.0f, 58.0f, 584.0f, 92.0f, 70.0f, 0xff090006u);
        draw_text_centered(72.0f, 5.0f, "DEMON", title_color);
        draw_text_centered(112.0f, 3.6f, "BAZOOKA", COL_GOLD);
    }
    else {
        float flicker = fsin(time * 9.0f) * 0.65f;
        draw_text_centered(51.0f + flicker, 5.4f, "DEMON", COL_BLACK);
        draw_text_centered(47.0f, 5.4f, "DEMON", COL_CYAN);
        draw_text_centered(44.0f, 5.4f, "DEMON", title_color);
        draw_text_centered(101.0f, 3.95f, "BAZOOKA", COL_BLACK);
        draw_text_centered(97.0f, 3.95f, "BAZOOKA", COL_ORANGE);
        draw_text_centered(94.0f, 3.95f, "BAZOOKA", COL_GOLD);
    }
    if(VISUAL_REVISION >= 14) {
        float pulse = fsin(time * 4.0f) * 0.5f + 0.5f;
        draw_rect_2d(42.0f, 34.0f, 170.0f, 3.0f, 75.0f, COL_CRIMSON);
        draw_rect_2d(428.0f, 34.0f, 170.0f, 3.0f, 75.0f, COL_CRIMSON);
        draw_rect_2d(42.0f, 142.0f, 145.0f, 2.0f, 75.0f, COL_GOLD);
        draw_rect_2d(453.0f, 142.0f, 145.0f, 2.0f, 75.0f, COL_GOLD);
        draw_text_centered(18.0f, 1.05f, "THE HUNTER ARRIVES", COL_BONE);
        draw_rect_2d(312.0f, 24.0f, 16.0f, 6.0f + pulse * 7.0f,
                     78.0f, COL_MAGENTA);
    }
    draw_rect_2d(126.0f, 368.0f, 388.0f, 99.0f, 70.0f,
                 color_alpha(COL_BLACK, 0.80f));
    draw_rect_2d(126.0f, 368.0f, 388.0f, 3.0f, 74.0f,
                 color_alpha(COL_CRIMSON, 0.92f));
    draw_rect_2d(126.0f, 464.0f, 388.0f, 3.0f, 74.0f,
                 color_alpha(COL_GOLD, 0.86f));
    draw_text_centered(385.0f, 2.15f, "PRESS START TO HUNT", COL_BLACK);
    draw_text_centered(382.0f, 2.15f, "PRESS START TO HUNT",
                       color_scale(COL_WHITE, 0.82f + prompt_pulse * 0.18f));
    draw_text_centered(VISUAL_REVISION >= 16 ? 420.0f : 393.0f, 1.45f,
                       "ONE MAN  ALL HELL  BIG GUN", COL_ORANGE);
    draw_text_centered(VISUAL_REVISION >= 16 ? 447.0f : 424.0f, 1.35f,
                       "A BAZOOKA  B DASH  X BARRAGE", COL_BONE);
    if(high_score > 0) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "HIGH SCORE %07d", high_score);
        draw_text_centered(344.0f, 1.4f, buffer, COL_GOLD);
    }
}

static void draw_game_over(float time) {
    char buffer[48];
    active_header = &ui_header;
    draw_rect_2d(68.0f, 120.0f, 504.0f, 230.0f, 72.0f,
                 color_alpha(COL_BLACK, 0.88f));
    draw_rect_2d(68.0f, 120.0f, 504.0f, 4.0f, 75.0f, COL_CRIMSON);
    draw_rect_2d(68.0f, 346.0f, 504.0f, 4.0f, 75.0f, COL_GOLD);
    draw_text_centered(145.0f, 4.2f, "HUNTER DOWN", COL_RED);
    draw_text_centered(195.0f, 1.7f, "THE DEMONS ATE EARTH", COL_CYAN);
    snprintf(buffer, sizeof(buffer), "SCORE %07d", score);
    draw_text_centered(236.0f, 2.4f, buffer, COL_GOLD);
    snprintf(buffer, sizeof(buffer), "WAVE %02d  DEMONS %03d", wave_number, kills);
    draw_text_centered(270.0f, 1.8f, buffer, COL_BONE);
    if(((int)(time * 2.0f) & 1) == 0)
        draw_text_centered(315.0f, 1.65f, "PRESS START TO RETRY", COL_WHITE);
}

static void draw_wave_transition(float time) {
    static const char *messages[5] = {
        "THE ABYSS OPENS",
        "DEMONS GET WORSE",
        "HELL ENGINE REDLINES",
        "HORNS LOCKED",
        "THE PIT RELOADS"
    };
    char buffer[64];
    int next_wave = wave_number + 1;
    int next_target = wave_target_for(next_wave);
    float elapsed = WAVE_TRANSITION_SECONDS - wave_transition_time;
    float progress = clampf(elapsed / WAVE_TRANSITION_SECONDS, 0.0f, 1.0f);
    float ritual = fsin(progress * PI);
    float center_x = SCREEN_W * 0.5f + fsin(time * 3.1f) * 7.0f * ritual;
    float center_y = SCREEN_H * 0.5f + fcos(time * 2.7f) * 5.0f * ritual;
    uint32_t accent = (next_wave % 5) == 0 ? COL_CYAN : COL_MAGENTA;
    const char *message = (next_wave % 5) == 0 ?
                          "BOSS WAVE INBOUND" : messages[next_wave % 5];
    int index;

    active_header = &ui_header;
    draw_rect_2d(0.0f, 0.0f, SCREEN_W, SCREEN_H, 58.0f,
                 color_alpha(COL_BLACK, 0.56f + ritual * 0.22f));

    /* Radial alarm machinery: two rotating crowns and a pentagram aperture. */
    for(index = 0; index < 32; ++index) {
        float angle = (float)index * TAU / 32.0f +
                      time * ((index & 1) ? -0.42f : 0.31f);
        float inner = 76.0f + (float)(index & 3) * 7.0f;
        float outer = 185.0f + ritual * 150.0f +
                      (float)(index % 5) * 9.0f;
        draw_line_2d(center_x + fcos(angle) * inner,
                     center_y + fsin(angle) * inner,
                     center_x + fcos(angle) * outer,
                     center_y + fsin(angle) * outer,
                     (index % 4) == 0 ? 5.0f : 2.0f, 63.0f,
                     color_alpha((index & 1) ? accent : COL_GOLD,
                                 0.24f + ritual * 0.38f));
    }
    for(index = 0; index < 48; ++index) {
        float a0 = (float)index * TAU / 48.0f + time * 0.55f;
        float a1 = (float)(index + 1) * TAU / 48.0f + time * 0.55f;
        float radius = 120.0f + fsin(time * 5.0f) * 10.0f;
        draw_line_2d(center_x + fcos(a0) * radius,
                     center_y + fsin(a0) * radius,
                     center_x + fcos(a1) * radius,
                     center_y + fsin(a1) * radius,
                     3.0f, 65.0f, color_alpha(accent, 0.72f));
    }
    for(index = 0; index < 5; ++index) {
        float a0 = -PI * 0.5f + (float)(index * 2) * TAU / 5.0f - time * 0.7f;
        float a1 = -PI * 0.5f + (float)((index * 2 + 2) % 5) * TAU / 5.0f -
                   time * 0.7f;
        draw_line_2d(center_x + fcos(a0) * 96.0f,
                     center_y + fsin(a0) * 96.0f,
                     center_x + fcos(a1) * 96.0f,
                     center_y + fsin(a1) * 96.0f,
                     4.0f, 66.0f, color_alpha(COL_ORANGE, 0.86f));
    }

    /* Glitch shutters chop across the ritual without touching the copy. */
    for(index = 0; index < 11; ++index) {
        uint32_t seed = (uint32_t)index * 0x9e3779b9u +
                        (uint32_t)(elapsed * 18.0f);
        float y = 28.0f + (float)index * 40.0f + hash_signed(seed) * 13.0f;
        float width = 42.0f + (hash_signed(seed + 31u) * 0.5f + 0.5f) * 190.0f;
        float x = hash_signed(seed + 71u) * 250.0f + SCREEN_W * 0.5f;
        draw_rect_2d(x - width * 0.5f, y, width, 2.0f + (float)(index & 3),
                     67.0f,
                     color_alpha((index & 1) ? COL_CRIMSON : COL_CYAN,
                                 0.20f + ritual * 0.30f));
    }

    draw_rect_2d(82.0f, 112.0f, 476.0f, 252.0f, 70.0f,
                 color_alpha(COL_BLACK, 0.90f));
    draw_rect_2d(82.0f, 112.0f, 476.0f, 5.0f, 75.0f, accent);
    draw_rect_2d(82.0f, 359.0f, 476.0f, 5.0f, 75.0f, COL_GOLD);
    draw_rect_2d(82.0f, 112.0f, 5.0f, 252.0f, 75.0f, COL_CRIMSON);
    draw_rect_2d(553.0f, 112.0f, 5.0f, 252.0f, 75.0f, COL_ORANGE);

    snprintf(buffer, sizeof(buffer), "WAVE %02d", next_wave);
    draw_text_centered(143.0f, 5.2f, buffer, COL_BLACK);
    draw_text_centered(138.0f, 5.2f, buffer,
                       (next_wave % 5) == 0 ? COL_CYAN : COL_RED);
    draw_text_centered(204.0f, 1.85f, message, COL_GOLD);
    draw_text_centered(237.0f, 1.2f, "DEMON THREAT ASSESSMENT", COL_BONE);
    snprintf(buffer, sizeof(buffer), "DEMON QUOTA %02d", next_target);
    draw_text_centered(266.0f, 1.8f, buffer, COL_WHITE);
    snprintf(buffer, sizeof(buffer), "HOSTILITY LEVEL %02d", next_wave);
    draw_text_centered(296.0f, 1.55f, buffer, accent);
    draw_text_centered(329.0f, 1.15f, "REALITY RECOMPILING", COL_ORANGE);

    for(index = 0; index < 16; ++index) {
        uint32_t color = (float)index / 16.0f < progress ? accent : 0xff28101cu;
        draw_rect_2d(132.0f + (float)index * 24.0f, 344.0f,
                     17.0f, 5.0f, 78.0f, color);
    }
    if(((int)(elapsed * 14.0f) & 7) == 0)
        draw_rect_2d(0.0f, 0.0f, SCREEN_W, SCREEN_H, 77.0f,
                     color_alpha(COL_WHITE, 0.08f));
}

static void render_frame(float time) {
    int index;
    setup_camera(time);
    pvr_set_bg_color(0.025f + flash_amount * 0.24f,
                     0.0f,
                     0.018f + flash_amount * 0.16f);

    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    active_header = &opaque_header;
    draw_hell_engine(time);
    draw_arena(time);
    draw_cinematic_opaque(time);

    if(game_mode == MODE_TITLE) {
        draw_hunter(time, 1);
    }
    else {
        draw_powerups(time);
        draw_hunter(time, 0);
        for(index = 0; index < MAX_ENEMIES; ++index) {
            if(enemies[index].active)
                draw_enemy(&enemies[index], time);
        }
        draw_bolts();
    }

    pvr_list_finish();
    pvr_list_begin(PVR_LIST_TR_POLY);
    draw_flames(time);
    draw_sky_effects(time);
    draw_cinematic_translucent(time);
    draw_particles();
    draw_screen_effects(time);
    if(game_mode == MODE_PLAY) {
        if(wave_transition_time > 0.0f)
            draw_wave_transition(time);
        else
            draw_hud();
    }
    else if(game_mode == MODE_TITLE)
        draw_title(time);
    else
        draw_game_over(time);
    pvr_list_finish();
    pvr_scene_finish();
}

int main(int argc, char **argv) {
    pvr_init_params_t pvr_params = {
        .opb_sizes = {PVR_BINSIZE_32, PVR_BINSIZE_0, PVR_BINSIZE_32,
                      PVR_BINSIZE_0, PVR_BINSIZE_0},
        .vertex_buf_size = 1536 * 1024,
        .dma_enabled = 0,
        .fsaa_enabled = 0,
        .autosort_disabled = 0,
        .opb_overflow_count = 5,
        .vbuf_doublebuf_disabled = 0
    };
    uint64_t previous_time;
    int running = 1;

    (void)argc;
    (void)argv;

    printf("Demon Bazooka: booting procedural demon hunt\n");
    if(pvr_init(&pvr_params) < 0) {
        printf("Demon Bazooka: PVR initialization failed\n");
        return 1;
    }
    init_headers();
    init_sound();
    setup_camera(0.0f);
#if defined(WAVE_TRANSITION_QA)
    /* Visual test harness: never enabled by the release Makefile. */
    reset_game();
    wave_kills = wave_target;
    begin_wave_transition();
#endif
#if defined(GAMEPLAY_CLARITY_QA)
    /* Populate a safe active wave for framebuffer readability checks. */
    reset_game();
    invulnerability = 999.0f;
    spawn_enemy();
    spawn_enemy();
    spawn_enemy();
    spawn_enemy();
    spawn_enemy();
    spawn_enemy();
#endif
    previous_time = timer_ms_gettime64();

    while(running) {
        uint64_t current_time = timer_ms_gettime64();
        float dt = (float)(current_time - previous_time) * 0.001f;
        cont_state_t *state = read_controller();
        uint32_t buttons = state ? state->buttons : 0;
        uint32_t pressed = buttons & ~previous_buttons;
        float total_time = (float)current_time * 0.001f;

        previous_time = current_time;
        previous_buttons = buttons;
        dt = clampf(dt, 0.001f, 0.05f);
        mode_time += dt;
        voice_cooldown -= dt;

        /* Flycast commonly maps Return/Enter to Dreamcast Start. Start used to
           terminate here, leaving the emulator on a black screen. Reserve a
           deliberate Start+Y chord for quitting instead. */
        if(game_mode == MODE_PLAY && (pressed & CONT_START) && state &&
           (state->buttons & CONT_Y)) {
            running = 0;
            continue;
        }

        if(game_mode == MODE_PLAY) {
            if(wave_transition_time > 0.0f)
                update_wave_transition(dt);
            else
                update_game(dt, state, pressed);
        }
        else {
            update_title(dt);
            if(pressed & (CONT_A | CONT_START))
                reset_game();
        }

        render_frame(total_time);
    }

    shutdown_sound();
    printf("Demon Bazooka: extraction complete; high score %d\n", high_score);
    return 0;
}
