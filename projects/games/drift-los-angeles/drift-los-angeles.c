/*
 * Drift Los Angeles -- open-city street drifting for Sega Dreamcast.
 *
 * The renderer submits native PowerVR opaque and translucent polygon lists.
 * The world is a deterministic streamed city grid, so there is no loading edge
 * even though the visible geometry stays within a real-Dreamcast budget.
 */

#include <kos.h>

#include <dc/biosfont.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/pvr.h>
#include <dc/sound/aica_comm.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/sound/stream.h>
#include <dc/video.h>

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets/generated/audio_assets.h"
#include "assets/generated/music_asset.h"
#include "assets/generated/texture_assets.h"
#include "model_data.h"
#include "pedestrian_data.h"
#include "render_math.h"
#include "render_visibility.h"

KOS_INIT_FLAGS(INIT_DEFAULT);

#define SCREEN_W 640.0f
#define SCREEN_H 480.0f
#define SCREEN_CX 320.0f
#define SCREEN_CY 240.0f
#define PI 3.14159265358979323846f
#define NEAR_PLANE 0.35f
#define FAR_PLANE 760.0f
#define CITY_CELL 120.0f
#define ROAD_HALF 14.0f
#define DRAW_RADIUS 3
#define GROUND_RADIUS 4
#define MAX_TRAFFIC 36
#define MAX_SMOKE 160
#define MAX_EXHAUST_FLAMES 24
#define MAX_SKIDS 192
#define MAX_HIT_PEDESTRIANS 24
#define CAR_MASS 1490.0f
#define CAR_INERTIA 2520.0f
/* Two overlapping discs approximate the coupe's footprint against street
   obstacles, so an off-centre lamppost or bumper hit turns the car instead
   of only stopping it. */
#define CAR_COLLIDER_OFFSET 1.35f
#define CAR_COLLIDER_RADIUS 1.02f
#define LAMP_POST_RADIUS 0.13f
#define PARKED_HALF_WIDTH 0.92f
#define PARKED_HALF_LENGTH 2.18f
#define PEDESTRIAN_RADIUS 0.30f
#define HUD_W 512
#define HUD_H 256
#define HUD_BYTES (HUD_W * HUD_H * sizeof(uint16_t))
#define ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

#ifndef DRIFT_LA_CAPTURE_LOCAL
#define DRIFT_LA_CAPTURE_LOCAL 8.0f
#endif
#ifndef DRIFT_LA_CAPTURE_SPAN
#define DRIFT_LA_CAPTURE_SPAN 4.0f
#endif

typedef struct { float x, y, z; } vec3_t;
typedef struct { float r, g, b; } color3_t;
typedef struct { float x, y, z; bool valid; } screen_point_t;

typedef enum {
    MODE_TITLE,
    MODE_PLAYING,
    MODE_PAUSED,
    MODE_DEMO
} game_mode_t;

typedef enum {
    DISTRICT_DOWNTOWN,
    DISTRICT_COAST,
    DISTRICT_ARTS,
    DISTRICT_NEON
} district_t;

typedef struct {
    float x, z;
    float yaw;
    float longitudinal;
    float lateral;
    float yaw_rate;
    float steer;
    float rear_grip_scale;
    /* Ground-speed equivalent at the driven rear tire tread. Keeping this
       separate from chassis speed lets the rear axle spin while the front
       brakes hold the car during a burnout. */
    float rear_wheel_speed;
    float rear_power_slip;
    float burnout;
    int drive_direction;
    float brake_light;
    float handbrake_lock;
    float clutch_kick_timer;
} car_t;

typedef struct {
    bool active;
    float x, y, z;
    float vx, vy, vz;
    float life, max_life;
    float size;
} smoke_t;

typedef smoke_t exhaust_flame_t;

typedef struct {
    bool active;
    float x1, z1, x2, z2;
    float life;
} skid_t;

typedef struct {
    bool exists;
    float cx, cz, width, depth, height;
    int texture;
    district_t district;
    uint32_t seed;
} building_t;

typedef struct {
    bool active;
    float x, z;
    float yaw;
    float speed;
    float cruise_speed;
    float brake_light;
    int axis;
    int direction;
    color3_t color;
    uint32_t seed;
} traffic_t;

typedef struct {
    float x, z, heading, phase;
    uint32_t seed;
} pedestrian_pose_t;

/* Walking pedestrians are stateless and follow the game clock along their
   sidewalk. Only the ones the car has hit need simulation state, so this
   records which block slot each downed figure replaces. */
typedef struct {
    bool active;
    bool grounded;
    int cell_x, cell_z, slot;
    uint32_t seed;
    float x, y, z;
    float vx, vy, vz;
    float heading;
    float tumble, tumble_rate;
    float rest_tumble, settle;
    float timer;
} hit_pedestrian_t;

typedef struct {
    bool connected;
    uint32_t buttons;
    uint32_t pressed;
    float steer;
    float throttle;
    float brake;
} input_t;

typedef struct {
    game_mode_t mode;
    float time;
    float score;
    float best_score;
    float drift_chain;
    float drift_multiplier;
    float drift_angle;
    float drift_hold;
    float drift_duration;
    float longest_drift;
    int drift_bonus_step;
    float smoke_timer;
    float exhaust_pop_timer;
    float exhaust_flash;
    float skid_timer;
    float impact_flash;
    float hud_timer;
    float district_banner;
    float demo_time;
    int demo_segment;
    district_t district;
    uint32_t previous_buttons;
    bool camera_close;
} game_t;

static car_t car;
static game_t game;
static traffic_t traffic[MAX_TRAFFIC];
static smoke_t smoke_pool[MAX_SMOKE];
static exhaust_flame_t exhaust_flame_pool[MAX_EXHAUST_FLAMES];
static skid_t skid_pool[MAX_SKIDS];
static hit_pedestrian_t hit_pedestrians[MAX_HIT_PEDESTRIANS];
static int smoke_cursor;
static int exhaust_flame_cursor;
static int skid_cursor;
static float front_wheel_spin;
static float rear_wheel_spin;
static uint32_t traffic_random_state = 0x7c7d41f3u;
static uint32_t effect_random_state = 0x63d83595u;
static vec3_t last_skid_position[2];
static bool skid_contact_valid;

#define AUDIO_RATE 22050
#define AUDIO_ASSET_RATE 32000
#define AUDIO_PHASE_PER_HZ 194783.0f
#define AUDIO_SINE_LUT_SIZE 1024
#define AUDIO_IDLE_BUS_VOLUME 155.0f
#define AUDIO_LOAD_BUS_VOLUME 165.0f
#define AUDIO_TIRE_BUS_VOLUME 105.0f
#define AUDIO_STREAM_BUS_VOLUME 115

#if DLA_MUSIC_RATE != AUDIO_RATE
#error "The embedded soundtrack must match the software stream rate"
#endif

typedef struct {
    float rpm;
    float throttle;
    float engine_load;
    float speed;
    float front_slip;
    float rear_slip;
    float handbrake;
    float clutch;
    float brake;
    float offroad;
    float steer;
    float yaw_rate;
    float traffic_level;
    float traffic_pan;
    float traffic_speed;
    int gear;
    uint32_t backfire_serial;
    uint32_t impact_serial;
    uint32_t clutch_serial;
    uint32_t handbrake_serial;
    uint32_t shift_serial;
    float backfire_strength;
    float impact_strength;
} audio_controls_t;

typedef struct {
    uint32_t crank_phase;
    uint32_t firing_phase;
    uint32_t intake_phase;
    uint32_t pipe_phase_left;
    uint32_t pipe_phase_right;
    uint32_t transmission_phase;
    uint32_t differential_phase;
    uint32_t tire_phase_front;
    uint32_t tire_phase_rear;
    uint32_t road_phase;
    uint32_t traffic_phase;
    uint32_t ambience_phase;
    uint32_t music_track;
    uint32_t music_frame;
    int music_predictor[2];
    int music_step_index[2];
    uint32_t noise_state;
    uint32_t seen_backfire_serial;
    uint32_t seen_impact_serial;
    uint32_t seen_clutch_serial;
    uint32_t seen_handbrake_serial;
    uint32_t seen_shift_serial;
    int firing_index;
    float rpm;
    float throttle;
    float engine_load;
    float speed;
    float front_slip;
    float rear_slip;
    float handbrake;
    float offroad;
    float fire_short_left;
    float fire_short_right;
    float fire_body_left;
    float fire_body_right;
    float intake_lowpass;
    float tire_fast_left;
    float tire_fast_right;
    float tire_slow_left;
    float tire_slow_right;
    float road_fast_left;
    float road_fast_right;
    float road_slow_left;
    float road_slow_right;
    float wind_lowpass_left;
    float wind_lowpass_right;
    float city_lowpass;
    float output_dc_left;
    float output_dc_right;
    float backfire_crack;
    float backfire_tail;
    float impact_envelope;
    float clutch_envelope;
    float handbrake_envelope;
    float shift_envelope;
    float music_gain;
} audio_synth_t;

typedef struct {
    sfxhnd_t handle;
    int channel;
    int last_frequency;
    int last_volume;
} audio_voice_t;

static snd_stream_hnd_t audio_stream=SND_STREAM_INVALID;
static int16_t audio_samples[16384] __attribute__((aligned(32)));
static int16_t audio_sine_lut[AUDIO_SINE_LUT_SIZE];
/* IMA ADPCM tables for the soundtrack bank; tools/build_music_asset.py
   encodes with bit-identical arithmetic. */
static const int16_t audio_adpcm_steps[89]={
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,
    73,80,88,97,107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,
    449,494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552,1707,1878,
    2066,2272,2499,2749,3024,3327,3660,4026,4428,4871,5358,5894,6484,7132,
    7845,8630,9493,10442,11487,12635,13899,15289,16818,18500,20350,22385,
    24623,27086,29794,32767
};
static const int8_t audio_adpcm_index_delta[16]={
    -1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8
};
static audio_controls_t audio_controls={
    .rpm=900.0f,
    .engine_load=.12f,
    .gear=1
};
static audio_synth_t audio_synth;
static float audio_shift_cooldown;
static float audio_impact_cooldown;
static audio_voice_t audio_idle_voice;
static audio_voice_t audio_load_voice;
static audio_voice_t audio_tire_voice;

static pvr_ptr_t texture_vram[DLA_TEXTURE_COUNT];
static pvr_poly_hdr_t texture_headers[DLA_TEXTURE_COUNT];
static pvr_poly_hdr_t backdrop_header;
static pvr_poly_hdr_t title_header;
static pvr_poly_hdr_t reflection_header;
static uint32_t graphics_texture_bytes;
static uint32_t graphics_vram_free;
#define GRAPHICS_VERTEX_BUFFER_BYTES (768u * 1024u)
#define MAX_CAR_MESH_VERTICES 4096
/* SH4 has a 16 KiB direct-mapped operand cache. Separate 4096-element
   arrays put the per-vertex writes in competing cache lines. Keep draw data
   together in the first 32-byte line and transform metadata in the second. */
typedef struct {
    screen_point_t projected;
    uint32_t color,reflect_color;
    float reflect_uv[2];
    vec3_t world;
    uint8_t material;
    bool referenced;
    uint8_t padding[18];
} car_render_vertex_t;
_Static_assert(sizeof(car_render_vertex_t)==64,"car cache record must span two lines");
_Static_assert(offsetof(car_render_vertex_t,world)==32,"car draw data must fit one line");
static _Alignas(32) car_render_vertex_t car_render[MAX_CAR_MESH_VERTICES];
static bool car_visible_faces[4096];
static vec3_t car_face_normals[4096];
typedef struct { float x,z,scale,depth; } palm_draw_t;
static palm_draw_t palm_draws[64];
#define MAX_PED_SHADOWS 48
typedef struct { float x,z,yaw,length,opacity; } ped_shadow_t;
static ped_shadow_t ped_shadow_draws[MAX_PED_SHADOWS];
static int ped_shadow_count;
static int palm_draw_count;
static pvr_poly_hdr_t world_header;
static pvr_poly_hdr_t translucent_header;
static pvr_poly_hdr_t additive_header;
static pvr_poly_hdr_t hud_header;
static pvr_poly_hdr_t hud_texture_header;
static pvr_poly_hdr_t *active_poly_header;
static pvr_ptr_t hud_texture;
static uint16_t *hud_pixels;

#if defined(DRIFT_LA_GEOMETRY_QA) && !defined(DRIFT_LA_VISUAL_QA)
#error "DRIFT_LA_GEOMETRY_QA requires DRIFT_LA_VISUAL_QA"
#endif
#if defined(DRIFT_LA_GEOMETRY_QA) && !defined(DRIFT_LA_SHOWCASE)
#error "DRIFT_LA_GEOMETRY_QA requires the complete DRIFT_LA_SHOWCASE tour"
#endif
#if defined(DRIFT_LA_GEOMETRY_QA) && defined(DRIFT_LA_CAPTURE_SEGMENT)
#error "DRIFT_LA_GEOMETRY_QA cannot use a looping capture segment"
#endif

#ifdef DRIFT_LA_VISUAL_QA
typedef struct {
    uint32_t triangles,vertices;
    uint32_t buildings,vehicles,pedestrians,smoke_particles,furnishing_clusters;
} render_qa_t;
static render_qa_t render_qa;
static render_qa_t render_qa_peak;
#ifdef DRIFT_LA_GEOMETRY_QA
static uint64_t geometry_qa_triangles,geometry_qa_vertices;
#define QA_TOTAL_GEOMETRY(triangle_count,vertex_count) do { \
    geometry_qa_triangles+=(uint32_t)(triangle_count); \
    geometry_qa_vertices+=(uint32_t)(vertex_count); \
} while(0)
#else
#define QA_TOTAL_GEOMETRY(triangle_count,vertex_count) ((void)0)
#endif
#define QA_GEOMETRY(triangle_count,vertex_count) do { \
    render_qa.triangles+=(uint32_t)(triangle_count); \
    render_qa.vertices+=(uint32_t)(vertex_count); \
    QA_TOTAL_GEOMETRY(triangle_count,vertex_count); \
} while(0)
#define QA_COUNT(member) (++render_qa.member)
#else
#define QA_GEOMETRY(triangle_count,vertex_count) ((void)0)
#define QA_COUNT(member) ((void)0)
#endif

static float camera_x, camera_y, camera_z;
static float camera_yaw;
static float camera_sin_yaw, camera_cos_yaw;
static float camera_sin_pitch, camera_cos_pitch;
static float camera_sin_roll, camera_cos_roll;
static float camera_focal = 430.0f;
static bool camera_initialized;
static dla_frustum_t scene_frustum;

static void draw_traffic_car(const traffic_t *vehicle);

static void reset_traffic(void);
static int traffic_signal_state(int cell_x, int cell_z);
static void emit_smoke(float lx, float lz);
static void update_exhaust_pops(float throttle, float rpm_level, float dt);
static void update_hit_pedestrians(float dt);
static void emit_skid(float lx, int wheel);
static float clampf(float value, float low, float high);

static inline float audio_sine(uint32_t phase) {
    return (float)audio_sine_lut[phase>>(32-10)]*(1.0f/32768.0f);
}

static inline float audio_triangle(uint32_t phase) {
    const float ramp=(float)(phase>>16)*(1.0f/65535.0f);
    return 1.0f-4.0f*fabsf(ramp-.5f);
}

static inline uint32_t audio_phase_step(float hz) {
    return (uint32_t)(fmaxf(hz,0.0f)*AUDIO_PHASE_PER_HZ);
}

static inline float audio_random_bipolar(audio_synth_t *synth) {
    int32_t value;
    synth->noise_state=synth->noise_state*1664525u+1013904223u;
    value=(int32_t)(uint16_t)(synth->noise_state>>16)-32768;
    return (float)value*(1.0f/32768.0f);
}

static inline float audio_bipolar_envelope(float envelope) {
    return envelope*(2.0f*envelope-1.0f);
}

static inline float audio_soft_clip(float sample) {
    sample=clampf(sample,-1.15f,1.15f);
    return sample*(1.0f-.12f*sample*sample);
}

static inline int audio_decode_adpcm(unsigned code, int *predictor, int *index) {
    const int step=audio_adpcm_steps[*index];
    int diff=step>>3;
    if(code&4u) diff+=step;
    if(code&2u) diff+=step>>1;
    if(code&1u) diff+=step>>2;
    *predictor+=(code&8u)?-diff:diff;
    if(*predictor>32767) *predictor=32767;
    else if(*predictor<-32768) *predictor=-32768;
    *index+=audio_adpcm_index_delta[code];
    if(*index<0) *index=0;
    else if(*index>88) *index=88;
    return *predictor;
}

static void audio_start_music_track(audio_synth_t *synth, uint32_t track) {
    synth->music_track=track%DLA_MUSIC_TRACK_COUNT;
    synth->music_frame=0u;
    synth->music_predictor[0]=synth->music_predictor[1]=0;
    synth->music_step_index[0]=synth->music_step_index[1]=0;
    printf("Drift Los Angeles: now playing \"%s\" (%u/%u).\n",
           dla_music_tracks[synth->music_track].title,
           (unsigned)synth->music_track+1u,(unsigned)DLA_MUSIC_TRACK_COUNT);
}

static void audio_trigger_backfire(float strength) {
    audio_controls.backfire_strength=clampf(strength,.25f,1.25f);
    ++audio_controls.backfire_serial;
}

static void audio_trigger_impact(float strength) {
    if(audio_impact_cooldown>0.0f) return;
    audio_controls.impact_strength=clampf(strength,.30f,1.25f);
    ++audio_controls.impact_serial;
    audio_impact_cooldown=.14f;
}

static void audio_trigger_clutch(void) {
    ++audio_controls.clutch_serial;
}

static void audio_trigger_handbrake(void) {
    ++audio_controls.handbrake_serial;
}

static bool audio_start_recorded_voice(audio_voice_t *voice,
                                       const int16_t *pcm,
                                       size_t byte_count,
                                       uint32_t frame_count) {
    sfx_play_data_t playback;
    memset(voice,0,sizeof(*voice));
    voice->channel=-1;
    voice->last_frequency=-1;
    voice->last_volume=-1;
    voice->handle=snd_sfx_load_raw_buf((char *)(void *)pcm,byte_count,
                                       AUDIO_ASSET_RATE,16,2);
    if(voice->handle==SFXHND_INVALID) return false;
    memset(&playback,0,sizeof(playback));
    playback.chn=-1;
    playback.idx=voice->handle;
    playback.vol=0;
    playback.pan=128;
    playback.loop=1;
    playback.freq=AUDIO_ASSET_RATE;
    playback.loopstart=0;
    playback.loopend=frame_count;
    voice->channel=snd_sfx_play_ex(&playback);
    if(voice->channel<0) {
        snd_sfx_unload(voice->handle);
        voice->handle=SFXHND_INVALID;
        return false;
    }
    return true;
}

static bool audio_voice_needs_update(const audio_voice_t *voice,
                                     int frequency, int volume) {
    return voice->channel>=0&&
           (voice->last_frequency!=frequency||voice->last_volume!=volume);
}

static void audio_queue_voice_update(audio_voice_t *voice,
                                     int frequency, int volume) {
    AICA_CMDSTR_CHANNEL(packet,command,channel);
    if(voice->channel<0) return;
    command->cmd=AICA_CMD_CHAN;
    command->timestamp=0;
    command->size=AICA_CMDSTR_CHANNEL_SIZE;
    command->cmd_id=(uint32_t)voice->channel;
    channel->cmd=AICA_CH_CMD_UPDATE|AICA_CH_UPDATE_SET_FREQ|
                 AICA_CH_UPDATE_SET_VOL;
    channel->freq=(uint32_t)frequency;
    channel->vol=(uint32_t)volume;
    snd_sh4_to_aica(packet,command->size);
    command->cmd_id=(uint32_t)(voice->channel+1);
    snd_sh4_to_aica(packet,command->size);
    voice->last_frequency=frequency;
    voice->last_volume=volume;
}

static void audio_update_recorded_voices(void) {
    const float rpm=audio_controls.rpm;
    float band_blend=clampf((rpm-1350.0f)/1050.0f,0.0f,1.0f);
    float idle_weight,load_weight;
    const float tire_weight=fmaxf(audio_controls.rear_slip,
                                  audio_controls.front_slip*.76f)*
                            (1.0f-audio_controls.offroad*.72f);
    const float engine_gain=game.mode==MODE_TITLE?.68f:
                            (game.mode==MODE_PAUSED?.46f:1.0f);
    int idle_frequency,load_frequency;
    const int tire_frequency=(int)clampf((float)AUDIO_ASSET_RATE*
        (.82f+audio_controls.speed*.0042f+audio_controls.rear_slip*.27f),
        18000.0f,56000.0f);

    /* Late-1990s-style sample banks keep every recording close to its natural
       rate. The smooth crossfade supplies the full tachometer sweep without
       turning a short throttle blip into a rapidly repeating pulse. */
    band_blend=band_blend*band_blend*(3.0f-2.0f*band_blend);
    idle_weight=sqrtf(1.0f-band_blend)*
        (.90f+.10f*(1.0f-audio_controls.throttle));
    load_weight=sqrtf(band_blend)*
        (.40f+.60f*audio_controls.engine_load);
    idle_frequency=(int)((float)AUDIO_ASSET_RATE*clampf(
        rpm/DLA_ENGINE_IDLE_BASE_RPM,.84f,1.95f));
    load_frequency=(int)((float)AUDIO_ASSET_RATE*clampf(
        rpm/DLA_ENGINE_LOAD_BASE_RPM,.52f,1.90f));

    const int idle_volume=(int)clampf(idle_weight*engine_gain*
        AUDIO_IDLE_BUS_VOLUME,0.0f,255.0f);
    const int load_volume=(int)clampf(load_weight*engine_gain*
        AUDIO_LOAD_BUS_VOLUME,0.0f,255.0f);
    const int tire_volume=(int)clampf(tire_weight*AUDIO_TIRE_BUS_VOLUME,
                                     0.0f,255.0f);
    const bool changed=
        audio_voice_needs_update(&audio_idle_voice,idle_frequency,idle_volume)||
        audio_voice_needs_update(&audio_load_voice,load_frequency,load_volume)||
        audio_voice_needs_update(&audio_tire_voice,tire_frequency,tire_volume);
    if(!changed) return;
    snd_sh4_to_aica_stop();
    if(audio_voice_needs_update(&audio_idle_voice,idle_frequency,idle_volume))
        audio_queue_voice_update(&audio_idle_voice,idle_frequency,idle_volume);
    if(audio_voice_needs_update(&audio_load_voice,load_frequency,load_volume))
        audio_queue_voice_update(&audio_load_voice,load_frequency,load_volume);
    if(audio_voice_needs_update(&audio_tire_voice,tire_frequency,tire_volume))
        audio_queue_voice_update(&audio_tire_voice,tire_frequency,tire_volume);
    snd_sh4_to_aica_start();
}

static void audio_release_recorded_voice(audio_voice_t *voice) {
    if(voice->channel>=0) {
        snd_sfx_stop(voice->channel);
        snd_sfx_stop(voice->channel+1);
        voice->channel=-1;
    }
    if(voice->handle!=SFXHND_INVALID) {
        snd_sfx_unload(voice->handle);
        voice->handle=SFXHND_INVALID;
    }
}

static void *audio_callback(snd_stream_hnd_t hnd, int bytes, int *actual) {
    audio_synth_t *synth=&audio_synth;
    int frames,i;
    (void)hnd;
    if(bytes>(int)sizeof(audio_samples)) bytes=(int)sizeof(audio_samples);
    frames=bytes/4;

    if(synth->seen_backfire_serial!=audio_controls.backfire_serial) {
        synth->seen_backfire_serial=audio_controls.backfire_serial;
        synth->backfire_crack=fmaxf(synth->backfire_crack,
                                    audio_controls.backfire_strength);
        synth->backfire_tail=fmaxf(synth->backfire_tail,
                                   audio_controls.backfire_strength);
        synth->pipe_phase_left=0x40000000u;
        synth->pipe_phase_right=0x40000000u;
    }
    if(synth->seen_impact_serial!=audio_controls.impact_serial) {
        synth->seen_impact_serial=audio_controls.impact_serial;
        synth->impact_envelope=fmaxf(synth->impact_envelope,
                                     audio_controls.impact_strength);
    }
    if(synth->seen_clutch_serial!=audio_controls.clutch_serial) {
        synth->seen_clutch_serial=audio_controls.clutch_serial;
        synth->clutch_envelope=1.0f;
    }
    if(synth->seen_handbrake_serial!=audio_controls.handbrake_serial) {
        synth->seen_handbrake_serial=audio_controls.handbrake_serial;
        synth->handbrake_envelope=1.0f;
    }
    if(synth->seen_shift_serial!=audio_controls.shift_serial) {
        synth->seen_shift_serial=audio_controls.shift_serial;
        synth->shift_envelope=1.0f;
    }

    for(i=0;i<frames;++i) {
        float noise_left=audio_random_bipolar(synth);
        float noise_right=audio_random_bipolar(synth);
        float noise_center=(noise_left+noise_right)*.5f;
        float tire_left,tire_right,tire_level;
        float road_level,road_left,road_right,wind_level,wind_left,wind_right;
        float transmission,traffic,city_ambience;
        float backfire,impact,control_noise,events;
        float music_left,music_right,music_target,music_duck;
        float mix_left,mix_right;
        int output_left,output_right;

        synth->rpm+=(audio_controls.rpm-synth->rpm)*.00105f;
        synth->throttle+=(audio_controls.throttle-synth->throttle)*.00155f;
        synth->speed+=(audio_controls.speed-synth->speed)*.00150f;
        synth->front_slip+=(audio_controls.front_slip-synth->front_slip)*.00195f;
        synth->rear_slip+=(audio_controls.rear_slip-synth->rear_slip)*.00220f;
        synth->offroad+=(audio_controls.offroad-synth->offroad)*.00160f;

        synth->pipe_phase_left+=audio_phase_step(82.0f);
        synth->pipe_phase_right+=audio_phase_step(87.0f);
        synth->transmission_phase+=audio_phase_step(92.0f+synth->rpm*.105f);
        synth->differential_phase+=audio_phase_step(58.0f+synth->speed*13.5f);
        synth->road_phase+=audio_phase_step(14.0f+synth->speed*2.4f);
        synth->traffic_phase+=audio_phase_step(54.0f+audio_controls.traffic_speed*3.2f);
        synth->ambience_phase+=audio_phase_step(.115f);

        synth->tire_fast_left+=(noise_left-synth->tire_fast_left)*.43f;
        synth->tire_fast_right+=(noise_right-synth->tire_fast_right)*.43f;
        synth->tire_slow_left+=(synth->tire_fast_left-synth->tire_slow_left)*.075f;
        synth->tire_slow_right+=(synth->tire_fast_right-synth->tire_slow_right)*.075f;
        tire_level=fmaxf(synth->rear_slip,synth->front_slip*.72f);
        tire_left=(synth->tire_fast_left-synth->tire_slow_left)*
                  tire_level*.16f;
        tire_right=(synth->tire_fast_right-synth->tire_slow_right)*
                   tire_level*.16f;

        synth->road_fast_left+=(noise_left-synth->road_fast_left)*.060f;
        synth->road_fast_right+=(noise_right-synth->road_fast_right)*.060f;
        synth->road_slow_left+=(synth->road_fast_left-synth->road_slow_left)*.009f;
        synth->road_slow_right+=(synth->road_fast_right-synth->road_slow_right)*.009f;
        road_level=clampf(synth->speed/52.0f,0.0f,1.0f);
        road_left=((synth->road_fast_left-synth->road_slow_left)*.31f+
                   audio_triangle(synth->road_phase)*.027f)*road_level;
        road_right=((synth->road_fast_right-synth->road_slow_right)*.31f+
                    audio_triangle(synth->road_phase+0x19000000u)*.027f)*road_level;
        if(synth->offroad>.01f) {
            road_left+=(fabsf(noise_left)>.76f?noise_left*.42f:noise_left*.10f)*
                       synth->offroad*road_level;
            road_right+=(fabsf(noise_right)>.76f?noise_right*.42f:noise_right*.10f)*
                        synth->offroad*road_level;
        }

        synth->wind_lowpass_left+=(noise_left-synth->wind_lowpass_left)*.018f;
        synth->wind_lowpass_right+=(noise_right-synth->wind_lowpass_right)*.018f;
        wind_level=clampf((synth->speed-19.0f)/62.0f,0.0f,1.0f);
        wind_level*=wind_level;
        wind_left=(noise_left-synth->wind_lowpass_left)*wind_level*.105f;
        wind_right=(noise_right-synth->wind_lowpass_right)*wind_level*.105f;

        transmission=(audio_sine(synth->transmission_phase)*.055f+
                      audio_sine(synth->differential_phase)*.035f)*
                     road_level*(.18f+.82f*synth->throttle);
        traffic=audio_sine(synth->traffic_phase)*audio_controls.traffic_level*.075f;
        synth->city_lowpass+=(noise_center-synth->city_lowpass)*.0045f;
        city_ambience=(synth->city_lowpass*.027f+
                       audio_sine(synth->ambience_phase)*.012f)*
                      (game.mode==MODE_TITLE?.35f:1.0f);

        backfire=audio_bipolar_envelope(synth->backfire_crack)*.90f+
                 noise_center*synth->backfire_crack*.34f+
                 audio_sine(synth->pipe_phase_left)*synth->backfire_tail*.42f;
        synth->backfire_crack*=.889f;
        synth->backfire_tail*=.9945f;
        impact=(audio_sine(synth->pipe_phase_right)*.72f+
                noise_center*.54f+
                audio_bipolar_envelope(synth->impact_envelope)*.44f)*
               synth->impact_envelope;
        synth->impact_envelope*=.9973f;
        control_noise=audio_bipolar_envelope(synth->clutch_envelope)*.30f+
                      noise_center*synth->handbrake_envelope*.13f;
        synth->clutch_envelope*=.9827f;
        synth->handbrake_envelope*=.9395f;
        events=control_noise+audio_sine(synth->pipe_phase_left)*
                synth->shift_envelope*.19f;
        synth->shift_envelope*=.9884f;

        music_target=game.mode==MODE_TITLE?.74f:
                     (game.mode==MODE_PAUSED?.26f:.60f);
        music_duck=clampf(tire_level*.10f+synth->backfire_crack*.12f+
                          synth->impact_envelope*.14f,0.0f,.20f);
        music_target*=1.0f-music_duck;
        synth->music_gain+=(music_target-synth->music_gain)*.00045f;
        {
            const dla_music_track_t *track=&dla_music_tracks[synth->music_track];
            const unsigned packed=dla_music_adpcm[track->offset+synth->music_frame];
            music_left=(float)audio_decode_adpcm(packed&15u,
                &synth->music_predictor[0],&synth->music_step_index[0])*
                (1.0f/32768.0f)*synth->music_gain;
            music_right=(float)audio_decode_adpcm(packed>>4,
                &synth->music_predictor[1],&synth->music_step_index[1])*
                (1.0f/32768.0f)*synth->music_gain;
            if(++synth->music_frame>=track->frames)
                audio_start_music_track(synth,synth->music_track+1u);
        }

        mix_left=tire_left+road_left+wind_left+transmission+city_ambience+
                 traffic*(1.0f-audio_controls.traffic_pan)*.72f+
                 backfire*.84f+impact*.78f+events+music_left;
        mix_right=tire_right+road_right+wind_right+transmission+city_ambience+
                  traffic*(1.0f+audio_controls.traffic_pan)*.72f+
                  backfire*.84f+impact*.78f+events+music_right;
        synth->output_dc_left+=(mix_left-synth->output_dc_left)*.00080f;
        synth->output_dc_right+=(mix_right-synth->output_dc_right)*.00080f;
        mix_left=audio_soft_clip((mix_left-synth->output_dc_left)*.96f);
        mix_right=audio_soft_clip((mix_right-synth->output_dc_right)*.96f);
        output_left=(int)(mix_left*20500.0f);
        output_right=(int)(mix_right*20500.0f);
        if(output_left>30000) output_left=30000;
        if(output_left<-30000) output_left=-30000;
        if(output_right>30000) output_right=30000;
        if(output_right<-30000) output_right=-30000;
        audio_samples[i*2]=(int16_t)output_left;
        audio_samples[i*2+1]=(int16_t)output_right;
    }
    *actual=frames*4;
    return audio_samples;
}

static void init_audio(void) {
    int i,recorded_voices=0;
    memset(&audio_synth,0,sizeof(audio_synth));
    memset(&audio_idle_voice,0,sizeof(audio_idle_voice));
    memset(&audio_load_voice,0,sizeof(audio_load_voice));
    memset(&audio_tire_voice,0,sizeof(audio_tire_voice));
    audio_idle_voice.channel=-1;
    audio_load_voice.channel=-1;
    audio_tire_voice.channel=-1;
    audio_synth.rpm=audio_controls.rpm;
    audio_synth.engine_load=audio_controls.engine_load;
    audio_synth.noise_state=0x9e3779b9u;
    audio_synth.seen_backfire_serial=audio_controls.backfire_serial;
    audio_synth.seen_impact_serial=audio_controls.impact_serial;
    audio_synth.seen_clutch_serial=audio_controls.clutch_serial;
    audio_synth.seen_handbrake_serial=audio_controls.handbrake_serial;
    audio_synth.seen_shift_serial=audio_controls.shift_serial;
    for(i=0;i<AUDIO_SINE_LUT_SIZE;++i)
        audio_sine_lut[i]=(int16_t)(fsin((float)i*PI*2.0f/
                                        (float)AUDIO_SINE_LUT_SIZE)*32767.0f);
    if(snd_stream_init_ex(2,4*1024)<0) {
        printf("Drift Los Angeles: audio stream initialization failed; continuing silent.\n");
        return;
    }
    audio_stream=snd_stream_alloc(audio_callback,4*1024);
    if(audio_stream==SND_STREAM_INVALID) {
        printf("Drift Los Angeles: engine stream allocation failed; continuing silent.\n");
        snd_stream_shutdown();
        return;
    }
    if(audio_start_recorded_voice(&audio_idle_voice,dla_engine_idle_pcm,
                                  sizeof(dla_engine_idle_pcm),
                                  DLA_ENGINE_IDLE_FRAMES)) ++recorded_voices;
    if(audio_start_recorded_voice(&audio_load_voice,dla_engine_load_pcm,
                                  sizeof(dla_engine_load_pcm),
                                  DLA_ENGINE_LOAD_FRAMES)) ++recorded_voices;
    if(audio_start_recorded_voice(&audio_tire_voice,dla_tire_squeal_pcm,
                                  sizeof(dla_tire_squeal_pcm),
                                  DLA_TIRE_SQUEAL_FRAMES)) ++recorded_voices;
    snd_stream_volume(audio_stream,AUDIO_STREAM_BUS_VOLUME);
    snd_stream_start(audio_stream,AUDIO_RATE,1);
    {
        uint32_t frames=0u;
        for(i=0;i<(int)DLA_MUSIC_TRACK_COUNT;++i) frames+=dla_music_tracks[i].frames;
        printf("Drift Los Angeles: %d recorded 32 kHz AICA engine/tire voices plus 22 kHz effects and a %u-track, %.1fs stereo soundtrack ready.\n",
               recorded_voices,(unsigned)DLA_MUSIC_TRACK_COUNT,
               (double)frames/(double)DLA_MUSIC_RATE);
    }
    audio_start_music_track(&audio_synth,0u);
}

static void shutdown_audio(void) {
    if(audio_stream!=SND_STREAM_INVALID) {
        audio_release_recorded_voice(&audio_idle_voice);
        audio_release_recorded_voice(&audio_load_voice);
        audio_release_recorded_voice(&audio_tire_voice);
        snd_stream_destroy(audio_stream);
        audio_stream=SND_STREAM_INVALID;
        snd_stream_shutdown();
    }
}

static float clampf(float value, float low, float high) {
    if(value < low) return low;
    if(value > high) return high;
    return value;
}

static float approachf(float value, float target, float amount) {
    if(value < target) return fminf(value + amount, target);
    return fmaxf(value - amount, target);
}

static float wrap_angle(float angle) {
    while(angle > PI) angle -= PI * 2.0f;
    while(angle < -PI) angle += PI * 2.0f;
    return angle;
}

static uint32_t hash_u32(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

static float hash_unit(int x, int z, uint32_t salt) {
    const uint32_t h = hash_u32((uint32_t)x * 0x9e3779b9u ^
                                (uint32_t)z * 0x85ebca6bu ^ salt);
    return (float)(h & 0xffffu) * (1.0f / 65535.0f);
}

static uint32_t traffic_random_u32(void) {
    traffic_random_state = traffic_random_state * 1664525u + 1013904223u;
    return traffic_random_state;
}

static float traffic_random_unit(void) {
    return (float)((traffic_random_u32() >> 8) & 0xffffu) * (1.0f / 65535.0f);
}

static uint32_t effect_random_u32(void) {
    effect_random_state=effect_random_state*1664525u+1013904223u;
    return effect_random_state;
}

static float effect_random_unit(void) {
    return (float)((effect_random_u32()>>8)&0xffffu)*(1.0f/65535.0f);
}

/* Let fixed scene colors fold at the call site, including their clamps. */
static inline __attribute__((always_inline)) uint32_t pack_color(float alpha, color3_t color) {
    return PVR_PACK_COLOR(clampf(alpha, 0.0f, 1.0f),
                          clampf(color.r, 0.0f, 1.0f),
                          clampf(color.g, 0.0f, 1.0f),
                          clampf(color.b, 0.0f, 1.0f));
}

static color3_t color_scale(color3_t color, float scale) {
    color.r = clampf(color.r * scale, 0.0f, 1.0f);
    color.g = clampf(color.g * scale, 0.0f, 1.0f);
    color.b = clampf(color.b * scale, 0.0f, 1.0f);
    return color;
}

static float grid_distance(float value) {
    float local = fmodf(value, CITY_CELL);
    if(local < 0.0f) local += CITY_CELL;
    return fminf(local, CITY_CELL - local);
}

static bool is_on_road(float x, float z) {
    return grid_distance(x) < ROAD_HALF || grid_distance(z) < ROAD_HALF;
}

static void update_audio_controls(const input_t *input, float dt) {
    static const float total_ratios[5]={10.1574f,7.0794f,4.8906f,3.4200f,2.4282f};
    static const float upshift_speed[4]={18.5f,29.5f,41.0f,56.0f};
    static const float downshift_speed[4]={15.5f,25.5f,35.5f,49.0f};
    const bool driving=game.mode==MODE_PLAYING||game.mode==MODE_DEMO;
    const float speed=driving?fabsf(car.longitudinal):0.0f;
    const float speed_for_slip=fmaxf(speed,1.15f);
    const float throttle=game.mode==MODE_DEMO?.88f:
                         (game.mode==MODE_PLAYING?input->throttle:
                          (game.mode==MODE_TITLE?.08f:0.0f));
    const float clutch=driving?clampf(car.clutch_kick_timer/.45f,0.0f,1.0f):0.0f;
    const float speed_gate=clampf((speed-2.0f)/10.0f,0.0f,1.0f);
    const float front_angle=fabsf(atan2f(car.lateral+1.32f*car.yaw_rate,
                                         speed_for_slip)-car.steer);
    const float rear_angle=fabsf(atan2f(car.lateral-1.23f*car.yaw_rate,
                                        speed_for_slip));
    float front_slip=clampf((front_angle-.025f)/.34f,0.0f,1.0f)*speed_gate;
    float rear_slip=clampf((rear_angle-.018f)/.46f,0.0f,1.0f)*speed_gate;
    float wheel_rpm,target_rpm,launch_flare,engine_load;
    float nearest_distance_sq=48.0f*48.0f;
    int i;

    audio_shift_cooldown=fmaxf(0.0f,audio_shift_cooldown-dt);
    audio_impact_cooldown=fmaxf(0.0f,audio_impact_cooldown-dt);
    if(!driving) {
        audio_controls.gear=1;
        audio_shift_cooldown=0.0f;
    }
    else if(audio_shift_cooldown<=0.0f) {
        const int gear=audio_controls.gear;
        if(gear<5&&speed>upshift_speed[gear-1]*(.86f+.14f*throttle)) {
            audio_controls.gear=gear+1;
            audio_shift_cooldown=.22f;
            ++audio_controls.shift_serial;
        }
        else if(gear>1&&speed<downshift_speed[gear-2]) {
            audio_controls.gear=gear-1;
            audio_shift_cooldown=.18f;
            ++audio_controls.shift_serial;
        }
    }
    if(audio_controls.gear<1||audio_controls.gear>5)
        audio_controls.gear=1;

    rear_slip=clampf(fmaxf(rear_slip,
                           car.rear_power_slip*(.68f+.32f*throttle))+
                     car.handbrake_lock*.78f*speed_gate+
                     clutch*.38f*speed_gate,0.0f,1.0f);
    front_slip=clampf(front_slip+input->brake*.16f*speed_gate,0.0f,1.0f);
    wheel_rpm=fmaxf(speed,fabsf(car.rear_wheel_speed))*
              (60.0f/(PI*2.0f*.34f));
    launch_flare=clampf(1.0f-speed/8.0f,0.0f,1.0f)*throttle*2200.0f;
    target_rpm=wheel_rpm*total_ratios[audio_controls.gear-1]+launch_flare+
               rear_slip*throttle*1550.0f+clutch*1750.0f;
    if(!driving) target_rpm=game.mode==MODE_TITLE?930.0f:850.0f;
    target_rpm=clampf(target_rpm,850.0f,6900.0f);
    engine_load=clampf(.08f+throttle*(.58f+.28f*(1.0f-speed/82.0f))+
                       rear_slip*throttle*.30f+clutch*.22f,.08f,1.0f);
    if(game.mode==MODE_PAUSED) engine_load=.08f;

    audio_controls.rpm=approachf(audio_controls.rpm,target_rpm,
        dt*(target_rpm>audio_controls.rpm ?
            (clutch>.08f?7600.0f:4300.0f) :
            (audio_shift_cooldown>0.0f?8600.0f:5900.0f)));
    audio_controls.throttle=throttle;
    audio_controls.engine_load=approachf(audio_controls.engine_load,engine_load,
        dt*(engine_load>audio_controls.engine_load?4.8f:2.8f));
    audio_controls.speed=speed;
    audio_controls.front_slip=front_slip;
    audio_controls.rear_slip=rear_slip;
    audio_controls.handbrake=driving?car.handbrake_lock:0.0f;
    audio_controls.clutch=clutch;
    audio_controls.brake=driving?input->brake:0.0f;
    audio_controls.offroad=driving&&!is_on_road(car.x,car.z)?1.0f:0.0f;
    audio_controls.steer=driving?car.steer:0.0f;
    audio_controls.yaw_rate=driving?car.yaw_rate:0.0f;
    audio_controls.traffic_level=0.0f;
    audio_controls.traffic_pan=0.0f;
    audio_controls.traffic_speed=0.0f;

    if(driving) {
        for(i=0;i<MAX_TRAFFIC;++i) {
            const traffic_t *vehicle=&traffic[i];
            const float dx=vehicle->x-car.x;
            const float dz=vehicle->z-car.z;
            const float distance_sq=dx*dx+dz*dz;
            if(!vehicle->active||distance_sq>=nearest_distance_sq) continue;
            nearest_distance_sq=distance_sq;
            audio_controls.traffic_speed=vehicle->speed;
            if(distance_sq>1.0f) {
                const float distance=sqrtf(distance_sq);
                const float local_right=(dx*fcos(car.yaw)-dz*fsin(car.yaw))/distance;
                audio_controls.traffic_level=
                    clampf((48.0f-distance)/44.0f,0.0f,1.0f);
                audio_controls.traffic_pan=clampf(local_right,-1.0f,1.0f);
            }
        }
    }
}

static district_t district_for_cell(int cell_x, int cell_z) {
    if(cell_x<=-2) return DISTRICT_COAST;
    if(cell_z>=2) return DISTRICT_NEON;
    if(cell_x>=2) return DISTRICT_ARTS;
    return DISTRICT_DOWNTOWN;
}

static district_t district_for_position(float x, float z) {
    return district_for_cell((int)floorf(x/CITY_CELL),(int)floorf(z/CITY_CELL));
}

static const char *district_name(district_t district) {
    switch(district) {
        case DISTRICT_COAST: return "PACIFIC COAST";
        case DISTRICT_ARTS: return "ARTS QUARTER";
        case DISTRICT_NEON: return "NEON STRIP";
        default: return "DOWNTOWN CORE";
    }
}

static color3_t district_color(district_t district) {
    switch(district) {
        case DISTRICT_COAST: return (color3_t){.18f,.88f,.82f};
        case DISTRICT_ARTS: return (color3_t){1.0f,.48f,.12f};
        case DISTRICT_NEON: return (color3_t){1.0f,.12f,.72f};
        default: return (color3_t){1.0f,.72f,.22f};
    }
}

static int district_detail_texture(district_t district) {
    switch(district) {
        case DISTRICT_COAST: return DLA_TEX_DISTRICT_COAST;
        case DISTRICT_ARTS: return DLA_TEX_DISTRICT_ARTS;
        case DISTRICT_NEON: return DLA_TEX_DISTRICT_NEON;
        default: return DLA_TEX_DISTRICT_DOWNTOWN;
    }
}

static int district_facade_texture(district_t district, uint32_t seed) {
    const bool alternate=((seed>>7)&1u)!=0u;
    switch(district) {
        case DISTRICT_COAST: return alternate ? DLA_TEX_FACADE_COAST_ALT :
                                                   DLA_TEX_FACADE_COAST;
        case DISTRICT_ARTS: return alternate ? DLA_TEX_FACADE_ARTS_ALT :
                                                  DLA_TEX_FACADE_ARTS;
        case DISTRICT_NEON: return alternate ? DLA_TEX_FACADE_NEON_ALT :
                                                  DLA_TEX_FACADE_NEON;
        default: return alternate ? DLA_TEX_FACADE_DOWNTOWN_ALT :
                                     DLA_TEX_FACADE_DOWNTOWN;
    }
}

static building_t building_for_cell(int cell_x, int cell_z) {
    building_t out;
    const uint32_t seed = hash_u32((uint32_t)cell_x * 0x45d9f3bu ^
                                   (uint32_t)cell_z * 0x119de1f3u ^ 0x564454u);
    const district_t district=district_for_cell(cell_x,cell_z);
    memset(&out, 0, sizeof(out));
    out.seed = seed;
    out.district=district;
    out.exists = (seed % 11u) != 0u;
    out.cx = ((float)cell_x + 0.5f) * CITY_CELL;
    out.cz = ((float)cell_z + 0.5f) * CITY_CELL;
    if(district==DISTRICT_COAST) {
        out.width=67.0f+hash_unit(cell_x,cell_z,0x1001u)*13.0f;
        out.depth=63.0f+hash_unit(cell_x,cell_z,0x2002u)*16.0f;
        out.height=12.0f+hash_unit(cell_x,cell_z,0x3003u)*20.0f;
        out.exists=(seed%13u)!=0u;
        if(cell_x<=-3) out.exists=false;
    }
    else if(district==DISTRICT_ARTS) {
        out.width=64.0f+hash_unit(cell_x,cell_z,0x1001u)*14.0f;
        out.depth=62.0f+hash_unit(cell_x,cell_z,0x2002u)*16.0f;
        out.height=9.0f+hash_unit(cell_x,cell_z,0x3003u)*15.0f;
    }
    else if(district==DISTRICT_NEON) {
        out.width=68.0f+hash_unit(cell_x,cell_z,0x1001u)*12.0f;
        out.depth=68.0f+hash_unit(cell_x,cell_z,0x2002u)*12.0f;
        out.height=22.0f+hash_unit(cell_x,cell_z,0x3003u)*38.0f;
    }
    else {
        const float core=clampf(1.0f-
            sqrtf((float)(cell_x*cell_x+cell_z*cell_z))/5.0f,0.0f,1.0f);
        out.width=66.0f+hash_unit(cell_x,cell_z,0x1001u)*14.0f;
        out.depth=66.0f+hash_unit(cell_x,cell_z,0x2002u)*14.0f;
        out.height=28.0f+hash_unit(cell_x,cell_z,0x3003u)*38.0f+
                   core*core*(28.0f+hash_unit(cell_x,cell_z,0x4004u)*58.0f);
    }
    if((cell_x==0&&cell_z==0)||(cell_x==-2&&cell_z==0)||
       (cell_x==2&&cell_z==0)||(cell_x==0&&cell_z==2))
        out.exists=true;
    out.texture=district_facade_texture(district,seed);
    return out;
}

static input_t poll_input(void) {
    static int last_analog_direction;
    input_t input;
    maple_device_t *device;
    cont_state_t *state;
    float steer;
    int analog_direction;
    memset(&input, 0, sizeof(input));
    device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if(!device) return input;
    state = (cont_state_t *)maple_dev_status(device);
    if(!state) return input;
    input.connected = true;
    input.buttons = state->buttons;
    input.pressed = input.buttons & ~game.previous_buttons;

    /* KOS exposes the Dreamcast X axis as -128..127. Treating both halves as
       127-wide leaves the Xbox/Flycast negative endpoint out of range. Apply
       a symmetric dead zone, rescale each half independently, and slightly
       lift the middle of the response so counter-steer arrives early enough. */
    if(state->joyx < -12) {
        const float magnitude=clampf((float)(-state->joyx-12)/116.0f,0.0f,1.0f);
        steer=-(magnitude*.68f+sqrtf(magnitude)*.32f);
    }
    else if(state->joyx > 12) {
        const float magnitude=clampf((float)(state->joyx-12)/115.0f,0.0f,1.0f);
        steer=magnitude*.68f+sqrtf(magnitude)*.32f;
    }
    else steer=0.0f;
    input.steer = steer;
    analog_direction=steer<-.35f?-1:(steer>.35f?1:0);
    if(analog_direction!=0&&analog_direction!=last_analog_direction)
        printf("Drift Los Angeles input: analog %s recognized (raw X=%d, steer=%.2f).\n",
               analog_direction<0?"LEFT":"RIGHT",state->joyx,steer);
    if(analog_direction!=last_analog_direction)
        last_analog_direction=analog_direction;
    if(input.buttons & CONT_DPAD_LEFT) input.steer = -1.0f;
    if(input.buttons & CONT_DPAD_RIGHT) input.steer = 1.0f;
    input.throttle = (float)state->rtrig / 255.0f;
    input.brake = (float)state->ltrig / 255.0f;
    if((input.buttons & CONT_DPAD_UP) && input.throttle < 0.05f) input.throttle = 1.0f;
    if((input.buttons & CONT_DPAD_DOWN) && input.brake < 0.05f) input.brake = 1.0f;
    return input;
}

static void reset_car(void) {
    memset(&car, 0, sizeof(car));
    car.x = 0.0f;
    car.z = 16.0f;
    car.yaw = 0.0f;
    car.drive_direction = 1;
    car.rear_grip_scale = 1.0f;
    front_wheel_spin = 0.0f;
    rear_wheel_spin = 0.0f;
    camera_initialized = false;
    audio_controls.rpm=900.0f;
    audio_controls.throttle=0.0f;
    audio_controls.engine_load=.10f;
    audio_controls.gear=1;
    audio_shift_cooldown=0.0f;
}

static void spawn_traffic_car(traffic_t *vehicle, int slot, bool initial) {
    static const color3_t colors[] = {
        {0.88f,0.16f,0.12f}, {0.10f,0.42f,0.86f},
        {0.58f,0.62f,0.70f}, {0.24f,0.27f,0.33f},
        {0.82f,0.58f,0.10f}, {0.16f,0.62f,0.48f},
        {0.50f,0.20f,0.65f}, {0.62f,0.64f,0.68f}
    };
    float distance = (initial ? 24.0f : 95.0f) +
                     traffic_random_unit() * (initial ? 205.0f : 240.0f);
    float side = initial ? ((slot%5)==0 ? -1.0f : 1.0f) :
                 ((traffic_random_u32() & 1u) ? 1.0f : -1.0f);
    int spread = (int)(traffic_random_u32() % 3u) - 1;
    vehicle->axis = (int)(traffic_random_u32() & 1u);
    vehicle->direction = (traffic_random_u32() & 1u) ? 1 : -1;
    if(initial && slot<6) {
        vehicle->axis=fabsf(fsin(car.yaw))>.70f ? 1 : 0;
        vehicle->direction=(slot&1) ? -1 : 1;
        spread=slot<4 ? 0 : ((slot&1) ? -1 : 1);
        side=1.0f;
        distance=32.0f+(float)slot*23.0f;
    }
    vehicle->seed = traffic_random_u32() ^ (uint32_t)slot * 0x9e3779b9u;
    vehicle->color = colors[vehicle->seed % ARRAY_COUNT(colors)];
    if((vehicle->seed%6u)==0u)
        vehicle->color=(color3_t){.92f,.62f,.08f};
    vehicle->cruise_speed = 10.5f + traffic_random_unit() * 10.5f;
    vehicle->speed = vehicle->cruise_speed * (0.72f + traffic_random_unit() * 0.28f);
    vehicle->brake_light = 0.0f;
    if(vehicle->axis == 0) {
        const int road = (int)roundf(car.x / CITY_CELL) + spread;
        const float lane=(vehicle->seed&2u)?4.35f:9.05f;
        vehicle->x = (float)road * CITY_CELL + (float)vehicle->direction * lane;
        vehicle->z = car.z + side * distance;
        vehicle->yaw = vehicle->direction > 0 ? 0.0f : PI;
    }
    else {
        const int road = (int)roundf(car.z / CITY_CELL) + spread;
        const float lane=(vehicle->seed&2u)?4.35f:9.05f;
        vehicle->z = (float)road * CITY_CELL - (float)vehicle->direction * lane;
        vehicle->x = car.x + side * distance;
        vehicle->yaw = vehicle->direction > 0 ? PI * 0.5f : -PI * 0.5f;
    }
    vehicle->active = true;
}

static void reset_traffic(void) {
    int i;
    traffic_random_state = hash_u32(0x7c7d41f3u ^ (uint32_t)(game.time * 1000.0f));
    memset(traffic, 0, sizeof(traffic));
    for(i = 0; i < MAX_TRAFFIC; ++i)
        spawn_traffic_car(&traffic[i], i, true);
}

/* Block pad extents shared by the renderer and the street collision code. */
static void block_extents(int cell_x, int cell_z,
                          float *x0, float *x1, float *z0, float *z1) {
    *x0 = (float)cell_x * CITY_CELL + ROAD_HALF + 1.5f;
    *x1 = (float)(cell_x + 1) * CITY_CELL - ROAD_HALF - 1.5f;
    *z0 = (float)cell_z * CITY_CELL + ROAD_HALF + 1.5f;
    *z1 = (float)(cell_z + 1) * CITY_CELL - ROAD_HALF - 1.5f;
}

/* Coast cells west of the boulevard are open water with no street life. */
static bool block_has_street_detail(int cell_x, int cell_z) {
    return !(district_for_cell(cell_x, cell_z) == DISTRICT_COAST && cell_x <= -3);
}

/* Four swan-neck lampposts stand just off each block's curb. */
static void block_lamppost(int cell_x, int cell_z, int index,
                           float *x, float *z) {
    float x0, x1, z0, z1;
    block_extents(cell_x, cell_z, &x0, &x1, &z0, &z1);
    switch(index) {
        case 0: *x = x0 - 1.2f; *z = z0 + 7.0f; break;
        case 1: *x = x1 + 1.2f; *z = z1 - 7.0f; break;
        case 2: *x = x0 + 7.0f; *z = z1 + 1.2f; break;
        default: *x = x1 - 7.0f; *z = z0 - 1.2f; break;
    }
}

/* Each block walks up to four pedestrians along its sidewalks. Their
   positions follow the game clock, so the renderer and the collision code
   agree without a per-block entity list. */
static bool block_pedestrian(int cell_x, int cell_z, const building_t *building,
                             int slot, pedestrian_pose_t *pose) {
    float x0, x1, z0, z1, span, speed, walk;
    block_extents(cell_x, cell_z, &x0, &x1, &z0, &z1);
    span = x1 - x0 - 12.0f;
    speed = 1.15f + hash_unit(cell_x, cell_z, 0x8765u) * .55f;
    walk = fmodf(game.time * speed / span + hash_unit(cell_x, cell_z, 0x4321u), 1.0f);
    switch(slot) {
        case 0:
            pose->x = x0 + 6.0f + walk * span;
            pose->z = z0 + 2.3f;
            pose->heading = PI * .5f;
            pose->seed = building->seed;
            pose->phase = game.time * speed * 5.2f;
            return true;
        case 1:
            if((building->seed & 3u) == 0u) return false;
            pose->x = x1 - 6.0f - fmodf(walk + .47f, 1.0f) * span;
            pose->z = z1 - 2.3f;
            pose->heading = -PI * .5f;
            pose->seed = building->seed >> 8;
            pose->phase = game.time * speed * 4.8f + PI;
            return true;
        case 2:
            pose->x = x0 + 2.3f;
            pose->z = z0 + 6.0f + fmodf(walk + .23f, 1.0f) * span;
            pose->heading = 0.0f;
            pose->seed = building->seed >> 13;
            pose->phase = game.time * speed * 4.5f + 1.2f;
            return true;
        default:
            pose->x = x1 - 2.3f;
            pose->z = z1 - 6.0f - fmodf(walk + .71f, 1.0f) * span;
            pose->heading = PI;
            pose->seed = building->seed >> 18;
            pose->phase = game.time * speed * 5.5f + 2.1f;
            return true;
    }
}

/* Parked cars are deterministic per block: up to two kerbside vehicles on
   the block's west road. Index 1 is only present on some blocks. */
static bool parked_car_for_cell(int cell_x, int cell_z, int index,
                                traffic_t *parked) {
    static const color3_t parked_colors[] = {
        {.36f,.08f,.07f}, {.06f,.22f,.46f}, {.42f,.44f,.48f},
        {.055f,.065f,.085f}, {.48f,.30f,.055f}, {.08f,.30f,.22f}
    };
    const uint32_t seed=hash_u32((uint32_t)cell_x*0x7f4a7c15u ^
                                 (uint32_t)cell_z*0x94d049bbu ^ 0x5041524bu);
    const float block_cx=((float)cell_x+.5f)*CITY_CELL;
    const float block_cz=((float)cell_z+.5f)*CITY_CELL;
    if((seed%2u)!=0u || !block_has_street_detail(cell_x,cell_z)) return false;
    if(index!=0 && (seed&2u)==0u) return false;
    memset(parked,0,sizeof(*parked));
    parked->active=true;
    parked->seed=seed;
    parked->color=parked_colors[(seed>>4)%ARRAY_COUNT(parked_colors)];
    if(index==0) {
        if(seed&1u) {
            parked->x=block_cx+((seed>>8)&1u ? 17.0f : -17.0f);
            parked->z=(float)cell_z*CITY_CELL+11.7f;
            parked->yaw=(seed&4u)?PI*.5f:-PI*.5f;
        }
        else {
            parked->x=(float)cell_x*CITY_CELL+11.7f;
            parked->z=block_cz+((seed>>8)&1u ? 17.0f : -17.0f);
            parked->yaw=(seed&4u)?0.0f:PI;
        }
        return true;
    }
    parked->seed^=0xb5297a4du;
    parked->color=parked_colors[(parked->seed>>5)%ARRAY_COUNT(parked_colors)];
    if(seed&1u) {
        parked->x=block_cx+((seed>>8)&1u ? -20.5f : 20.5f);
        parked->z=(float)cell_z*CITY_CELL-11.7f;
        parked->yaw=(seed&4u)?-PI*.5f:PI*.5f;
    }
    else {
        parked->x=(float)cell_x*CITY_CELL-11.7f;
        parked->z=block_cz+((seed>>8)&1u ? -20.5f : 20.5f);
        parked->yaw=(seed&4u)?PI:0.0f;
    }
    return true;
}

static void start_run(void) {
    reset_car();
    memset(smoke_pool, 0, sizeof(smoke_pool));
    memset(exhaust_flame_pool, 0, sizeof(exhaust_flame_pool));
    memset(skid_pool, 0, sizeof(skid_pool));
    memset(hit_pedestrians, 0, sizeof(hit_pedestrians));
    exhaust_flame_cursor=0;
    effect_random_state=0x63d83595u;
    game.score = 0.0f;
    game.drift_chain = 0.0f;
    game.drift_multiplier = 1.0f;
    game.drift_hold = 0.0f;
    game.drift_duration = 0.0f;
    game.longest_drift = 0.0f;
    game.drift_bonus_step = 0;
    game.exhaust_pop_timer=.18f;
    game.exhaust_flash=0.0f;
    game.demo_time=0.0f;
    game.demo_segment=-1;
    game.district=district_for_position(car.x,car.z);
    game.district_banner=3.0f;
    skid_contact_valid = false;
    reset_traffic();
    game.mode = MODE_PLAYING;
    game.hud_timer = 0.0f;
    printf("Drift Los Angeles: run started.\n");
}

static void start_demo(void) {
    start_run();
    game.mode=MODE_DEMO;
    game.demo_time=0.0f;
#ifdef DRIFT_LA_CAPTURE_SEGMENT
    game.demo_time=(float)(DRIFT_LA_CAPTURE_SEGMENT%4)*15.0f+
                   DRIFT_LA_CAPTURE_LOCAL;
#endif
    game.demo_segment=-1;
    game.score=0.0f;
    printf("Drift Los Angeles: district showcase started.\n");
}

static vec3_t car_local_to_world(float x, float y, float z) {
    const shz_sincos_t rotation=shz_sincosf(car.yaw);
    const float c=rotation.cos,s=rotation.sin;
    return (vec3_t){car.x + x * c + z * s, y,
                    car.z - x * s + z * c};
}

static void emit_smoke(float lx, float lz) {
    smoke_t *particle = &smoke_pool[smoke_cursor++ % MAX_SMOKE];
    vec3_t position = car_local_to_world(lx, 0.28f, lz);
    const float phase = (float)((smoke_cursor * 37) & 255) * (PI * 2.0f / 255.0f);
    const float spread=.74f+(float)(smoke_cursor&7)*.075f;
    const float car_vx=fsin(car.yaw)*car.longitudinal+fcos(car.yaw)*car.lateral;
    const float car_vz=fcos(car.yaw)*car.longitudinal-fsin(car.yaw)*car.lateral;
    particle->active = true;
    particle->x = position.x;
    particle->y = position.y;
    particle->z = position.z;
    particle->vx = car_vx*.075f+fcos(phase)*spread;
    particle->vy = 0.72f + (float)(smoke_cursor & 7) * 0.105f;
    particle->vz = car_vz*.075f+fsin(phase)*spread;
    particle->life = particle->max_life =
        2.20f+(float)(smoke_cursor&3)*.18f;
    particle->size = 0.64f + (float)(smoke_cursor & 3) * 0.075f;
}

static float engine_rpm_level(void) {
    return clampf((audio_controls.rpm-850.0f)/6050.0f,0.0f,1.0f);
}

static void emit_exhaust_flame(float local_x) {
    exhaust_flame_t *flame=
        &exhaust_flame_pool[exhaust_flame_cursor++%MAX_EXHAUST_FLAMES];
    const vec3_t position=car_local_to_world(local_x,.160f,-2.50f);
    const float fwd_x=fsin(car.yaw),fwd_z=fcos(car.yaw);
    const float right_x=fcos(car.yaw),right_z=-fsin(car.yaw);
    const float car_vx=fwd_x*car.longitudinal+right_x*car.lateral;
    const float car_vz=fwd_z*car.longitudinal+right_z*car.lateral;
    const float ejection=5.2f+effect_random_unit()*3.8f;
    const float jitter=(effect_random_unit()-.5f)*1.25f;
    flame->active=true;
    flame->x=position.x;
    flame->y=position.y;
    flame->z=position.z;
    flame->vx=car_vx*.58f-fwd_x*ejection+right_x*jitter;
    flame->vy=.10f+effect_random_unit()*.42f;
    flame->vz=car_vz*.58f-fwd_z*ejection+right_z*jitter;
    flame->life=flame->max_life=.18f+effect_random_unit()*.17f;
    flame->size=.11f+effect_random_unit()*.075f;
}

static void update_exhaust_pops(float throttle, float rpm_level, float dt) {
    static const float outlets[4]={-.255f,-.085f,.085f,.255f};
    int i;
    game.exhaust_pop_timer-=dt;
    if(throttle>.70f&&rpm_level>.78f&&fabsf(car.longitudinal)>5.0f&&
       game.exhaust_pop_timer<=0.0f) {
        for(i=0;i<4;++i) emit_exhaust_flame(outlets[i]);
        if(effect_random_u32()&1u) {
            emit_exhaust_flame(outlets[0]);
            emit_exhaust_flame(outlets[3]);
        }
        game.exhaust_flash=1.0f;
        audio_trigger_backfire(.82f+rpm_level*.35f);
        game.exhaust_pop_timer=game.mode==MODE_DEMO ?
            (.18f+effect_random_unit()*.32f) :
            (.34f+effect_random_unit()*.62f);
    }
    else if(game.exhaust_pop_timer<-.20f)
        game.exhaust_pop_timer=-.20f;
}

static void emit_skid(float lx, int wheel) {
    skid_t *mark = &skid_pool[skid_cursor++ % MAX_SKIDS];
    const vec3_t rear = car_local_to_world(lx, 0.018f, -1.55f);
    const float fwd_x = fsin(car.yaw);
    const float fwd_z = fcos(car.yaw);
    mark->active = true;
    if(skid_contact_valid) {
        mark->x1 = last_skid_position[wheel].x;
        mark->z1 = last_skid_position[wheel].z;
    }
    else {
        mark->x1 = rear.x - fwd_x * 0.38f;
        mark->z1 = rear.z - fwd_z * 0.38f;
    }
    mark->x2 = rear.x;
    mark->z2 = rear.z;
    mark->life = 18.0f;
    last_skid_position[wheel] = rear;
}

static void update_effects(float dt) {
    int i;
    for(i = 0; i < MAX_SMOKE; ++i) {
        smoke_t *particle = &smoke_pool[i];
        if(!particle->active) continue;
        particle->life -= dt;
        if(particle->life <= 0.0f) {
            particle->active = false;
            continue;
        }
        particle->x += particle->vx * dt;
        particle->y += particle->vy * dt;
        particle->z += particle->vz * dt;
        particle->vx += fcos(game.time*2.1f+(float)i*1.73f)*dt*.34f;
        particle->vz += fsin(game.time*1.7f+(float)i*1.31f)*dt*.34f;
        particle->vx *= 1.0f - dt * 0.45f;
        particle->vz *= 1.0f - dt * 0.45f;
        particle->vy += dt*.08f;
        particle->size += dt * (1.82f+(float)(i&3)*.10f);
    }
    for(i=0;i<MAX_EXHAUST_FLAMES;++i) {
        exhaust_flame_t *flame=&exhaust_flame_pool[i];
        if(!flame->active) continue;
        flame->life-=dt;
        if(flame->life<=0.0f) {
            flame->active=false;
            continue;
        }
        flame->x+=flame->vx*dt;
        flame->y+=flame->vy*dt;
        flame->z+=flame->vz*dt;
        flame->vx*=1.0f-dt*3.8f;
        flame->vz*=1.0f-dt*3.8f;
        flame->size+=dt*.82f;
    }
    for(i = 0; i < MAX_SKIDS; ++i) {
        if(!skid_pool[i].active) continue;
        skid_pool[i].life -= dt;
        if(skid_pool[i].life <= 0.0f) skid_pool[i].active = false;
    }
    update_hit_pedestrians(dt);
}

static float traffic_forward_distance(const traffic_t *vehicle,
                                      float x, float z) {
    return vehicle->axis == 0 ?
        (float)vehicle->direction * (z - vehicle->z) :
        (float)vehicle->direction * (x - vehicle->x);
}

static float traffic_lane_distance(const traffic_t *vehicle,
                                   float x, float z) {
    return vehicle->axis == 0 ? fabsf(x - vehicle->x) : fabsf(z - vehicle->z);
}

static void update_traffic(float dt) {
    int i, j;
    for(i = 0; i < MAX_TRAFFIC; ++i) {
        traffic_t *vehicle = &traffic[i];
        float desired = vehicle->cruise_speed;
        float signal_distance;
        int signal_x, signal_z;
        const float player_ahead = traffic_forward_distance(vehicle, car.x, car.z);
        if(!vehicle->active) {
            spawn_traffic_car(vehicle, i, false);
            continue;
        }

        /* Keep a readable gap instead of letting cars stack into each other. */
        for(j = 0; j < MAX_TRAFFIC; ++j) {
            const traffic_t *other = &traffic[j];
            float ahead;
            if(i == j || !other->active || other->axis != vehicle->axis ||
               other->direction != vehicle->direction ||
               traffic_lane_distance(vehicle, other->x, other->z) > 2.0f)
                continue;
            ahead = traffic_forward_distance(vehicle, other->x, other->z);
            if(ahead > 0.0f && ahead < 34.0f)
                desired = fminf(desired, other->speed *
                    clampf((ahead - 6.0f) / 20.0f, 0.0f, 1.0f));
        }
        if(player_ahead > 0.0f && player_ahead < 26.0f &&
           traffic_lane_distance(vehicle, car.x, car.z) < 3.5f)
            desired = fminf(desired, clampf((player_ahead - 6.0f) / 20.0f,
                                            0.0f, 1.0f) * vehicle->cruise_speed);

        /* Approach the next city-grid signal and stop short of its crosswalk
           on red or yellow. This also gives traffic brake lights a readable,
           repeatable reason to illuminate beyond emergency spacing. */
        if(vehicle->axis==0) {
            signal_x=(int)roundf(vehicle->x/CITY_CELL);
            signal_z=vehicle->direction>0 ?
                (int)floorf(vehicle->z/CITY_CELL)+1 :
                (int)ceilf(vehicle->z/CITY_CELL)-1;
            signal_distance=(float)vehicle->direction*
                ((float)signal_z*CITY_CELL-vehicle->z);
        }
        else {
            signal_z=(int)roundf(vehicle->z/CITY_CELL);
            signal_x=vehicle->direction>0 ?
                (int)floorf(vehicle->x/CITY_CELL)+1 :
                (int)ceilf(vehicle->x/CITY_CELL)-1;
            signal_distance=(float)vehicle->direction*
                ((float)signal_x*CITY_CELL-vehicle->x);
        }
        if(signal_distance>0.0f&&signal_distance<44.0f&&
           traffic_signal_state(signal_x,signal_z)!=2)
            desired=fminf(desired,vehicle->cruise_speed*
                clampf((signal_distance-15.0f)/24.0f,0.0f,1.0f));

        vehicle->brake_light=approachf(vehicle->brake_light,
            desired<vehicle->speed-.18f?1.0f:0.0f,
            dt*(desired<vehicle->speed-.18f?12.0f:3.4f));
        vehicle->speed = approachf(vehicle->speed, desired,
            dt * (desired < vehicle->speed ? 10.0f : 2.4f));
        if(vehicle->axis == 0)
            vehicle->z += (float)vehicle->direction * vehicle->speed * dt;
        else
            vehicle->x += (float)vehicle->direction * vehicle->speed * dt;

        {
            const float dx = car.x - vehicle->x;
            const float dz = car.z - vehicle->z;
            const float distance_sq = dx*dx + dz*dz;
            if(distance_sq > 560.0f * 560.0f) {
                spawn_traffic_car(vehicle, i, false);
                continue;
            }
            if(distance_sq < 3.05f * 3.05f) {
                const float impact_speed=fabsf(car.longitudinal);
                const float inv = 1.0f / sqrtf(fmaxf(distance_sq, 0.01f));
                const float nx = dx * inv;
                const float nz = dz * inv;
                const float push = 3.05f - sqrtf(distance_sq);
                car.x += nx * push * 0.65f;
                car.z += nz * push * 0.65f;
                car.longitudinal *= 0.56f;
                car.lateral += (nx * fcos(car.yaw) - nz * fsin(car.yaw)) * 4.5f;
                car.yaw_rate += (nx * fsin(car.yaw) + nz * fcos(car.yaw)) * 0.18f;
                vehicle->speed *= 0.38f;
                vehicle->brake_light = 1.0f;
                game.impact_flash = 0.22f;
                audio_trigger_impact(.48f+clampf(impact_speed/32.0f,
                                                 0.0f,.70f));
                game.drift_hold = fminf(game.drift_hold, 0.25f);
            }
        }
    }
}

/* Kicks up a short-lived puff of dust at a street impact point. */
static void emit_impact_dust(float x, float z, float strength) {
    const int count = 2 + (int)(clampf(strength, 0.0f, 1.0f) * 4.0f);
    int i;
    for(i = 0; i < count; ++i) {
        smoke_t *particle = &smoke_pool[smoke_cursor++ % MAX_SMOKE];
        const float angle = effect_random_unit() * PI * 2.0f;
        const float speed = .8f + effect_random_unit() * 2.2f;
        particle->active = true;
        particle->x = x;
        particle->y = .35f;
        particle->z = z;
        particle->vx = fcos(angle) * speed;
        particle->vy = 1.2f + effect_random_unit() * 1.6f;
        particle->vz = fsin(angle) * speed;
        particle->life = particle->max_life = .55f + effect_random_unit() * .45f;
        particle->size = .36f + effect_random_unit() * .22f;
    }
}

static void street_impact(float x, float z, float closing_speed,
                          float flash, float audio_base, float audio_range) {
    const float strength = clampf(closing_speed / 24.0f, 0.0f, 1.0f);
    if(closing_speed < 1.5f) return;
    game.impact_flash = fmaxf(game.impact_flash, flash * (.4f + .6f * strength));
    audio_trigger_impact(audio_base + strength * audio_range);
    emit_impact_dust(x, z, strength);
    game.drift_hold = fminf(game.drift_hold, 0.25f);
}

static void car_collider_center(int index, float *x, float *z) {
    const float offset = index == 0 ? CAR_COLLIDER_OFFSET : -CAR_COLLIDER_OFFSET;
    *x = car.x + fsin(car.yaw) * offset;
    *z = car.z + fcos(car.yaw) * offset;
}

/* Rigid-body contact between the car and a fixed street obstacle. The
   normal points from the obstacle into the car and the contact point is
   where the car's collision disc touches it. Returns the closing speed
   that was removed, or zero when the car was already separating. */
static float resolve_street_contact(float contact_x, float contact_z,
                                    float nx, float nz, float penetration,
                                    float restitution, float friction) {
    const float s = fsin(car.yaw), c = fcos(car.yaw);
    float vx = s * car.longitudinal + c * car.lateral;
    float vz = c * car.longitudinal - s * car.lateral;
    const float rx = contact_x - car.x, rz = contact_z - car.z;
    const float tx = -nz, tz = nx;
    /* Yaw moves a point r at yaw_rate * (rz, -rx), so these scalar cross
       products give each impulse's lever arm about the car's centre. */
    const float cross_n = rz * nx - rx * nz;
    const float cross_t = rz * tx - rx * tz;
    const float normal_speed = vx * nx + vz * nz + car.yaw_rate * cross_n;
    car.x += nx * penetration;
    car.z += nz * penetration;
    if(normal_speed >= 0.0f) return 0.0f;
    {
        const float normal_mass =
            1.0f / (1.0f / CAR_MASS + cross_n * cross_n / CAR_INERTIA);
        const float tangent_mass =
            1.0f / (1.0f / CAR_MASS + cross_t * cross_t / CAR_INERTIA);
        const float jn = -(1.0f + restitution) * normal_speed * normal_mass;
        const float tangent_speed = vx * tx + vz * tz + car.yaw_rate * cross_t;
        const float jt = clampf(-tangent_speed * tangent_mass,
                                -friction * jn, friction * jn);
        vx += (jn * nx + jt * tx) / CAR_MASS;
        vz += (jn * nz + jt * tz) / CAR_MASS;
        car.yaw_rate += (jn * cross_n + jt * cross_t) / CAR_INERTIA;
    }
    car.longitudinal = clampf(s * vx + c * vz, -11.0f, 82.0f);
    car.lateral = clampf(c * vx - s * vz, -24.0f, 24.0f);
    car.yaw_rate = clampf(car.yaw_rate, -2.05f, 2.05f);
    return -normal_speed;
}

/* Disc-versus-post contact for lampposts and similar thin poles. */
static float collide_post(float px, float pz, float post_radius,
                          float restitution, float friction) {
    const float reach = CAR_COLLIDER_RADIUS + post_radius;
    int disc;
    for(disc = 0; disc < 2; ++disc) {
        float cx, cz, dx, dz, dist_sq, dist, nx, nz;
        car_collider_center(disc, &cx, &cz);
        dx = cx - px;
        dz = cz - pz;
        dist_sq = dx * dx + dz * dz;
        if(dist_sq >= reach * reach) continue;
        if(dist_sq < 1e-4f) {
            nx = fcos(car.yaw);
            nz = -fsin(car.yaw);
            dist = 0.0f;
        }
        else {
            dist = sqrtf(dist_sq);
            nx = dx / dist;
            nz = dz / dist;
        }
        return resolve_street_contact(px + nx * post_radius, pz + nz * post_radius,
                                      nx, nz, reach - dist, restitution, friction);
    }
    return 0.0f;
}

/* Disc-versus-oriented-box contact for a stationary vehicle. */
static float collide_parked_car(const traffic_t *parked,
                                float restitution, float friction) {
    const shz_sincos_t rotation = shz_sincosf(parked->yaw);
    const float c = rotation.cos, s = rotation.sin;
    const float reach_x = PARKED_HALF_WIDTH + CAR_COLLIDER_RADIUS;
    const float reach_z = PARKED_HALF_LENGTH + CAR_COLLIDER_RADIUS;
    int disc;
    for(disc = 0; disc < 2; ++disc) {
        float cx, cz, dx, dz, lx, lz, qx, qz, ex, ez, dist_sq;
        float nx, nz, penetration, wx, wz;
        car_collider_center(disc, &cx, &cz);
        dx = cx - parked->x;
        dz = cz - parked->z;
        /* Inverse of traffic_point: local right and forward offsets. */
        lx = dx * c - dz * s;
        lz = dx * s + dz * c;
        if(fabsf(lx) >= reach_x || fabsf(lz) >= reach_z) continue;
        qx = clampf(lx, -PARKED_HALF_WIDTH, PARKED_HALF_WIDTH);
        qz = clampf(lz, -PARKED_HALF_LENGTH, PARKED_HALF_LENGTH);
        ex = lx - qx;
        ez = lz - qz;
        dist_sq = ex * ex + ez * ez;
        if(dist_sq > 1e-6f) {
            const float dist = sqrtf(dist_sq);
            if(dist >= CAR_COLLIDER_RADIUS) continue;
            nx = ex / dist;
            nz = ez / dist;
            penetration = CAR_COLLIDER_RADIUS - dist;
        }
        else {
            /* The disc centre is inside the body: leave by the thinner side. */
            const float pen_x = PARKED_HALF_WIDTH - fabsf(lx);
            const float pen_z = PARKED_HALF_LENGTH - fabsf(lz);
            if(pen_x < pen_z) {
                nx = lx < 0.0f ? -1.0f : 1.0f;
                nz = 0.0f;
                penetration = pen_x + CAR_COLLIDER_RADIUS;
                qx = nx * PARKED_HALF_WIDTH;
            }
            else {
                nx = 0.0f;
                nz = lz < 0.0f ? -1.0f : 1.0f;
                penetration = pen_z + CAR_COLLIDER_RADIUS;
                qz = nz * PARKED_HALF_LENGTH;
            }
        }
        wx = nx * c + nz * s;
        wz = -nx * s + nz * c;
        return resolve_street_contact(parked->x + qx * c + qz * s,
                                      parked->z - qx * s + qz * c,
                                      wx, wz, penetration, restitution, friction);
    }
    return 0.0f;
}

static void collide_street_furniture(void) {
    const int base_x = (int)floorf(car.x / CITY_CELL);
    const int base_z = (int)floorf(car.z / CITY_CELL);
    int dx, dz, index;
    for(dz = -1; dz <= 1; ++dz) {
        for(dx = -1; dx <= 1; ++dx) {
            const int cell_x = base_x + dx, cell_z = base_z + dz;
            if(!block_has_street_detail(cell_x, cell_z)) continue;
            for(index = 0; index < 4; ++index) {
                float px, pz, closing;
                block_lamppost(cell_x, cell_z, index, &px, &pz);
                if(fabsf(px - car.x) > 6.0f || fabsf(pz - car.z) > 6.0f) continue;
                closing = collide_post(px, pz, LAMP_POST_RADIUS, .28f, .35f);
                if(closing > 0.0f)
                    street_impact(px, pz, closing, .20f, .42f, .62f);
            }
            for(index = 0; index < 2; ++index) {
                traffic_t parked;
                float closing;
                if(!parked_car_for_cell(cell_x, cell_z, index, &parked)) continue;
                if(fabsf(parked.x - car.x) > 8.0f || fabsf(parked.z - car.z) > 8.0f)
                    continue;
                closing = collide_parked_car(&parked, .22f, .30f);
                if(closing > 0.0f)
                    street_impact(car.x, car.z, closing, .22f, .52f, .68f);
            }
        }
    }
}

static hit_pedestrian_t *find_hit_pedestrian(int cell_x, int cell_z, int slot) {
    int i;
    for(i = 0; i < MAX_HIT_PEDESTRIANS; ++i) {
        hit_pedestrian_t *p = &hit_pedestrians[i];
        if(p->active && p->cell_x == cell_x && p->cell_z == cell_z &&
           p->slot == slot)
            return p;
    }
    return NULL;
}

static hit_pedestrian_t *allocate_hit_pedestrian(void) {
    hit_pedestrian_t *oldest = &hit_pedestrians[0];
    int i;
    for(i = 0; i < MAX_HIT_PEDESTRIANS; ++i) {
        hit_pedestrian_t *p = &hit_pedestrians[i];
        if(!p->active) return p;
        if(p->timer > oldest->timer) oldest = p;
    }
    return oldest;
}

/* Throws a pedestrian along the car's motion with a tumble. The push is
   the world-space velocity handed to the figure. */
static void launch_pedestrian(hit_pedestrian_t *p, float push_x, float push_z,
                              float lift) {
    const float push = sqrtf(push_x * push_x + push_z * push_z);
    p->vx = push_x;
    p->vz = push_z;
    p->vy = lift;
    p->grounded = false;
    if(push > .5f) p->heading = atan2f(push_x, push_z);
    p->tumble_rate = (4.0f + fminf(push, 30.0f) * .25f) *
                     ((effect_random_u32() & 1u) ? 1.0f : -1.0f);
}

static void run_over_pedestrian(int cell_x, int cell_z, int slot,
                                const pedestrian_pose_t *pose,
                                float nx, float nz) {
    const float s = fsin(car.yaw), c = fcos(car.yaw);
    const float vx = s * car.longitudinal + c * car.lateral;
    const float vz = c * car.longitudinal - s * car.lateral;
    const float speed = sqrtf(vx * vx + vz * vz);
    const float shove = 3.0f + speed * .15f;
    hit_pedestrian_t *p = allocate_hit_pedestrian();
    memset(p, 0, sizeof(*p));
    p->active = true;
    p->cell_x = cell_x;
    p->cell_z = cell_z;
    p->slot = slot;
    p->seed = pose->seed;
    p->x = pose->x;
    p->z = pose->z;
    p->heading = pose->heading;
    launch_pedestrian(p, vx * .80f + nx * shove, vz * .80f + nz * shove,
                      clampf(3.0f + speed * .12f, 3.0f, 9.0f));
    /* A body is light next to the car: a brief scrub and a nudge towards
       the side that took the hit. */
    car.longitudinal *= .985f;
    car.yaw_rate = clampf(car.yaw_rate + (nx * c - nz * s) *
                          clampf(speed / 30.0f, 0.0f, 1.0f) * .05f,
                          -2.05f, 2.05f);
    game.impact_flash = fmaxf(game.impact_flash, .07f);
    audio_trigger_impact(.34f + clampf(speed / 40.0f, 0.0f, .30f));
    emit_impact_dust(pose->x, pose->z, clampf(speed / 30.0f, 0.0f, .5f));
}

static void collide_pedestrians(void) {
    const int base_x = (int)floorf(car.x / CITY_CELL);
    const int base_z = (int)floorf(car.z / CITY_CELL);
    const float reach = CAR_COLLIDER_RADIUS + PEDESTRIAN_RADIUS;
    int dx, dz, slot, disc;
    for(dz = -1; dz <= 1; ++dz) {
        for(dx = -1; dx <= 1; ++dx) {
            const int cell_x = base_x + dx, cell_z = base_z + dz;
            building_t building;
            if(!block_has_street_detail(cell_x, cell_z)) continue;
            building = building_for_cell(cell_x, cell_z);
            for(slot = 0; slot < 4; ++slot) {
                pedestrian_pose_t pose;
                hit_pedestrian_t *hit;
                float target_x, target_z;
                if(!block_pedestrian(cell_x, cell_z, &building, slot, &pose))
                    continue;
                hit = find_hit_pedestrian(cell_x, cell_z, slot);
                if(hit && !hit->grounded) continue;
                target_x = hit ? hit->x : pose.x;
                target_z = hit ? hit->z : pose.z;
                if(fabsf(target_x - car.x) > 5.0f || fabsf(target_z - car.z) > 5.0f)
                    continue;
                for(disc = 0; disc < 2; ++disc) {
                    float cx, cz, ex, ez, dist_sq, dist, nx, nz;
                    car_collider_center(disc, &cx, &cz);
                    ex = target_x - cx;
                    ez = target_z - cz;
                    dist_sq = ex * ex + ez * ez;
                    if(dist_sq >= reach * reach) continue;
                    dist = sqrtf(fmaxf(dist_sq, 1e-4f));
                    nx = ex / dist;
                    nz = ez / dist;
                    if(hit) {
                        /* Rolling over a downed figure gives it a short hop. */
                        const float s = fsin(car.yaw), c = fcos(car.yaw);
                        const float vx = s * car.longitudinal + c * car.lateral;
                        const float vz = c * car.longitudinal - s * car.lateral;
                        if(vx * vx + vz * vz > 2.0f * 2.0f) {
                            launch_pedestrian(hit, vx * .45f + nx * 1.5f,
                                              vz * .45f + nz * 1.5f, 1.8f);
                            hit->timer = 0.0f;
                        }
                    }
                    else
                        run_over_pedestrian(cell_x, cell_z, slot, &pose, nx, nz);
                    break;
                }
            }
        }
    }
}

static void update_hit_pedestrians(float dt) {
    int i;
    for(i = 0; i < MAX_HIT_PEDESTRIANS; ++i) {
        hit_pedestrian_t *p = &hit_pedestrians[i];
        float dx, dz;
        if(!p->active) continue;
        p->timer += dt;
        p->x += p->vx * dt;
        p->z += p->vz * dt;
        if(!p->grounded) {
            p->vy -= 14.0f * dt;
            p->y += p->vy * dt;
            p->tumble += p->tumble_rate * dt;
            if(p->y <= 0.0f) {
                p->y = 0.0f;
                if(p->vy < -2.2f) {
                    p->vy = -p->vy * .28f;
                    p->vx *= .62f;
                    p->vz *= .62f;
                    p->tumble_rate *= .55f;
                }
                else {
                    p->vy = 0.0f;
                    p->grounded = true;
                    p->settle = 0.0f;
                    /* Come to rest on the nearest side, face up or face down. */
                    p->rest_tumble = floorf((p->tumble - PI * .5f) / PI + .5f) * PI + PI * .5f;
                }
            }
        }
        else {
            const float damping = fmaxf(0.0f, 1.0f - dt * 4.5f);
            p->vx *= damping;
            p->vz *= damping;
            p->settle = fminf(1.0f, p->settle + dt * 3.0f);
            p->tumble += (p->rest_tumble - p->tumble) * fminf(1.0f, dt * 8.0f);
        }
        /* The figure stays down until the player has moved on, then the
           walking pedestrian quietly resumes its route. */
        dx = p->x - car.x;
        dz = p->z - car.z;
        if((p->timer > 6.0f && dx * dx + dz * dz > 110.0f * 110.0f) ||
           p->timer > 45.0f)
            p->active = false;
    }
}

static void collide_buildings(void) {
    const int base_x = (int)floorf(car.x / CITY_CELL);
    const int base_z = (int)floorf(car.z / CITY_CELL);
    const float radius = 1.12f;
    int dx, dz;
    for(dz = -1; dz <= 1; ++dz) {
        for(dx = -1; dx <= 1; ++dx) {
            const building_t b = building_for_cell(base_x + dx, base_z + dz);
            const float half_w = b.width * 0.5f + radius;
            const float half_d = b.depth * 0.5f + radius;
            float local_x, local_z, pen_x, pen_z, impact_speed;
            if(!b.exists) continue;
            local_x = car.x - b.cx;
            local_z = car.z - b.cz;
            if(fabsf(local_x) >= half_w || fabsf(local_z) >= half_d) continue;
            impact_speed=fabsf(car.longitudinal);
            pen_x = half_w - fabsf(local_x);
            pen_z = half_d - fabsf(local_z);
            if(pen_x < pen_z)
                car.x = b.cx + (local_x < 0.0f ? -half_w : half_w);
            else
                car.z = b.cz + (local_z < 0.0f ? -half_d : half_d);
            car.longitudinal *= -0.18f;
            car.lateral *= -0.26f;
            car.yaw_rate *= -0.30f;
            game.impact_flash = 0.18f;
            audio_trigger_impact(.52f+clampf(impact_speed/28.0f,
                                             0.0f,.68f));
        }
    }
}

static void update_physics(const input_t *input, float dt) {
    const float mass = CAR_MASS;
    const float inertia = CAR_INERTIA;
    const float front_arm = 1.32f;
    const float rear_arm = 1.23f;
    const bool handbrake = (input->buttons & CONT_A) != 0;
    const bool clutch_kick_pressed = (input->pressed & CONT_X) != 0;
    const bool road = is_on_road(car.x, car.z);
    const float speed_abs = fabsf(car.longitudinal);
    const float planar_speed_abs=sqrtf(car.longitudinal*car.longitudinal+
                                       car.lateral*car.lateral);
    const float speed_mix = clampf(speed_abs / 48.0f, 0.0f, 1.0f);
    const float steering_limit = 0.70f - speed_mix * 0.29f;
    const float steer_target = input->steer * steering_limit;
    const float speed_for_slip = fmaxf(speed_abs, 1.15f);
    /* Tire forces still fade to zero at rest, but they reach useful authority
       by walking pace. The former 1.85 m/s ramp let a full-throttle car travel
       most of a block before the front axle could bend its path. */
    const float lateral_force_gate=clampf(planar_speed_abs/.85f,0.0f,1.0f);
    float front_slip, rear_slip, front_force, rear_force, body_slip;
    float rear_grip = road ? 1.05f : 0.56f;
    const float front_grip = road ? 1.14f : 0.62f;
    float rear_grip_target = 1.0f;
    float clutch_strength, rotation_request, maneuver_speed, rotation_room;
    float body_slip_pre, drift_load, spin_guard, throttle_oversteer;
    float yaw_damping, drift_amount, donut_intent;
    float burnout_input;
    bool forward_driveline, brake_requested;
    bool counter_steering;
    bool rear_effect_active=false;
    float engine_force=0.0f, service_brake=0.0f;
    float front_brake_request=0.0f, rear_brake_request=0.0f;
    float front_brake_force=0.0f, rear_long_request, rear_long_force;
    float rear_long_capacity, rear_total_capacity;
    float rear_combined_magnitude, rear_combined_scale;
    float power_slip_target, power_breakaway_gate, power_turn_load;
    float wheel_speed_target, motion_direction;
    float drag, longitudinal_force;
    float du, dv, dr;
    float world_vx, world_vz;

    /* An automatic arcade reverse is a selected driveline direction, not a
       conclusion drawn from instantaneous body velocity. A rotating car can
       briefly have negative longitudinal velocity while every driven wheel
       is still applying forward torque. Only a deliberate opposite-pedal
       press near rest changes direction. */
    if(car.drive_direction==0) car.drive_direction=1;
    if(car.drive_direction>0&&planar_speed_abs<.65f&&
       input->brake>.18f&&input->throttle<=.08f)
        car.drive_direction=-1;
    else if(car.drive_direction<0&&planar_speed_abs<.65f&&
            input->throttle>.18f&&input->brake<=.08f)
        car.drive_direction=1;
    forward_driveline=car.drive_direction>0;
    burnout_input=forward_driveline?
        clampf((input->throttle-.20f)/.65f,0.0f,1.0f)*
        clampf((input->brake-.18f)/.62f,0.0f,1.0f):0.0f;
    brake_requested=handbrake||
        (forward_driveline&&input->brake>.045f&&
         (car.longitudinal>.15f||input->throttle>.08f))||
        (!forward_driveline&&input->throttle>.045f&&
         car.longitudinal<-.15f);

    car.brake_light=approachf(car.brake_light,brake_requested?1.0f:0.0f,
                              dt*(brake_requested?14.0f:4.0f));

    car.clutch_kick_timer=fmaxf(0.0f,car.clutch_kick_timer-dt);
    car.handbrake_lock=approachf(car.handbrake_lock,handbrake?1.0f:0.0f,
                                 dt*(handbrake?18.0f:6.0f));
    if((input->pressed&CONT_A)&&speed_abs>3.0f)
    {
        audio_trigger_handbrake();
        printf("Drift Los Angeles: handbrake engaged at %.1f mph.\n",
               (double)(speed_abs*2.23694f));
    }

    /* X is a clutch dump, not a second handbrake. The rising edge stores a
       short driveline-energy pulse so a tap is enough. Steering or existing
       rotation decides which way the rear steps out. */
    if(clutch_kick_pressed&&input->throttle>.18f&&speed_abs>3.5f) {
        const float kick_speed=clampf((speed_abs-3.5f)/17.0f,0.15f,1.0f);
        const float kick_direction=fabsf(input->steer)>.055f?input->steer:
            (fabsf(car.yaw_rate)>.055f?(car.yaw_rate>0.0f?1.0f:-1.0f):0.0f);
        car.clutch_kick_timer=.45f;
        audio_trigger_clutch();
        car.longitudinal+=(car.longitudinal>=0.0f?1.0f:-1.0f)*
                          (.48f+.72f*input->throttle);
        car.lateral+=kick_direction*(.72f+.72f*kick_speed);
        car.yaw_rate+=kick_direction*(.11f+.15f*kick_speed);
        game.exhaust_flash=fmaxf(game.exhaust_flash,.72f);
        emit_smoke(-.94f,-1.55f);
        emit_smoke(.94f,-1.55f);
        emit_smoke(-.66f,-1.68f);
        emit_smoke(.66f,-1.68f);
        printf("Drift Los Angeles: clutch kick -- driveline dumped at %.1f mph.\n",
               (double)(speed_abs*2.23694f));
    }
    clutch_strength=clampf(car.clutch_kick_timer/.45f,0.0f,1.0f);
    body_slip_pre=atan2f(car.lateral,speed_for_slip);
    drift_load=clampf((fabsf(body_slip_pre)-.08f)/.54f,0.0f,1.0f);
    spin_guard=clampf((fabsf(body_slip_pre)-.48f)/.25f,0.0f,1.0f);

    car.steer = approachf(car.steer, steer_target,
        dt * (car.steer * steer_target < -0.002f ? 9.5f : 6.2f));

    /* Resolve the driven axle before lateral forces so the rear tires share
       one friction budget between acceleration and cornering. The previous
       model added engine and brake forces directly to the chassis, which
       made simultaneous pedals cancel algebraically: the rear wheels could
       never rotate faster than the car and a brake-standing burnout was
       impossible. */
    if(forward_driveline) {
        if(input->throttle>.001f) {
            const float throttle_curve=input->throttle*
                (.46f+.54f*input->throttle);
            const float speed_ratio=clampf(speed_abs/82.0f,0.0f,1.0f);
            const float power_curve=1.0f-.44f*speed_ratio;
            const float drift_torque=1.0f+input->throttle*drift_load*
                (.30f-.10f*spin_guard);
            engine_force=throttle_curve*14500.0f*power_curve*drift_torque;
            engine_force*=1.0f+clutch_strength*.55f;
        }
        if(input->brake>.02f&&
           (car.longitudinal>.15f||input->throttle>.08f))
            service_brake=input->brake;
        else if(input->brake>.12f&&input->throttle<=.08f&&
                car.longitudinal<=.5f)
            engine_force=-input->brake*4300.0f;
    }
    else {
        engine_force=-input->brake*4300.0f;
        if(input->throttle>.05f)
            service_brake=input->throttle*(9000.0f/13200.0f);
    }

    {
        /* The C7-like brake balance is front-heavy. During pedal overlap the
           rear share falls as the engine overcomes the rear brakes, while the
           front axle retains enough static capacity to hold the chassis. */
        const float rear_brake_share=.34f-.16f*burnout_input;
        const float rear_load=.48f+.14f*input->throttle*
            clampf(1.0f-speed_abs/55.0f,0.0f,1.0f);
        front_brake_request=service_brake*13200.0f*(1.0f-rear_brake_share);
        rear_brake_request=service_brake*13200.0f*rear_brake_share+
                           car.handbrake_lock*2650.0f;
        if(car.longitudinal>.15f) motion_direction=1.0f;
        else if(car.longitudinal<-.15f) motion_direction=-1.0f;
        else if(engine_force>.01f) motion_direction=1.0f;
        else if(engine_force<-.01f) motion_direction=-1.0f;
        else motion_direction=0.0f;

        rear_long_capacity=mass*9.81f*rear_load*(road?1.18f:.58f);
        rear_long_request=engine_force-motion_direction*rear_brake_request;
        rear_long_force=clampf(rear_long_request,-rear_long_capacity,
                               rear_long_capacity);
        front_brake_force=-motion_direction*front_brake_request;

        power_slip_target=rear_long_request>0.0f?
            clampf((rear_long_request-rear_long_capacity*.88f)/
                   (rear_long_capacity*.34f),0.0f,1.0f):0.0f;
        /* Full throttle can overwhelm first gear from rest, but a straight
           car should hook up as road speed and gearing rise. Existing body
           slip re-opens the breakaway window so throttle still balances a
           fast drift instead of becoming inert above launch speed. */
        power_breakaway_gate=fmaxf(
            clampf((28.0f-speed_abs)/16.0f,0.0f,1.0f),
            clampf(drift_load*1.55f,0.0f,1.0f));
        power_slip_target*=power_breakaway_gate;
        power_slip_target=fmaxf(power_slip_target,burnout_input*.92f);
        power_slip_target=fmaxf(power_slip_target,clutch_strength*.55f);
        car.rear_power_slip=approachf(car.rear_power_slip,power_slip_target,
            dt*(power_slip_target>car.rear_power_slip?7.8f:2.8f));
        car.burnout=approachf(car.burnout,
            burnout_input*car.rear_power_slip,
            dt*(burnout_input>.01f?8.5f:3.2f));
        power_turn_load=car.rear_power_slip*
            clampf((fabsf(input->steer)-.32f)/.48f,0.0f,1.0f)*
            clampf((15.0f-speed_abs)/12.0f,0.0f,1.0f);

        wheel_speed_target=car.longitudinal;
        if(car.rear_power_slip>.001f) {
            const float wheel_direction=rear_long_request>=0.0f?1.0f:-1.0f;
            wheel_speed_target+=wheel_direction*car.rear_power_slip*
                (3.0f+31.0f*input->throttle);
        }
        wheel_speed_target*=1.0f-car.handbrake_lock*.94f;
        car.rear_wheel_speed=approachf(car.rear_wheel_speed,
            wheel_speed_target,dt*(fabsf(wheel_speed_target)>
                                   fabsf(car.rear_wheel_speed)?48.0f:34.0f));

        /* Spinning rubber has a lower resultant-force peak than a hooked-up
           tire, but its final direction depends on both longitudinal and
           lateral slip. The vector normalization below resolves that once
           the rear lateral demand is known. */
        rear_total_capacity=rear_long_capacity*
            (1.0f-.18f*car.rear_power_slip-.40f*power_turn_load);
    }

    if(car.handbrake_lock>.01f)
        rear_grip_target=1.0f-car.handbrake_lock*.86f;
    else if(clutch_strength>.01f) rear_grip_target = 0.28f;
    car.rear_grip_scale=approachf(car.rear_grip_scale,rear_grip_target,
        dt*(rear_grip_target<car.rear_grip_scale?
            (handbrake?13.0f:10.0f):3.0f));
    rear_grip *= car.rear_grip_scale;

    front_slip = atan2f(car.lateral + front_arm * car.yaw_rate, speed_for_slip) - car.steer;
    rear_slip = atan2f(car.lateral - rear_arm * car.yaw_rate, speed_for_slip);
    front_force = clampf(-72000.0f * front_slip,
                         -mass * 9.81f * 0.52f * front_grip,
                          mass * 9.81f * 0.52f * front_grip)*
                  lateral_force_gate;
    rear_force = clampf(-65000.0f * rear_slip,
                        -mass * 9.81f * 0.48f * rear_grip,
                         mass * 9.81f * 0.48f * rear_grip)*
                 lateral_force_gate;
    /* Inside the useful drift-angle window, throttle progressively spends a
       small part of the rear tire's lateral capacity. Lifting restores rear
       bite; the effect fades before the car reaches a spin angle. */
    throttle_oversteer=road?input->throttle*drift_load*(1.0f-spin_guard):0.0f;
    rear_force*=clampf(1.0f-car.handbrake_lock*.74f-
                       clutch_strength*.34f-throttle_oversteer*.16f,
                       0.12f,1.0f);

    /* Resolve the rear contact patch as one force vector. In a donut, large
       lateral slip rotates some of the available force sideways instead of
       allowing maximum forward thrust and maximum cornering independently.
       That naturally keeps the rear circling the loaded front axle. */
    rear_combined_magnitude=sqrtf(rear_long_force*rear_long_force+
                                  rear_force*rear_force);
    rear_combined_scale=rear_combined_magnitude>rear_total_capacity?
        rear_total_capacity/rear_combined_magnitude:1.0f;
    rear_long_force*=rear_combined_scale;
    rear_force*=rear_combined_scale;
    if(planar_speed_abs<.65f&&service_brake>.0f) {
        const float held=fminf(front_brake_request,fabsf(rear_long_force));
        front_brake_force=rear_long_force>=0.0f?-held:held;
    }

    drag = 0.42f * car.longitudinal * speed_abs +
           (road ? 66.0f : 360.0f) * car.longitudinal;
    longitudinal_force = rear_long_force + front_brake_force - drag;
    du = longitudinal_force / mass + car.yaw_rate * car.lateral;
    dv = (front_force + rear_force) / mass - car.yaw_rate * car.longitudinal;
    dr = (front_arm * front_force - rear_arm * rear_force) / inertia;

    /* A locked rear axle should pivot the car around the loaded front tires.
       Preserve forward momentum while adding a speed-scaled yaw/lateral
       response. This also makes a short A tap useful for drift initiation. */
    if(fabsf(input->steer)>.045f) rotation_request=input->steer;
    else if(fabsf(car.yaw_rate)>.055f)
        rotation_request=car.yaw_rate>0.0f?1.0f:-1.0f;
    else if(fabsf(car.lateral)>.18f)
        rotation_request=car.lateral>0.0f?1.0f:-1.0f;
    else rotation_request=0.0f;
    maneuver_speed=clampf((speed_abs-3.0f)/18.0f,0.0f,1.0f);
    rotation_room=clampf((.72f-fabsf(atan2f(car.lateral,speed_for_slip)))/
                         .48f,0.0f,1.0f);
    if(car.handbrake_lock>.01f) {
        dv+=rotation_request*3.25f*maneuver_speed*car.handbrake_lock*
            rotation_room;
        dr+=rotation_request*1.72f*maneuver_speed*car.handbrake_lock*
            rotation_room;
    }
    if(clutch_strength>.01f) {
        dv+=rotation_request*1.38f*maneuver_speed*clutch_strength;
        dr+=rotation_request*.62f*maneuver_speed*clutch_strength;
    }

    car.longitudinal = clampf(car.longitudinal + du * dt, -11.0f, 82.0f);
    car.lateral = clampf(car.lateral + dv * dt, -24.0f, 24.0f);
    car.yaw_rate = clampf(car.yaw_rate + dr * dt, -2.05f, 2.05f);
    body_slip=atan2f(car.lateral,speed_for_slip);
    drift_amount=clampf((fabsf(body_slip)-0.10f)/0.52f,0.0f,1.0f);
    donut_intent=car.rear_power_slip*
        clampf((fabsf(car.steer)-.22f)/.30f,0.0f,1.0f)*
        clampf((14.0f-speed_abs)/11.0f,0.0f,1.0f);
    counter_steering=speed_abs>6.0f && fabsf(car.yaw_rate)>0.12f &&
                     car.steer*car.yaw_rate<0.0f;
    yaw_damping=road?0.42f:1.25f;
    yaw_damping+=drift_amount*.72f*(1.0f-.72f*donut_intent);
    if(counter_steering)
        yaw_damping+=1.25f+2.15f*fabsf(input->steer)*drift_amount;
    /* Above roughly 28 degrees, progressively trade yaw for stability. This
       keeps full throttle useful without turning it into a spin button. */
    spin_guard=clampf((fabsf(body_slip)-.48f)/.25f,0.0f,1.0f);
    yaw_damping+=spin_guard*(1.30f+.90f*input->throttle)*
                 (1.0f-.84f*donut_intent);
    if(fabsf(body_slip)>.72f)
        yaw_damping+=(fabsf(body_slip)-.72f)*3.0f*
                     (1.0f-.84f*donut_intent);
    car.yaw_rate *= fmaxf(0.0f,1.0f-dt*yaw_damping);
    if(spin_guard>0.0f)
        car.lateral*=fmaxf(0.0f,1.0f-dt*spin_guard*
                           (.14f+.20f*input->throttle)*
                           (1.0f-.82f*donut_intent));
    if(counter_steering)
        car.lateral *= fmaxf(0.0f,1.0f-dt*(0.10f+0.22f*drift_amount));
    if(speed_abs < 0.15f && input->throttle < 0.02f && input->brake < 0.02f)
        car.longitudinal = 0.0f;
    car.yaw = wrap_angle(car.yaw + car.yaw_rate * dt);

    world_vx = fsin(car.yaw) * car.longitudinal + fcos(car.yaw) * car.lateral;
    world_vz = fcos(car.yaw) * car.longitudinal - fsin(car.yaw) * car.lateral;
    car.x += world_vx * dt;
    car.z += world_vz * dt;
    front_wheel_spin=fmodf(front_wheel_spin+
        car.longitudinal*dt/.44f,PI*2.0f);
    rear_wheel_spin=fmodf(rear_wheel_spin+
        car.rear_wheel_speed*dt/.44f,PI*2.0f);
    collide_buildings();
    collide_street_furniture();
    collide_pedestrians();

    game.drift_angle = atan2f(fabsf(car.lateral), fmaxf(fabsf(car.longitudinal), 0.8f)) * 180.0f / PI;
    if(road&&input->throttle>.20f&&car.rear_power_slip>.18f&&
       fabsf(car.rear_wheel_speed-car.longitudinal)>2.5f&&
       (car.burnout>.18f||speed_abs<18.0f||game.drift_angle>10.0f)) {
        const bool heavy=car.rear_power_slip>.62f;
        rear_effect_active=true;
        game.smoke_timer-=dt;
        game.skid_timer-=dt;
        if(game.smoke_timer<=0.0f) {
            emit_smoke(-.94f,-1.55f);
            emit_smoke(.94f,-1.55f);
            if(heavy) {
                emit_smoke(-.68f,-1.70f);
                emit_smoke(.68f,-1.70f);
            }
            game.smoke_timer=heavy?.040f:.055f;
        }
        if(game.skid_timer<=0.0f) {
            emit_skid(-.92f,0);
            emit_skid(.92f,1);
            skid_contact_valid=true;
            game.skid_timer=.052f;
        }
    }
    if(road&&speed_abs>6.0f&&car.handbrake_lock>.30f&&
       game.drift_angle<=10.0f) {
        rear_effect_active=true;
        game.smoke_timer-=dt;
        game.skid_timer-=dt;
        if(game.smoke_timer<=0.0f) {
            emit_smoke(-.94f,-1.55f);
            emit_smoke(.94f,-1.55f);
            game.smoke_timer=.055f;
        }
        if(game.skid_timer<=0.0f) {
            emit_skid(-.92f,0);
            emit_skid(.92f,1);
            skid_contact_valid=true;
            game.skid_timer=.055f;
        }
    }
    if(road && speed_abs > 8.0f && game.drift_angle > 10.0f && game.drift_angle < 72.0f) {
        const float quality = clampf((game.drift_angle - 8.0f) / 32.0f, 0.0f, 1.35f);
        const int bonus_step = (int)(game.drift_duration / 2.5f);
        rear_effect_active=true;
        game.drift_hold = 1.15f;
        game.drift_duration += dt;
        if(game.drift_duration > game.longest_drift)
            game.longest_drift = game.drift_duration;
        game.drift_chain += speed_abs * quality * dt * 22.0f;
        if(bonus_step > game.drift_bonus_step) {
            game.drift_chain += (float)bonus_step * 420.0f;
            game.drift_bonus_step = bonus_step;
        }
        game.drift_multiplier = clampf(1.0f + game.drift_chain / 3800.0f +
                                       game.drift_duration * 0.105f, 1.0f, 6.0f);
        game.smoke_timer -= dt;
        game.skid_timer -= dt;
        if(game.smoke_timer <= 0.0f) {
            emit_smoke(-0.94f, -1.55f);
            emit_smoke(0.94f, -1.55f);
            if(game.drift_duration > 2.2f) {
                emit_smoke(-0.82f, -1.45f);
                emit_smoke(0.82f, -1.45f);
            }
            if(game.drift_duration > 5.0f) {
                emit_smoke(-0.54f, -1.80f);
                emit_smoke(0.54f, -1.80f);
            }
            game.smoke_timer = game.drift_duration > 5.0f ? 0.045f :
                               (game.drift_duration > 2.2f ? 0.052f : 0.060f);
        }
        if(game.skid_timer <= 0.0f) {
            emit_skid(-0.92f, 0);
            emit_skid(0.92f, 1);
            skid_contact_valid = true;
            game.skid_timer = 0.060f;
        }
    }
    else if(game.drift_chain > 0.0f) {
        if(!rear_effect_active) skid_contact_valid = false;
        game.drift_hold -= dt;
        if(game.drift_hold <= 0.0f) {
            game.score += game.drift_chain * game.drift_multiplier;
            if(game.score > game.best_score) game.best_score = game.score;
            game.drift_chain = 0.0f;
            game.drift_multiplier = 1.0f;
            game.drift_duration = 0.0f;
            game.drift_bonus_step = 0;
        }
    }
    else {
        if(!rear_effect_active) skid_contact_valid = false;
        game.drift_duration = 0.0f;
        game.drift_bonus_step = 0;
    }
}

#ifdef DRIFT_LA_COLLISION_QA
/* Scripted street-collision regression: the car is placed a short run from
   a lamppost, a parked car and a walking pedestrian in turn. Each phase
   checks that the obstacle registered an impact, that the car never passed
   through it, and that the pedestrian was launched and came to rest. */
typedef struct {
    int phase;
    int failures;
    float phase_time;
    float target_x, target_z;
    float forward_x, forward_z;
    float min_clearance;
    float peak_height;
    bool impact_seen;
    bool pedestrian_hit;
    bool pedestrian_grounded;
    bool finished;
} collision_qa_t;

static collision_qa_t collision_qa;

static void collision_qa_place(float x, float z, float yaw, float speed) {
    reset_car();
    car.x = x;
    car.z = z;
    car.yaw = yaw;
    car.longitudinal = speed;
    car.rear_wheel_speed = speed;
    game.impact_flash = 0.0f;
}

static void collision_qa_begin_phase(int phase) {
    collision_qa_t *qa = &collision_qa;
    qa->phase = phase;
    qa->phase_time = 0.0f;
    qa->min_clearance = 1000.0f;
    qa->peak_height = 0.0f;
    qa->impact_seen = false;
    qa->pedestrian_hit = false;
    qa->pedestrian_grounded = false;
    memset(hit_pedestrians, 0, sizeof(hit_pedestrians));
    if(phase == 0) {
        block_lamppost(0, 0, 0, &qa->target_x, &qa->target_z);
        qa->forward_x = 0.0f;
        qa->forward_z = 1.0f;
        collision_qa_place(qa->target_x, qa->target_z - 30.0f, 0.0f, 22.0f);
        printf("Drift Los Angeles collision QA: lamppost run at %.1f,%.1f.\n",
               qa->target_x, qa->target_z);
    }
    else if(phase == 1) {
        traffic_t parked;
        bool found = false;
        int cx, cz;
        for(cz = -2; cz <= 2 && !found; ++cz)
            for(cx = -2; cx <= 2 && !found; ++cx)
                found = parked_car_for_cell(cx, cz, 0, &parked);
        if(!found) {
            printf("Drift Los Angeles collision QA: parked car FAIL -- none near the origin.\n");
            ++qa->failures;
            collision_qa_begin_phase(2);
            return;
        }
        qa->target_x = parked.x;
        qa->target_z = parked.z;
        qa->forward_x = fsin(parked.yaw);
        qa->forward_z = fcos(parked.yaw);
        collision_qa_place(parked.x - qa->forward_x * 28.0f,
                           parked.z - qa->forward_z * 28.0f, parked.yaw, 22.0f);
        printf("Drift Los Angeles collision QA: parked car run at %.1f,%.1f yaw=%.2f.\n",
               parked.x, parked.z, parked.yaw);
    }
    else {
        const building_t building = building_for_cell(0, 0);
        pedestrian_pose_t pose;
        block_pedestrian(0, 0, &building, 0, &pose);
        qa->target_x = pose.x;
        qa->target_z = pose.z;
        qa->forward_x = 1.0f;
        qa->forward_z = 0.0f;
        collision_qa_place(pose.x - 24.0f, pose.z, PI * .5f, 18.0f);
        printf("Drift Los Angeles collision QA: pedestrian run at %.1f,%.1f.\n",
               pose.x, pose.z);
    }
}

static void collision_qa_start(void) {
    memset(&collision_qa, 0, sizeof(collision_qa));
    collision_qa_begin_phase(0);
}

static void collision_qa_input(input_t *input) {
    const collision_qa_t *qa = &collision_qa;
    const bool done = qa->phase == 2 ? qa->pedestrian_hit : false;
    input->connected = true;
    input->buttons = 0;
    input->pressed = 0;
    input->steer = 0.0f;
    input->throttle = done ? 0.0f : 1.0f;
    input->brake = done ? 1.0f : 0.0f;
}

static bool collision_qa_state_sane(void) {
    return car.x == car.x && car.z == car.z && car.yaw == car.yaw &&
           fabsf(car.longitudinal) <= 82.0f && fabsf(car.lateral) <= 24.0f &&
           fabsf(car.yaw_rate) <= 2.05f;
}

/* Returns true once every phase has reported. */
static bool collision_qa_observe(float dt) {
    collision_qa_t *qa = &collision_qa;
    const float ahead = (qa->target_x - car.x) * qa->forward_x +
                        (qa->target_z - car.z) * qa->forward_z;
    const float phase_length = qa->phase == 2 ? 4.5f : 3.0f;
    if(qa->finished) return true;
    qa->phase_time += dt;
    if(!collision_qa_state_sane()) {
        printf("Drift Los Angeles collision QA: phase %d FAIL -- car state left its limits.\n",
               qa->phase);
        ++qa->failures;
        qa->finished = true;
    }
    if(game.impact_flash > 0.0f) qa->impact_seen = true;
    if(qa->phase < 2) {
        qa->min_clearance = fminf(qa->min_clearance, ahead);
    }
    else {
        const hit_pedestrian_t *hit = find_hit_pedestrian(0, 0, 0);
        if(hit) {
            qa->pedestrian_hit = true;
            qa->peak_height = fmaxf(qa->peak_height, hit->y);
            if(hit->grounded) qa->pedestrian_grounded = true;
        }
    }
    if(qa->finished || qa->phase_time < phase_length) return qa->finished;
    if(qa->phase == 0) {
        /* The front disc rests 2.5 m short of the post's axis. */
        const bool pass = qa->impact_seen && qa->min_clearance > 2.0f;
        printf("Drift Los Angeles collision QA: lamppost %s -- impact=%d clearance=%.2fm speed=%.2fm/s.\n",
               pass ? "PASS" : "FAIL", qa->impact_seen ? 1 : 0,
               qa->min_clearance, car.longitudinal);
        if(!pass) ++qa->failures;
        collision_qa_begin_phase(1);
    }
    else if(qa->phase == 1) {
        /* Half length plus the disc and its offset keep 4.55 m of axis. */
        const bool pass = qa->impact_seen && qa->min_clearance > 4.0f;
        printf("Drift Los Angeles collision QA: parked car %s -- impact=%d clearance=%.2fm speed=%.2fm/s.\n",
               pass ? "PASS" : "FAIL", qa->impact_seen ? 1 : 0,
               qa->min_clearance, car.longitudinal);
        if(!pass) ++qa->failures;
        collision_qa_begin_phase(2);
    }
    else {
        const bool pass = qa->pedestrian_hit && qa->peak_height > .4f &&
                          qa->pedestrian_grounded;
        printf("Drift Los Angeles collision QA: pedestrian %s -- hit=%d peak=%.2fm grounded=%d.\n",
               pass ? "PASS" : "FAIL", qa->pedestrian_hit ? 1 : 0,
               qa->peak_height, qa->pedestrian_grounded ? 1 : 0);
        if(!pass) ++qa->failures;
        printf("Drift Los Angeles collision QA: %s.\n",
               qa->failures == 0 ? "ALL TESTS PASS" : "FAILED");
        qa->finished = true;
    }
    return qa->finished;
}
#endif

static bool update_game(const input_t *input, float dt) {
    district_t district;
    game.time += dt;
    game.impact_flash = fmaxf(0.0f, game.impact_flash - dt);
    game.exhaust_flash=fmaxf(0.0f,game.exhaust_flash-dt*3.8f);
    game.hud_timer -= dt;
    game.district_banner=fmaxf(0.0f,game.district_banner-dt);
    if(game.mode == MODE_TITLE) {
        if(input->pressed & (CONT_START | CONT_A)) start_run();
        else if(input->pressed & CONT_X) start_demo();
        else if(input->pressed & CONT_B) return false;
        car.longitudinal = 8.0f;
        car.z += dt * 3.0f;
        front_wheel_spin=fmodf(front_wheel_spin+dt*8.0f/.40f,PI*2.0f);
        rear_wheel_spin=front_wheel_spin;
        car.yaw = fsin(game.time * 0.22f) * 0.04f;
    }
    else if(game.mode == MODE_DEMO) {
#ifdef DRIFT_LA_CAPTURE_SEGMENT
        const int segment=DRIFT_LA_CAPTURE_SEGMENT%4;
#else
        const int segment=(int)(game.demo_time/15.0f)%4;
#endif
        const float local=fmodf(game.demo_time,15.0f);
        const float distance=local*11.0f;
        if(input->pressed&(CONT_START|CONT_A)) {
            start_run();
        }
        else if(input->pressed&CONT_B) {
            game.mode=MODE_TITLE;
            reset_car();
            game.hud_timer=0.0f;
        }
        else {
            game.demo_time+=dt;
#ifdef DRIFT_LA_CAPTURE_SEGMENT
            /* Capture builds repeat a narrow moving review take. This keeps
               composition comparable without freezing the car underneath an
               ever-growing smoke cloud. */
            if(game.demo_time>=(float)segment*15.0f+DRIFT_LA_CAPTURE_LOCAL+
                               DRIFT_LA_CAPTURE_SPAN) {
                game.demo_time=(float)segment*15.0f+DRIFT_LA_CAPTURE_LOCAL;
                camera_initialized=false;
            }
#endif
            car.lateral=fsin(local*1.15f)*3.8f;
            car.yaw_rate=fsin(local*.82f)*.28f;
            car.steer=-fsin(local*.82f)*.24f;
            car.longitudinal=17.0f;
            game.drift_duration=1.2f+local;
            game.drift_chain=650.0f+local*820.0f;
            game.drift_multiplier=1.30f+local*.25f;
            game.drift_angle=18.0f+fabsf(fsin(local*.82f))*32.0f;
            game.longest_drift=fmaxf(game.longest_drift,game.drift_duration);
            if(segment==0) {
                car.x=6.5f+fsin(local*.45f)*3.0f;
                car.z=10.0f+distance;
                car.yaw=fsin(local*.82f)*.20f;
            }
            else if(segment==1) {
                car.x=-233.5f+fsin(local*.40f)*2.5f;
                car.z=8.0f+distance;
                car.yaw=fsin(local*.82f)*.19f;
            }
            else if(segment==2) {
                car.x=246.5f+distance;
                car.z=-6.5f+fsin(local*.45f)*2.8f;
                car.yaw=PI*.5f+fsin(local*.82f)*.20f;
            }
            else {
                car.x=8.0f+distance;
                car.z=246.5f+fsin(local*.50f)*2.8f;
                car.yaw=PI*.5f+fsin(local*.82f)*.21f;
            }
            if(segment!=game.demo_segment) {
                game.demo_segment=segment;
                camera_initialized=false;
                reset_traffic();
                game.district_banner=3.5f;
            }
            front_wheel_spin=fmodf(front_wheel_spin+
                car.longitudinal*dt/.44f,PI*2.0f);
            rear_wheel_spin=front_wheel_spin;
            game.smoke_timer-=dt;
            game.skid_timer-=dt;
            if(game.smoke_timer<=0.0f) {
                emit_smoke(-.94f,-1.55f);
                emit_smoke(.94f,-1.55f);
                if(game.drift_duration>2.2f) {
                    emit_smoke(-.82f,-1.45f);
                    emit_smoke(.82f,-1.45f);
                }
                if(game.drift_duration>5.0f) {
                    emit_smoke(-.54f,-1.80f);
                    emit_smoke(.54f,-1.80f);
                }
                game.smoke_timer=game.drift_duration>5.0f?.045f:
                                 (game.drift_duration>2.2f?.052f:.060f);
            }
            if(game.skid_timer<=0.0f) {
                emit_skid(-.92f,0);
                emit_skid(.92f,1);
                skid_contact_valid=true;
                game.skid_timer=.065f;
            }
            update_traffic(dt);
            update_exhaust_pops(.88f,.90f,dt);
            update_effects(dt);
        }
    }
    else if(game.mode == MODE_PAUSED) {
        if(input->pressed & CONT_START) game.mode = MODE_PLAYING;
        else if(input->pressed & CONT_B) {
            game.mode = MODE_TITLE;
            reset_car();
            game.hud_timer = 0.0f;
        }
    }
    else {
        if(input->pressed & CONT_START) game.mode = MODE_PAUSED;
        else {
            if(input->pressed & CONT_Y) {
                reset_car();
                reset_traffic();
                memset(hit_pedestrians, 0, sizeof(hit_pedestrians));
            }
            if(input->pressed & CONT_B) game.camera_close = !game.camera_close;
            update_physics(input, dt);
#if !defined(DRIFT_LA_PHYSICS_QA) && !defined(DRIFT_LA_COLLISION_QA)
            update_traffic(dt);
#endif
            update_exhaust_pops(input->throttle,engine_rpm_level(),dt);
            update_effects(dt);
        }
    }
    district=district_for_position(car.x,car.z);
    if(district!=game.district) {
        game.district=district;
        game.district_banner=3.0f;
        game.hud_timer=0.0f;
        printf("Drift Los Angeles: entered %s.\n",district_name(district));
    }
    game.previous_buttons = input->buttons;
    return true;
}

static void setup_camera(float dt) {
    const bool demo=game.mode==MODE_DEMO;
#ifdef DRIFT_LA_CAR_CAPTURE
    if(demo) {
#ifndef DRIFT_LA_CAR_CAPTURE_BASE
#define DRIFT_LA_CAR_CAPTURE_BASE 2.05f
#endif
#ifndef DRIFT_LA_CAR_CAPTURE_SPEED
#define DRIFT_LA_CAR_CAPTURE_SPEED .08f
#endif
        const float orbit=car.yaw+DRIFT_LA_CAR_CAPTURE_BASE+
                          game.demo_time*DRIFT_LA_CAR_CAPTURE_SPEED;
        const float distance=5.70f;
        camera_yaw=orbit;
        camera_x=car.x-fsin(orbit)*distance;
        camera_z=car.z-fcos(orbit)*distance;
        camera_y=1.46f;
        camera_sin_yaw=fsin(camera_yaw);
        camera_cos_yaw=fcos(camera_yaw);
        camera_sin_pitch=fsin(-.115f);
        camera_cos_pitch=fcos(-.115f);
        camera_sin_roll=0.0f;
        camera_cos_roll=1.0f;
        camera_focal=500.0f;
        camera_initialized=true;
        return;
    }
#endif
    const float speed_mix=clampf(fabsf(car.longitudinal)/66.0f,0.0f,1.0f);
    const float drift_view=clampf(
        atan2f(car.lateral,fmaxf(fabsf(car.longitudinal),8.0f))*.62f,
        -.30f,.30f);
    /* A low, close chase rig is fundamental to the new composition.  The old
       camera treated the vehicle like a map marker and spent half the 480p
       frame on empty asphalt.  This framing keeps the hero car near one third
       of the screen while following velocity enough to expose its side during
       a drift instead of staring squarely at the rear fascia. */
    const float distance = (demo ? 5.18f : (game.camera_close ? 4.48f : 5.38f))+
                           speed_mix*.38f;
    const float height = (demo ? 2.28f : (game.camera_close ? 2.04f : 2.34f))+
                         speed_mix*.10f;
    const float follow = 1.0f - expf(-dt * 6.4f);
    const bool snap = !camera_initialized;
    float yaw_delta;
    float target_x, target_z, side_offset;
    if(!camera_initialized) {
        camera_yaw = car.yaw;
        camera_initialized = true;
    }
    yaw_delta = wrap_angle(car.yaw+drift_view+
        (demo?.30f+fsin(game.demo_time*.42f)*.09f:0.0f)-camera_yaw);
    camera_yaw = wrap_angle(camera_yaw + yaw_delta * follow);
    target_x = car.x - fsin(camera_yaw) * distance;
    target_z = car.z - fcos(camera_yaw) * distance;
    side_offset=clampf(car.lateral*.026f+car.yaw_rate*.16f,-.46f,.46f);
    target_x += fcos(camera_yaw)*side_offset;
    target_z -= fsin(camera_yaw)*side_offset;
    if(snap || game.time < 0.1f || !isfinite(camera_x)) {
        camera_x = target_x;
        camera_z = target_z;
        camera_y = height;
    }
    camera_x += (target_x - camera_x) * follow;
    camera_z += (target_z - camera_z) * follow;
    camera_y += (height - camera_y) * follow;
    camera_sin_yaw = fsin(camera_yaw);
    camera_cos_yaw = fcos(camera_yaw);
    camera_sin_pitch = fsin(demo ? -.130f : (game.camera_close ? -.105f : -.130f));
    camera_cos_pitch = fcos(demo ? -.130f : (game.camera_close ? -.105f : -.130f));
    camera_sin_roll = fsin(clampf(-car.yaw_rate * 0.045f, -0.080f, 0.080f));
    camera_cos_roll = fcos(clampf(-car.yaw_rate * 0.045f, -0.080f, 0.080f));
    camera_focal = (demo ? 500.0f : (game.camera_close ? 512.0f : 498.0f))-
                   speed_mix*12.0f;
}

static vec3_t world_to_camera(vec3_t world) {
    const float dx = world.x - camera_x;
    const float dy = world.y - camera_y;
    const float dz = world.z - camera_z;
#ifdef DRIFT_LA_SCALAR_RENDER_MATH
    const float yaw_x = dx * camera_cos_yaw - dz * camera_sin_yaw;
    const float yaw_z = dx * camera_sin_yaw + dz * camera_cos_yaw;
    const float pitch_y = dy * camera_cos_pitch - yaw_z * camera_sin_pitch;
    const float pitch_z = dy * camera_sin_pitch + yaw_z * camera_cos_pitch;
    const float roll_x = yaw_x * camera_cos_roll - pitch_y * camera_sin_roll;
    const float roll_y = yaw_x * camera_sin_roll + pitch_y * camera_cos_roll;
    return (vec3_t){roll_x,roll_y,pitch_z};
#else
    const shz_vec3_t rotated=dla_rotate_camera(dx,dy,dz);
    return (vec3_t){rotated.x,rotated.y,rotated.z};
#endif
}

static bool project_camera(vec3_t camera, screen_point_t *out) {
    if(camera.z<NEAR_PLANE||camera.z>FAR_PLANE) {
        out->valid = false;
        return false;
    }
    out->z=dla_depth_reciprocal(camera.z);
    out->x=SCREEN_CX+camera.x*camera_focal*out->z;
    out->y=SCREEN_CY-camera.y*camera_focal*out->z;
    out->valid = true;
    return true;
}

static bool project_world(vec3_t world, screen_point_t *out) {
    return project_camera(world_to_camera(world),out);
}

static bool world_sphere_visible(vec3_t center, float radius, float margin) {
    const vec3_t view=world_to_camera(center);
    const float half=(SCREEN_CX+margin)*fmaxf(view.z,NEAR_PLANE)/camera_focal+radius;
    if(view.z+radius<NEAR_PLANE || view.z-radius>FAR_PLANE) return false;
    return fabsf(view.x)<=half;
}

static bool screen_quad_visible(const screen_point_t *a,
                                const screen_point_t *b,
                                const screen_point_t *c,
                                const screen_point_t *d) {
    const float margin=48.0f;
    if(a->x<-margin && b->x<-margin && c->x<-margin && d->x<-margin) return false;
    if(a->x>SCREEN_W+margin && b->x>SCREEN_W+margin &&
       c->x>SCREEN_W+margin && d->x>SCREEN_W+margin) return false;
    if(a->y<-margin && b->y<-margin && c->y<-margin && d->y<-margin) return false;
    if(a->y>SCREEN_H+margin && b->y>SCREEN_H+margin &&
       c->y>SCREEN_H+margin && d->y>SCREEN_H+margin) return false;
    return true;
}

static bool screen_triangle_visible(const screen_point_t *a,
                                    const screen_point_t *b,
                                    const screen_point_t *c) {
    const float margin=48.0f;
    if(a->x<-margin && b->x<-margin && c->x<-margin) return false;
    if(a->x>SCREEN_W+margin && b->x>SCREEN_W+margin && c->x>SCREEN_W+margin)
        return false;
    if(a->y<-margin && b->y<-margin && c->y<-margin) return false;
    if(a->y>SCREEN_H+margin && b->y>SCREEN_H+margin && c->y>SCREEN_H+margin)
        return false;
    return true;
}

static void begin_poly_list(pvr_list_t list) {
    pvr_list_begin(list);
    active_poly_header = NULL;
}

static void submit_header(const pvr_poly_hdr_t *header) {
    if(active_poly_header == header) return;
    pvr_prim(header, sizeof(*header));
    active_poly_header = (pvr_poly_hdr_t *)header;
}

static inline void make_vertex(pvr_vertex_t *vertex, const screen_point_t *point,
                        float u, float v, uint32_t color, bool end) {
    vertex->flags = end ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
    vertex->x = point->x;
    vertex->y = point->y;
    vertex->z = point->z;
    vertex->u = u;
    vertex->v = v;
    vertex->argb = color;
    vertex->oargb = 0;
}

#ifndef DRIFT_LA_STAGED_SUBMISSION
/* Immediate-mode lists own the store queues between begin/finish. Fill every
   word of the write-only target, then commit before requesting the next one.
   Keep headers ahead of vertex targets; this path is not a DMA RAM buffer. */
static inline void submit_vertex(const screen_point_t *point,
                                 float u,float v,uint32_t color,bool end) {
    pvr_vertex_t *vertex=pvr_dr_target();
    make_vertex(vertex,point,u,v,color,end);
    pvr_dr_commit(vertex);
}
#endif

static void submit_triangle(const pvr_poly_hdr_t *header,
                            const screen_point_t *a, const screen_point_t *b,
                            const screen_point_t *c,
                            float ua, float va, float ub, float vb, float uc, float vc,
                            uint32_t ca, uint32_t cb, uint32_t cc) {
    submit_header(header);
#ifdef DRIFT_LA_STAGED_SUBMISSION
    pvr_vertex_t vertices[3];
    make_vertex(&vertices[0], a, ua, va, ca, false);
    make_vertex(&vertices[1], b, ub, vb, cb, false);
    make_vertex(&vertices[2], c, uc, vc, cc, true);
    pvr_prim(vertices, sizeof(vertices));
#else
    submit_vertex(a,ua,va,ca,false);
    submit_vertex(b,ub,vb,cb,false);
    submit_vertex(c,uc,vc,cc,true);
#endif
    QA_GEOMETRY(1,3);
}

static void submit_quad(const pvr_poly_hdr_t *header,
                        const screen_point_t *a, const screen_point_t *b,
                        const screen_point_t *c, const screen_point_t *d,
                        float ua, float va, float ub, float vb,
                        float uc, float vc, float ud, float vd,
                        uint32_t ca, uint32_t cb, uint32_t cc, uint32_t cd) {
    submit_header(header);
#ifdef DRIFT_LA_STAGED_SUBMISSION
    pvr_vertex_t vertices[4];
    make_vertex(&vertices[0], a, ua, va, ca, false);
    make_vertex(&vertices[1], b, ub, vb, cb, false);
    make_vertex(&vertices[2], c, uc, vc, cc, false);
    make_vertex(&vertices[3], d, ud, vd, cd, true);
    pvr_prim(vertices, sizeof(vertices));
#else
    submit_vertex(a,ua,va,ca,false);
    submit_vertex(b,ub,vb,cb,false);
    submit_vertex(c,uc,vc,cc,false);
    submit_vertex(d,ud,vd,cd,true);
#endif
    QA_GEOMETRY(2,4);
}

static void draw_rect(const pvr_poly_hdr_t *header, float x, float y,
                      float width, float height, float z, uint32_t color) {
    const screen_point_t a = {x, y, z, true};
    const screen_point_t b = {x + width, y, z, true};
    const screen_point_t c = {x, y + height, z, true};
    const screen_point_t d = {x + width, y + height, z, true};
    submit_quad(header, &a, &b, &c, &d, 0,0,1,0,0,1,1,1,
                color,color,color,color);
}

static void draw_disc(const pvr_poly_hdr_t *header, float x, float y,
                      float radius, float z, int segments,
                      uint32_t center_color, uint32_t edge_color) {
    const screen_point_t center = {x, y, z, true};
    int i;
    if(x+radius<-24.0f || x-radius>SCREEN_W+24.0f ||
       y+radius<-24.0f || y-radius>SCREEN_H+24.0f)
        return;
    for(i = 0; i < segments; ++i) {
        const float a0 = (float)i * PI * 2.0f / (float)segments;
        const float a1 = (float)(i + 1) * PI * 2.0f / (float)segments;
        const screen_point_t p0 = {x + fcos(a0) * radius, y + fsin(a0) * radius, z, true};
        const screen_point_t p1 = {x + fcos(a1) * radius, y + fsin(a1) * radius, z, true};
        submit_triangle(header, &center, &p0, &p1, 0.5f,0.5f,0,0,1,0,
                        center_color, edge_color, edge_color);
    }
}

typedef struct {
    vec3_t point;
    float u,v;
} clip_vertex_t;

static clip_vertex_t clip_lerp(clip_vertex_t a, clip_vertex_t b, float z) {
    const float t=(z-a.point.z)/(b.point.z-a.point.z);
    clip_vertex_t out;
    out.point.x=a.point.x+(b.point.x-a.point.x)*t;
    out.point.y=a.point.y+(b.point.y-a.point.y)*t;
    out.point.z=z;
    out.u=a.u+(b.u-a.u)*t;
    out.v=a.v+(b.v-a.v)*t;
    return out;
}

static int clip_depth_plane(const clip_vertex_t *input, int count,
                            clip_vertex_t *output, float plane,
                            bool keep_greater) {
    clip_vertex_t previous=input[count-1];
    bool previous_inside=keep_greater ? previous.point.z>=plane : previous.point.z<=plane;
    int i,out_count=0;
    for(i=0;i<count;++i) {
        const clip_vertex_t current=input[i];
        const bool current_inside=keep_greater ? current.point.z>=plane : current.point.z<=plane;
        if(current_inside!=previous_inside)
            output[out_count++]=clip_lerp(previous,current,plane);
        if(current_inside) output[out_count++]=current;
        previous=current;
        previous_inside=current_inside;
    }
    return out_count;
}

/* Keep clipping storage out of the ordinary quad path. Only depth-rejected
   quads enter this helper; its first test also discards fully hidden geometry. */
static __attribute__((noinline)) void draw_clipped_camera_quad(
                            const pvr_poly_hdr_t *header,
                            const vec3_t camera[4],
                            float ua, float va, float ub, float vb,
                            float uc, float vc, float ud, float vd,
                            uint32_t color) {
    clip_vertex_t first[4],second[8];
    screen_point_t projected[8];
    float min_z=FAR_PLANE+1.0f,max_z=0.0f;
    int count,i;
    for(i=0;i<4;++i) {
        min_z=fminf(min_z,camera[i].z);
        max_z=fmaxf(max_z,camera[i].z);
    }
    /* Only near-plane crossings need a clipped fan. Distant geometry is
       intentionally culled as a whole, just as it was before clipping. */
    if(max_z<NEAR_PLANE || min_z>=NEAR_PLANE) return;
    first[0]=(clip_vertex_t){camera[0],ua,va};
    first[1]=(clip_vertex_t){camera[1],ub,vb};
    first[2]=(clip_vertex_t){camera[2],ud,vd};
    first[3]=(clip_vertex_t){camera[3],uc,vc};
    count=clip_depth_plane(first,4,second,NEAR_PLANE,true);
    if(count<3) return;
    for(i=0;i<count;++i) {
        if(!project_camera(second[i].point,&projected[i])) return;
    }
    for(i=1;i<count-1;++i)
        if(screen_triangle_visible(&projected[0],&projected[i],&projected[i+1]))
            submit_triangle(header,&projected[0],&projected[i],&projected[i+1],
                            second[0].u,second[0].v,
                            second[i].u,second[i].v,
                            second[i+1].u,second[i+1].v,
                            color,color,color);
}

static void draw_world_quad(const pvr_poly_hdr_t *header,
                            vec3_t a, vec3_t b, vec3_t c, vec3_t d,
                            float ua, float va, float ub, float vb,
                            float uc, float vc, float ud, float vd,
                            uint32_t color) {
    screen_point_t sa, sb, sc, sd;
    /* Preserve the boundary order used by the clipped fan: a, b, d, c. */
    const vec3_t camera[4]={world_to_camera(a),world_to_camera(b),
                            world_to_camera(d),world_to_camera(c)};
    if(project_camera(camera[0],&sa) &&
       project_camera(camera[1],&sb) &&
       project_camera(camera[3],&sc) &&
       project_camera(camera[2],&sd)) {
        if(!screen_quad_visible(&sa,&sb,&sc,&sd)) return;
        submit_quad(header, &sa,&sb,&sc,&sd, ua,va,ub,vb,uc,vc,ud,vd,
                    color,color,color,color);
    }
    else
        draw_clipped_camera_quad(header,camera,ua,va,ub,vb,uc,vc,ud,vd,color);
}

static void draw_world_quad_colored(const pvr_poly_hdr_t *header,
                                    vec3_t a, vec3_t b, vec3_t c, vec3_t d,
                                    uint32_t ca, uint32_t cb,
                                    uint32_t cc, uint32_t cd) {
    screen_point_t sa,sb,sc,sd;
    if(project_world(a,&sa) && project_world(b,&sb) &&
       project_world(c,&sc) && project_world(d,&sd) &&
       screen_quad_visible(&sa,&sb,&sc,&sd))
        submit_quad(header,&sa,&sb,&sc,&sd,0,0,1,0,0,1,1,1,ca,cb,cc,cd);
}

static void draw_world_triangle(const pvr_poly_hdr_t *header,
                                vec3_t a, vec3_t b, vec3_t c,
                                uint32_t color) {
    screen_point_t sa, sb, sc;
    if(project_world(a, &sa) && project_world(b, &sb) && project_world(c, &sc))
        if(screen_triangle_visible(&sa,&sb,&sc))
            submit_triangle(header, &sa,&sb,&sc,0,0,1,0,0.5f,1,
                            color,color,color);
}

static void draw_world_disc(const pvr_poly_hdr_t *header, vec3_t center,
                            float radius, int segments,
                            uint32_t center_color, uint32_t edge_color) {
    screen_point_t projected_center;
    const vec3_t view=world_to_camera(center);
    float screen_radius;
    int i;
    if(!project_camera(view,&projected_center)) return;
    screen_radius=camera_focal*radius/view.z;
    if(projected_center.x+screen_radius<-24.0f ||
       projected_center.x-screen_radius>SCREEN_W+24.0f ||
       projected_center.y+screen_radius<-24.0f ||
       projected_center.y-screen_radius>SCREEN_H+24.0f)
        return;
    for(i=0;i<segments;++i) {
        const float a0=(float)i*PI*2.0f/(float)segments;
        const float a1=(float)(i+1)*PI*2.0f/(float)segments;
        screen_point_t p0,p1;
        if(!project_world((vec3_t){center.x+fcos(a0)*radius,center.y,
                                   center.z+fsin(a0)*radius},&p0) ||
           !project_world((vec3_t){center.x+fcos(a1)*radius,center.y,
                                   center.z+fsin(a1)*radius},&p1))
            continue;
        submit_triangle(header,&projected_center,&p0,&p1,
                        .5f,.5f,0,0,1,0,
                        center_color,edge_color,edge_color);
    }
}

static void draw_world_box(float cx, float cy, float cz,
                           float hx, float hy, float hz,
                           const pvr_poly_hdr_t *header, uint32_t color) {
    const vec3_t p[8] = {
        {cx-hx,cy+hy,cz+hz},{cx+hx,cy+hy,cz+hz},
        {cx-hx,cy-hy,cz+hz},{cx+hx,cy-hy,cz+hz},
        {cx-hx,cy+hy,cz-hz},{cx+hx,cy+hy,cz-hz},
        {cx-hx,cy-hy,cz-hz},{cx+hx,cy-hy,cz-hz}
    };
    /* These boxes are opaque solids. Reject their hidden faces before the
       four-vertex camera transform and near-plane clipping work. */
    if(camera_z>cz+hz)
        draw_world_quad(header,p[0],p[1],p[2],p[3],0,0,1,0,0,1,1,1,color);
    if(camera_z<cz-hz)
        draw_world_quad(header,p[5],p[4],p[7],p[6],0,0,1,0,0,1,1,1,color);
    if(camera_x<cx-hx)
        draw_world_quad(header,p[4],p[0],p[6],p[2],0,0,1,0,0,1,1,1,color);
    if(camera_x>cx+hx)
        draw_world_quad(header,p[1],p[5],p[3],p[7],0,0,1,0,0,1,1,1,color);
    if(camera_y>cy+hy)
        draw_world_quad(header,p[4],p[5],p[0],p[1],0,0,1,0,0,1,1,1,color);
    if(camera_y<cy-hy)
        draw_world_quad(header,p[2],p[3],p[6],p[7],0,0,1,0,0,1,1,1,color);
}

static void draw_vertical_billboard(const pvr_poly_hdr_t *header,
                                    float x, float base_y, float z,
                                    float half_width, float height,
                                    uint32_t color) {
    const float rx=fcos(camera_yaw)*half_width;
    const float rz=-fsin(camera_yaw)*half_width;
    draw_world_quad(header,
        (vec3_t){x-rx,base_y+height,z-rz},
        (vec3_t){x+rx,base_y+height,z+rz},
        (vec3_t){x-rx,base_y,z-rz},
        (vec3_t){x+rx,base_y,z+rz},
        0,0,1,0,0,1,1,1,color);
}

static void draw_sky(void) {
    const screen_point_t a={0,0,.000001f,true};
    const screen_point_t b={SCREEN_W,0,.000001f,true};
    const screen_point_t c={0,310,.000001f,true};
    const screen_point_t d={SCREEN_W,310,.000001f,true};
    const float u_shift=fsin(camera_yaw)*.025f;
    const uint32_t white=pack_color(1.0f,(color3_t){1,1,1});
    submit_quad(&backdrop_header,&a,&b,&c,&d,
                .04f+u_shift,0,.96f+u_shift,0,
                .04f+u_shift,1,.96f+u_shift,1,
                white,white,white,white);
}

static void draw_title_art(void) {
    const screen_point_t a={0,0,.00001f,true},b={SCREEN_W,0,.00001f,true};
    const screen_point_t c={0,SCREEN_H,.00001f,true},d={SCREEN_W,SCREEN_H,.00001f,true};
    const uint32_t white=pack_color(1.0f,(color3_t){1,1,1});
    submit_quad(&title_header,&a,&b,&c,&d,0,0,1,0,0,1,1,1,
                white,white,white,white);
}

static void draw_sun(void) {
    draw_disc(&additive_header, 520.0f, 232.0f, 34.0f, 0.00002f, 16,
              pack_color(0.30f,(color3_t){1.0f,.55f,.20f}),
              pack_color(0.0f,(color3_t){1.0f,.18f,.08f}));
}

static void draw_volume_walls(float cx, float cz,
                                 float width, float depth,
                                 float y0, float y1, int texture,
                                 uint32_t front, uint32_t side) {
    const float x0=cx-width*.5f,x1=cx+width*.5f;
    const float z0=cz-depth*.5f,z1=cz+depth*.5f;
    const bool shop=texture>=DLA_TEX_FACADE_STOREFRONT_DOWNTOWN && texture<=DLA_TEX_FACADE_STOREFRONT_NEON_ALT;
    const float module_width=shop?8.0f:16.0f;
    const float ux=fmaxf(.25f,width/module_width);
    const float uz=fmaxf(.25f,depth/module_width);
    const float v=texture>=DLA_TEX_FACADE_STOREFRONT_DOWNTOWN ? 1.0f : fmaxf(.10f,(y1-y0)/12.0f);
    const pvr_poly_hdr_t *header=&texture_headers[texture];
    if(camera_z<=cz)
        draw_world_quad(header,(vec3_t){x0,y1,z0},(vec3_t){x1,y1,z0},
                        (vec3_t){x0,y0,z0},(vec3_t){x1,y0,z0},
                        0,0,ux,0,0,v,ux,v,front);
    else
        draw_world_quad(header,(vec3_t){x1,y1,z1},(vec3_t){x0,y1,z1},
                        (vec3_t){x1,y0,z1},(vec3_t){x0,y0,z1},
                        0,0,ux,0,0,v,ux,v,side);
    if(camera_x<=cx)
        draw_world_quad(header,(vec3_t){x0,y1,z1},(vec3_t){x0,y1,z0},
                        (vec3_t){x0,y0,z1},(vec3_t){x0,y0,z0},
                        0,0,uz,0,0,v,uz,v,side);
    else
        draw_world_quad(header,(vec3_t){x1,y1,z0},(vec3_t){x1,y1,z1},
                        (vec3_t){x1,y0,z0},(vec3_t){x1,y0,z1},
                        0,0,uz,0,0,v,uz,v,front);
}

static void draw_textured_volume(float cx,float cz,float width,float depth,
                                 float y0,float y1,int texture,uint32_t front,uint32_t side) {
    const bool facade=texture>=DLA_TEX_FACADE_DOWNTOWN && texture<=DLA_TEX_FACADE_NEON_ALT;
    const float roof_y=facade?fmaxf(y0,y1-.48f):y1;
    float wall_y=y0;
    if(facade && y0<.2f) {
        const float shop_top=fminf(4.6f,roof_y);
        const int shop=DLA_TEX_FACADE_STOREFRONT_DOWNTOWN+texture-DLA_TEX_FACADE_DOWNTOWN;
        draw_volume_walls(cx,cz,width,depth,y0,shop_top,shop,front,side);
        wall_y=shop_top;
    }
    if(roof_y>wall_y)
        draw_volume_walls(cx,cz,width,depth,wall_y,roof_y,texture,front,side);
    if(facade) {
        /* A continuous roof band caps each module. No shopfront or roof imagery
           is repeated through the upper-floor texture. */
        const uint32_t trim=pack_color(1.0f,(color3_t){.22f,.25f,.30f});
        const uint32_t trim_side=pack_color(1.0f,(color3_t){.14f,.17f,.22f});
        const int cornice=DLA_TEX_FACADE_CORNICE_DOWNTOWN+(texture-DLA_TEX_FACADE_DOWNTOWN)%4;
        draw_volume_walls(cx,cz,width+.18f,depth+.18f,roof_y,y1,cornice,trim,trim_side);
    }
    draw_world_quad(&world_header,
        (vec3_t){cx-width*.5f,y1,cz+depth*.5f},(vec3_t){cx+width*.5f,y1,cz+depth*.5f},
        (vec3_t){cx-width*.5f,y1,cz-depth*.5f},(vec3_t){cx+width*.5f,y1,cz-depth*.5f},
        0,0,1,0,0,1,1,1,pack_color(1.0f,(color3_t){.06f,.075f,.105f}));
}

static void draw_upper_volume(const building_t *building,
                              float width,float depth,float height,float offset_x,float offset_z) {
    if(building->district==DISTRICT_DOWNTOWN && (building->seed&1u)) {
        const float middle=building->height+height*.63f;
        draw_textured_volume(building->cx+offset_x,building->cz+offset_z,width,depth,
            building->height,middle,building->texture,
            pack_color(1.0f,(color3_t){.78f,.82f,.91f}),
            pack_color(1.0f,(color3_t){.57f,.64f,.76f}));
        draw_textured_volume(building->cx+offset_x,building->cz+offset_z,
            width*.64f,depth*.64f,middle,building->height+height,building->texture,
            pack_color(1.0f,(color3_t){.80f,.83f,.90f}),
            pack_color(1.0f,(color3_t){.61f,.67f,.77f}));
        return;
    }
    draw_textured_volume(building->cx+offset_x,building->cz+offset_z,width,depth,
        building->height,building->height+height,building->texture,
        pack_color(1.0f,(color3_t){.79f,.83f,.91f}),
        pack_color(1.0f,(color3_t){.59f,.66f,.78f}));
}

static void draw_warehouse_roof(const building_t *building) {
    const float x0=building->cx-building->width*.5f;
    const float x1=building->cx+building->width*.5f;
    const float z0=building->cz-building->depth*.5f;
    const float z1=building->cz+building->depth*.5f;
    const float roof=building->height+.05f;
    const float ridge=building->height+3.8f+(float)((building->seed>>8)&3u);
    const uint32_t lit=pack_color(1.0f,(color3_t){.30f,.15f,.10f});
    const uint32_t dark=pack_color(1.0f,(color3_t){.12f,.10f,.11f});
    if((building->seed&3u)==0u) {
        int bay;
        const float pitch=(z1-z0)/3.0f;
        const uint32_t glazing=pack_color(1.0f,(color3_t){.12f,.23f,.29f});
        /* North-light factory roofs break the warehouse skyline into an
           identifiable industrial silhouette with three glazed sawteeth. */
        for(bay=0;bay<3;++bay) {
            const float back=z0+(float)bay*pitch,front=back+pitch;
            draw_world_quad(&world_header,
                (vec3_t){x0,ridge,front},(vec3_t){x1,ridge,front},
                (vec3_t){x0,roof,back},(vec3_t){x1,roof,back},
                0,0,1,0,0,1,1,1,dark);
            if(camera_z>front)
                draw_world_quad(&world_header,
                    (vec3_t){x1,ridge,front},(vec3_t){x0,ridge,front},
                    (vec3_t){x1,roof,front},(vec3_t){x0,roof,front},
                    0,0,1,0,0,1,1,1,glazing);
            if(camera_x<x0)
                draw_world_triangle(&world_header,(vec3_t){x0,roof,back},
                    (vec3_t){x0,roof,front},(vec3_t){x0,ridge,front},lit);
            else if(camera_x>x1)
                draw_world_triangle(&world_header,(vec3_t){x1,roof,front},
                    (vec3_t){x1,roof,back},(vec3_t){x1,ridge,front},lit);
        }
        return;
    }
    draw_world_triangle(&world_header,(vec3_t){x0,roof,z0},
        (vec3_t){x0,roof,z1},(vec3_t){x0,ridge,building->cz},lit);
    draw_world_triangle(&world_header,(vec3_t){x1,roof,z1},
        (vec3_t){x1,roof,z0},(vec3_t){x1,ridge,building->cz},dark);
    draw_world_quad(&world_header,
        (vec3_t){x0,ridge,building->cz},(vec3_t){x1,ridge,building->cz},
        (vec3_t){x0,roof,z0},(vec3_t){x1,roof,z0},0,0,1,0,0,1,1,1,lit);
    draw_world_quad(&world_header,
        (vec3_t){x1,ridge,building->cz},(vec3_t){x0,ridge,building->cz},
        (vec3_t){x1,roof,z1},(vec3_t){x0,roof,z1},0,0,1,0,0,1,1,1,dark);
}

static void draw_storefront_depth(const building_t *building) {
    const float x0=building->cx-building->width*.5f;
    const float x1=building->cx+building->width*.5f;
    const float z0=building->cz-building->depth*.5f;
    const float z1=building->cz+building->depth*.5f;
    const float dx=camera_x-building->cx,dz=camera_z-building->cz;
    const color3_t accent=district_color(building->district);
    const uint32_t frame=pack_color(1.0f,(color3_t){.055f,.075f,.12f});
    const uint32_t metal=pack_color(1.0f,color_scale(accent,.72f));
    const uint32_t white=pack_color(1.0f,(color3_t){.92f,.95f,1.0f});
    int bay;

    if(fabsf(dz)>=fabsf(dx)) {
        const float face=dz<0.0f ? z0-.66f : z1+.66f;
        const float outward=dz<0.0f ? -1.0f : 1.0f;
        draw_world_box(building->cx,3.70f,face,building->width*.32f,.13f,.64f,
                       &world_header,metal);
        draw_world_quad(&texture_headers[DLA_TEX_STOREFRONT_MICRO],
            (vec3_t){building->cx-6.4f,3.58f,face-outward*.70f},
            (vec3_t){building->cx+6.4f,3.58f,face-outward*.70f},
            (vec3_t){building->cx-6.4f,.20f,face-outward*.70f},
            (vec3_t){building->cx+6.4f,.20f,face-outward*.70f},
            0,0,1,0,0,1,1,1,white);
        for(bay=-1;bay<=1;++bay) {
            const float bx=building->cx+(float)bay*building->width*.205f;
            draw_world_box(bx,2.02f,face,.12f,2.0f,.72f,&world_header,frame);
            draw_world_box(bx,3.72f,face,building->width*.085f,.075f,.76f,
                           &world_header,(bay&1)?metal:frame);
        }
        draw_world_box(building->cx-building->width*.275f,5.20f,
                       face-outward*.34f,.055f,.76f,.72f,&world_header,metal);
        draw_world_box(building->cx,0.18f,face-outward*.72f,
                       1.15f,.10f,.42f,&world_header,frame);
    }
    else {
        const float face=dx<0.0f ? x0-.66f : x1+.66f;
        const float outward=dx<0.0f ? -1.0f : 1.0f;
        draw_world_box(face,3.70f,building->cz,.64f,.13f,building->depth*.32f,
                       &world_header,metal);
        draw_world_quad(&texture_headers[DLA_TEX_STOREFRONT_MICRO],
            (vec3_t){face-outward*.70f,3.58f,building->cz+6.4f},
            (vec3_t){face-outward*.70f,3.58f,building->cz-6.4f},
            (vec3_t){face-outward*.70f,.20f,building->cz+6.4f},
            (vec3_t){face-outward*.70f,.20f,building->cz-6.4f},
            0,0,1,0,0,1,1,1,white);
        for(bay=-1;bay<=1;++bay) {
            const float bz=building->cz+(float)bay*building->depth*.205f;
            draw_world_box(face,2.02f,bz,.72f,2.0f,.12f,&world_header,frame);
            draw_world_box(face,3.72f,bz,.76f,.075f,
                           building->depth*.085f,&world_header,
                           (bay&1)?metal:frame);
        }
        draw_world_box(face-outward*.34f,5.20f,
                       building->cz-building->depth*.275f,.72f,.76f,.055f,
                       &world_header,metal);
        draw_world_box(face-outward*.72f,0.18f,building->cz,
                       .42f,.10f,1.15f,&world_header,frame);
    }
}

static void draw_rooftop_detail(const building_t *building) {
    const float x0=building->cx-building->width*.5f;
    const float x1=building->cx+building->width*.5f;
    const float z0=building->cz-building->depth*.5f;
    const float z1=building->cz+building->depth*.5f;
    const float base=building->height+.05f;
    const float top=building->height+.64f;
    const float y=building->height+.34f;
    const uint32_t parapet=pack_color(1.0f,(color3_t){.11f,.13f,.18f});
    const uint32_t plant=pack_color(1.0f,(color3_t){.20f,.23f,.27f});
    const uint32_t duct=pack_color(1.0f,(color3_t){.35f,.34f,.32f});
    draw_world_quad(&world_header,(vec3_t){x0,top,z0},(vec3_t){x1,top,z0},
                    (vec3_t){x0,base,z0},(vec3_t){x1,base,z0},
                    0,0,1,0,0,1,1,1,parapet);
    draw_world_quad(&world_header,(vec3_t){x1,top,z1},(vec3_t){x0,top,z1},
                    (vec3_t){x1,base,z1},(vec3_t){x0,base,z1},
                    0,0,1,0,0,1,1,1,parapet);
    draw_world_quad(&world_header,(vec3_t){x0,top,z1},(vec3_t){x0,top,z0},
                    (vec3_t){x0,base,z1},(vec3_t){x0,base,z0},
                    0,0,1,0,0,1,1,1,parapet);
    draw_world_quad(&world_header,(vec3_t){x1,top,z0},(vec3_t){x1,top,z1},
                    (vec3_t){x1,base,z0},(vec3_t){x1,base,z1},
                    0,0,1,0,0,1,1,1,parapet);
    if((building->seed&1u)!=0u) return;
    draw_world_box(building->cx-building->width*.16f,y+.55f,
                   building->cz-building->depth*.12f,2.10f,.52f,1.55f,
                   &world_header,plant);
    draw_world_box(building->cx+building->width*.10f,y+.38f,
                   building->cz+building->depth*.14f,1.40f,.34f,1.05f,
                   &world_header,duct);
    draw_world_box(building->cx+building->width*.10f,y+.92f,
                   building->cz+building->depth*.14f,.62f,.55f,.62f,
                   &world_header,parapet);
}

static void draw_facade_relief(const building_t *building) {
    const float x0=building->cx-building->width*.5f;
    const float x1=building->cx+building->width*.5f;
    const float z0=building->cz-building->depth*.5f;
    const float z1=building->cz+building->depth*.5f;
    const bool north=camera_z<building->cz;
    const bool west=camera_x<building->cx;
    const float face_z=north?z0-.24f:z1+.24f;
    const float face_x=west?x0-.24f:x1+.24f;
    const color3_t accent=district_color(building->district);
    const uint32_t shadow=pack_color(1.0f,(color3_t){.045f,.055f,.085f});
    const uint32_t canopy=pack_color(1.0f,color_scale(accent,.70f));
    const int alternate=district_facade_texture(building->district,
                                                 building->seed^0xa511e9b3u);
    int bay, level;

    /* Narrow vertical mullions add real parallax without the long near-plane
       crossings that a block-wide horizontal molding can create. */
    for(bay=-2;bay<=2;++bay) {
        const float bx=building->cx+(float)bay*building->width*.155f;
        const float bz=building->cz+(float)bay*building->depth*.155f;
        draw_world_quad(&world_header,
            (vec3_t){bx-.075f,7.75f,face_z},(vec3_t){bx+.075f,7.75f,face_z},
            (vec3_t){bx-.075f,.25f,face_z},(vec3_t){bx+.075f,.25f,face_z},
            0,0,1,0,0,1,1,1,shadow);
        draw_world_quad(&world_header,
            (vec3_t){face_x,7.75f,bz+.075f},(vec3_t){face_x,7.75f,bz-.075f},
            (vec3_t){face_x,.25f,bz+.075f},(vec3_t){face_x,.25f,bz-.075f},
            0,0,1,0,0,1,1,1,shadow);
    }

    /* Only the camera-facing wall receives short, flat cornice strips.  Side
       boxes can straddle the near plane and stretch into screen-wide streaks. */
    for(level=0;level<3;++level) {
        const float y=7.25f+(float)level*4.65f;
        if(y>building->height-.8f) break;
        for(bay=-1;bay<=1;++bay) {
            const uint32_t band=(level&1)?canopy:shadow;
            if(fabsf(camera_z-building->cz)>=fabsf(camera_x-building->cx)) {
                const float bx=building->cx+(float)bay*building->width*.225f;
                const float half=building->width*.105f;
                const vec3_t view_a=world_to_camera((vec3_t){bx-half,y,face_z});
                const vec3_t view_b=world_to_camera((vec3_t){bx+half,y,face_z});
                if(view_a.z>12.0f && view_b.z>12.0f)
                    draw_world_quad(&world_header,
                        (vec3_t){bx-half,y+.055f,face_z},
                        (vec3_t){bx+half,y+.055f,face_z},
                        (vec3_t){bx-half,y-.055f,face_z},
                        (vec3_t){bx+half,y-.055f,face_z},
                        0,0,1,0,0,1,1,1,band);
            }
            else {
                const float bz=building->cz+(float)bay*building->depth*.225f;
                const float half=building->depth*.105f;
                const vec3_t view_a=world_to_camera((vec3_t){face_x,y,bz+half});
                const vec3_t view_b=world_to_camera((vec3_t){face_x,y,bz-half});
                if(view_a.z>12.0f && view_b.z>12.0f)
                    draw_world_quad(&world_header,
                        (vec3_t){face_x,y+.055f,bz+half},
                        (vec3_t){face_x,y+.055f,bz-half},
                        (vec3_t){face_x,y-.055f,bz+half},
                        (vec3_t){face_x,y-.055f,bz-half},
                        0,0,1,0,0,1,1,1,band);
            }
        }
    }

    /* One lower pavilion per close block breaks the sixty-metre monolith into
       a main tower, street wing, recessed entrance and illuminated canopy. */
    if(building->seed&1u) {
        const float wing_z=north?z0-1.45f:z1+1.45f;
        draw_textured_volume(building->cx+building->width*.23f,wing_z,
                             building->width*.31f,3.1f,.12f,
                             fminf(10.5f,building->height-.4f),alternate,
                             pack_color(1.0f,(color3_t){.92f,.95f,1.0f}),
                             pack_color(1.0f,(color3_t){.68f,.73f,.84f}));
        draw_world_box(building->cx-building->width*.16f,3.4f,
                       north?z0-.92f:z1+.92f,building->width*.15f,.14f,.88f,
                       &world_header,canopy);
    }
    else {
        const float wing_x=west?x0-1.45f:x1+1.45f;
        draw_textured_volume(wing_x,building->cz-building->depth*.23f,
                             3.1f,building->depth*.31f,.12f,
                             fminf(10.5f,building->height-.4f),alternate,
                             pack_color(1.0f,(color3_t){.92f,.95f,1.0f}),
                             pack_color(1.0f,(color3_t){.68f,.73f,.84f}));
        draw_world_box(west?x0-.92f:x1+.92f,3.4f,
                       building->cz+building->depth*.16f,.88f,.14f,
                       building->depth*.15f,&world_header,canopy);
    }
}

static void draw_storefront_canopies(const building_t *building) {
    const float x0=building->cx-building->width*.5f;
    const float x1=building->cx+building->width*.5f;
    const float z0=building->cz-building->depth*.5f;
    const float z1=building->cz+building->depth*.5f;
    const bool face_z=fabsf(camera_z-building->cz)>=fabsf(camera_x-building->cx);
    const bool near_negative=face_z ? camera_z<building->cz : camera_x<building->cx;
    const color3_t accent=district_color(building->district);
    const uint32_t bright=pack_color(1.0f,color_scale(accent,.92f));
    const uint32_t dark=pack_color(1.0f,color_scale(accent,.28f));
    const uint32_t glass=pack_color(1.0f,(color3_t){.035f,.075f,.12f});
    const float span=face_z ? building->width : building->depth;
    const float spacing=fminf(span*.14f,5.4f);
    int bay;
    for(bay=-1;bay<=1;++bay) {
        const uint32_t color=((bay+(int)(building->seed&1u))&1)?bright:dark;
        const float offset=(float)bay*spacing;
        if(face_z) {
            const float face=near_negative?z0-.07f:z1+.07f;
            const float outer=near_negative?z0-.74f:z1+.74f;
            draw_world_quad(&world_header,
                (vec3_t){building->cx+offset-1.65f,3.13f,face},
                (vec3_t){building->cx+offset+1.65f,3.13f,face},
                (vec3_t){building->cx+offset-1.65f,3.02f,outer},
                (vec3_t){building->cx+offset+1.65f,3.02f,outer},
                0,0,1,0,0,1,1,1,color);
            draw_world_quad(&world_header,
                (vec3_t){building->cx+offset-1.65f,3.02f,outer},
                (vec3_t){building->cx+offset+1.65f,3.02f,outer},
                (vec3_t){building->cx+offset-1.65f,2.75f,outer},
                (vec3_t){building->cx+offset+1.65f,2.75f,outer},
                0,0,1,0,0,1,1,1,color);
            draw_world_quad(&world_header,
                (vec3_t){building->cx+offset-1.38f,2.62f,face},
                (vec3_t){building->cx+offset+1.38f,2.62f,face},
                (vec3_t){building->cx+offset-1.38f,.14f,face},
                (vec3_t){building->cx+offset+1.38f,.14f,face},
                0,0,1,0,0,1,1,1,glass);
        }
        else {
            const float face=near_negative?x0-.07f:x1+.07f;
            const float outer=near_negative?x0-.74f:x1+.74f;
            draw_world_quad(&world_header,
                (vec3_t){face,3.13f,building->cz+offset+1.65f},
                (vec3_t){face,3.13f,building->cz+offset-1.65f},
                (vec3_t){outer,3.02f,building->cz+offset+1.65f},
                (vec3_t){outer,3.02f,building->cz+offset-1.65f},
                0,0,1,0,0,1,1,1,color);
            draw_world_quad(&world_header,
                (vec3_t){outer,3.02f,building->cz+offset+1.65f},
                (vec3_t){outer,3.02f,building->cz+offset-1.65f},
                (vec3_t){outer,2.75f,building->cz+offset+1.65f},
                (vec3_t){outer,2.75f,building->cz+offset-1.65f},
                0,0,1,0,0,1,1,1,color);
            draw_world_quad(&world_header,
                (vec3_t){face,2.62f,building->cz+offset+1.38f},
                (vec3_t){face,2.62f,building->cz+offset-1.38f},
                (vec3_t){face,.14f,building->cz+offset+1.38f},
                (vec3_t){face,.14f,building->cz+offset-1.38f},
                0,0,1,0,0,1,1,1,glass);
        }
    }
}

static void draw_building(const building_t *building) {
    const float x0 = building->cx - building->width * 0.5f;
    const float x1 = building->cx + building->width * 0.5f;
    const float z0 = building->cz - building->depth * 0.5f;
    const float z1 = building->cz + building->depth * 0.5f;
    const float y0 = 0.10f;
    const float y1 = building->height;
    const int facade=building->texture;
    const float shade = 0.80f + (float)(building->seed & 15u) * 0.011f;
    const uint32_t front = pack_color(1.0f,color_scale((color3_t){1,1,1},shade));
    const uint32_t side = pack_color(1.0f,color_scale((color3_t){0.83f,0.87f,0.98f},shade));
    const float dx=building->cx-car.x,dz=building->cz-car.z;
    const bool nearby=dx*dx+dz*dz<110.0f*110.0f;
    const bool close=dx*dx+dz*dz<82.0f*82.0f;
    const bool facade_nearby=dx*dx+dz*dz<138.0f*138.0f;
    /* Include the 14 m cap and 3.09 m pavilion/cornice projection. A
       centre/depth estimate can reject a visible wall as the camera turns. */
    const float bounds_top=y1+14.0f;
    if(!dla_frustum_aabb_visible(&scene_frustum,
        building->cx,bounds_top*.5f,building->cz,
        building->width*.5f+3.2f,bounds_top*.5f,building->depth*.5f+3.2f)) return;
    QA_COUNT(buildings);
    if(building->district==DISTRICT_ARTS) {
        draw_textured_volume(building->cx,building->cz,
                             building->width,building->depth,y0,y1,
                             facade,front,side);
    }
    else {
        const float podium_top=fminf(y1,
            building->district==DISTRICT_COAST ? 6.8f : 9.2f);
        const float width_scale=.60f+(float)((building->seed>>8)&3u)*.035f;
        const float depth_scale=.60f+(float)((building->seed>>10)&3u)*.035f;
        const float offset_x=((int)((building->seed>>14)&3u)-1.5f)*1.35f;
        const float offset_z=((int)((building->seed>>16)&3u)-1.5f)*1.35f;
        draw_textured_volume(building->cx,building->cz,
                             building->width,building->depth,y0,podium_top,
                             facade,front,side);
        if(y1>podium_top+.5f)
            draw_textured_volume(building->cx+offset_x,building->cz+offset_z,
                                 building->width*width_scale,
                                 building->depth*depth_scale,podium_top,y1,
                                 facade,front,side);
    }

    if((building->district==DISTRICT_DOWNTOWN ||
        building->district==DISTRICT_NEON) && y1>34.0f &&
       (building->seed&3u)!=0u) {
        const float cap_height=7.0f+(float)((building->seed>>5)&7u);
        const float ox=((building->seed>>12)&1u) ? building->width*.08f :
                                                     -building->width*.08f;
        const float oz=((building->seed>>13)&1u) ? building->depth*.06f :
                                                     -building->depth*.06f;
        draw_upper_volume(building,building->width*.58f,building->depth*.60f,
                          cap_height,ox,oz);
    }
    else if(building->district==DISTRICT_ARTS)
        draw_warehouse_roof(building);

    /* Bright street-level businesses give each block a readable destination. */
    if(building->height>8.0f && facade_nearby) {
        const int detail=((building->seed>>10)&3u)==0u ?
                         DLA_TEX_STOREFRONT_MICRO :
                         district_detail_texture(building->district);
        const float facade_y=fminf(8.0f,building->height-.35f);
        const float facade_x_half=fminf(building->width*.22f,7.5f);
        const float facade_z_half=fminf(building->depth*.22f,7.5f);
        const uint32_t emissive=pack_color(1.0f,(color3_t){1.0f,.95f,.88f});
        draw_world_quad(&texture_headers[detail],
            (vec3_t){building->cx-facade_x_half,facade_y,z0-.055f},
            (vec3_t){building->cx+facade_x_half,facade_y,z0-.055f},
            (vec3_t){building->cx-facade_x_half,.16f,z0-.055f},
            (vec3_t){building->cx+facade_x_half,.16f,z0-.055f},
            0,0,1,0,0,1,1,1,emissive);
        draw_world_quad(&texture_headers[detail],
            (vec3_t){building->cx+facade_x_half,facade_y,z1+.055f},
            (vec3_t){building->cx-facade_x_half,facade_y,z1+.055f},
            (vec3_t){building->cx+facade_x_half,.16f,z1+.055f},
            (vec3_t){building->cx-facade_x_half,.16f,z1+.055f},
            0,0,1,0,0,1,1,1,emissive);
        draw_world_quad(&texture_headers[detail],
            (vec3_t){x0-.055f,facade_y,building->cz+facade_z_half},
            (vec3_t){x0-.055f,facade_y,building->cz-facade_z_half},
            (vec3_t){x0-.055f,.16f,building->cz+facade_z_half},
            (vec3_t){x0-.055f,.16f,building->cz-facade_z_half},
            0,0,1,0,0,1,1,1,emissive);
        draw_world_quad(&texture_headers[detail],
            (vec3_t){x1+.055f,facade_y,building->cz-facade_z_half},
            (vec3_t){x1+.055f,facade_y,building->cz+facade_z_half},
            (vec3_t){x1+.055f,.16f,building->cz-facade_z_half},
            (vec3_t){x1+.055f,.16f,building->cz+facade_z_half},
            0,0,1,0,0,1,1,1,emissive);
    }
    /* Close blocks get district-specific silhouettes instead of relying only
       on a façade texture: fins, balconies, fire escapes and marquees. */
    if(nearby) {
        const float front_z=z0-.12f;
        if(building->district==DISTRICT_DOWNTOWN) {
            const uint32_t brass=pack_color(1.0f,(color3_t){.66f,.46f,.16f});
            const float pier_h=fminf(y1*.48f,18.0f);
            int bay;
            draw_world_box(x0+1.15f,pier_h*.5f,front_z,.12f,pier_h*.5f,.10f,
                           &world_header,brass);
            draw_world_box(x1-1.15f,pier_h*.5f,front_z,.12f,pier_h*.5f,.10f,
                           &world_header,brass);
            draw_world_box(building->cx,4.65f,z0-.72f,building->width*.24f,
                           .16f,.70f,&world_header,
                           pack_color(1.0f,(color3_t){.10f,.14f,.22f}));
            draw_world_box(x0-.72f,4.65f,building->cz,.70f,.16f,
                           building->depth*.24f,&world_header,
                           pack_color(1.0f,(color3_t){.10f,.14f,.22f}));
            draw_world_box(x0-.12f,pier_h*.5f,z0+1.15f,.10f,pier_h*.5f,.12f,
                           &world_header,brass);
            draw_world_box(x0-.12f,pier_h*.5f,z1-1.15f,.10f,pier_h*.5f,.12f,
                           &world_header,brass);
            if(close) {
                for(bay=-1;bay<=1;++bay)
                    draw_world_box(building->cx+(float)bay*building->width*.22f,
                                   pier_h*.5f,front_z,.075f,pier_h*.5f,.11f,
                                   &world_header,brass);
            }
        }
        else if(building->district==DISTRICT_COAST) {
            const uint32_t white=pack_color(1.0f,(color3_t){.80f,.88f,.84f});
            int balcony;
            for(balcony=0;balcony<2;++balcony) {
                const float y=4.6f+(float)balcony*3.8f;
                if(y<y1-.8f) {
                    draw_world_box(building->cx,y,z0-.54f,building->width*.36f,
                                   .10f,.52f,&world_header,white);
                    draw_world_box(building->cx,y+.52f,z0-1.02f,
                                   building->width*.36f,.045f,.045f,&world_header,white);
                    draw_world_box(x0-.54f,y,building->cz,.52f,.10f,
                                   building->depth*.36f,&world_header,white);
                    draw_world_box(x0-1.02f,y+.52f,building->cz,.045f,.045f,
                                   building->depth*.36f,&world_header,white);
                }
            }
        }
        else if(building->district==DISTRICT_ARTS) {
            const uint32_t steel=pack_color(1.0f,(color3_t){.16f,.18f,.19f});
            int bay;
            if(y1>8.5f) {
                draw_world_box(building->cx,5.0f,z0-.52f,4.2f,.10f,.50f,
                               &world_header,steel);
                draw_world_box(building->cx+3.45f,3.0f,z0-.98f,.075f,3.0f,.075f,
                               &world_header,steel);
                draw_world_box(x0-.52f,5.0f,building->cz,.50f,.10f,4.2f,
                               &world_header,steel);
                draw_world_box(x0-.98f,3.0f,building->cz+3.45f,.075f,3.0f,.075f,
                               &world_header,steel);
            }
            draw_world_box(x0+1.0f,fminf(y1*.42f,7.0f),front_z,.16f,
                           fminf(y1*.42f,7.0f),.10f,&world_header,
                           pack_color(1.0f,(color3_t){.46f,.22f,.10f}));
            draw_world_box(x0-.12f,fminf(y1*.42f,7.0f),z0+1.0f,.10f,
                           fminf(y1*.42f,7.0f),.16f,&world_header,
                           pack_color(1.0f,(color3_t){.46f,.22f,.10f}));
            if(close) {
                for(bay=-1;bay<=1;bay+=2)
                    draw_world_box(building->cx+(float)bay*building->width*.27f,
                                   fminf(y1*.38f,6.5f),front_z,.14f,
                                   fminf(y1*.38f,6.5f),.11f,&world_header,steel);
            }
            if((building->seed%3u)==0u) {
                const uint32_t tank=pack_color(1.0f,(color3_t){.26f,.18f,.12f});
                const uint32_t legs=pack_color(1.0f,(color3_t){.12f,.13f,.14f});
                draw_world_box(building->cx,y1+5.0f,building->cz,2.2f,1.35f,2.2f,
                               &world_header,tank);
                draw_world_box(building->cx-1.5f,y1+2.15f,building->cz-1.5f,
                               .09f,2.15f,.09f,&world_header,legs);
                draw_world_box(building->cx+1.5f,y1+2.15f,building->cz-1.5f,
                               .09f,2.15f,.09f,&world_header,legs);
                draw_world_box(building->cx-1.5f,y1+2.15f,building->cz+1.5f,
                               .09f,2.15f,.09f,&world_header,legs);
                draw_world_box(building->cx+1.5f,y1+2.15f,building->cz+1.5f,
                               .09f,2.15f,.09f,&world_header,legs);
            }
        }
        else {
            const uint32_t cyan=pack_color(1.0f,(color3_t){.08f,.72f,.96f});
            const uint32_t pink=pack_color(1.0f,(color3_t){1.0f,.08f,.58f});
            int bay;
            draw_world_box(building->cx,4.25f,z0-.92f,building->width*.31f,
                           .18f,.90f,&world_header,pink);
            draw_world_box(x0-.92f,4.25f,building->cz,.90f,.18f,
                           building->depth*.31f,&world_header,cyan);
            draw_world_box(x0+1.0f,fminf(y1*.38f,9.0f),front_z,.13f,
                           fminf(y1*.38f,9.0f),.11f,&world_header,cyan);
            draw_world_box(x1-1.0f,fminf(y1*.38f,9.0f),front_z,.13f,
                           fminf(y1*.38f,9.0f),.11f,&world_header,pink);
            draw_world_box(x0-.12f,fminf(y1*.38f,9.0f),z0+1.0f,.11f,
                           fminf(y1*.38f,9.0f),.13f,&world_header,cyan);
            draw_world_box(x0-.12f,fminf(y1*.38f,9.0f),z1-1.0f,.11f,
                           fminf(y1*.38f,9.0f),.13f,&world_header,pink);
            if(close) {
                for(bay=-1;bay<=1;++bay)
                    draw_world_box(building->cx+(float)bay*building->width*.225f,
                                   fminf(y1*.26f,7.0f),front_z,.055f,
                                   fminf(y1*.26f,7.0f),.075f,&world_header,
                                   (bay&1)?pink:cyan);
            }
        }
        draw_rooftop_detail(building);
        if(close && building->height>7.5f) {
            draw_storefront_depth(building);
            draw_facade_relief(building);
            draw_storefront_canopies(building);
        }
    }
    if((building->seed%3u)==0u) {
        const uint32_t rooftop=pack_color(1.0f,(color3_t){.18f,.21f,.27f});
        draw_world_box(building->cx-building->width*.18f,y1+.48f,building->cz,
                       1.9f,.46f,1.45f,&world_header,rooftop);
        draw_world_box(building->cx+building->width*.13f,y1+.34f,building->cz+1.2f,
                       1.15f,.32f,1.05f,&world_header,rooftop);
    }
    if((building->seed%11u)==0u)
        draw_world_box(building->cx,y1+2.6f,building->cz,
                       .055f,2.6f,.055f,&world_header,
                       pack_color(1.0f,(color3_t){.30f,.34f,.42f}));
}

static void draw_palm(float x,float z,float scale) {
    const float height=8.5f*scale;
    const vec3_t view=world_to_camera((vec3_t){x,height*.5f,z});
    int segment,side;
    if(!world_sphere_visible((vec3_t){x,height*.5f,z},height*.75f,32.0f)) return;
    if(palm_draw_count<(int)ARRAY_COUNT(palm_draws))
        palm_draws[palm_draw_count++]=(palm_draw_t){x,z,scale,view.z};
    /* Tapered, gently leaning trunk with directional face lighting. */
    for(segment=0;segment<3;++segment) {
        const float t0=(float)segment/3.0f,t1=(float)(segment+1)/3.0f;
        const float r0=(.28f-.12f*t0)*scale,r1=(.28f-.12f*t1)*scale;
        for(side=0;side<4;++side) {
            const float a=(float)side*PI*.5f,b=(float)(side+1)*PI*.5f;
            const uint32_t color=pack_color(1.0f,(color3_t){.26f+.025f*side,.20f+.018f*side,.14f+.012f*side});
            draw_world_quad(&world_header,
                (vec3_t){x+.35f*t1*t1*scale+fcos(a)*r1,t1*height,z+fsin(a)*r1},
                (vec3_t){x+.35f*t1*t1*scale+fcos(b)*r1,t1*height,z+fsin(b)*r1},
                (vec3_t){x+.35f*t0*t0*scale+fcos(a)*r0,t0*height+.04f,z+fsin(a)*r0},
                (vec3_t){x+.35f*t0*t0*scale+fcos(b)*r0,t0*height+.04f,z+fsin(b)*r0},
                0,0,1,0,0,1,1,1,color);
        }
    }
}

static void draw_palm_foliage(void) {
    int i,frond,segment;
    for(i=0;i<palm_draw_count;++i) {
        const palm_draw_t *p=&palm_draws[i];
        const int fronds=p->depth>210.0f?5:8;
        const int segments=p->depth>145.0f?1:3;
        const float scale=p->scale, y=8.5f*scale;
        const float twist=fsin(p->x*.17f+p->z*.23f)*.7f;
        const float sway=fsin(game.time*.8f+p->x*.03f)*.12f;
        for(frond=0;frond<fronds;++frond) {
            const float angle=(float)frond*PI*2.0f/(float)fronds+twist;
            const float dx=fcos(angle),dz=fsin(angle);
            const float width=.95f*scale;
            const uint32_t tint=pack_color(1.0f,(color3_t){.64f+.18f*fmaxf(0,dx),.73f,.72f});
            for(segment=0;segment<segments;++segment) {
                const float t0=(float)segment/(float)segments,t1=(float)(segment+1)/(float)segments;
                const float l0=4.4f*scale*t0,l1=4.4f*scale*t1;
                const float h0=y+scale*(2.4f*t0-3.4f*t0*t0)+sway*t0;
                const float h1=y+scale*(2.4f*t1-3.4f*t1*t1)+sway*t1;
                const float x=p->x+.35f*scale;
                draw_world_quad(&texture_headers[DLA_TEX_EFFECT_PALM],
                    (vec3_t){x+dx*l1-dz*width,h1,p->z+dz*l1+dx*width},
                    (vec3_t){x+dx*l1+dz*width,h1,p->z+dz*l1-dx*width},
                    (vec3_t){x+dx*l0-dz*width,h0,p->z+dz*l0+dx*width},
                    (vec3_t){x+dx*l0+dz*width,h0,p->z+dz*l0-dx*width},
                    0,1.0f-t1,1,1.0f-t1,0,1.0f-t0,1,1.0f-t0,tint);
            }
        }
    }
}

/* A four-sided tapered sweep makes thin poles and trunks solid from every
   angle. Hidden faces are rejected before camera transforms or clipping. */
static void draw_street_swept_segment(vec3_t start, vec3_t end,
                                      float lower_radius, float upper_radius,
                                      color3_t tone) {
    static const float sign_u[4]={1.0f,-1.0f,-1.0f,1.0f};
    static const float sign_z[4]={1.0f,1.0f,-1.0f,-1.0f};
    const float dx=end.x-start.x,dy=end.y-start.y;
    const float inv=1.0f/sqrtf(fmaxf(dx*dx+dy*dy,.0001f));
    const float ux=dy*inv,uy=-dx*inv;
    vec3_t lower[4],upper[4];
    int side;
    for(side=0;side<4;++side) {
        lower[side]=(vec3_t){start.x+ux*sign_u[side]*lower_radius,
                             start.y+uy*sign_u[side]*lower_radius,
                             start.z+sign_z[side]*lower_radius};
        upper[side]=(vec3_t){end.x+ux*sign_u[side]*upper_radius,
                             end.y+uy*sign_u[side]*upper_radius,
                             end.z+sign_z[side]*upper_radius};
    }
    for(side=0;side<4;++side) {
        const int next=(side+1)&3;
        const float nu=(sign_u[side]+sign_u[next])*.5f;
        const float nz=(sign_z[side]+sign_z[next])*.5f;
        const float mx=(lower[side].x+lower[next].x+upper[side].x+upper[next].x)*.25f;
        const float my=(lower[side].y+lower[next].y+upper[side].y+upper[next].y)*.25f;
        const float mz=(lower[side].z+lower[next].z+upper[side].z+upper[next].z)*.25f;
        const float facing=(camera_x-mx)*nu*ux+(camera_y-my)*nu*uy+(camera_z-mz)*nz;
        const float shade=.79f+fmaxf(0.0f,nu*(-ux*.32f+uy*.86f)-nz*.24f)*.28f;
        if(facing<=0.0f) continue;
        draw_world_quad(&world_header,lower[side],lower[next],upper[side],upper[next],
                        0,0,1,0,0,1,1,1,pack_color(1.0f,color_scale(tone,shade)));
    }
}

static void draw_streetlamp_opaque(float x, float z, district_t district) {
    const vec3_t view=world_to_camera((vec3_t){x,3.5f,z});
    const float frustum=(SCREEN_CX+24.0f)*fmaxf(view.z,NEAR_PLANE)/camera_focal+1.5f;
    const color3_t accent=district_color(district);
    const color3_t metal={.14f+accent.r*.035f,.17f+accent.g*.025f,.21f+accent.b*.025f};
    const uint32_t fixture=pack_color(1.0f,(color3_t){.23f,.245f,.26f});
    const uint32_t bulb=pack_color(1.0f,(color3_t){1.0f,.84f,.56f});
    const bool close=view.z<62.0f;
    if(view.z<NEAR_PLANE-1.0f || view.z>FAR_PLANE+1.0f || fabsf(view.x)>frustum) return;
    /* The changing tangent describes a swan-neck arm, rather than stacked
       boxes. Its end and the underside lens retain the existing light anchor. */
    draw_street_swept_segment((vec3_t){x,.12f,z},(vec3_t){x,6.12f,z},.13f,.072f,metal);
    if(close) {
        draw_street_swept_segment((vec3_t){x,6.12f,z},(vec3_t){x+.13f,6.59f,z},.072f,.067f,metal);
        draw_street_swept_segment((vec3_t){x+.13f,6.59f,z},(vec3_t){x+.48f,6.87f,z},.067f,.057f,metal);
        draw_street_swept_segment((vec3_t){x+.48f,6.87f,z},(vec3_t){x+.98f,6.94f,z},.057f,.047f,metal);
        draw_world_box(x,.20f,z,.21f,.16f,.21f,&world_header,fixture);
    }
    else {
        draw_street_swept_segment((vec3_t){x,6.12f,z},(vec3_t){x+.25f,6.73f,z},.072f,.063f,metal);
        draw_street_swept_segment((vec3_t){x+.25f,6.73f,z},(vec3_t){x+.98f,6.94f,z},.063f,.047f,metal);
    }
    draw_world_box(x+.98f,6.89f,z,.32f,.105f,.19f,&world_header,fixture);
    draw_world_quad(&world_header,
        (vec3_t){x+.73f,6.78f,z-.135f},(vec3_t){x+1.23f,6.78f,z-.135f},
        (vec3_t){x+.73f,6.78f,z+.135f},(vec3_t){x+1.23f,6.78f,z+.135f},
        0,0,1,0,0,1,1,1,bulb);
}

static void draw_street_crown_face(vec3_t a, vec3_t b, vec3_t c,
                                    vec3_t center, color3_t leaf) {
    const vec3_t ab={b.x-a.x,b.y-a.y,b.z-a.z};
    const vec3_t ac={c.x-a.x,c.y-a.y,c.z-a.z};
    const vec3_t mid={(a.x+b.x+c.x)/3.0f,(a.y+b.y+c.y)/3.0f,(a.z+b.z+c.z)/3.0f};
    vec3_t normal={ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
    float shade,length;
    if(normal.x*(mid.x-center.x)+normal.y*(mid.y-center.y)+normal.z*(mid.z-center.z)<0.0f) {
        normal.x=-normal.x;
        normal.y=-normal.y;
        normal.z=-normal.z;
    }
    if(normal.x*(camera_x-mid.x)+normal.y*(camera_y-mid.y)+normal.z*(camera_z-mid.z)<=0.0f) return;
    /* The L1 normalization keeps this very small diffuse bake free of per-face
       square roots. Top foliage catches the cool sky; undersides stay shaded. */
    length=fmaxf(fabsf(normal.x)+fabsf(normal.y)+fabsf(normal.z),.0001f);
    shade=.63f+fmaxf(0.0f,(-.32f*normal.x+.86f*normal.y-.24f*normal.z)/length)*.48f;
    draw_world_triangle(&world_header,a,b,c,pack_color(1.0f,color_scale(leaf,shade)));
}

static void draw_street_tree(float x, float z, district_t district,
                             uint32_t seed) {
    static const float ring_x[6]={1.0f,.5f,-.5f,-1.0f,-.5f,.5f};
    static const float ring_z[6]={0.0f,.8660254f,.8660254f,0.0f,-.8660254f,-.8660254f};
    const float height=4.3f+(float)(seed&3u)*.22f;
    const vec3_t center={x,height,z};
    const vec3_t view=world_to_camera(center);
    const float frustum=(SCREEN_CX+24.0f)*fmaxf(view.z,NEAR_PLANE)/camera_focal+2.6f;
    const color3_t accent=district_color(district);
    const color3_t bark={.29f,.205f,.13f};
    const color3_t leaf=district==DISTRICT_NEON ? (color3_t){.15f,.36f,.29f} :
                        district==DISTRICT_COAST ? (color3_t){.18f,.43f,.27f} :
                                                     (color3_t){.15f,.39f,.20f};
    const vec3_t top={x+.12f,height+1.75f,z-.09f};
    const vec3_t bottom={x-.08f,height-1.35f,z+.06f};
    const int rings=view.z<55.0f?3:(view.z<100.0f?2:1);
    vec3_t crown[3][6];
    int ring,side;
    if(view.z<NEAR_PLANE-2.6f || view.z>FAR_PLANE+2.6f || fabsf(view.x)>frustum) return;
    if(view.z<100.0f)
        draw_world_box(x,.30f,z,.68f,.28f,.68f,&world_header,
                       pack_color(1.0f,color_scale(accent,.38f)));
    draw_street_swept_segment((vec3_t){x,.55f,z},
                              (vec3_t){x+.10f,height-.62f,z},.16f,.075f,bark);
    /* A closed, irregular crown retains volume while the camera orbits.  The
       near mesh has lower/middle/upper rings; distant silhouettes shed rings
       and the tiny planter instead of drawing two crossed diamond cards. */
    for(ring=0;ring<rings;++ring) {
        const float y=rings==1?height+.05f:
                      rings==2?height+(ring==0?-.68f:.78f):
                               height+(ring==0?-.88f:(ring==1?.18f:1.08f));
        const float radius=rings==1?2.10f:
                           rings==2?(ring==0?1.74f:1.66f):
                                    (ring==0?1.40f:(ring==1?2.10f:1.34f));
        for(side=0;side<6;++side) {
            const float irregular=.94f+(float)((seed>>(side*3))&3u)*.035f;
            crown[ring][side]=(vec3_t){x+ring_x[side]*radius*irregular,
                y+(float)((seed>>(side*2+4))&3u)*.035f,
                z+ring_z[side]*radius*irregular*.90f};
        }
    }
    for(side=0;side<6;++side) {
        const int next=(side+1)%6;
        draw_street_crown_face(bottom,crown[0][next],crown[0][side],center,leaf);
        for(ring=0;ring<rings-1;++ring) {
            draw_street_crown_face(crown[ring][side],crown[ring][next],crown[ring+1][side],center,leaf);
            draw_street_crown_face(crown[ring][next],crown[ring+1][next],crown[ring+1][side],center,leaf);
        }
        draw_street_crown_face(top,crown[rings-1][side],crown[rings-1][next],center,leaf);
    }
}

static void draw_bus_shelter(float x, float z, district_t district,
                             uint32_t seed) {
    const color3_t accent=district_color(district);
    const uint32_t frame=pack_color(1.0f,(color3_t){.075f,.10f,.15f});
    const uint32_t glass=pack_color(1.0f,color_scale(accent,.20f));
    const uint32_t trim=pack_color(1.0f,color_scale(accent,.78f));
    const uint32_t white=pack_color(1.0f,(color3_t){.92f,.94f,.98f});
    const int poster=(seed&3u)==0u ? DLA_TEX_CIVIC_MICRO :
                     ((seed&1u) ? district_detail_texture(district) : DLA_TEX_BILLBOARD);
    draw_world_box(x,3.32f,z+1.18f,3.75f,.12f,1.16f,&world_header,frame);
    draw_world_box(x-3.62f,1.70f,z+1.18f,.10f,1.65f,1.05f,&world_header,frame);
    draw_world_box(x+3.62f,1.70f,z+1.18f,.10f,1.65f,1.05f,&world_header,frame);
    draw_world_box(x,1.78f,z+2.17f,3.54f,1.40f,.055f,&world_header,glass);
    draw_world_box(x-.55f,.72f,z+.72f,2.30f,.12f,.46f,&world_header,trim);
    draw_world_box(x-.55f,.37f,z+.97f,.12f,.36f,.12f,&world_header,frame);
    draw_world_quad(&texture_headers[poster],
        (vec3_t){x+1.22f,3.02f,z+2.105f},(vec3_t){x+3.32f,3.02f,z+2.105f},
        (vec3_t){x+1.22f,.48f,z+2.105f},(vec3_t){x+3.32f,.48f,z+2.105f},
        0,0,1,0,0,1,1,1,white);
}

static void draw_sidewalk_kiosk(float x, float z, district_t district,
                                uint32_t seed) {
    const color3_t accent=district_color(district);
    const uint32_t frame=pack_color(1.0f,(color3_t){.055f,.075f,.11f});
    const uint32_t trim=pack_color(1.0f,color_scale(accent,.80f));
    const uint32_t white=pack_color(1.0f,(color3_t){.94f,.96f,1.0f});
    const int poster=(seed&1u)?DLA_TEX_BILLBOARD:district_detail_texture(district);
    draw_textured_volume(x,z,3.30f,2.04f,.07f,2.57f,poster,white,white);
    draw_world_box(x,2.72f,z,2.02f,.14f,1.30f,&world_header,trim);
    draw_world_box(x,3.12f,z,.16f,.30f,.16f,&world_header,frame);
    draw_world_box(x-1.57f,1.31f,z-1.00f,.055f,1.24f,.055f,
                   &world_header,frame);
    draw_world_box(x+1.57f,1.31f,z-1.00f,.055f,1.24f,.055f,
                   &world_header,frame);
    draw_world_box(x-1.57f,1.31f,z+1.00f,.055f,1.24f,.055f,
                   &world_header,frame);
    draw_world_box(x+1.57f,1.31f,z+1.00f,.055f,1.24f,.055f,
                   &world_header,frame);
    draw_world_box(x-2.25f,.42f,z,.28f,.40f,.28f,&world_header,trim);
    draw_world_box(x+2.25f,.42f,z,.28f,.40f,.28f,&world_header,trim);
}

static void draw_cafe_cluster(float x, float z, district_t district,
                              uint32_t seed) {
    const color3_t accent=district_color(district);
    const uint32_t metal=pack_color(1.0f,(color3_t){.11f,.12f,.15f});
    const uint32_t cloth=pack_color(1.0f,color_scale(accent,.72f));
    int table;
    for(table=-1;table<=1;table+=2) {
        const float tx=x+(float)table*1.45f;
        draw_world_box(tx,.72f,z,.72f,.075f,.72f,&world_header,metal);
        draw_world_box(tx,.36f,z,.075f,.35f,.075f,&world_header,metal);
        draw_world_box(tx-1.0f,.42f,z,.32f,.40f,.32f,&world_header,metal);
        draw_world_box(tx+1.0f,.42f,z,.32f,.40f,.32f,&world_header,metal);
    }
    draw_world_box(x,2.15f,z,.075f,1.40f,.075f,&world_header,metal);
    draw_world_triangle(&world_header,(vec3_t){x,3.55f,z},
        (vec3_t){x-2.75f,2.72f,z-1.0f},(vec3_t){x+2.75f,2.72f,z-1.0f},cloth);
    draw_world_triangle(&world_header,(vec3_t){x,3.55f,z},
        (vec3_t){x+2.75f,2.72f,z+1.0f},(vec3_t){x-2.75f,2.72f,z+1.0f},cloth);
    if(seed&2u)
        draw_world_box(x+3.35f,.62f,z,.48f,.60f,.48f,&world_header,
                       pack_color(1.0f,(color3_t){.15f,.34f,.18f}));
}

static void draw_parking_meter(float x, float z, uint32_t seed) {
    const uint32_t pole=pack_color(1.0f,(color3_t){.13f,.15f,.18f});
    const uint32_t cap=pack_color(1.0f,(seed&1u)?
        (color3_t){.38f,.46f,.52f}:(color3_t){.62f,.50f,.24f});
    draw_world_box(x,.63f,z,.055f,.60f,.055f,&world_header,pole);
    draw_world_box(x,1.25f,z,.16f,.13f,.11f,&world_header,cap);
}

static int traffic_signal_state(int cell_x, int cell_z) {
    const uint32_t seed=hash_u32((uint32_t)cell_x*0x51ed270bu^
                                 (uint32_t)cell_z*0x85ebca6bu);
    const float cycle=fmodf(game.time+(float)(seed&255u)*.03125f,12.0f);
    return cycle<6.5f ? 2 : (cycle<7.5f ? 1 : 0);
}

static void draw_traffic_signal_opaque(int cell_x, int cell_z) {
    const float x=(float)cell_x*CITY_CELL+14.3f;
    const float z=(float)cell_z*CITY_CELL+14.3f;
    const vec3_t view=world_to_camera((vec3_t){x,3.0f,z});
    const float frustum=(SCREEN_CX+32.0f)*fmaxf(view.z,NEAR_PLANE)/camera_focal+8.0f;
    const int state=traffic_signal_state(cell_x,cell_z);
    const color3_t colors[3]={{.95f,.05f,.035f},{1.0f,.58f,.04f},{.06f,.92f,.28f}};
    const uint32_t pole=pack_color(1.0f,(color3_t){.08f,.10f,.14f});
    int i;
    if(view.z<-8.0f || view.z>FAR_PLANE+8.0f || fabsf(view.x)>frustum) return;
    draw_world_box(x,2.9f,z,.075f,2.8f,.075f,&world_header,pole);
    draw_world_box(x-2.35f,5.55f,z,2.4f,.06f,.07f,&world_header,pole);
    draw_world_box(x-4.42f,5.05f,z,.30f,.76f,.24f,&world_header,
                   pack_color(1.0f,(color3_t){.035f,.045f,.055f}));
    for(i=0;i<3;++i) {
        screen_point_t point;
        if(project_world((vec3_t){x-4.43f,5.48f-(float)i*.43f,z-.25f},&point))
            draw_disc(&world_header,point.x,point.y,3.0f,point.z+.00001f,6,
                      pack_color(1.0f,color_scale(colors[i],i==state?1.0f:.16f)),
                      pack_color(1.0f,color_scale(colors[i],i==state?.68f:.08f)));
    }
}

/* ------------------------------------------------------------------------
   Pedestrians: a 16-part articulated body from pedestrian_data.h, tinted
   per figure from its seed and posed from joint pitch angles. Close figures
   use the full mesh, mid-range ones a lighter mesh, and distant ones the
   original flat billboards.
   ------------------------------------------------------------------------ */
#define PED_LOD0_DISTANCE 32.0f
#define PED_LOD1_DISTANCE 92.0f
#define MAX_PED_PART_VERTICES 128

typedef struct {
    color3_t skin, shirt, trousers, hair, shoe, accent;
    int style, face, topper;
    bool shorts;
    float height;
} pedestrian_look_t;

typedef struct {
    float root_pitch, root_y;
    float angle[DLA_PED_PART_COUNT];
} pedestrian_joints_t;

static screen_point_t ped_points[MAX_PED_PART_VERTICES];
static uint32_t ped_colors[MAX_PED_PART_VERTICES];
static float ped_facing[MAX_PED_PART_VERTICES];

static pedestrian_look_t pedestrian_look(uint32_t seed) {
    static const color3_t skins[5]={{1.0f,.82f,.68f},{.96f,.74f,.58f},
        {.82f,.58f,.42f},{.62f,.42f,.30f},{.44f,.29f,.20f}};
    static const color3_t shirts[8]={{.95f,.22f,.20f},{.10f,.70f,.86f},
        {.96f,.66f,.12f},{.58f,.26f,.86f},{.24f,.78f,.40f},{.92f,.92f,.92f},
        {.16f,.18f,.24f},{.96f,.48f,.72f}};
    static const color3_t trousers[4]={{.36f,.44f,.64f},{.16f,.17f,.20f},
        {.64f,.56f,.40f},{.46f,.47f,.52f}};
    static const color3_t hairs[5]={{.14f,.10f,.08f},{.36f,.22f,.12f},
        {.86f,.72f,.40f},{.60f,.26f,.12f},{.70f,.70f,.72f}};
    static const color3_t shoes[3]={{.95f,.95f,.95f},{.16f,.16f,.18f},{.86f,.16f,.16f}};
    const uint32_t h=hash_u32(seed^0x9d2c5680u);
    pedestrian_look_t look;
    look.style=(int)(h&3u);
    look.face=(int)((h>>2)&3u);
    look.topper=(int)((h>>4)&3u);
    look.skin=skins[(h>>6)%5u];
    look.shirt=shirts[(h>>9)&7u];
    look.trousers=trousers[(h>>12)&3u];
    look.hair=hairs[(h>>14)%5u];
    look.shoe=shoes[(h>>17)%3u];
    look.accent=shirts[(h>>20)&7u];
    look.shorts=((h>>23)&7u)<2u && look.style!=2;
    look.height=.92f+(float)((h>>26)&63u)*(.16f/63.0f);
    return look;
}

/* Which colour and atlas column each body part takes for this figure. */
static void pedestrian_part_style(const pedestrian_look_t *look, int part,
                                  int slot, color3_t *tint, float *du) {
    *du=0.0f;
    switch(slot) {
        case DLA_PED_SLOT_SKIN:
            *tint=look->skin;
            if(part==DLA_PED_HEAD) *du=(float)look->face*DLA_PED_ATLAS_REGION;
            break;
        case DLA_PED_SLOT_SHIRT:
            *tint=look->shirt;
            *du=(float)look->style*DLA_PED_ATLAS_REGION;
            break;
        case DLA_PED_SLOT_TROUSERS: *tint=look->trousers; break;
        case DLA_PED_SLOT_HAIR: *tint=look->hair; break;
        case DLA_PED_SLOT_ACCENT: *tint=look->accent; break;
        case DLA_PED_SLOT_SHOE: *tint=look->shoe; break;
        case DLA_PED_SLOT_SLEEVE: *tint=look->shirt; break;
        case DLA_PED_SLOT_FOREARM:
            /* Hoodies and jackets cover the forearm with the sleeve cell. */
            if(look->style==1||look->style==2) {
                *tint=look->shirt;
                *du=-DLA_PED_ATLAS_REGION;
            }
            else *tint=look->skin;
            break;
        default:
            if(look->shorts) {
                *tint=look->skin;
                *du=-2.0f*DLA_PED_ATLAS_REGION;
            }
            else *tint=look->trousers;
            break;
    }
}

/* Positive pitch swings a hanging limb forward. */
static void pedestrian_walk_joints(pedestrian_joints_t *joints, float phase) {
    const float s=fsin(phase),c=fcos(phase);
    memset(joints,0,sizeof(*joints));
    joints->root_y=fabsf(s)*.025f;
    joints->angle[DLA_PED_TORSO]=-.06f;
    joints->angle[DLA_PED_HEAD]=.04f;
    joints->angle[DLA_PED_UPPER_ARM_L]=-s*.55f;
    joints->angle[DLA_PED_UPPER_ARM_R]=s*.55f;
    joints->angle[DLA_PED_FOREARM_L]=.45f+fmaxf(0.0f,-s)*.35f;
    joints->angle[DLA_PED_FOREARM_R]=.45f+fmaxf(0.0f,s)*.35f;
    joints->angle[DLA_PED_THIGH_L]=s*.58f;
    joints->angle[DLA_PED_THIGH_R]=-s*.58f;
    /* The knee folds while its leg swings forward and locks at heel strike. */
    joints->angle[DLA_PED_SHIN_L]=-(.12f+fmaxf(0.0f,c)*.95f);
    joints->angle[DLA_PED_SHIN_R]=-(.12f+fmaxf(0.0f,-c)*.95f);
}

static float lerpf(float a, float b, float t) {
    return a+(b-a)*t;
}

/* Airborne figures somersault with flailing limbs, then relax as they settle. */
static void pedestrian_ragdoll_joints(pedestrian_joints_t *joints,
                                      const hit_pedestrian_t *p) {
    const float settle=p->grounded?clampf(p->settle,0.0f,1.0f):0.0f;
    const float flail=fsin(p->tumble*1.7f)*.35f;
    memset(joints,0,sizeof(*joints));
    joints->root_pitch=p->tumble;
    joints->root_y=p->y-.77f*settle;
    joints->angle[DLA_PED_HEAD]=lerpf(-.30f,-.10f,settle);
    joints->angle[DLA_PED_UPPER_ARM_L]=lerpf(-2.3f+flail,-.9f,settle);
    joints->angle[DLA_PED_UPPER_ARM_R]=lerpf(-2.0f-flail,-.4f,settle);
    joints->angle[DLA_PED_FOREARM_L]=lerpf(.7f,.3f,settle);
    joints->angle[DLA_PED_FOREARM_R]=lerpf(.9f,.2f,settle);
    joints->angle[DLA_PED_THIGH_L]=lerpf(.7f+flail*.5f,.25f,settle);
    joints->angle[DLA_PED_THIGH_R]=lerpf(.2f-flail*.5f,.05f,settle);
    joints->angle[DLA_PED_SHIN_L]=lerpf(-.9f,-.35f,settle);
    joints->angle[DLA_PED_SHIN_R]=lerpf(-.5f,-.15f,settle);
}

static void draw_pedestrian_model(const pedestrian_look_t *look,
                                  float x, float z, float yaw,
                                  const pedestrian_joints_t *joints, int lod) {
    const dla_ped_mesh_t *mesh=&dla_pedestrian_lods[lod];
    const pvr_poly_hdr_t *header=&texture_headers[DLA_TEX_PEDESTRIAN];
    const shz_sincos_t yaw_rotation=shz_sincosf(yaw);
    const float c=yaw_rotation.cos,s=yaw_rotation.sin;
    const float height=look->height;
    float part_cos[DLA_PED_PART_COUNT],part_sin[DLA_PED_PART_COUNT];
    float part_ty[DLA_PED_PART_COUNT],part_tz[DLA_PED_PART_COUNT];
    int part;
    QA_COUNT(pedestrians);
    /* Compose each part's pitch about its pivot down the parent chain. The
       root spins about the body centre so a tumble reads as a whole-body
       somersault. */
    for(part=0;part<mesh->part_count;++part) {
        const dla_ped_part_t *p=&mesh->parts[part];
        const shz_sincos_t r=shz_sincosf(joints->angle[part]);
        if(p->parent<0) {
            const shz_sincos_t root=shz_sincosf(joints->root_pitch);
            const float d=p->pivot_y-.90f;
            part_cos[part]=root.cos*r.cos-root.sin*r.sin;
            part_sin[part]=root.cos*r.sin+root.sin*r.cos;
            part_ty[part]=.90f+joints->root_y+root.cos*d;
            part_tz[part]=-root.sin*d;
        }
        else {
            const dla_ped_part_t *parent=&mesh->parts[p->parent];
            const float pc=part_cos[p->parent],ps=part_sin[p->parent];
            const float dy=p->pivot_y-parent->pivot_y;
            const float dz=p->pivot_z-parent->pivot_z;
            part_cos[part]=pc*r.cos-ps*r.sin;
            part_sin[part]=pc*r.sin+ps*r.cos;
            part_ty[part]=part_ty[p->parent]+pc*dy+ps*dz;
            part_tz[part]=part_tz[p->parent]-ps*dy+pc*dz;
        }
    }
    for(part=0;part<mesh->part_count;++part) {
        const dla_ped_part_t *p=&mesh->parts[part];
        const float pc=part_cos[part],ps=part_sin[part];
        const float ty=part_ty[part],tz=part_tz[part];
        color3_t tint;
        float du;
        int i;
        if(p->face_count==0||p->vertex_count>MAX_PED_PART_VERTICES) continue;
        if(part==DLA_PED_HAIR_SHORT&&look->topper!=0) continue;
        if(part==DLA_PED_HAIR_LONG&&look->topper!=1) continue;
        if(part==DLA_PED_CAP&&look->topper!=2) continue;
        pedestrian_part_style(look,part,p->slot,&tint,&du);
        for(i=0;i<p->vertex_count;++i) {
            const dla_ped_vertex_t *v=&mesh->vertices[p->first_vertex+i];
            const float my=(ty+pc*v->y+ps*v->z)*height;
            const float mz=(tz-ps*v->y+pc*v->z)*height;
            const float mx=(v->x+p->pivot_x)*height;
            const vec3_t world={x+mx*c+mz*s,my,z-mx*s+mz*c};
            const float ny=pc*v->ny+ps*v->nz;
            const float lz=-ps*v->ny+pc*v->nz;
            const float nx=v->nx*c+lz*s,nz=-v->nx*s+lz*c;
            const float key=fmaxf(0.0f,nx*-.48f+ny*.64f+nz*-.60f);
            const float shade=fminf(1.0f,.36f+(ny*.5f+.5f)*.28f+key*.40f);
            ped_facing[i]=nx*(camera_x-world.x)+ny*(camera_y-world.y)+
                          nz*(camera_z-world.z);
            ped_colors[i]=pack_color(1.0f,color_scale(tint,shade));
            project_world(world,&ped_points[i]);
        }
        for(i=0;i<p->face_count;++i) {
            const dla_ped_face_t *f=&mesh->faces[p->first_face+i];
            const int a=f->a-p->first_vertex,b=f->b-p->first_vertex,
                      d=f->c-p->first_vertex;
            const dla_ped_vertex_t *va,*vb,*vd;
            /* Smooth tubes: keep any face with a camera-facing corner so the
               silhouette never opens up. */
            if(ped_facing[a]<=0.0f&&ped_facing[b]<=0.0f&&ped_facing[d]<=0.0f) continue;
            if(!ped_points[a].valid||!ped_points[b].valid||!ped_points[d].valid) continue;
            if(!screen_triangle_visible(&ped_points[a],&ped_points[b],&ped_points[d])) continue;
            va=&mesh->vertices[f->a];
            vb=&mesh->vertices[f->b];
            vd=&mesh->vertices[f->c];
            submit_triangle(header,&ped_points[a],&ped_points[b],&ped_points[d],
                            va->u+du,va->v,vb->u+du,vb->v,vd->u+du,vd->v,
                            ped_colors[a],ped_colors[b],ped_colors[d]);
        }
    }
}

/* Distant figures keep the flat six-quad silhouette, tinted like the model. */
static void draw_pedestrian_billboard(float x, float z, float heading,
                                      const pedestrian_look_t *look, float phase) {
    const float bob=fabsf(fsin(phase))*0.045f;
    const float stride=fsin(phase)*.16f;
    const float sx=fsin(heading),sz=fcos(heading);
    const uint32_t shirt=pack_color(1.0f,color_scale(look->shirt,.72f));
    const uint32_t trousers=pack_color(1.0f,color_scale(look->trousers,.55f));
    const uint32_t skin=pack_color(1.0f,color_scale(look->skin,.70f));
    QA_COUNT(pedestrians);
    draw_vertical_billboard(&world_header,x,.76f+bob,z,.21f,.72f,shirt);
    draw_vertical_billboard(&world_header,x,1.48f+bob,z,.135f,.27f,skin);
    draw_vertical_billboard(&world_header,x-.24f,1.00f+bob,z,.055f,.48f,skin);
    draw_vertical_billboard(&world_header,x+.24f,1.00f+bob,z,.055f,.48f,skin);
    draw_vertical_billboard(&world_header,x+sx*stride,.08f,z+sz*stride,.075f,.68f,trousers);
    draw_vertical_billboard(&world_header,x-sx*stride,.08f,z-sz*stride,.075f,.68f,trousers);
}

static void record_pedestrian_shadow(float x, float z, float yaw,
                                     float length, float opacity) {
    if(ped_shadow_count>=MAX_PED_SHADOWS) return;
    ped_shadow_draws[ped_shadow_count++]=(ped_shadow_t){x,z,yaw,length,opacity};
}

/* -1 culls, 0 and 1 pick a mesh, 2 falls back to the billboard. */
static int pedestrian_lod(float x, float z) {
    const float dx=x-camera_x,dz=z-camera_z;
    const float distance_sq=dx*dx+dz*dz;
    if(!world_sphere_visible((vec3_t){x,.95f,z},1.1f,16.0f)) return -1;
    if(distance_sq<PED_LOD0_DISTANCE*PED_LOD0_DISTANCE) return 0;
    return distance_sq<PED_LOD1_DISTANCE*PED_LOD1_DISTANCE?1:2;
}

static void draw_pedestrian_walking(const pedestrian_pose_t *pose) {
    const int lod=pedestrian_lod(pose->x,pose->z);
    pedestrian_look_t look;
    pedestrian_joints_t joints;
    if(lod<0) return;
    look=pedestrian_look(pose->seed);
    if(lod==2) {
        draw_pedestrian_billboard(pose->x,pose->z,pose->heading,&look,pose->phase);
        return;
    }
    pedestrian_walk_joints(&joints,pose->phase);
    draw_pedestrian_model(&look,pose->x,pose->z,pose->heading,&joints,lod);
    record_pedestrian_shadow(pose->x,pose->z,pose->heading,1.0f,lod==0?.55f:.40f);
}

/* A pedestrian the car has hit: tumbling through the air, then lying on
   the pavement along the direction it was thrown. */
static void draw_pedestrian_hit(const hit_pedestrian_t *p) {
    int lod=pedestrian_lod(p->x,p->z);
    pedestrian_look_t look;
    pedestrian_joints_t joints;
    if(lod<0) return;
    if(lod>1) lod=1;
    look=pedestrian_look(p->seed);
    pedestrian_ragdoll_joints(&joints,p);
    draw_pedestrian_model(&look,p->x,p->z,p->heading,&joints,lod);
    record_pedestrian_shadow(p->x,p->z,p->heading,
                             1.0f+1.6f*(p->grounded?p->settle:0.0f),
                             .55f*clampf(1.0f-p->y*.25f,.15f,1.0f));
}

static void draw_roadside_billboard(float x0, float x1, float z,
                                    uint32_t seed) {
    const float center=(x0+x1)*.5f;
    const float half=fminf((x1-x0)*.28f,11.0f);
    const uint32_t frame=pack_color(1.0f,(color3_t){.09f,.11f,.16f});
    draw_world_box(center-half*.72f,5.0f,z,.075f,4.5f,.075f,&world_header,frame);
    draw_world_box(center+half*.72f,5.0f,z,.075f,4.5f,.075f,&world_header,frame);
    draw_world_box(center,9.35f,z,half+.22f,.18f,.16f,&world_header,frame);
    draw_world_quad(&texture_headers[(seed&1u)?DLA_TEX_BILLBOARD:DLA_TEX_NEON_FACADE],
        (vec3_t){center-half,13.0f,z-.18f},(vec3_t){center+half,13.0f,z-.18f},
        (vec3_t){center-half,6.0f,z-.18f},(vec3_t){center+half,6.0f,z-.18f},
        0,0,1,0,0,1,1,1,pack_color(1.0f,(color3_t){1,1,1}));
}

static void draw_wire_segment(float x0, float y0, float x1, float y1,
                              float z, uint32_t color) {
    draw_world_quad(&world_header,
        (vec3_t){x0,y0+.018f,z},(vec3_t){x1,y1+.018f,z},
        (vec3_t){x0,y0-.018f,z},(vec3_t){x1,y1-.018f,z},
        0,0,1,0,0,1,1,1,color);
    draw_world_quad(&world_header,
        (vec3_t){x0,y0,z-.018f},(vec3_t){x1,y1,z-.018f},
        (vec3_t){x0,y0,z+.018f},(vec3_t){x1,y1,z+.018f},
        0,0,1,0,0,1,1,1,color);
}

static void draw_utility_run(float x0, float x1, float z,
                             district_t district, uint32_t seed) {
    const float poles[2]={x0+4.1f,x1-4.1f};
    const uint32_t wood=pack_color(1.0f,(color3_t){.19f,.13f,.095f});
    const uint32_t cable=pack_color(1.0f,(color3_t){.025f,.030f,.040f});
    const uint32_t ceramic=pack_color(1.0f,(color3_t){.70f,.75f,.72f});
    const uint32_t transformer=pack_color(1.0f,color_scale(
        district_color(district),.36f));
    int pole, wire;
    for(pole=0;pole<2;++pole) {
        int insulator;
        draw_world_box(poles[pole],4.45f,z,.11f,4.35f,.11f,
                       &world_header,wood);
        draw_world_box(poles[pole],8.52f,z,1.16f,.085f,.11f,
                       &world_header,wood);
        for(insulator=-1;insulator<=1;++insulator)
            draw_world_box(poles[pole]+(float)insulator*.82f,8.74f,z,
                           .075f,.16f,.075f,&world_header,ceramic);
    }
    draw_world_box(poles[(seed>>4)&1u],7.32f,z+.18f,.36f,.56f,.31f,
                   &world_header,transformer);
    draw_world_box(poles[(seed>>4)&1u]+.43f,5.65f,z+.04f,.035f,1.55f,.035f,
                   &world_header,cable);
    for(wire=0;wire<3;++wire) {
        const float wire_z=z+(float)(wire-1)*.24f;
        const float y=8.86f+(float)wire*.18f;
        const float middle=(poles[0]+poles[1])*.5f;
        draw_wire_segment(poles[0],y,middle,y-.31f,wire_z,cable);
        draw_wire_segment(middle,y-.31f,poles[1],y,wire_z,cable);
    }
}

static void draw_street_name_sign(float x, float z, district_t district,
                                  uint32_t seed) {
    const color3_t accent=district_color(district);
    const uint32_t pole=pack_color(1.0f,(color3_t){.18f,.21f,.25f});
    const uint32_t board=pack_color(1.0f,color_scale(accent,.78f));
    const uint32_t edge=pack_color(1.0f,(color3_t){.84f,.88f,.90f});
    draw_world_box(x,1.92f,z,.055f,1.88f,.055f,&world_header,pole);
    draw_world_box(x+((seed&1u)?.65f:-.65f),3.78f,z,
                   .72f,.20f,.075f,&world_header,board);
    draw_world_box(x,3.37f,z+((seed&2u)?.65f:-.65f),
                   .075f,.18f,.72f,&world_header,board);
    draw_world_box(x,4.03f,z,.11f,.07f,.11f,&world_header,edge);
}

static void draw_newspaper_boxes(float x, float z, district_t district,
                                 uint32_t seed) {
    const uint32_t dark=pack_color(1.0f,(color3_t){.075f,.09f,.12f});
    const uint32_t glass=pack_color(1.0f,(color3_t){.60f,.72f,.78f});
    int item;
    for(item=0;item<2;++item) {
        const float bx=x+(float)item*.72f;
        const color3_t color=item==0 ? district_color(district) :
            ((seed&1u)?(color3_t){.92f,.42f,.08f}:(color3_t){.12f,.50f,.72f});
        const uint32_t body=pack_color(1.0f,color_scale(color,.66f));
        draw_world_box(bx,.62f,z,.29f,.48f,.26f,&world_header,body);
        draw_world_box(bx,1.10f,z-.02f,.31f,.08f,.28f,&world_header,dark);
        draw_world_box(bx,.72f,z-.275f,.19f,.20f,.025f,&world_header,glass);
        draw_world_box(bx,.15f,z,.055f,.14f,.055f,&world_header,dark);
    }
}

static void draw_bike_rack(float x, float z, uint32_t seed) {
    const uint32_t metal=pack_color(1.0f,(color3_t){.45f,.49f,.52f});
    const uint32_t tire=pack_color(1.0f,(color3_t){.035f,.04f,.05f});
    int rack;
    for(rack=-1;rack<=1;++rack) {
        const float rx=x+(float)rack*.58f;
        draw_world_box(rx,.38f,z-.36f,.035f,.36f,.035f,&world_header,metal);
        draw_world_box(rx,.72f,z,.035f,.035f,.38f,&world_header,metal);
        draw_world_box(rx,.38f,z+.36f,.035f,.36f,.035f,&world_header,metal);
    }
    if(seed&1u) {
        draw_world_box(x+.16f,.39f,z,.035f,.36f,.48f,&world_header,tire);
        draw_world_box(x+.16f,.76f,z,.035f,.035f,.48f,&world_header,metal);
    }
}

static void draw_storm_drain(float x, float z, bool along_x) {
    const uint32_t recess=pack_color(1.0f,(color3_t){.025f,.030f,.038f});
    const uint32_t grate=pack_color(1.0f,(color3_t){.19f,.21f,.23f});
    const float hx=along_x?.72f:.23f;
    const float hz=along_x?.23f:.72f;
    int slat;
    draw_world_quad(&world_header,
        (vec3_t){x-hx,.046f,z+hz},(vec3_t){x+hx,.046f,z+hz},
        (vec3_t){x-hx,.046f,z-hz},(vec3_t){x+hx,.046f,z-hz},
        0,0,1,0,0,1,1,1,recess);
    for(slat=-2;slat<=2;++slat) {
        const float offset=(float)slat*.16f;
        if(along_x)
            draw_world_quad(&world_header,
                (vec3_t){x+offset-.025f,.051f,z+hz*.86f},
                (vec3_t){x+offset+.025f,.051f,z+hz*.86f},
                (vec3_t){x+offset-.025f,.051f,z-hz*.86f},
                (vec3_t){x+offset+.025f,.051f,z-hz*.86f},
                0,0,1,0,0,1,1,1,grate);
        else
            draw_world_quad(&world_header,
                (vec3_t){x-hx*.86f,.051f,z+offset+.025f},
                (vec3_t){x+hx*.86f,.051f,z+offset+.025f},
                (vec3_t){x-hx*.86f,.051f,z+offset-.025f},
                (vec3_t){x+hx*.86f,.051f,z+offset-.025f},
                0,0,1,0,0,1,1,1,grate);
    }
}

static void draw_direction_arrow(float x, float z, bool along_x,
                                 float direction) {
    const uint32_t paint=pack_color(1.0f,(color3_t){.84f,.86f,.82f});
    const float y=.043f;
    if(along_x) {
        draw_world_quad(&world_header,
            (vec3_t){x-direction*1.25f,y,z-.16f},
            (vec3_t){x+direction*.42f,y,z-.16f},
            (vec3_t){x-direction*1.25f,y,z+.16f},
            (vec3_t){x+direction*.42f,y,z+.16f},0,0,1,0,0,1,1,1,paint);
        draw_world_triangle(&world_header,(vec3_t){x+direction*1.58f,y,z},
            (vec3_t){x+direction*.34f,y,z-.58f},
            (vec3_t){x+direction*.34f,y,z+.58f},paint);
    }
    else {
        draw_world_quad(&world_header,
            (vec3_t){x-.16f,y,z-direction*1.25f},
            (vec3_t){x+.16f,y,z-direction*1.25f},
            (vec3_t){x-.16f,y,z+direction*.42f},
            (vec3_t){x+.16f,y,z+direction*.42f},0,0,1,0,0,1,1,1,paint);
        draw_world_triangle(&world_header,(vec3_t){x,y,z+direction*1.58f},
            (vec3_t){x-.58f,y,z+direction*.34f},
            (vec3_t){x+.58f,y,z+direction*.34f},paint);
    }
}

static void draw_cross_post(float x, float base_y, float z,
                            float half_width, float height,
                            uint32_t color) {
    draw_world_quad(&world_header,
        (vec3_t){x-half_width,base_y+height,z},
        (vec3_t){x+half_width,base_y+height,z},
        (vec3_t){x-half_width,base_y,z},
        (vec3_t){x+half_width,base_y,z},
        0,0,1,0,0,1,1,1,color);
    draw_world_quad(&world_header,
        (vec3_t){x,base_y+height,z-half_width},
        (vec3_t){x,base_y+height,z+half_width},
        (vec3_t){x,base_y,z-half_width},
        (vec3_t){x,base_y,z+half_width},
        0,0,1,0,0,1,1,1,color);
}

static void draw_downtown_valet_cluster(float x, float z, float road_dir,
                                        uint32_t seed) {
    const uint32_t dark=pack_color(1.0f,(color3_t){.045f,.065f,.11f});
    const uint32_t brass=pack_color(1.0f,(color3_t){.72f,.48f,.14f});
    const uint32_t canopy=pack_color(1.0f,(seed&1u)?
        (color3_t){.08f,.25f,.46f}:(color3_t){.30f,.10f,.18f});
    const uint32_t white=pack_color(1.0f,(color3_t){.94f,.95f,.98f});
    const float front_z=z+road_dir*1.18f;
    const float rear_z=z-road_dir*1.18f;
    const float stanchion_z=z+road_dir*1.55f;
    draw_world_quad(&world_header,
        (vec3_t){x-3.55f,3.42f,front_z},(vec3_t){x+3.55f,3.42f,front_z},
        (vec3_t){x-3.55f,3.42f,rear_z},(vec3_t){x+3.55f,3.42f,rear_z},
        0,0,1,0,0,1,1,1,canopy);
    draw_world_quad(&texture_headers[DLA_TEX_DISTRICT_DOWNTOWN],
        (vec3_t){x-3.55f,3.42f,front_z+road_dir*.025f},
        (vec3_t){x+3.55f,3.42f,front_z+road_dir*.025f},
        (vec3_t){x-3.55f,2.82f,front_z+road_dir*.025f},
        (vec3_t){x+3.55f,2.82f,front_z+road_dir*.025f},
        0,0,1,0,0,1,1,1,white);
    draw_cross_post(x-3.12f,.08f,z-road_dir*.72f,.075f,3.26f,dark);
    draw_cross_post(x+3.12f,.08f,z-road_dir*.72f,.075f,3.26f,dark);
    draw_world_box(x-1.55f,.66f,z+road_dir*.62f,.56f,.58f,.42f,
                   &world_header,dark);
    draw_world_quad(&texture_headers[DLA_TEX_CIVIC_MICRO],
        (vec3_t){x-2.02f,1.12f,z+road_dir*1.05f},
        (vec3_t){x-1.08f,1.12f,z+road_dir*1.05f},
        (vec3_t){x-2.02f,.25f,z+road_dir*1.05f},
        (vec3_t){x-1.08f,.25f,z+road_dir*1.05f},
        0,0,1,0,0,1,1,1,white);
    draw_cross_post(x+.55f,.08f,stanchion_z,.055f,.74f,brass);
    draw_cross_post(x+2.25f,.08f,stanchion_z,.055f,.74f,brass);
    draw_world_quad(&world_header,
        (vec3_t){x+.55f,.72f,stanchion_z},(vec3_t){x+2.25f,.72f,stanchion_z},
        (vec3_t){x+.55f,.64f,stanchion_z},(vec3_t){x+2.25f,.64f,stanchion_z},
        0,0,1,0,0,1,1,1,brass);
}

static void draw_surfboard(float x, float z, uint32_t color) {
    draw_world_quad(&world_header,
        (vec3_t){x-.29f,2.22f,z},(vec3_t){x+.29f,2.22f,z},
        (vec3_t){x-.34f,.43f,z},(vec3_t){x+.34f,.43f,z},
        0,0,1,0,0,1,1,1,color);
    draw_world_triangle(&world_header,(vec3_t){x,2.72f,z},
        (vec3_t){x-.29f,2.22f,z},(vec3_t){x+.29f,2.22f,z},color);
    draw_world_triangle(&world_header,(vec3_t){x,.12f,z},
        (vec3_t){x+.34f,.43f,z},(vec3_t){x-.34f,.43f,z},color);
}

static void draw_coast_rental_cluster(float x, float z, float road_dir,
                                      uint32_t seed) {
    static const color3_t board_colors[4]={
        {.96f,.42f,.12f},{.08f,.68f,.86f},{.96f,.80f,.15f},{.86f,.20f,.48f}
    };
    const uint32_t frame=pack_color(1.0f,(color3_t){.10f,.24f,.28f});
    const uint32_t white=pack_color(1.0f,(color3_t){.94f,.97f,.92f});
    const uint32_t cooler=pack_color(1.0f,(color3_t){.15f,.64f,.70f});
    const float front_z=z+road_dir*.48f;
    int board;
    draw_cross_post(x-3.18f,.08f,z,.065f,3.12f,frame);
    draw_cross_post(x+3.18f,.08f,z,.065f,3.12f,frame);
    draw_world_quad(&texture_headers[DLA_TEX_DISTRICT_COAST],
        (vec3_t){x-3.45f,3.30f,front_z},(vec3_t){x+3.45f,3.30f,front_z},
        (vec3_t){x-3.45f,2.47f,front_z},(vec3_t){x+3.45f,2.47f,front_z},
        0,0,1,0,0,1,1,1,white);
    draw_world_quad(&world_header,
        (vec3_t){x-2.65f,2.30f,z},(vec3_t){x+2.65f,2.30f,z},
        (vec3_t){x-2.65f,2.20f,z},(vec3_t){x+2.65f,2.20f,z},
        0,0,1,0,0,1,1,1,frame);
    for(board=0;board<3;++board)
        draw_surfboard(x-1.55f+(float)board*1.55f,
                       z+road_dir*.12f,
                       pack_color(1.0f,board_colors[(seed+(uint32_t)board)&3u]));
    draw_world_box(x+3.78f,.50f,z+road_dir*.10f,.48f,.42f,.58f,
                   &world_header,cooler);
    draw_world_quad(&world_header,
        (vec3_t){x+3.42f,.74f,z+road_dir*.70f},
        (vec3_t){x+4.14f,.74f,z+road_dir*.70f},
        (vec3_t){x+3.42f,.58f,z+road_dir*.70f},
        (vec3_t){x+4.14f,.58f,z+road_dir*.70f},
        0,0,1,0,0,1,1,1,white);
}

static void draw_arts_service_cluster(float x, float z, float road_dir,
                                      uint32_t seed) {
    const uint32_t steel=pack_color(1.0f,(seed&1u)?
        (color3_t){.08f,.34f,.32f}:(color3_t){.27f,.30f,.19f});
    const uint32_t dark=pack_color(1.0f,(color3_t){.045f,.055f,.065f});
    const uint32_t orange=pack_color(1.0f,(color3_t){.92f,.32f,.045f});
    const uint32_t white=pack_color(1.0f,(color3_t){.90f,.86f,.70f});
    const float front_z=z+road_dir*1.12f;
    int rib;
    draw_world_box(x-1.30f,.80f,z,.36f+1.76f,.72f,1.05f,
                   &world_header,steel);
    draw_world_quad(&world_header,
        (vec3_t){x-3.44f,1.59f,z-road_dir*1.04f},
        (vec3_t){x+.84f,1.59f,z-road_dir*1.04f},
        (vec3_t){x-3.15f,1.45f,z+road_dir*1.04f},
        (vec3_t){x+.55f,1.45f,z+road_dir*1.04f},
        0,0,1,0,0,1,1,1,dark);
    draw_world_quad(&texture_headers[DLA_TEX_GRAFFITI],
        (vec3_t){x-3.05f,1.32f,front_z},(vec3_t){x+.45f,1.32f,front_z},
        (vec3_t){x-3.05f,.28f,front_z},(vec3_t){x+.45f,.28f,front_z},
        0,0,1,0,0,1,1,1,white);
    draw_world_quad(&texture_headers[DLA_TEX_GRAFFITI],
        (vec3_t){x-3.43f,1.32f,z-road_dir*.95f},
        (vec3_t){x-3.43f,1.32f,z+road_dir*.95f},
        (vec3_t){x-3.43f,.28f,z-road_dir*.95f},
        (vec3_t){x-3.43f,.28f,z+road_dir*.95f},
        0,0,1,0,0,1,1,1,white);
    for(rib=0;rib<4;++rib) {
        const float rx=x-2.85f+(float)rib*1.03f;
        draw_world_quad(&world_header,
            (vec3_t){rx-.035f,1.38f,front_z+road_dir*.015f},
            (vec3_t){rx+.035f,1.38f,front_z+road_dir*.015f},
            (vec3_t){rx-.035f,.20f,front_z+road_dir*.015f},
            (vec3_t){rx+.035f,.20f,front_z+road_dir*.015f},
            0,0,1,0,0,1,1,1,dark);
    }
    draw_world_box(x+2.70f,1.02f,z+road_dir*.20f,1.72f,.17f,.14f,
                   &world_header,orange);
    draw_world_triangle(&world_header,(vec3_t){x+1.35f,.12f,z+road_dir*.18f},
        (vec3_t){x+1.75f,.85f,z+road_dir*.18f},
        (vec3_t){x+2.10f,.12f,z+road_dir*.18f},dark);
    draw_world_triangle(&world_header,(vec3_t){x+3.30f,.12f,z+road_dir*.18f},
        (vec3_t){x+3.65f,.85f,z+road_dir*.18f},
        (vec3_t){x+4.05f,.12f,z+road_dir*.18f},dark);
    draw_world_triangle(&world_header,(vec3_t){x+1.45f,.10f,front_z+.20f*road_dir},
        (vec3_t){x+1.72f,.78f,front_z+.20f*road_dir},
        (vec3_t){x+1.99f,.10f,front_z+.20f*road_dir},orange);
    draw_world_quad(&world_header,
        (vec3_t){x+1.52f,.42f,front_z+.21f*road_dir},
        (vec3_t){x+1.92f,.42f,front_z+.21f*road_dir},
        (vec3_t){x+1.58f,.31f,front_z+.21f*road_dir},
        (vec3_t){x+1.86f,.31f,front_z+.21f*road_dir},
        0,0,1,0,0,1,1,1,white);
}

static void draw_neon_market_cluster(float x, float z, float road_dir,
                                     uint32_t seed) {
    const uint32_t dark=pack_color(1.0f,(color3_t){.035f,.045f,.075f});
    const uint32_t cyan=pack_color(1.0f,(color3_t){.04f,.76f,.96f});
    const uint32_t pink=pack_color(1.0f,(color3_t){.98f,.06f,.54f});
    const uint32_t warm=pack_color(1.0f,(color3_t){1.0f,.58f,.12f});
    const uint32_t white=pack_color(1.0f,(color3_t){.96f,.96f,1.0f});
    const float front_z=z+road_dir*1.22f;
    const float back_z=z-road_dir*1.15f;
    int lantern,vendor;
    draw_world_quad_colored(&world_header,
        (vec3_t){x-3.45f,3.35f,front_z},(vec3_t){x+3.45f,3.35f,front_z},
        (vec3_t){x-3.05f,3.12f,back_z},(vec3_t){x+3.05f,3.12f,back_z},
        (seed&1u)?pink:cyan,(seed&1u)?cyan:pink,dark,dark);
    draw_cross_post(x-3.05f,.08f,z-road_dir*.70f,.065f,3.10f,dark);
    draw_cross_post(x+3.05f,.08f,z-road_dir*.70f,.065f,3.10f,dark);
    draw_world_box(x,.72f,z+road_dir*.36f,2.75f,.62f,.58f,
                   &world_header,dark);
    draw_world_quad(&texture_headers[DLA_TEX_STOREFRONT_MICRO],
        (vec3_t){x-2.64f,1.20f,z+road_dir*.95f},
        (vec3_t){x+2.64f,1.20f,z+road_dir*.95f},
        (vec3_t){x-2.64f,.24f,z+road_dir*.95f},
        (vec3_t){x+2.64f,.24f,z+road_dir*.95f},
        0,0,1,0,0,1,1,1,white);
    draw_world_quad(&texture_headers[DLA_TEX_NEON_FACADE],
        (vec3_t){x-2.60f,3.72f,front_z+road_dir*.02f},
        (vec3_t){x+2.60f,3.72f,front_z+road_dir*.02f},
        (vec3_t){x-2.60f,3.12f,front_z+road_dir*.02f},
        (vec3_t){x+2.60f,3.12f,front_z+road_dir*.02f},
        0,0,1,0,0,1,1,1,white);
    draw_world_quad(&texture_headers[DLA_TEX_NEON_FACADE],
        (vec3_t){x-3.46f,3.72f,back_z},
        (vec3_t){x-3.46f,3.72f,front_z},
        (vec3_t){x-3.46f,3.12f,back_z},
        (vec3_t){x-3.46f,3.12f,front_z},
        0,0,1,0,0,1,1,1,white);
    draw_world_quad(&texture_headers[DLA_TEX_STOREFRONT_MICRO],
        (vec3_t){x-2.76f,1.20f,z-road_dir*.22f},
        (vec3_t){x-2.76f,1.20f,z+road_dir*.94f},
        (vec3_t){x-2.76f,.24f,z-road_dir*.22f},
        (vec3_t){x-2.76f,.24f,z+road_dir*.94f},
        0,0,1,0,0,1,1,1,white);
    /* A bright vending bank gives the stall a readable side elevation while
       doubling as a close-range district landmark from the driving lane. */
    for(vendor=-1;vendor<=1;++vendor) {
        const float vz=z+(float)vendor*.78f;
        const uint32_t body=((vendor+(int)(seed&1u))&1)?pink:cyan;
        draw_world_quad(&world_header,
            (vec3_t){x-3.50f,2.45f,vz-.34f},
            (vec3_t){x-3.50f,2.45f,vz+.34f},
            (vec3_t){x-3.50f,.16f,vz-.34f},
            (vec3_t){x-3.50f,.16f,vz+.34f},
            0,0,1,0,0,1,1,1,body);
        draw_world_quad(&world_header,
            (vec3_t){x-3.515f,2.05f,vz-.25f},
            (vec3_t){x-3.515f,2.05f,vz+.25f},
            (vec3_t){x-3.515f,.83f,vz-.25f},
            (vec3_t){x-3.515f,.83f,vz+.25f},
            0,0,1,0,0,1,1,1,dark);
        draw_world_quad(&world_header,
            (vec3_t){x-3.52f,.62f,vz-.11f},
            (vec3_t){x-3.52f,.62f,vz+.11f},
            (vec3_t){x-3.52f,.44f,vz-.11f},
            (vec3_t){x-3.52f,.44f,vz+.11f},
            0,0,1,0,0,1,1,1,warm);
    }
    draw_world_quad_colored(&world_header,
        (vec3_t){x-3.53f,5.35f,z-1.72f},
        (vec3_t){x-3.53f,5.35f,z-.12f},
        (vec3_t){x-3.53f,2.68f,z-1.72f},
        (vec3_t){x-3.53f,2.68f,z-.12f},
        cyan,pink,pack_color(1.0f,color_scale((color3_t){.04f,.76f,.96f},.32f)),
        pack_color(1.0f,color_scale((color3_t){.98f,.06f,.54f},.32f)));
    draw_world_quad_colored(&world_header,
        (vec3_t){x-3.53f,5.35f,z+.12f},
        (vec3_t){x-3.53f,5.35f,z+1.72f},
        (vec3_t){x-3.53f,2.68f,z+.12f},
        (vec3_t){x-3.53f,2.68f,z+1.72f},
        pink,cyan,pack_color(1.0f,color_scale((color3_t){.98f,.06f,.54f},.32f)),
        pack_color(1.0f,color_scale((color3_t){.04f,.76f,.96f},.32f)));
    for(lantern=-1;lantern<=1;++lantern)
        draw_vertical_billboard(&world_header,x+(float)lantern*1.45f,
                                2.40f,z+road_dir*.96f,.16f,.42f,
                                lantern==0?warm:((seed&1u)?pink:cyan));
}

static void draw_district_furnishing_cluster(float x0, float x1,
                                             float z0, float z1,
                                             const building_t *building) {
    const uint32_t seed=building->seed;
    const float center_x=(x0+x1)*.5f;
    int edge;
    /* Both sidewalk faces own a stable furnishing site, but only the site near
       the player is registered. This avoids camera-dependent prop teleporting
       while ensuring that a long block never presents an empty 90 m frontage. */
    for(edge=0;edge<2;++edge) {
        const float x=building->district==DISTRICT_COAST ? x0+4.2f :
            (building->district==DISTRICT_DOWNTOWN ?
                (center_x<0.0f ? x1-4.2f : x0+4.2f) :
                center_x+(((seed>>(9+edge))&1u) ? -12.0f : 12.0f));
        const float z=edge ? z1-1.75f : z0+1.75f;
        const float road_dir=edge ? 1.0f : -1.0f;
        const float dx=x-car.x,dz=z-car.z;
        if(dx*dx+dz*dz>90.0f*90.0f) continue;
        QA_COUNT(furnishing_clusters);
        if(building->district==DISTRICT_DOWNTOWN)
            draw_downtown_valet_cluster(x,z,road_dir,seed^(uint32_t)edge);
        else if(building->district==DISTRICT_COAST)
            draw_coast_rental_cluster(x,z,road_dir,seed^(uint32_t)edge);
        else if(building->district==DISTRICT_ARTS)
            draw_arts_service_cluster(x,z,road_dir,seed^(uint32_t)edge);
        else
            draw_neon_market_cluster(x,z,road_dir,seed^(uint32_t)edge);
    }
}

static void draw_block_street_detail(int cell_x, int cell_z,
                                     float x0, float x1, float z0, float z1,
                                     const building_t *building) {
    const float cx=(x0+x1)*.5f,cz=(z0+z1)*.5f;
    const float dx=cx-car.x,dz=cz-car.z;
    const float distance_sq=dx*dx+dz*dz;
    const vec3_t view=world_to_camera((vec3_t){cx,2.0f,cz});
    const uint32_t metal=pack_color(1.0f,(color3_t){.12f,.14f,.18f});
    const color3_t accent=district_color(building->district);
    const uint32_t curb=pack_color(1.0f,color_scale(accent,.64f));
    if(distance_sq>195.0f*195.0f || view.z<-72.0f || view.z>FAR_PLANE+72.0f) return;
    draw_world_quad(&world_header,(vec3_t){x0,.078f,z0},(vec3_t){x1,.078f,z0},
                    (vec3_t){x0,.078f,z0+.22f},(vec3_t){x1,.078f,z0+.22f},
                    0,0,1,0,0,1,1,1,curb);
    draw_world_quad(&world_header,(vec3_t){x1,.078f,z1},(vec3_t){x0,.078f,z1},
                    (vec3_t){x1,.078f,z1-.22f},(vec3_t){x0,.078f,z1-.22f},
                    0,0,1,0,0,1,1,1,curb);
    draw_world_quad(&world_header,(vec3_t){x0,.079f,z1},(vec3_t){x0,.079f,z0},
                    (vec3_t){x0+.22f,.079f,z1},(vec3_t){x0+.22f,.079f,z0},
                    0,0,1,0,0,1,1,1,curb);
    draw_world_quad(&world_header,(vec3_t){x1,.079f,z0},(vec3_t){x1,.079f,z1},
                    (vec3_t){x1-.22f,.079f,z0},(vec3_t){x1-.22f,.079f,z1},
                    0,0,1,0,0,1,1,1,curb);
    if(distance_sq<132.0f*132.0f) {
        const uint32_t curb_face=pack_color(1.0f,color_scale(accent,.34f));
        draw_world_quad(&world_header,(vec3_t){x1,.11f,z0},(vec3_t){x0,.11f,z0},
                        (vec3_t){x1,.005f,z0},(vec3_t){x0,.005f,z0},
                        0,0,1,0,0,1,1,1,curb_face);
        draw_world_quad(&world_header,(vec3_t){x0,.11f,z1},(vec3_t){x1,.11f,z1},
                        (vec3_t){x0,.005f,z1},(vec3_t){x1,.005f,z1},
                        0,0,1,0,0,1,1,1,curb_face);
        draw_world_quad(&world_header,(vec3_t){x0,.11f,z0},(vec3_t){x0,.11f,z1},
                        (vec3_t){x0,.005f,z0},(vec3_t){x0,.005f,z1},
                        0,0,1,0,0,1,1,1,curb_face);
        draw_world_quad(&world_header,(vec3_t){x1,.11f,z1},(vec3_t){x1,.11f,z0},
                        (vec3_t){x1,.005f,z1},(vec3_t){x1,.005f,z0},
                        0,0,1,0,0,1,1,1,curb_face);
    }
    draw_streetlamp_opaque(x0-1.2f,z0+7.0f,building->district);
    draw_streetlamp_opaque(x1+1.2f,z1-7.0f,building->district);

    /* Dense, readable curbside silhouettes keep the sidewalks feeling lived in. */
    draw_world_box(x0+8.0f,.48f,z0+1.0f,2.0f,.14f,.38f,&world_header,metal);
    draw_world_box(x0+6.5f,.26f,z0+1.0f,.12f,.25f,.28f,&world_header,metal);
    draw_world_box(x0+9.5f,.26f,z0+1.0f,.12f,.25f,.28f,&world_header,metal);
    if(distance_sq<132.0f*132.0f) {
        int meter;
        if(distance_sq<104.0f*104.0f) {
            draw_storm_drain(x0+21.0f,z0-.38f,true);
            draw_storm_drain(x1+.38f,z1-21.0f,false);
        }
        draw_streetlamp_opaque(x0+7.0f,z1+1.2f,building->district);
        draw_streetlamp_opaque(x1-7.0f,z0-1.2f,building->district);
        draw_street_tree(x1-5.0f,z0+4.1f,building->district,building->seed>>6);
        draw_street_tree(x0+5.0f,z1-4.1f,building->district,building->seed>>11);
        draw_world_box(x1-2.1f,.30f,z0+1.0f,.25f,.28f,.25f,&world_header,
                       pack_color(1.0f,(color3_t){.78f,.12f,.055f}));
        draw_world_box(x1-2.1f,.63f,z0+1.0f,.12f,.12f,.12f,&world_header,
                       pack_color(1.0f,(color3_t){.92f,.72f,.18f}));
        if((building->seed%3u)==0u)
            draw_bus_shelter(x0+15.0f,z0+.55f,building->district,building->seed);
        if((building->seed&1u)!=0u)
            draw_sidewalk_kiosk(x0+12.0f,z1-2.2f,building->district,
                                building->seed>>4);
        else
            draw_cafe_cluster(x1-9.0f,z0+3.0f,building->district,
                              building->seed>>5);
        if(distance_sq<104.0f*104.0f && (building->seed%3u)==0u)
            draw_street_name_sign(x0-.85f,z0-.85f,building->district,
                                  building->seed>>3);
        if(distance_sq<104.0f*104.0f && (building->seed&3u)!=0u)
            draw_newspaper_boxes(x0+23.0f,z0+1.38f,building->district,
                                 building->seed>>7);
        if(distance_sq<104.0f*104.0f &&
           (building->district==DISTRICT_ARTS ||
            building->district==DISTRICT_COAST ||
            (building->seed%5u)==0u) && (building->seed&1u)==0u)
            draw_utility_run(x0,x1,z0+2.05f,building->district,building->seed);
        for(meter=0;meter<4;++meter)
            draw_parking_meter(x0+12.0f+(float)meter*11.0f,z0+.72f,
                               building->seed>>(meter+2));
        draw_district_furnishing_cluster(x0,x1,z0,z1,building);
    }
    if((building->seed%5u)==0u)
        draw_world_box(x1-9.2f,.65f,z0+.8f,.45f,.62f,.45f,&world_header,
                       pack_color(1.0f,(color3_t){.12f,.32f,.36f}));
    if(building->exists && ((building->seed%7u)==0u ||
       (building->district==DISTRICT_NEON && (building->seed%3u)==0u)))
        draw_roadside_billboard(x0,x1,z0-.6f,building->seed);

    if(building->district==DISTRICT_COAST) {
        draw_palm(x0+3.0f,z1-5.0f,.78f);
        draw_world_box(x1-5.0f,.72f,z0+1.0f,.08f,.68f,2.4f,&world_header,
                       pack_color(1.0f,(color3_t){.82f,.88f,.82f}));
        if(distance_sq<104.0f*104.0f)
            draw_bike_rack(x1-13.0f,z1-1.35f,building->seed>>9);
    }
    else if(building->district==DISTRICT_ARTS) {
        draw_world_box(x1-6.0f,1.05f,z0+2.2f,3.0f,1.0f,1.2f,&world_header,
                       pack_color(1.0f,color_scale(accent,.78f)));
        if((building->seed&1u)==0u)
            draw_world_box(x1-12.5f,.78f,z0+2.0f,2.4f,.73f,1.0f,&world_header,
                           pack_color(1.0f,(color3_t){.08f,.42f,.50f}));
        if(distance_sq<104.0f*104.0f) {
            const uint32_t pallet=pack_color(1.0f,(color3_t){.34f,.20f,.10f});
            const uint32_t barrel=pack_color(1.0f,(color3_t){.60f,.18f,.065f});
            int crate;
            for(crate=0;crate<3;++crate)
                draw_world_box(x1-18.0f+(float)crate*.78f,
                               .30f+(float)(crate&1)*.28f,z0+1.55f,
                               .34f,.28f,.34f,&world_header,pallet);
            draw_world_box(x1-15.0f,.52f,z0+1.55f,.30f,.50f,.30f,
                           &world_header,barrel);
        }
    }
    else if(building->district==DISTRICT_DOWNTOWN) {
        int bollard;
        for(bollard=0;bollard<3;++bollard)
            draw_world_box(x0+4.0f+(float)bollard*2.3f,.42f,z0+.8f,
                           .12f,.40f,.12f,&world_header,
                           pack_color(1.0f,(color3_t){.40f,.34f,.20f}));
        if(distance_sq<104.0f*104.0f) {
            const uint32_t stone=pack_color(1.0f,(color3_t){.45f,.47f,.52f});
            const uint32_t green=pack_color(1.0f,(color3_t){.08f,.29f,.15f});
            draw_world_box(x1-12.0f,.38f,z1-1.2f,1.05f,.34f,1.05f,
                           &world_header,stone);
            draw_world_box(x1-12.0f,1.25f,z1-1.2f,.78f,.55f,.78f,
                           &world_header,green);
        }
    }
    else {
        draw_world_quad(&texture_headers[DLA_TEX_NEON_FACADE],
            (vec3_t){x1-7.5f,3.55f,z0+.76f},(vec3_t){x1-2.5f,3.55f,z0+.76f},
            (vec3_t){x1-7.5f,.18f,z0+.76f},(vec3_t){x1-2.5f,.18f,z0+.76f},
            0,0,1,0,0,1,1,1,pack_color(1.0f,(color3_t){1,1,1}));
        draw_world_box(x1-7.65f,1.86f,z0+.82f,.09f,1.78f,.09f,
                       &world_header,pack_color(1.0f,color_scale(accent,.62f)));
        draw_world_box(x1-2.35f,1.86f,z0+.82f,.09f,1.78f,.09f,
                       &world_header,pack_color(1.0f,color_scale(accent,.62f)));
        if(distance_sq<104.0f*104.0f) {
            const uint32_t cyan=pack_color(1.0f,(color3_t){.06f,.78f,.96f});
            const uint32_t magenta=pack_color(1.0f,(color3_t){.98f,.055f,.54f});
            draw_world_box(x0+27.0f,2.05f,z1-1.35f,.62f,1.95f,.10f,
                           &world_header,(building->seed&1u)?cyan:magenta);
            draw_world_box(x0+27.0f,4.12f,z1-1.35f,.76f,.10f,.16f,
                           &world_header,(building->seed&1u)?magenta:cyan);
        }
    }

    if(distance_sq<145.0f*145.0f) {
        int slot;
        for(slot=0;slot<4;++slot) {
            pedestrian_pose_t pose;
            const hit_pedestrian_t *hit;
            if(slot>=2 && distance_sq>=105.0f*105.0f) break;
            if(!block_pedestrian(cell_x,cell_z,building,slot,&pose)) continue;
            hit=find_hit_pedestrian(cell_x,cell_z,slot);
            if(hit) draw_pedestrian_hit(hit);
            else draw_pedestrian_walking(&pose);
        }
    }
}

static void draw_district_landmark(int cell_x, int cell_z,
                                   const building_t *building) {
    const float cx=building->cx,cz=building->cz,h=building->height;
    const uint32_t gold=pack_color(1.0f,(color3_t){.94f,.66f,.18f});
    if(cell_x==0&&cell_z==0) {
        draw_world_box(cx,h+3.0f,cz,7.0f,3.0f,7.0f,&world_header,
                       pack_color(1.0f,(color3_t){.08f,.14f,.27f}));
        draw_world_box(cx,h+8.8f,cz,3.4f,2.8f,3.4f,&world_header,gold);
        draw_world_box(cx,h+18.0f,cz,.12f,7.2f,.12f,&world_header,gold);
    }
    else if(cell_x==-2&&cell_z==0) {
        const float front_z=cz-building->depth*.5f-.7f;
        const uint32_t white=pack_color(1.0f,(color3_t){.82f,.91f,.86f});
        draw_world_box(cx-11.0f,5.2f,front_z,.22f,5.0f,.22f,&world_header,white);
        draw_world_box(cx+11.0f,5.2f,front_z,.22f,5.0f,.22f,&world_header,white);
        draw_world_quad(&texture_headers[DLA_TEX_DISTRICT_COAST],
            (vec3_t){cx-11.0f,10.2f,front_z-.12f},(vec3_t){cx+11.0f,10.2f,front_z-.12f},
            (vec3_t){cx-8.5f,5.8f,front_z-.12f},(vec3_t){cx+8.5f,5.8f,front_z-.12f},
            0,0,1,0,0,1,1,1,pack_color(1.0f,(color3_t){1,1,1}));
    }
    else if(cell_x==2&&cell_z==0) {
        int leg;
        for(leg=-1;leg<=1;leg+=2)
            draw_world_box(cx+(float)leg*4.0f,h+3.0f,cz,
                           .18f,3.0f,.18f,&world_header,
                           pack_color(1.0f,(color3_t){.24f,.20f,.17f}));
        draw_world_box(cx,h+6.0f,cz,5.0f,.25f,4.0f,&world_header,
                       pack_color(1.0f,(color3_t){.48f,.23f,.10f}));
        draw_world_box(cx,h+8.2f,cz,3.3f,2.0f,3.3f,&world_header,
                       pack_color(1.0f,(color3_t){.17f,.30f,.32f}));
    }
    else if(cell_x==0&&cell_z==2) {
        const float front_z=cz-building->depth*.5f-.8f;
        draw_world_box(cx-12.0f,7.0f,front_z,.28f,6.8f,.28f,&world_header,
                       pack_color(1.0f,(color3_t){.20f,.72f,.98f}));
        draw_world_box(cx+12.0f,7.0f,front_z,.28f,6.8f,.28f,&world_header,
                       pack_color(1.0f,(color3_t){1.0f,.12f,.68f}));
        draw_world_quad(&texture_headers[DLA_TEX_DISTRICT_NEON],
            (vec3_t){cx-12.0f,13.8f,front_z-.14f},(vec3_t){cx+12.0f,13.8f,front_z-.14f},
            (vec3_t){cx-12.0f,7.0f,front_z-.14f},(vec3_t){cx+12.0f,7.0f,front_z-.14f},
            0,0,1,0,0,1,1,1,pack_color(1.0f,(color3_t){1,1,1}));
    }
}

static void draw_crosswalk(int cell_x, int cell_z) {
    const float road_x=(float)cell_x*CITY_CELL;
    const float road_z=(float)cell_z*CITY_CELL;
    const uint32_t worn=pack_color(1.0f,(color3_t){.88f,.90f,.92f});
    const pvr_poly_hdr_t *marks=&texture_headers[DLA_TEX_ROAD_MARKS];
    draw_world_quad(marks,
        (vec3_t){road_x-7.4f,.030f,road_z+8.8f},
        (vec3_t){road_x+7.4f,.030f,road_z+8.8f},
        (vec3_t){road_x-7.4f,.030f,road_z+14.0f},
        (vec3_t){road_x+7.4f,.030f,road_z+14.0f},
        0,0,.78f,0,0,1,.78f,1,worn);
    draw_world_quad(marks,
        (vec3_t){road_x+7.4f,.031f,road_z-8.8f},
        (vec3_t){road_x-7.4f,.031f,road_z-8.8f},
        (vec3_t){road_x+7.4f,.031f,road_z-14.0f},
        (vec3_t){road_x-7.4f,.031f,road_z-14.0f},
        0,0,.78f,0,0,1,.78f,1,worn);
    draw_world_quad(marks,
        (vec3_t){road_x+8.8f,.032f,road_z+7.4f},
        (vec3_t){road_x+8.8f,.032f,road_z-7.4f},
        (vec3_t){road_x+14.0f,.032f,road_z+7.4f},
        (vec3_t){road_x+14.0f,.032f,road_z-7.4f},
        0,0,.78f,0,0,1,.78f,1,worn);
    draw_world_quad(marks,
        (vec3_t){road_x-8.8f,.033f,road_z-7.4f},
        (vec3_t){road_x-8.8f,.033f,road_z+7.4f},
        (vec3_t){road_x-14.0f,.033f,road_z-7.4f},
        (vec3_t){road_x-14.0f,.033f,road_z+7.4f},
        0,0,.78f,0,0,1,.78f,1,worn);
}

static void draw_road_microdetail(int center_x, int center_z,
                                  int road_cell_x, int road_cell_z) {
    const float road_x=(float)road_cell_x*CITY_CELL;
    const float road_z=(float)road_cell_z*CITY_CELL;
    const uint32_t neutral=pack_color(1.0f,(color3_t){.60f,.64f,.70f});
    int offset;
    for(offset=-2;offset<=2;++offset) {
        const float along=((float)(center_z+offset)+.5f)*CITY_CELL;
        const float across=((float)(center_x+offset)+.5f)*CITY_CELL;
        /* Seed each patch from its world road/segment, never the moving
           window offset: crossing an intersection must not move old patches. */
        const float vertical_jitter=(float)((int)(hash_u32((uint32_t)road_cell_x*0x9e37u^
                                            (uint32_t)(center_z+offset)*0x85ebu)&7u)-3);
        const float horizontal_jitter=(float)((int)(hash_u32((uint32_t)(center_x+offset)*0x9e37u^
                                            (uint32_t)road_cell_z*0x85ebu)&7u)-3);
        const pvr_poly_hdr_t *vertical=&texture_headers[((center_z+offset)&1) ?
            DLA_TEX_STREET_REPAIR : DLA_TEX_STREET_UTILITY];
        const pvr_poly_hdr_t *horizontal=&texture_headers[((center_x+offset)&1) ?
            DLA_TEX_STREET_UTILITY : DLA_TEX_STREET_REPAIR];
        draw_world_quad(vertical,
            (vec3_t){road_x+9.8f,.034f,along+1.8f+vertical_jitter},
            (vec3_t){road_x+6.2f,.034f,along+1.8f+vertical_jitter},
            (vec3_t){road_x+9.8f,.034f,along-1.8f+vertical_jitter},
            (vec3_t){road_x+6.2f,.034f,along-1.8f+vertical_jitter},
            0,0,1,0,0,1,1,1,neutral);
        draw_world_quad(horizontal,
            (vec3_t){across-1.8f+horizontal_jitter,.035f,road_z-6.2f},
            (vec3_t){across+1.8f+horizontal_jitter,.035f,road_z-6.2f},
            (vec3_t){across-1.8f+horizontal_jitter,.035f,road_z-9.8f},
            (vec3_t){across+1.8f+horizontal_jitter,.035f,road_z-9.8f},
            0,0,1,0,0,1,1,1,neutral);
        draw_world_quad(&texture_headers[DLA_TEX_STREET_REPAIR],
            (vec3_t){road_x-5.0f,.036f,along+6.0f-vertical_jitter*.4f},
            (vec3_t){road_x-9.2f,.036f,along+5.2f-vertical_jitter*.4f},
            (vec3_t){road_x-4.5f,.036f,along+2.1f-vertical_jitter*.4f},
            (vec3_t){road_x-8.7f,.036f,along+1.3f-vertical_jitter*.4f},
            0,0,1,0,0,1,1,1,neutral);
        draw_world_quad(&texture_headers[DLA_TEX_STREET_UTILITY],
            (vec3_t){across-6.0f+horizontal_jitter*.4f,.037f,road_z+9.2f},
            (vec3_t){across-5.2f+horizontal_jitter*.4f,.037f,road_z+5.0f},
            (vec3_t){across-2.1f+horizontal_jitter*.4f,.037f,road_z+8.7f},
            (vec3_t){across-1.3f+horizontal_jitter*.4f,.037f,road_z+4.5f},
            0,0,1,0,0,1,1,1,neutral);
    }
}

static void draw_coast_boulevard(int center_z) {
    const uint32_t rail=pack_color(1.0f,(color3_t){.82f,.90f,.88f});
    int z,p;
    for(z=center_z-GROUND_RADIUS;z<=center_z+GROUND_RADIUS;++z) {
        const float z0=(float)z*CITY_CELL;
        const float z1=z0+CITY_CELL;
        draw_world_quad(&texture_headers[DLA_TEX_ASPHALT],
            (vec3_t){-255.0f,.012f,z1},(vec3_t){-225.0f,.012f,z1},
            (vec3_t){-255.0f,.012f,z0},(vec3_t){-225.0f,.012f,z0},
            0,0,3,0,0,12,3,12,pack_color(1.0f,(color3_t){.78f,.88f,.87f}));
        draw_world_quad(&texture_headers[DLA_TEX_STUCCO],
            (vec3_t){-265.0f,.045f,z1},(vec3_t){-255.0f,.045f,z1},
            (vec3_t){-265.0f,.045f,z0},(vec3_t){-255.0f,.045f,z0},
            0,0,1,0,0,3,1,3,pack_color(1.0f,(color3_t){.70f,.62f,.48f}));
        draw_world_box(-264.4f,1.05f,(z0+z1)*.5f,.07f,.07f,CITY_CELL*.5f,
                       &world_header,rail);
        for(p=0;p<4;++p)
            draw_world_box(-264.4f,.62f,z0+8.0f+(float)p*32.0f,
                           .08f,.60f,.08f,&world_header,rail);
    }
}

static void draw_parked_cars(int center_x, int center_z) {
    int x,z,index;
    for(z=center_z-2;z<=center_z+2;++z) {
        for(x=center_x-2;x<=center_x+2;++x) {
            for(index=0;index<2;++index) {
                traffic_t parked;
                const float reach=index==0?132.0f:96.0f;
                float dx,dz;
                if(!parked_car_for_cell(x,z,index,&parked)) continue;
                dx=parked.x-car.x;
                dz=parked.z-car.z;
                if(dx*dx+dz*dz<reach*reach)
                    draw_traffic_car(&parked);
            }
        }
    }
}

static void draw_road_gantry(float x, float z, bool road_runs_z,
                             district_t district, uint32_t seed) {
    const float dx=x-car.x,dz=z-car.z;
    const color3_t accent=district_color(district);
    const color3_t board_color=district==DISTRICT_DOWNTOWN ?
        (color3_t){.035f,.20f,.16f} :
        (district==DISTRICT_COAST ? (color3_t){.04f,.28f,.32f} :
        (district==DISTRICT_ARTS ? (color3_t){.34f,.15f,.045f} :
                                   (color3_t){.16f,.035f,.30f}));
    const uint32_t steel=pack_color(1.0f,(color3_t){.15f,.18f,.22f});
    const uint32_t frame=pack_color(1.0f,color_scale(accent,.72f));
    const uint32_t board=pack_color(1.0f,board_color);
    const uint32_t glyph=pack_color(1.0f,(color3_t){.82f,.91f,.88f});
    int sign;
    if(dx*dx+dz*dz>230.0f*230.0f ||
       !world_sphere_visible((vec3_t){x,5.0f,z},18.0f,56.0f)) return;

    if(road_runs_z) {
        draw_world_box(x-12.25f,3.35f,z,.13f,3.35f,.13f,&world_header,steel);
        draw_world_box(x+12.25f,3.35f,z,.13f,3.35f,.13f,&world_header,steel);
        draw_world_box(x,6.68f,z,12.38f,.13f,.13f,&world_header,steel);
        for(sign=0;sign<2;++sign) {
            const float sx=x+(sign==0?-4.0f:4.1f);
            const float half=(sign==0?3.1f:2.65f);
            draw_world_box(sx,6.03f,z,half+.12f,.75f,.095f,&world_header,frame);
            draw_world_box(sx,6.03f,z+(camera_z<z?-.11f:.11f),half,.63f,.045f,
                           &world_header,board);
            draw_world_triangle(&world_header,
                (vec3_t){sx,5.55f,z+(camera_z<z?-.17f:.17f)},
                (vec3_t){sx-.38f,6.14f,z+(camera_z<z?-.17f:.17f)},
                (vec3_t){sx+.38f,6.14f,z+(camera_z<z?-.17f:.17f)},glyph);
        }
    }
    else {
        draw_world_box(x,3.35f,z-12.25f,.13f,3.35f,.13f,&world_header,steel);
        draw_world_box(x,3.35f,z+12.25f,.13f,3.35f,.13f,&world_header,steel);
        draw_world_box(x,6.68f,z,.13f,.13f,12.38f,&world_header,steel);
        for(sign=0;sign<2;++sign) {
            const float sz=z+(sign==0?-4.0f:4.1f);
            const float half=(sign==0?3.1f:2.65f);
            draw_world_box(x,6.03f,sz,.095f,.75f,half+.12f,&world_header,frame);
            draw_world_box(x+(camera_x<x?-.11f:.11f),6.03f,sz,.045f,.63f,half,
                           &world_header,board);
            draw_world_triangle(&world_header,
                (vec3_t){x+(camera_x<x?-.17f:.17f),5.55f,sz},
                (vec3_t){x+(camera_x<x?-.17f:.17f),6.14f,sz-.38f},
                (vec3_t){x+(camera_x<x?-.17f:.17f),6.14f,sz+.38f},glyph);
        }
    }
    /* One tiny color tab makes repeated structures district-specific even
       before the larger sign face becomes readable in the distance. */
    if(seed&1u)
        draw_world_box(x,6.92f,z,road_runs_z?.72f:.10f,.08f,
                       road_runs_z?.10f:.72f,&world_header,frame);
}

static void draw_lane_reflectors(float x, float z, bool road_runs_z,
                                 district_t district) {
    const uint32_t warm=pack_color(1.0f,(color3_t){1.0f,.68f,.14f});
    const uint32_t cool=pack_color(1.0f,color_scale(district_color(district),.90f));
    int marker;
    for(marker=-5;marker<=5;++marker) {
        const float along=(float)marker*8.0f;
        const uint32_t color=(marker&1)?warm:cool;
        if(road_runs_z) {
            draw_world_quad(&world_header,
                (vec3_t){x-.945f,.043f,z+along+.28f},
                (vec3_t){x-.815f,.043f,z+along+.28f},
                (vec3_t){x-.945f,.043f,z+along-.28f},
                (vec3_t){x-.815f,.043f,z+along-.28f},0,0,1,0,0,1,1,1,color);
            draw_world_quad(&world_header,
                (vec3_t){x+.815f,.043f,z+along+.28f},
                (vec3_t){x+.945f,.043f,z+along+.28f},
                (vec3_t){x+.815f,.043f,z+along-.28f},
                (vec3_t){x+.945f,.043f,z+along-.28f},0,0,1,0,0,1,1,1,color);
        }
        else {
            draw_world_quad(&world_header,
                (vec3_t){x+along-.28f,.043f,z-.815f},
                (vec3_t){x+along+.28f,.043f,z-.815f},
                (vec3_t){x+along-.28f,.043f,z-.945f},
                (vec3_t){x+along+.28f,.043f,z-.945f},0,0,1,0,0,1,1,1,color);
            draw_world_quad(&world_header,
                (vec3_t){x+along-.28f,.043f,z+.945f},
                (vec3_t){x+along+.28f,.043f,z+.945f},
                (vec3_t){x+along-.28f,.043f,z+.815f},
                (vec3_t){x+along+.28f,.043f,z+.815f},0,0,1,0,0,1,1,1,color);
        }
    }
}

static void draw_district_road_rhythm(float x, float z, bool road_runs_z,
                                      district_t district) {
    int band;
    if(district==DISTRICT_ARTS) {
        const uint32_t seam=pack_color(1.0f,(color3_t){.12f,.095f,.09f});
        const uint32_t hazard=pack_color(1.0f,(color3_t){.92f,.34f,.055f});
        for(band=-2;band<=2;++band) {
            const float offset=(float)band*15.0f;
            if(road_runs_z) {
                draw_world_quad(&world_header,
                    (vec3_t){x-11.7f,.041f,z+offset+.055f},
                    (vec3_t){x+11.7f,.041f,z+offset+.055f},
                    (vec3_t){x-11.7f,.041f,z+offset-.055f},
                    (vec3_t){x+11.7f,.041f,z+offset-.055f},0,0,1,0,0,1,1,1,seam);
                draw_world_quad(&world_header,
                    (vec3_t){x+9.8f,.043f,z+offset+.42f},
                    (vec3_t){x+11.2f,.043f,z+offset+.42f},
                    (vec3_t){x+9.8f,.043f,z+offset-.42f},
                    (vec3_t){x+11.2f,.043f,z+offset-.42f},0,0,1,0,0,1,1,1,hazard);
            }
            else {
                draw_world_quad(&world_header,
                    (vec3_t){x+offset-.055f,.041f,z+11.7f},
                    (vec3_t){x+offset+.055f,.041f,z+11.7f},
                    (vec3_t){x+offset-.055f,.041f,z-11.7f},
                    (vec3_t){x+offset+.055f,.041f,z-11.7f},0,0,1,0,0,1,1,1,seam);
                draw_world_quad(&world_header,
                    (vec3_t){x+offset-.42f,.043f,z-9.8f},
                    (vec3_t){x+offset+.42f,.043f,z-9.8f},
                    (vec3_t){x+offset-.42f,.043f,z-11.2f},
                    (vec3_t){x+offset+.42f,.043f,z-11.2f},0,0,1,0,0,1,1,1,hazard);
            }
        }
    }
    else if(district==DISTRICT_NEON) {
        const uint32_t cyan=pack_color(1.0f,(color3_t){.04f,.74f,.94f});
        const uint32_t pink=pack_color(1.0f,(color3_t){.92f,.035f,.52f});
        const uint32_t seam=pack_color(1.0f,(color3_t){.14f,.085f,.17f});
        int side;
        for(side=-1;side<=1;side+=2) {
            const uint32_t color=side<0?cyan:pink;
            if(road_runs_z)
                draw_world_quad(&world_header,
                    (vec3_t){x+(float)side*11.42f,.043f,z+42.0f},
                    (vec3_t){x+(float)side*11.58f,.043f,z+42.0f},
                    (vec3_t){x+(float)side*11.42f,.043f,z-42.0f},
                    (vec3_t){x+(float)side*11.58f,.043f,z-42.0f},
                    0,0,1,0,0,1,1,1,color);
            else
                draw_world_quad(&world_header,
                    (vec3_t){x-42.0f,.043f,z+(float)side*11.58f},
                    (vec3_t){x+42.0f,.043f,z+(float)side*11.58f},
                    (vec3_t){x-42.0f,.043f,z+(float)side*11.42f},
                    (vec3_t){x+42.0f,.043f,z+(float)side*11.42f},
                    0,0,1,0,0,1,1,1,color);
        }
        /* Subtle transverse pavement joints carry motion through the lower
           frame; paired inlaid reflectors echo the district palette without
           making the whole boulevard look like a science-fiction grid. */
        for(side=-2;side<=2;++side) {
            const float offset=(float)side*15.5f;
            const uint32_t inset=(side&1)?cyan:pink;
            if(road_runs_z) {
                draw_world_quad(&world_header,
                    (vec3_t){x-11.7f,.042f,z+offset+.052f},
                    (vec3_t){x+11.7f,.042f,z+offset+.052f},
                    (vec3_t){x-11.7f,.042f,z+offset-.052f},
                    (vec3_t){x+11.7f,.042f,z+offset-.052f},
                    0,0,1,0,0,1,1,1,seam);
                draw_world_quad(&world_header,
                    (vec3_t){x-7.45f,.045f,z+offset+.18f},
                    (vec3_t){x-6.10f,.045f,z+offset+.18f},
                    (vec3_t){x-7.45f,.045f,z+offset-.18f},
                    (vec3_t){x-6.10f,.045f,z+offset-.18f},
                    0,0,1,0,0,1,1,1,inset);
            }
            else {
                draw_world_quad(&world_header,
                    (vec3_t){x+offset-.052f,.042f,z+11.7f},
                    (vec3_t){x+offset+.052f,.042f,z+11.7f},
                    (vec3_t){x+offset-.052f,.042f,z-11.7f},
                    (vec3_t){x+offset+.052f,.042f,z-11.7f},
                    0,0,1,0,0,1,1,1,seam);
                draw_world_quad(&world_header,
                    (vec3_t){x+offset-.18f,.045f,z+7.45f},
                    (vec3_t){x+offset+.18f,.045f,z+7.45f},
                    (vec3_t){x+offset-.18f,.045f,z+6.10f},
                    (vec3_t){x+offset+.18f,.045f,z+6.10f},
                    0,0,1,0,0,1,1,1,inset);
            }
        }
    }
}

static void draw_city(void) {
    const int center_x = (int)floorf(car.x / CITY_CELL);
    const int center_z = (int)floorf(car.z / CITY_CELL);
    /* Roads lie on grid boundaries; containing block indices change in the
       middle of a road when the car drifts across its centerline. */
    const int road_cell_x=dla_nearest_road_cell(car.x,CITY_CELL);
    const int road_cell_z=dla_nearest_road_cell(car.z,CITY_CELL);
    int x, z;
    ped_shadow_count=0;
    /* Asphalt is the shared base; raised blocks leave wide road corridors. */
    for(z = center_z - GROUND_RADIUS; z <= center_z + GROUND_RADIUS; ++z) {
        for(x = center_x - GROUND_RADIUS; x <= center_x + GROUND_RADIUS; ++x) {
            const float x0 = (float)x * CITY_CELL;
            const float z0 = (float)z * CITY_CELL;
            const district_t district=district_for_cell(x,z);
            const color3_t pavement=district==DISTRICT_COAST ?
                (color3_t){.82f,.91f,.90f} :
                (district==DISTRICT_ARTS ? (color3_t){.90f,.82f,.76f} :
                (district==DISTRICT_NEON ? (color3_t){.80f,.74f,.91f} :
                                           (color3_t){.83f,.87f,.95f}));
            if(district==DISTRICT_COAST&&x<=-3)
                draw_world_quad(&world_header,
                    (vec3_t){x0,0,z0+CITY_CELL},(vec3_t){x0+CITY_CELL,0,z0+CITY_CELL},
                    (vec3_t){x0,0,z0},(vec3_t){x0+CITY_CELL,0,z0},
                    0,0,1,0,0,1,1,1,
                    pack_color(1.0f,(color3_t){.025f,.20f,.30f}));
            else
                draw_world_quad(&texture_headers[DLA_TEX_ASPHALT],
                    (vec3_t){x0,0,z0+CITY_CELL},(vec3_t){x0+CITY_CELL,0,z0+CITY_CELL},
                    (vec3_t){x0,0,z0},(vec3_t){x0+CITY_CELL,0,z0},
                    0,0,14,0,0,14,14,14,pack_color(1.0f,pavement));
        }
    }
    for(z = center_z - DRAW_RADIUS; z <= center_z + DRAW_RADIUS; ++z) {
        for(x = center_x - DRAW_RADIUS; x <= center_x + DRAW_RADIUS; ++x) {
            const float x0 = (float)x * CITY_CELL + ROAD_HALF + 1.5f;
            const float x1 = (float)(x+1) * CITY_CELL - ROAD_HALF - 1.5f;
            const float z0 = (float)z * CITY_CELL + ROAD_HALF + 1.5f;
            const float z1 = (float)(z+1) * CITY_CELL - ROAD_HALF - 1.5f;
            const building_t building = building_for_cell(x,z);
            const color3_t pad=building.district==DISTRICT_COAST ?
                (color3_t){.84f,.90f,.86f} :
                (building.district==DISTRICT_ARTS ? (color3_t){.68f,.58f,.52f} :
                (building.district==DISTRICT_NEON ? (color3_t){.60f,.48f,.72f} :
                                                    (color3_t){.70f,.72f,.80f}));
            const int pad_texture=(building.district==DISTRICT_DOWNTOWN ||
                                   building.district==DISTRICT_NEON) ?
                                  DLA_TEX_PAVERS : DLA_TEX_SIDEWALK;
            /* This gate covers the entire block, including street furniture
               and the Downtown landmark's 25.2 m rooftop spire. */
            const float block_top=building.height+26.0f;
            if(building.district==DISTRICT_COAST&&x<=-3) continue;
            draw_world_quad(&texture_headers[pad_texture],
                (vec3_t){x0,.06f,z1},(vec3_t){x1,.06f,z1},
                (vec3_t){x0,.06f,z0},(vec3_t){x1,.06f,z0},
                0,0,10,0,0,10,10,10,
                pack_color(1.0f,pad));
            if(!dla_frustum_aabb_visible(&scene_frustum,
                building.cx,block_top*.5f,building.cz,
                CITY_CELL*.5f,block_top*.5f,CITY_CELL*.5f))
                continue;
            if(building.exists) {
                draw_building(&building);
                draw_district_landmark(x,z,&building);
            }
            if(building.district==DISTRICT_COAST || (building.seed%5u)==0u)
                draw_palm(x0 + 4.0f, z0 + 4.0f, 0.85f + (building.seed & 3u)*0.07f);
            if((building.district==DISTRICT_COAST && (building.seed&1u)) ||
               (building.seed%7u)==0u)
                draw_palm(x1 - 4.0f, z1 - 4.0f, 0.78f + (building.seed & 1u)*0.1f);
            draw_block_street_detail(x,z,x0,x1,z0,z1,&building);
        }
    }
    if(center_x<=-1) draw_coast_boulevard(center_z);
    /* Long center stripes and pale edge lines make the 30 m roads readable. */
    for(x = center_x - DRAW_RADIUS; x <= center_x + DRAW_RADIUS + 1; ++x) {
        const float road_x = (float)x * CITY_CELL;
        for(z = center_z - DRAW_RADIUS; z <= center_z + DRAW_RADIUS; ++z) {
            const float center = ((float)z + 0.5f) * CITY_CELL;
            int side;
            for(side = -1; side <= 1; side += 2) {
                draw_world_quad(&texture_headers[DLA_TEX_ROAD_MARKS],
                    (vec3_t){road_x+side*.75f,.025f,center+42},
                    (vec3_t){road_x+side*1.00f,.025f,center+42},
                    (vec3_t){road_x+side*.75f,.025f,center-42},
                    (vec3_t){road_x+side*1.00f,.025f,center-42},
                    .84f,0,1,0,.84f,1,1,1,
                    pack_color(1.0f,(color3_t){1.0f,.90f,.66f}));
                if(x==road_cell_x && abs(z-center_z)<=1) {
                    int dash;
                    draw_world_quad(&world_header,
                        (vec3_t){road_x+side*13.15f,.027f,center+42},
                        (vec3_t){road_x+side*13.38f,.027f,center+42},
                        (vec3_t){road_x+side*13.15f,.027f,center-42},
                        (vec3_t){road_x+side*13.38f,.027f,center-42},
                        0,0,1,0,0,1,1,1,
                        pack_color(1.0f,(color3_t){.66f,.70f,.72f}));
                    for(dash=-1;dash<=1;++dash) {
                        const float mark_z=center+(float)dash*25.0f;
                        draw_world_quad(&world_header,
                            (vec3_t){road_x+side*6.62f,.029f,mark_z+5.0f},
                            (vec3_t){road_x+side*6.88f,.029f,mark_z+5.0f},
                            (vec3_t){road_x+side*6.62f,.029f,mark_z-5.0f},
                            (vec3_t){road_x+side*6.88f,.029f,mark_z-5.0f},
                            0,0,1,0,0,1,1,1,
                            pack_color(1.0f,(color3_t){.72f,.75f,.78f}));
                    }
                }
            }
        }
    }
    for(z = center_z - DRAW_RADIUS; z <= center_z + DRAW_RADIUS + 1; ++z) {
        const float road_z = (float)z * CITY_CELL;
        for(x = center_x - DRAW_RADIUS; x <= center_x + DRAW_RADIUS; ++x) {
            const float center = ((float)x + 0.5f) * CITY_CELL;
            int side;
            for(side = -1; side <= 1; side += 2) {
                draw_world_quad(&texture_headers[DLA_TEX_ROAD_MARKS],
                    (vec3_t){center-42,.026f,road_z+side*1.00f},
                    (vec3_t){center+42,.026f,road_z+side*1.00f},
                    (vec3_t){center-42,.026f,road_z+side*.75f},
                    (vec3_t){center+42,.026f,road_z+side*.75f},
                    .84f,0,1,0,.84f,1,1,1,
                    pack_color(1.0f,(color3_t){1.0f,.90f,.66f}));
                if(z==road_cell_z && abs(x-center_x)<=1) {
                    int dash;
                    draw_world_quad(&world_header,
                        (vec3_t){center-42,.028f,road_z+side*13.38f},
                        (vec3_t){center+42,.028f,road_z+side*13.38f},
                        (vec3_t){center-42,.028f,road_z+side*13.15f},
                        (vec3_t){center+42,.028f,road_z+side*13.15f},
                        0,0,1,0,0,1,1,1,
                        pack_color(1.0f,(color3_t){.66f,.70f,.72f}));
                    for(dash=-1;dash<=1;++dash) {
                        const float mark_x=center+(float)dash*25.0f;
                        draw_world_quad(&world_header,
                            (vec3_t){mark_x-5.0f,.030f,road_z+side*6.88f},
                            (vec3_t){mark_x+5.0f,.030f,road_z+side*6.88f},
                            (vec3_t){mark_x-5.0f,.030f,road_z+side*6.62f},
                            (vec3_t){mark_x+5.0f,.030f,road_z+side*6.62f},
                            0,0,1,0,0,1,1,1,
                            pack_color(1.0f,(color3_t){.72f,.75f,.78f}));
                    }
                }
            }
        }
    }
    /* Direction arrows at the player's nearest cross streets add lane-scale
       detail without laying another full-road texture over the asphalt. */
    for(x=-1;x<=1;++x) {
        const float segment_z=((float)(center_z+x)+.5f)*CITY_CELL;
        const float segment_x=((float)(center_x+x)+.5f)*CITY_CELL;
        const float road_x=(float)road_cell_x*CITY_CELL;
        const float road_z=(float)road_cell_z*CITY_CELL;
        draw_direction_arrow(road_x-6.75f,segment_z-25.0f,false,1.0f);
        draw_direction_arrow(road_x+6.75f,segment_z+25.0f,false,-1.0f);
        draw_direction_arrow(segment_x-25.0f,road_z+6.75f,true,1.0f);
        draw_direction_arrow(segment_x+25.0f,road_z-6.75f,true,-1.0f);
    }
    /* Repeating depth gates and raised lane reflectors give long boulevards a
       readable cadence at speed.  Alternating cells keep intersections open
       while ensuring every district has a recognizable piece of road kit. */
    for(x=-1;x<=1;++x) {
        const int vertical_cell=center_z+x;
        const int horizontal_cell=center_x+x;
        const float vertical_z=((float)vertical_cell+.5f)*CITY_CELL;
        const float horizontal_x=((float)horizontal_cell+.5f)*CITY_CELL;
        const float vertical_x=(float)road_cell_x*CITY_CELL;
        const float horizontal_z=(float)road_cell_z*CITY_CELL;
        const district_t vertical_district=district_for_position(vertical_x,vertical_z);
        const district_t horizontal_district=district_for_position(horizontal_x,horizontal_z);
        draw_lane_reflectors(vertical_x,vertical_z,true,vertical_district);
        draw_lane_reflectors(horizontal_x,horizontal_z,false,horizontal_district);
        draw_district_road_rhythm(vertical_x,vertical_z,true,vertical_district);
        draw_district_road_rhythm(horizontal_x,horizontal_z,false,horizontal_district);
        if((vertical_cell&1)==0)
            draw_road_gantry(vertical_x,vertical_z,true,vertical_district,
                hash_u32((uint32_t)vertical_cell^0x47414e54u));
        if((horizontal_cell&1)==0)
            draw_road_gantry(horizontal_x,horizontal_z,false,horizontal_district,
                hash_u32((uint32_t)horizontal_cell^0x5349474eu));
    }
    draw_road_microdetail(center_x,center_z,road_cell_x,road_cell_z);
    /* Animated signals mark nearby intersections and make cross streets pulse. */
    for(z=center_z-2;z<=center_z+2;++z) {
        for(x=center_x-2;x<=center_x+2;++x) {
            const float sx=(float)x*CITY_CELL+14.3f-car.x;
            const float sz=(float)z*CITY_CELL+14.3f-car.z;
            if(sx*sx+sz*sz<225.0f*225.0f)
                draw_crosswalk(x,z);
            if(sx*sx+sz*sz<275.0f*275.0f)
                draw_traffic_signal_opaque(x,z);
        }
    }
    draw_parked_cars(center_x,center_z);
}

static void draw_streetlamp_glow(float x, float z, uint32_t seed,
                                 district_t district) {
    const vec3_t view=world_to_camera((vec3_t){x,3.5f,z});
    const float flicker=clampf((245.0f-view.z)/150.0f,0.0f,1.0f);
    (void)seed;
    const color3_t color=district==DISTRICT_COAST ? (color3_t){.78f,.87f,1.0f} : (color3_t){1.0f,.69f,.38f};
    screen_point_t bulb;
    if(view.z<2.25f) return;
    draw_world_disc(&additive_header,(vec3_t){x+.55f,.038f,z},7.4f,6,
                    pack_color(.24f*flicker,color),pack_color(0.0f,color));
    if(project_world((vec3_t){x+.98f,6.78f,z},&bulb)) {
        const float radius=clampf(155.0f*bulb.z,2.5f,18.0f);
        draw_disc(&additive_header,bulb.x,bulb.y,radius,bulb.z+.00002f,6,
                  pack_color(.75f*flicker,color),pack_color(0.0f,color));
    }
}

static void draw_storefront_spill(const building_t *building) {
    const float x0=building->cx-building->width*.5f;
    const float x1=building->cx+building->width*.5f;
    const float z0=building->cz-building->depth*.5f;
    const float z1=building->cz+building->depth*.5f;
    const color3_t color=district_color(building->district);
    if(!building->exists || (building->seed%4u)==0u) return;
    if(building->seed&1u)
        draw_world_quad_colored(&additive_header,
            (vec3_t){x0+4.0f,.041f,z0-.1f},(vec3_t){x1-4.0f,.041f,z0-.1f},
            (vec3_t){x0+8.0f,.041f,z0-6.2f},(vec3_t){x1-8.0f,.041f,z0-6.2f},
            pack_color(.17f,color),pack_color(.17f,color),
            pack_color(0.0f,color),pack_color(0.0f,color));
    else
        draw_world_quad_colored(&additive_header,
            (vec3_t){x0-.1f,.041f,z1-4.0f},(vec3_t){x0-.1f,.041f,z0+4.0f},
            (vec3_t){x0-6.2f,.041f,z1-8.0f},(vec3_t){x0-6.2f,.041f,z0+8.0f},
            pack_color(.17f,color),pack_color(.17f,color),
            pack_color(0.0f,color),pack_color(0.0f,color));
}

static void draw_traffic_signal_glow(int cell_x, int cell_z) {
    const float x=(float)cell_x*CITY_CELL+14.3f;
    const float z=(float)cell_z*CITY_CELL+14.3f;
    const int state=traffic_signal_state(cell_x,cell_z);
    const color3_t colors[3]={{1.0f,.035f,.02f},{1.0f,.52f,.02f},{.03f,1.0f,.20f}};
    screen_point_t point;
    if(project_world((vec3_t){x-4.43f,5.48f-(float)state*.43f,z-.27f},&point)) {
        const float radius=clampf(92.0f*point.z,2.5f,10.0f);
        draw_disc(&additive_header,point.x,point.y,radius,point.z+.00003f,6,
                  pack_color(.86f,colors[state]),pack_color(0.0f,colors[state]));
    }
}

static void draw_district_landmark_lighting(void) {
    static const int cells[4][2]={{0,0},{-2,0},{2,0},{0,2}};
    int i;
    for(i=0;i<4;++i) {
        const building_t building=building_for_cell(cells[i][0],cells[i][1]);
        const color3_t color=district_color(building.district);
        const float dx=building.cx-car.x,dz=building.cz-car.z;
        vec3_t point;
        screen_point_t projected;
        if(dx*dx+dz*dz>330.0f*330.0f) continue;
        if(building.district==DISTRICT_DOWNTOWN)
            point=(vec3_t){building.cx,building.height+18.0f,building.cz};
        else if(building.district==DISTRICT_ARTS)
            point=(vec3_t){building.cx,building.height+8.5f,building.cz};
        else
            point=(vec3_t){building.cx,12.0f,
                building.cz-building.depth*.5f-1.0f};
        draw_world_disc(&additive_header,(vec3_t){point.x,.042f,point.z},
                        9.0f,6,pack_color(.20f,color),pack_color(0.0f,color));
        if(project_world(point,&projected)) {
            const float radius=clampf(260.0f*projected.z,5.0f,24.0f);
            draw_disc(&additive_header,projected.x,projected.y,radius,
                      projected.z+.00004f,8,pack_color(.72f,color),
                      pack_color(0.0f,color));
        }
    }
}

static void draw_coast_water_lighting(int center_z) {
    const color3_t cyan={.10f,.82f,.92f};
    int z,line;
    for(z=center_z-3;z<=center_z+3;++z) {
        const float z0=(float)z*CITY_CELL;
        for(line=0;line<2;++line) {
            const float wave_z=z0+30.0f+(float)line*52.0f+
                               fsin(game.time*.8f+(float)z)*3.0f;
            draw_world_quad_colored(&additive_header,
                (vec3_t){-520.0f,.035f,wave_z+.28f},
                (vec3_t){-272.0f,.035f,wave_z+.28f},
                (vec3_t){-520.0f,.035f,wave_z-.28f},
                (vec3_t){-272.0f,.035f,wave_z-.28f},
                pack_color(0.0f,cyan),pack_color(.22f,cyan),
                pack_color(0.0f,cyan),pack_color(.22f,cyan));
        }
    }
}

static void draw_road_reflections(int center_x, int center_z,
                                   int road_cell_x, int road_cell_z) {
    const color3_t warm={1.0f,.48f,.12f};
    int offset,side;
    for(offset=-2;offset<=2;++offset) {
        const int vertical_segment=center_z+offset;
        const int horizontal_segment=center_x+offset;
        const float along=((float)vertical_segment+.5f)*CITY_CELL;
        const float across=((float)horizontal_segment+.5f)*CITY_CELL;
        const float vertical_x=(float)road_cell_x*CITY_CELL;
        const float horizontal_z=(float)road_cell_z*CITY_CELL;
        const color3_t vertical_accent=district_color(district_for_position(vertical_x,along));
        const color3_t horizontal_accent=district_color(district_for_position(across,horizontal_z));
        for(side=-1;side<=1;side+=2) {
            /* Like the road patches, reflections belong to world segments,
               not to offsets in the player's moving draw window. */
            const color3_t vertical_color=((vertical_segment+side)&1)?vertical_accent:warm;
            const color3_t horizontal_color=((horizontal_segment+side)&1)?horizontal_accent:warm;
            const float vertical_lane=(float)side*(5.0f+(float)((vertical_segment+3)&1)*3.6f);
            const float horizontal_lane=(float)side*(5.0f+(float)((horizontal_segment+3)&1)*3.6f);
            draw_world_quad_colored(&additive_header,
                (vec3_t){vertical_x+vertical_lane-.52f,.040f,along+12.0f},
                (vec3_t){vertical_x+vertical_lane+.52f,.040f,along+12.0f},
                (vec3_t){vertical_x+vertical_lane-.18f,.040f,along-12.0f},
                (vec3_t){vertical_x+vertical_lane+.18f,.040f,along-12.0f},
                pack_color(0.0f,vertical_color),pack_color(0.0f,vertical_color),
                pack_color(.115f,vertical_color),pack_color(.115f,vertical_color));
            draw_world_quad_colored(&additive_header,
                (vec3_t){across-12.0f,.041f,horizontal_z+horizontal_lane-.52f},
                (vec3_t){across+12.0f,.041f,horizontal_z+horizontal_lane-.18f},
                (vec3_t){across-12.0f,.041f,horizontal_z+horizontal_lane+.52f},
                (vec3_t){across+12.0f,.041f,horizontal_z+horizontal_lane+.18f},
                pack_color(0.0f,horizontal_color),pack_color(.10f,horizontal_color),
                pack_color(0.0f,horizontal_color),pack_color(.10f,horizontal_color));
        }
    }
}

static void draw_city_lighting(void) {
    const int center_x=(int)floorf(car.x/CITY_CELL);
    const int center_z=(int)floorf(car.z/CITY_CELL);
    int x,z;
    for(z=center_z-2;z<=center_z+2;++z) {
        for(x=center_x-2;x<=center_x+2;++x) {
            const float x0=(float)x*CITY_CELL+ROAD_HALF+1.5f;
            const float x1=(float)(x+1)*CITY_CELL-ROAD_HALF-1.5f;
            const float z0=(float)z*CITY_CELL+ROAD_HALF+1.5f;
            const float z1=(float)(z+1)*CITY_CELL-ROAD_HALF-1.5f;
            const float cx=(x0+x1)*.5f,cz=(z0+z1)*.5f;
            const float dx=cx-car.x,dz=cz-car.z;
            const building_t building=building_for_cell(x,z);
            if(dx*dx+dz*dz>265.0f*265.0f) continue;
            draw_streetlamp_glow(x0-1.2f,z0+7.0f,building.seed,
                                  building.district);
            draw_streetlamp_glow(x1+1.2f,z1-7.0f,building.seed>>8,
                                  building.district);
            if(dx*dx+dz*dz<155.0f*155.0f) {
                draw_streetlamp_glow(x0+7.0f,z1+1.2f,building.seed>>12,
                                      building.district);
                draw_streetlamp_glow(x1-7.0f,z0-1.2f,building.seed>>18,
                                      building.district);
            }
            if(dx*dx+dz*dz<215.0f*215.0f)
                draw_storefront_spill(&building);
        }
    }
    for(z=center_z-2;z<=center_z+2;++z) {
        for(x=center_x-2;x<=center_x+2;++x) {
            const float dx=(float)x*CITY_CELL+14.3f-car.x;
            const float dz=(float)z*CITY_CELL+14.3f-car.z;
            if(dx*dx+dz*dz<275.0f*275.0f)
                draw_traffic_signal_glow(x,z);
        }
    }
    draw_road_reflections(center_x,center_z,
        dla_nearest_road_cell(car.x,CITY_CELL),dla_nearest_road_cell(car.z,CITY_CELL));
    draw_district_landmark_lighting();
    if(center_x<=-1) draw_coast_water_lighting(center_z);
}

static vec3_t traffic_point(const traffic_t *vehicle,
                            float x, float y, float z) {
    const shz_sincos_t rotation=shz_sincosf(vehicle->yaw);
    const float c=rotation.cos,s=rotation.sin;
    return (vec3_t){vehicle->x + x*c + z*s, y,
                    vehicle->z - x*s + z*c};
}

static void draw_traffic_box(const traffic_t *vehicle,
                             float center_x, float center_y, float center_z,
                             float half_x, float half_y, float half_z,
                             const pvr_poly_hdr_t *header, uint32_t color) {
    const shz_sincos_t rotation=shz_sincosf(vehicle->yaw);
    const float c=rotation.cos,s=rotation.sin;
    const float dx=camera_x-vehicle->x,dz=camera_z-vehicle->z;
    const float local_camera_x=dx*c-dz*s;
    const float local_camera_z=dx*s+dz*c;
    vec3_t p[8]={
        {center_x-half_x,center_y+half_y,center_z+half_z},
        {center_x+half_x,center_y+half_y,center_z+half_z},
        {center_x-half_x,center_y-half_y,center_z+half_z},
        {center_x+half_x,center_y-half_y,center_z+half_z},
        {center_x-half_x,center_y+half_y,center_z-half_z},
        {center_x+half_x,center_y+half_y,center_z-half_z},
        {center_x-half_x,center_y-half_y,center_z-half_z},
        {center_x+half_x,center_y-half_y,center_z-half_z}
    };
    int i;
    for(i=0;i<8;++i) {
        const float x=p[i].x,z=p[i].z;
        p[i].x=vehicle->x+x*c+z*s;
        p[i].z=vehicle->z-x*s+z*c;
    }
    if(local_camera_z>center_z+half_z)
        draw_world_quad(header,p[0],p[1],p[2],p[3],0,0,1,0,0,1,1,1,color);
    if(local_camera_z<center_z-half_z)
        draw_world_quad(header,p[5],p[4],p[7],p[6],0,0,1,0,0,1,1,1,color);
    if(local_camera_x<center_x-half_x)
        draw_world_quad(header,p[4],p[0],p[6],p[2],0,0,1,0,0,1,1,1,color);
    if(local_camera_x>center_x+half_x)
        draw_world_quad(header,p[1],p[5],p[3],p[7],0,0,1,0,0,1,1,1,color);
    if(camera_y>center_y+half_y)
        draw_world_quad(header,p[4],p[5],p[0],p[1],0,0,1,0,0,1,1,1,color);
    if(camera_y<center_y-half_y)
        draw_world_quad(header,p[2],p[3],p[6],p[7],0,0,1,0,0,1,1,1,color);
}

static void draw_traffic_body(const traffic_t *vehicle,
                              uint32_t paint, uint32_t highlight) {
    const vec3_t fl=traffic_point(vehicle,-.78f,.16f,2.16f);
    const vec3_t fr=traffic_point(vehicle, .78f,.16f,2.16f);
    const vec3_t ftl=traffic_point(vehicle,-.88f,.67f,1.91f);
    const vec3_t ftr=traffic_point(vehicle, .88f,.67f,1.91f);
    const vec3_t rl=traffic_point(vehicle,-.80f,.16f,-2.10f);
    const vec3_t rr=traffic_point(vehicle, .80f,.16f,-2.10f);
    const vec3_t rtl=traffic_point(vehicle,-.91f,.68f,-1.96f);
    const vec3_t rtr=traffic_point(vehicle, .91f,.68f,-1.96f);
    draw_world_quad(&world_header,ftl,ftr,fl,fr,0,0,1,0,0,1,1,1,paint);
    draw_world_quad(&world_header,rtr,rtl,rr,rl,0,0,1,0,0,1,1,1,paint);
    draw_world_quad(&world_header,rtl,ftl,rl,fl,0,0,1,0,0,1,1,1,paint);
    draw_world_quad(&world_header,ftr,rtr,fr,rr,0,0,1,0,0,1,1,1,paint);
    draw_world_quad(&world_header,
        traffic_point(vehicle,-.88f,.68f,1.91f),
        traffic_point(vehicle, .88f,.68f,1.91f),
        traffic_point(vehicle,-.78f,.73f,.97f),
        traffic_point(vehicle, .78f,.73f,.97f),
        0,0,1,0,0,1,1,1,highlight);
    draw_world_quad(&world_header,
        traffic_point(vehicle,-.77f,.72f,-1.04f),
        traffic_point(vehicle, .77f,.72f,-1.04f),
        traffic_point(vehicle,-.91f,.69f,-1.96f),
        traffic_point(vehicle, .91f,.69f,-1.96f),
        0,0,1,0,0,1,1,1,highlight);
}

static void draw_traffic_wheel(const traffic_t *vehicle,
                               float local_x, float local_z,
                               bool detailed) {
    const uint32_t tire=pack_color(1.0f,(color3_t){.035f,.038f,.048f});
    const uint32_t rim=pack_color(1.0f,(color3_t){.44f,.48f,.56f});
    const float radius=.37f,center_y=.39f;
    if(detailed) {
        const vec3_t center=traffic_point(vehicle,local_x,center_y,local_z);
        int i;
        for(i=0;i<6;++i) {
            const float a0=(float)i*PI/3.0f;
            const float a1=(float)(i+1)*PI/3.0f;
            const vec3_t p0=traffic_point(vehicle,local_x,
                center_y+fsin(a0)*radius,local_z+fcos(a0)*radius);
            const vec3_t p1=traffic_point(vehicle,local_x,
                center_y+fsin(a1)*radius,local_z+fcos(a1)*radius);
            draw_world_triangle(&world_header,center,p0,p1,tire);
        }
        draw_world_quad(&world_header,
            traffic_point(vehicle,local_x,center_y+radius*.31f,local_z+radius*.31f),
            traffic_point(vehicle,local_x,center_y+radius*.31f,local_z-radius*.31f),
            traffic_point(vehicle,local_x,center_y-radius*.31f,local_z+radius*.31f),
            traffic_point(vehicle,local_x,center_y-radius*.31f,local_z-radius*.31f),
            0,0,1,0,0,1,1,1,rim);
    }
    else {
        draw_world_quad(&world_header,
            traffic_point(vehicle,local_x,center_y+radius,local_z+radius),
            traffic_point(vehicle,local_x,center_y+radius,local_z-radius),
            traffic_point(vehicle,local_x,center_y-radius,local_z+radius),
            traffic_point(vehicle,local_x,center_y-radius,local_z-radius),
            0,0,1,0,0,1,1,1,tire);
    }
}

static void draw_traffic_car(const traffic_t *vehicle) {
    const float sun=.5f+.5f*fcos(vehicle->yaw+.7f);
    const color3_t body={vehicle->color.r*(.48f+.14f*sun),
                         vehicle->color.g*(.52f+.12f*sun),
                         vehicle->color.b*(.64f+.08f*sun)};
    const uint32_t paint=pack_color(1.0f,body);
    const uint32_t paint_top=pack_color(1.0f,color_scale(body,1.23f));
    const uint32_t glass=pack_color(1.0f,(color3_t){.55f,.64f,.78f});
    const uint32_t carbon=pack_color(1.0f,(color3_t){.075f,.085f,.115f});
    const uint32_t metal=pack_color(1.0f,(color3_t){.36f,.40f,.48f});
    const float brake=clampf(vehicle->brake_light,0.0f,1.0f);
    const uint32_t tail=pack_color(1.0f,(color3_t){.58f+.42f*brake,
        .24f+.68f*brake,.20f+.58f*brake});
    const float dx=vehicle->x-car.x,dz=vehicle->z-car.z;
    const float distance_sq=dx*dx+dz*dz;
    const bool detailed=distance_sq<48.0f*48.0f;
    const bool medium=distance_sq<105.0f*105.0f;
    const int variant=(int)((vehicle->seed>>9)%5u);
    int side, axle;

    QA_COUNT(vehicles);
    draw_traffic_body(vehicle,paint,paint_top);

    /* Compact coupe/sedan greenhouse: two sloped screens and tapering sides. */
    draw_world_quad(&texture_headers[DLA_TEX_CAR_GLASS],
        traffic_point(vehicle,-0.78f,.72f,.98f),traffic_point(vehicle,.78f,.72f,.98f),
        traffic_point(vehicle,-0.57f,1.27f,.36f),traffic_point(vehicle,.57f,1.27f,.36f),
        0,1,1,1,0,0,1,0,glass);
    draw_world_quad(&texture_headers[DLA_TEX_CAR_GLASS],
        traffic_point(vehicle,-0.57f,1.27f,-.63f),traffic_point(vehicle,.57f,1.27f,-.63f),
        traffic_point(vehicle,-0.77f,.72f,-1.05f),traffic_point(vehicle,.77f,.72f,-1.05f),
        0,0,1,0,0,1,1,1,glass);
    draw_world_quad(&texture_headers[DLA_TEX_CAR_GLASS],
        traffic_point(vehicle,-0.57f,1.27f,.36f),traffic_point(vehicle,-0.57f,1.27f,-.63f),
        traffic_point(vehicle,-0.78f,.72f,.98f),traffic_point(vehicle,-0.77f,.72f,-1.05f),
        0,0,1,0,0,1,1,1,glass);
    draw_world_quad(&texture_headers[DLA_TEX_CAR_GLASS],
        traffic_point(vehicle,.57f,1.27f,-.63f),traffic_point(vehicle,.57f,1.27f,.36f),
        traffic_point(vehicle,.77f,.72f,-1.05f),traffic_point(vehicle,.78f,.72f,.98f),
        0,0,1,0,0,1,1,1,glass);
    draw_world_quad(&world_header,
        traffic_point(vehicle,-.57f,1.28f,-.63f),traffic_point(vehicle,.57f,1.28f,-.63f),
        traffic_point(vehicle,-.57f,1.28f,.36f),traffic_point(vehicle,.57f,1.28f,.36f),
        0,0,1,0,0,1,1,1,paint_top);

    if(medium && variant==1) {
        /* Touring wagon: longer roof, rear quarter glass and roof rails. */
        draw_traffic_box(vehicle,0.0f,1.22f,-.76f,.60f,.08f,.62f,
                         &world_header,paint_top);
        draw_traffic_box(vehicle,-.50f,1.37f,-.36f,.035f,.035f,.88f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,.50f,1.37f,-.36f,.035f,.035f,.88f,
                         &world_header,carbon);
    }
    else if(medium && variant==2) {
        /* Boxier crossover profile with a high rear cabin and bright rails. */
        draw_traffic_box(vehicle,0.0f,1.28f,-.68f,.64f,.18f,.70f,
                         &world_header,paint_top);
        draw_traffic_box(vehicle,0.0f,1.30f,-1.39f,.58f,.14f,.055f,
                         &texture_headers[DLA_TEX_CAR_GLASS],glass);
        draw_traffic_box(vehicle,-.51f,1.50f,-.40f,.035f,.035f,.90f,
                         &world_header,metal);
        draw_traffic_box(vehicle,.51f,1.50f,-.40f,.035f,.035f,.90f,
                         &world_header,metal);
    }
    else if(medium && variant==3) {
        /* Street pickup/ute silhouette: dark bed, cover and roll hoop. */
        draw_traffic_box(vehicle,0.0f,.76f,-1.25f,.72f,.08f,.72f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,-.58f,1.04f,-.72f,.045f,.30f,.045f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,.58f,1.04f,-.72f,.045f,.30f,.045f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,0.0f,1.30f,-.72f,.60f,.045f,.045f,
                         &world_header,carbon);
    }
    else if(medium && variant==4) {
        /* Compact delivery van: a tall cargo volume, split rear doors and
           colored waist stripe provide a genuinely different street shape. */
        const uint32_t cargo=pack_color(1.0f,color_scale(vehicle->color,.88f));
        draw_traffic_box(vehicle,0.0f,1.24f,-.72f,.78f,.57f,1.13f,
                         &world_header,cargo);
        draw_traffic_box(vehicle,0.0f,1.27f,-1.875f,.035f,.49f,.025f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,0.0f,.78f,-.78f,.805f,.075f,1.04f,
                         &world_header,paint_top);
        draw_traffic_box(vehicle,-.80f,1.31f,-.55f,.028f,.40f,.78f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,.80f,1.31f,-.55f,.028f,.40f,.78f,
                         &world_header,carbon);
    }

    for(side=-1;side<=1;side+=2) {
        for(axle=-1;axle<=1;axle+=2) {
            const float x=(float)side*.965f;
            const float z=(float)axle*1.30f;
            draw_traffic_wheel(vehicle,x,z,detailed);
        }
    }
    draw_traffic_box(vehicle,0.0f,.25f,2.08f,.82f,.10f,.08f,
                     &world_header,carbon);
    draw_traffic_box(vehicle,0.0f,.25f,-2.08f,.82f,.10f,.08f,
                     &world_header,carbon);
    if((vehicle->seed%6u)==0u) {
        draw_traffic_box(vehicle,0.0f,1.38f,-.02f,.34f,.10f,.24f,
                         &world_header,
                         pack_color(1.0f,(color3_t){1.0f,.72f,.14f}));
    }
    if(detailed) {
        const uint32_t mirror=pack_color(1.0f,color_scale(vehicle->color,.58f));
        const uint32_t plate=pack_color(1.0f,(color3_t){.76f,.78f,.68f});
        draw_traffic_box(vehicle,-1.04f,.91f,.37f,.14f,.09f,.18f,
                         &world_header,mirror);
        draw_traffic_box(vehicle,1.04f,.91f,.37f,.14f,.09f,.18f,
                         &world_header,mirror);
        draw_traffic_box(vehicle,0.0f,.31f,2.17f,.32f,.11f,.035f,
                         &world_header,plate);
        draw_traffic_box(vehicle,0.0f,.31f,-2.17f,.32f,.11f,.035f,
                         &world_header,plate);
        draw_traffic_box(vehicle,-.925f,.49f,.02f,.026f,.035f,.82f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,.925f,.49f,.02f,.026f,.035f,.82f,
                         &world_header,carbon);
        draw_traffic_box(vehicle,-.944f,.72f,-.35f,.026f,.035f,.16f,
                         &world_header,metal);
        draw_traffic_box(vehicle,.944f,.72f,-.35f,.026f,.035f,.16f,
                         &world_header,metal);
        if((vehicle->seed&3u)==1u) {
            draw_traffic_box(vehicle,-.47f,1.39f,-.10f,.035f,.035f,.66f,
                             &world_header,carbon);
            draw_traffic_box(vehicle,.47f,1.39f,-.10f,.035f,.035f,.66f,
                             &world_header,carbon);
        }
        if((vehicle->seed&7u)==2u) {
            draw_traffic_box(vehicle,0.0f,.92f,-1.86f,.76f,.055f,.20f,
                             &world_header,carbon);
            draw_traffic_box(vehicle,-.64f,.77f,-1.78f,.045f,.18f,.045f,
                             &world_header,carbon);
            draw_traffic_box(vehicle,.64f,.77f,-1.78f,.045f,.18f,.045f,
                             &world_header,carbon);
        }
    }

    /* Two white forward lamps and two red rear lamps. */
    for(side=-1;side<=1;side+=2) {
        const float x=(float)side*.56f;
        draw_world_quad(&texture_headers[DLA_TEX_CAR_LIGHTS],
            traffic_point(vehicle,x-.22f,.58f,2.125f),traffic_point(vehicle,x+.22f,.58f,2.125f),
            traffic_point(vehicle,x-.18f,.39f,2.125f),traffic_point(vehicle,x+.18f,.39f,2.125f),
            .58f,.05f,.98f,.05f,.58f,.54f,.98f,.54f,
            pack_color(1.0f,(color3_t){.88f,.94f,1.0f}));
        draw_world_quad(&texture_headers[DLA_TEX_CAR_LIGHTS],
            traffic_point(vehicle,x+.22f,.58f,-2.125f),traffic_point(vehicle,x-.22f,.58f,-2.125f),
            traffic_point(vehicle,x+.18f,.39f,-2.125f),traffic_point(vehicle,x-.18f,.39f,-2.125f),
            .02f,.02f,.48f,.02f,.02f,.48f,.48f,.48f,
            tail);
    }
}

static void draw_traffic(void) {
    int i;
    if(game.mode==MODE_TITLE) return;
    for(i=0;i<MAX_TRAFFIC;++i) {
        const float dx=traffic[i].x-car.x;
        const float dz=traffic[i].z-car.z;
        const float camera_dx=traffic[i].x-camera_x;
        const float camera_dz=traffic[i].z-camera_z;
        const bool demo_staging_clear=game.mode!=MODE_DEMO ||
                                      dx*dx+dz*dz>8.0f*8.0f;
        if(traffic[i].active && dx*dx+dz*dz < FAR_PLANE*FAR_PLANE &&
           demo_staging_clear &&
           camera_dx*camera_dx+camera_dz*camera_dz>6.0f*6.0f &&
           world_sphere_visible((vec3_t){traffic[i].x,.72f,traffic[i].z},
                                3.1f,34.0f))
            draw_traffic_car(&traffic[i]);
    }
}

static vec3_t oriented_world_point(float x, float z, float yaw,
                                   float lx, float y, float lz) {
    const shz_sincos_t rotation=shz_sincosf(yaw);
    const float c=rotation.cos,s=rotation.sin;
    return (vec3_t){x+lx*c+lz*s,y,z-lx*s+lz*c};
}

static void draw_headlight_cone(float x, float z, float yaw,
                                float length, float alpha) {
    const uint32_t color=pack_color(alpha*1.8f,(color3_t){.67f,.79f,1.0f});
    draw_world_quad(&texture_headers[DLA_TEX_EFFECT_LIGHT],
        oriented_world_point(x,z,yaw,-4.0f,.046f,length),
        oriented_world_point(x,z,yaw, 4.0f,.046f,length),
        oriented_world_point(x,z,yaw,-.70f,.046f,1.9f),
        oriented_world_point(x,z,yaw, .70f,.046f,1.9f),
        0,0,1,0,0,1,1,1,color);
}

static void draw_brake_lamp_glow(float x, float z, float yaw,
                                 float rear_z, float y,
                                 float spacing, float level) {
    const color3_t red={1.0f,.035f,.012f};
    int side;
    if(level<.025f) return;
    for(side=-1;side<=1;side+=2) {
        screen_point_t point;
        if(project_world(oriented_world_point(x,z,yaw,
                         (float)side*spacing,y,rear_z),&point)) {
            const float radius=clampf((45.0f+34.0f*level)*point.z,2.4f,9.5f);
            draw_disc(&additive_header,point.x,point.y,radius,
                      point.z+.00005f,6,
                      pack_color(.20f+.48f*level,red),
                      pack_color(0.0f,red));
        }
    }
}

static void draw_vehicle_lighting(void) {
    int i;
    draw_headlight_cone(car.x,car.z,car.yaw,20.0f,.105f);
    draw_world_disc(&additive_header,car_local_to_world(0,.043f,-1.75f),
                    2.0f+car.brake_light*1.15f,8,
                    pack_color(.11f+car.brake_light*.27f,
                               (color3_t){1.0f,.04f,.02f}),
                    pack_color(0.0f,(color3_t){1.0f,.02f,.01f}));
    draw_brake_lamp_glow(car.x,car.z,car.yaw,-2.32f,.51f,.59f,
                         car.brake_light);
    if(game.mode==MODE_TITLE) return;
    for(i=0;i<MAX_TRAFFIC;++i) {
        const traffic_t *vehicle=&traffic[i];
        const float dx=vehicle->x-car.x,dz=vehicle->z-car.z;
        const float camera_dx=vehicle->x-camera_x,camera_dz=vehicle->z-camera_z;
        if(!vehicle->active || dx*dx+dz*dz>155.0f*155.0f ||
           (game.mode==MODE_DEMO && dx*dx+dz*dz<=64.0f) ||
           camera_dx*camera_dx+camera_dz*camera_dz<=6.0f*6.0f ||
           !world_sphere_visible((vec3_t){vehicle->x,.72f,vehicle->z},
                                 16.0f,42.0f))
            continue;
        draw_headlight_cone(vehicle->x,vehicle->z,vehicle->yaw,14.0f,.07f);
        draw_world_disc(&additive_header,
            traffic_point(vehicle,0,.043f,-2.12f),
            1.35f+vehicle->brake_light*.75f,6,
            pack_color(.07f+vehicle->brake_light*.20f,
                       (color3_t){1.0f,.035f,.015f}),
            pack_color(0.0f,(color3_t){1.0f,.02f,.01f}));
        draw_brake_lamp_glow(vehicle->x,vehicle->z,vehicle->yaw,
                             -2.14f,.49f,.56f,vehicle->brake_light);
    }
}

static void draw_contact_shadow(float x,float z,float yaw,float scale,float opacity) {
    const uint32_t color=pack_color(opacity,(color3_t){1,1,1});
    draw_world_quad(&texture_headers[DLA_TEX_EFFECT_SHADOW],
        oriented_world_point(x,z,yaw,-1.45f*scale,.036f,3.20f*scale),
        oriented_world_point(x,z,yaw, 1.45f*scale,.036f,3.20f*scale),
        oriented_world_point(x,z,yaw,-1.45f*scale,.036f,-3.20f*scale),
        oriented_world_point(x,z,yaw, 1.45f*scale,.036f,-3.20f*scale),
        0,0,1,0,0,1,1,1,color);
}

static void draw_vehicle_shadows(void) {
    int i;
    draw_contact_shadow(car.x,car.z,car.yaw,1.0f,.92f);
    for(i=0;i<MAX_TRAFFIC;++i) {
        const traffic_t *v=&traffic[i];
        const float dx=v->x-car.x,dz=v->z-car.z;
        const float camera_dx=v->x-camera_x,camera_dz=v->z-camera_z;
        const float distance=sqrtf(dx*dx+dz*dz);
        if(!v->active || distance>125.0f ||
           camera_dx*camera_dx+camera_dz*camera_dz<=36.0f ||
           (game.mode==MODE_DEMO && distance<=8.0f) ||
           !world_sphere_visible((vec3_t){v->x,.1f,v->z},3.5f,24.0f)) continue;
        draw_contact_shadow(v->x,v->z,v->yaw,.84f,.78f*clampf((125.0f-distance)/45.0f,0.0f,1.0f));
    }
}

static void draw_pedestrian_shadows(void) {
    int i;
    for(i=0;i<ped_shadow_count;++i) {
        const ped_shadow_t *shadow=&ped_shadow_draws[i];
        const float hw=.34f,hl=.30f*shadow->length;
        draw_world_quad(&texture_headers[DLA_TEX_EFFECT_SHADOW],
            oriented_world_point(shadow->x,shadow->z,shadow->yaw,-hw,.034f,hl),
            oriented_world_point(shadow->x,shadow->z,shadow->yaw, hw,.034f,hl),
            oriented_world_point(shadow->x,shadow->z,shadow->yaw,-hw,.034f,-hl),
            oriented_world_point(shadow->x,shadow->z,shadow->yaw, hw,.034f,-hl),
            0,0,1,0,0,1,1,1,pack_color(shadow->opacity,(color3_t){1,1,1}));
    }
}

static void draw_skids(void) {
    int i;
    for(i = 0; i < MAX_SKIDS; ++i) {
        const skid_t *mark = &skid_pool[i];
        float dx, dz, length, nx, nz, alpha;
        if(!mark->active) continue;
        dx = mark->x2 - mark->x1;
        dz = mark->z2 - mark->z1;
        length = sqrtf(dx*dx + dz*dz);
        if(length < 0.01f) continue;
        nx = -dz / length * 0.105f;
        nz = dx / length * 0.105f;
        alpha = clampf(mark->life / 2.0f, 0.0f, 0.58f);
        draw_world_quad(&translucent_header,
            (vec3_t){mark->x1+nx,.032f,mark->z1+nz},
            (vec3_t){mark->x2+nx,.032f,mark->z2+nz},
            (vec3_t){mark->x1-nx,.032f,mark->z1-nz},
            (vec3_t){mark->x2-nx,.032f,mark->z2-nz},
            0,0,1,0,0,1,1,1,
            pack_color(alpha,(color3_t){0.015f,0.012f,0.018f}));
    }
}

static void draw_car_mesh(void) {
    const shz_sincos_t rotation=shz_sincosf(car.yaw);
    const float c=rotation.cos,s=rotation.sin;
    const float body_roll=clampf(-car.lateral*.0065f-car.yaw_rate*.028f,-.050f,.050f);
    static bool materials_ready;
    int material,i;
    _Static_assert(DLA_COUNT_OF(dla_car_vertices)<=MAX_CAR_MESH_VERTICES,
                   "player car mesh exceeds renderer scratch capacity");
    _Static_assert(DLA_COUNT_OF(dla_car_faces)<=DLA_COUNT_OF(car_visible_faces),
                   "player car face visibility capacity exceeded");
    if(!materials_ready) {
        for(i=0;i<dla_car_mesh.face_count;++i) {
            const dla_mesh_face_t *f=&dla_car_mesh.faces[i];
            const dla_mesh_vertex_t *a=&dla_car_mesh.vertices[f->a];
            const dla_mesh_vertex_t *b=&dla_car_mesh.vertices[f->b];
            const dla_mesh_vertex_t *c=&dla_car_mesh.vertices[f->c];
            const vec3_t ab={b->x-a->x,b->y-a->y,b->z-a->z};
            const vec3_t ac={c->x-a->x,c->y-a->y,c->z-a->z};
            vec3_t n={ab.y*ac.z-ab.z*ac.y,ab.z*ac.x-ab.x*ac.z,ab.x*ac.y-ab.y*ac.x};
            if(n.x*(a->nx+b->nx+c->nx)+n.y*(a->ny+b->ny+c->ny)+
               n.z*(a->nz+b->nz+c->nz)<0.0f) {
                n.x=-n.x; n.y=-n.y; n.z=-n.z;
            }
            car_face_normals[i]=n;
            car_render[f->a].material=car_render[f->b].material=car_render[f->c].material=f->material;
        }
        materials_ready=true;
    }
    /* Keep the original world-space backface arithmetic. Geometry only needs
       its world position until a front-facing face references the vertex. */
    for(i=0;i<dla_car_mesh.vertex_count;++i) {
        const dla_mesh_vertex_t *v=&dla_car_mesh.vertices[i];
        const float x=v->x-v->y*body_roll, y=v->y+v->x*body_roll;
        car_render[i].world=(vec3_t){car.x+x*c+v->z*s,y,car.z-x*s+v->z*c};
        car_render[i].referenced=false;
    }
    memset(car_visible_faces,0,sizeof(car_visible_faces));
    for(i=0;i<dla_car_mesh.face_count;++i) {
        const dla_mesh_face_t *f=&dla_car_mesh.faces[i];
        const vec3_t local=car_face_normals[i];
        const float nx=local.x-local.y*body_roll,ny=local.y+local.x*body_roll;
        const vec3_t n={nx*c+local.z*s,ny,-nx*s+local.z*c};
        const vec3_t view={camera_x-car_render[f->a].world.x,camera_y-car_render[f->a].world.y,
                           camera_z-car_render[f->a].world.z};
        if(n.x*view.x+n.y*view.y+n.z*view.z<0.0f) continue;
        /* Provisional face candidates become submitted-face flags below. */
        car_visible_faces[i]=true;
        car_render[f->a].referenced=car_render[f->b].referenced=car_render[f->c].referenced=true;
    }
    for(i=0;i<dla_car_mesh.vertex_count;++i) {
        const dla_mesh_vertex_t *v=&dla_car_mesh.vertices[i];
        vec3_t n,world;
        float nx,ny;
        color3_t light;
        if(!car_render[i].referenced) continue;
        nx=v->nx-v->ny*body_roll; ny=v->ny+v->nx*body_roll;
        n=(vec3_t){nx*c+v->nz*s,ny,-nx*s+v->nz*c};
        world=car_render[i].world;
        material=car_render[i].material;
        if(material==DLA_MAT_GLASS) light=(color3_t){.42f,.49f,.61f};
        else if(material==DLA_MAT_LIGHTS) light=(color3_t){1.0f,.92f,.90f};
        else {
            const float key=fmaxf(0.0f,n.x*-.48f+n.y*.64f+n.z*-.60f);
            const float sky=clampf(n.y*.5f+.5f,0.0f,1.0f);
            /* Cool hemisphere fill, warm low sun, and baked cavity occlusion. */
            light=(color3_t){(.25f+sky*.27f+key*.33f)*v->ao,
                             (.29f+sky*.29f+key*.25f)*v->ao,
                             (.38f+sky*.31f+key*.18f)*v->ao};
            if(material==DLA_MAT_CARBON) light=color_scale(light,.39f);
            else if(material==DLA_MAT_METAL) light=color_scale(light,.85f);
        }
        if(game.impact_flash>0.0f) light=(color3_t){1.0f,.90f,.85f};
        project_world(world,&car_render[i].projected);
        car_render[i].color=pack_color(1.0f,light);
        /* Only paint and glass are submitted by the reflection pass. */
        if(material==DLA_MAT_PAINT || material==DLA_MAT_GLASS) {
            float vx=camera_x-world.x,vy=camera_y-world.y,vz=camera_z-world.z;
            const float inv=dla_reflection_inv_length(vx*vx+vy*vy+vz*vz);
            float facing,rim,reflectivity;
            vx*=inv; vy*=inv; vz*=inv;
            facing=clampf(n.x*vx+n.y*vy+n.z*vz,0.0f,1.0f);
            rim=1.0f-facing;
            reflectivity=(.045f+rim*rim*rim*.23f)*v->ao;
            if(material==DLA_MAT_GLASS) reflectivity=.35f+rim*rim*.43f;
            car_render[i].reflect_color=pack_color(reflectivity,(color3_t){.84f,.90f,1.0f});
            /* Preserve the world-oriented hemisphere environment lookup. */
            car_render[i].reflect_uv[0]=clampf(.5f+(2.0f*facing*n.x-vx)*.48f,.01f,.99f);
            car_render[i].reflect_uv[1]=clampf(.5f-(2.0f*facing*n.y-vy)*.48f,.01f,.99f);
        }
    }
    for(material=0;material<5;++material) {
        const int texture=material==DLA_MAT_PAINT?DLA_TEX_CAR_PAINT:
            material==DLA_MAT_GLASS?DLA_TEX_CAR_GLASS:
            material==DLA_MAT_LIGHTS?DLA_TEX_CAR_LIGHTS:DLA_TEX_CAR_CARBON;
        const pvr_poly_hdr_t *header=material==DLA_MAT_METAL?&world_header:&texture_headers[texture];
        const int end=dla_car_material_ranges[material].first_face+
                      dla_car_material_ranges[material].face_count;
        for(i=dla_car_material_ranges[material].first_face;i<end;++i) {
            const dla_mesh_face_t *f=&dla_car_mesh.faces[i];
            const dla_mesh_vertex_t *a=&dla_car_mesh.vertices[f->a];
            const dla_mesh_vertex_t *b=&dla_car_mesh.vertices[f->b];
            const dla_mesh_vertex_t *d=&dla_car_mesh.vertices[f->c];
            if(!car_visible_faces[i]) continue;
            /* Reflections may only consume faces that reach submission. */
            car_visible_faces[i]=false;
            if(!car_render[f->a].projected.valid || !car_render[f->b].projected.valid ||
               !car_render[f->c].projected.valid ||
               !screen_triangle_visible(&car_render[f->a].projected,&car_render[f->b].projected,&car_render[f->c].projected)) continue;
            car_visible_faces[i]=true;
            submit_triangle(header,&car_render[f->a].projected,&car_render[f->b].projected,&car_render[f->c].projected,
                a->u,a->v,b->u,b->v,d->u,d->v,car_render[f->a].color,car_render[f->b].color,car_render[f->c].color);
        }
    }
}

static void draw_player_brake_lights(void) {
    static const float lamps[4][8]={
        {-.955f,.808f,-.635f,.795f,-.865f,.610f,-.635f,.615f},
        {-.585f,.793f,-.285f,.770f,-.530f,.615f,-.325f,.605f},
        { .285f,.770f, .585f,.793f, .325f,.605f, .530f,.615f},
        { .635f,.795f, .955f,.808f, .635f,.615f, .865f,.610f}
    };
#ifdef DRIFT_LA_CAR_CAPTURE_BRAKE
    const float brake=1.0f;
#else
    const float brake=clampf(car.brake_light,0.0f,1.0f);
#endif
    const uint32_t tint=pack_color(1.0f,(color3_t){.50f+.50f*brake,
        .25f+.67f*brake,.22f+.58f*brake});
    int i;
    if(brake<=.02f) return;
    for(i=0;i<4;++i)
        draw_world_quad(&texture_headers[DLA_TEX_CAR_LIGHTS],
            car_local_to_world(lamps[i][0],lamps[i][1],-2.638f),
            car_local_to_world(lamps[i][2],lamps[i][3],-2.638f),
            car_local_to_world(lamps[i][4],lamps[i][5],-2.638f),
            car_local_to_world(lamps[i][6],lamps[i][7],-2.638f),
            .125f,.125f,.125f,.125f,.125f,.125f,.125f,.125f,tint);
    draw_world_quad(&texture_headers[DLA_TEX_CAR_LIGHTS],
        car_local_to_world(-.315f,.868f,-2.230f),
        car_local_to_world( .315f,.868f,-2.230f),
        car_local_to_world(-.292f,.849f,-2.230f),
        car_local_to_world( .292f,.849f,-2.230f),
        .125f,.125f,.125f,.125f,.125f,.125f,.125f,.125f,tint);
}

typedef struct {
    float center_x,center_y,center_z;
    float car_x,car_z;
    shz_sincos_t car_rotation,part_rotation;
} wheel_transform_t;

static inline vec3_t rotate_part_point(const wheel_transform_t *transform,
                                       float x, float y, float z) {
    const float pc=transform->part_rotation.cos,ps=transform->part_rotation.sin;
    const float c=transform->car_rotation.cos,s=transform->car_rotation.sin;
    /* Retain the original two-stage arithmetic and floating-point order. */
    const float px=transform->center_x+x*pc+z*ps;
    const float pz=transform->center_z-x*ps+z*pc;
    return (vec3_t){transform->car_x+px*c+pz*s,transform->center_y+y,
                    transform->car_z-px*s+pz*c};
}

static void draw_wheel(float local_x, float local_z, float steer,
                       float radius, float width) {
    enum { SEGMENTS = 20 };
    vec3_t inner[SEGMENTS];
    vec3_t outer[SEGMENTS];
    const float half_width = width * 0.5f;
    const float outer_sign = local_x < 0.0f ? -1.0f : 1.0f;
    const float center_y = radius + 0.015f;
    const wheel_transform_t transform={
        .center_x=local_x,.center_y=center_y,.center_z=local_z,
        .car_x=car.x,.car_z=car.z,
        .car_rotation=shz_sincosf(car.yaw),.part_rotation=shz_sincosf(steer)
    };
    const float rotation=local_z>0.0f?front_wheel_spin:rear_wheel_spin;
    const uint32_t tread = pack_color(1.0f,(color3_t){0.27f,0.29f,0.33f});
    const uint32_t sidewall = pack_color(1.0f,(color3_t){0.055f,0.06f,0.075f});
    const uint32_t rotor = pack_color(1.0f,(color3_t){0.21f,0.23f,0.27f});
    const uint32_t rim = pack_color(1.0f,(color3_t){0.46f,0.49f,0.55f});
    const uint32_t rim_edge = pack_color(1.0f,(color3_t){0.40f,0.43f,0.49f});
    const uint32_t hub = pack_color(1.0f,(color3_t){0.16f,0.18f,0.23f});
    const uint32_t lug = pack_color(1.0f,(color3_t){0.66f,0.69f,0.74f});
    const uint32_t center_badge = pack_color(1.0f,(color3_t){0.72f,0.055f,0.035f});
    const uint32_t caliper = pack_color(1.0f,(color3_t){0.90f,0.06f,0.035f});
    vec3_t center;
    int i;

    for(i = 0; i < SEGMENTS; ++i) {
        const float angle = rotation + (float)i * PI * 2.0f / (float)SEGMENTS;
        const float y = fsin(angle) * radius;
        const float z = fcos(angle) * radius;
        inner[i] = rotate_part_point(&transform,-half_width,y,z);
        outer[i] = rotate_part_point(&transform, half_width,y,z);
    }

    for(i = 0; i < SEGMENTS; ++i) {
        const int next = (i + 1) % SEGMENTS;
        draw_world_quad(&texture_headers[DLA_TEX_CAR_CARBON],
            inner[i],outer[i],inner[next],outer[next],
            (float)i/(float)SEGMENTS,0.0f,(float)i/(float)SEGMENTS,1.0f,
            (float)(i+1)/(float)SEGMENTS,0.0f,(float)(i+1)/(float)SEGMENTS,1.0f,
            tread);
    }

    center = rotate_part_point(&transform,
                               outer_sign*(half_width+0.006f),0.0f,0.0f);
    for(i = 0; i < SEGMENTS; ++i) {
        const int next = (i + 1) % SEGMENTS;
        const float a0 = rotation + (float)i * PI * 2.0f / (float)SEGMENTS;
        const float a1 = rotation + (float)next * PI * 2.0f / (float)SEGMENTS;
        const vec3_t p0 = rotate_part_point(&transform,
            outer_sign*(half_width+0.006f),fsin(a0)*radius,fcos(a0)*radius);
        const vec3_t p1 = rotate_part_point(&transform,
            outer_sign*(half_width+0.006f),fsin(a1)*radius,fcos(a1)*radius);
        draw_world_triangle(&world_header,center,p0,p1,sidewall);
    }

    /* Layered brake rotor, restrained red caliper, machined rim lip, and five
       tapered spokes.  Separate depth planes keep the face legible at 480p. */
    {
        const vec3_t rotor_center=rotate_part_point(&transform,
            outer_sign*(half_width+0.010f),0.0f,0.0f);
        for(i=0;i<SEGMENTS;++i) {
            const float a0=(float)i*PI*2.0f/(float)SEGMENTS;
            const float a1=(float)(i+1)*PI*2.0f/(float)SEGMENTS;
            const vec3_t p0=rotate_part_point(&transform,
                outer_sign*(half_width+0.010f),fsin(a0)*radius*.61f,fcos(a0)*radius*.61f);
            const vec3_t p1=rotate_part_point(&transform,
                outer_sign*(half_width+0.010f),fsin(a1)*radius*.61f,fcos(a1)*radius*.61f);
            draw_world_triangle(&world_header,rotor_center,p0,p1,rotor);
        }
    }
    draw_world_quad(&world_header,
        rotate_part_point(&transform,outer_sign*(half_width+0.013f),-.040f,-.21f),
        rotate_part_point(&transform,outer_sign*(half_width+0.013f), .040f,-.21f),
        rotate_part_point(&transform,outer_sign*(half_width+0.013f),-.040f,-.08f),
        rotate_part_point(&transform,outer_sign*(half_width+0.013f), .040f,-.08f),
        0,0,1,0,0,1,1,1,caliper);

    for(i=0;i<SEGMENTS;++i) {
        const int next=(i+1)%SEGMENTS;
        const float a0=(float)i*PI*2.0f/(float)SEGMENTS;
        const float a1=(float)next*PI*2.0f/(float)SEGMENTS;
        const vec3_t inner0=rotate_part_point(&transform,
            outer_sign*(half_width+0.015f),fsin(a0)*radius*.70f,fcos(a0)*radius*.70f);
        const vec3_t outer0=rotate_part_point(&transform,
            outer_sign*(half_width+0.015f),fsin(a0)*radius*.82f,fcos(a0)*radius*.82f);
        const vec3_t inner1=rotate_part_point(&transform,
            outer_sign*(half_width+0.015f),fsin(a1)*radius*.70f,fcos(a1)*radius*.70f);
        const vec3_t outer1=rotate_part_point(&transform,
            outer_sign*(half_width+0.015f),fsin(a1)*radius*.82f,fcos(a1)*radius*.82f);
        draw_world_quad(&world_header,inner0,outer0,inner1,outer1,
                        0,0,1,0,0,1,1,1,rim_edge);
    }
    for(i = 0; i < 5; ++i) {
        const float angle=rotation*.15f+(float)i*PI*2.0f/5.0f;
        const float inner_spread=.180f,outer_spread=.065f;
        const vec3_t inner0=rotate_part_point(&transform,
            outer_sign*(half_width+0.017f),fsin(angle-inner_spread)*radius*.16f,
            fcos(angle-inner_spread)*radius*.16f);
        const vec3_t inner1=rotate_part_point(&transform,
            outer_sign*(half_width+0.017f),fsin(angle+inner_spread)*radius*.16f,
            fcos(angle+inner_spread)*radius*.16f);
        const vec3_t outer0=rotate_part_point(&transform,
            outer_sign*(half_width+0.017f),fsin(angle-outer_spread)*radius*.72f,
            fcos(angle-outer_spread)*radius*.72f);
        const vec3_t outer1=rotate_part_point(&transform,
            outer_sign*(half_width+0.017f),fsin(angle+outer_spread)*radius*.72f,
            fcos(angle+outer_spread)*radius*.72f);
        draw_world_quad(&world_header,inner0,outer0,inner1,outer1,
                        0,0,1,0,0,1,1,1,rim);
    }
    for(i = 0; i < 8; ++i) {
        const float a0 = (float)i*PI*2.0f/8.0f;
        const float a1 = (float)(i+1)*PI*2.0f/8.0f;
        const vec3_t p0 = rotate_part_point(&transform,
            outer_sign*(half_width+0.019f),fsin(a0)*radius*.15f,fcos(a0)*radius*.15f);
        const vec3_t p1 = rotate_part_point(&transform,
            outer_sign*(half_width+0.019f),fsin(a1)*radius*.15f,fcos(a1)*radius*.15f);
        draw_world_triangle(&world_header,center,p0,p1,hub);
    }
    /* Five raised lug nuts and a colored center cap survive close orbit shots
       and stop the rims reading as flat five-point decals. */
    for(i=0;i<5;++i) {
        const float angle=rotation*.15f+(float)i*PI*2.0f/5.0f;
        const float lug_y=fsin(angle)*radius*.105f;
        const float lug_z=fcos(angle)*radius*.105f;
        const vec3_t lug_center=rotate_part_point(&transform,
            outer_sign*(half_width+0.024f),lug_y,lug_z);
        int edge;
        for(edge=0;edge<6;++edge) {
            const float a0=(float)edge*PI/3.0f;
            const float a1=(float)(edge+1)*PI/3.0f;
            const vec3_t p0=rotate_part_point(&transform,
                outer_sign*(half_width+0.024f),lug_y+fsin(a0)*radius*.050f,
                lug_z+fcos(a0)*radius*.050f);
            const vec3_t p1=rotate_part_point(&transform,
                outer_sign*(half_width+0.024f),lug_y+fsin(a1)*radius*.050f,
                lug_z+fcos(a1)*radius*.050f);
            draw_world_triangle(&world_header,lug_center,p0,p1,lug);
        }
    }
    {
        const vec3_t badge_center=rotate_part_point(&transform,
            outer_sign*(half_width+0.026f),0.0f,0.0f);
        for(i=0;i<8;++i) {
            const float a0=(float)i*PI/4.0f;
            const float a1=(float)(i+1)*PI/4.0f;
            const vec3_t p0=rotate_part_point(&transform,
                outer_sign*(half_width+0.026f),fsin(a0)*radius*.070f,
                fcos(a0)*radius*.070f);
            const vec3_t p1=rotate_part_point(&transform,
                outer_sign*(half_width+0.026f),fsin(a1)*radius*.070f,
                fcos(a1)*radius*.070f);
            draw_world_triangle(&world_header,badge_center,p0,p1,center_badge);
        }
    }
}

static void draw_car(void) {
#ifdef DRIFT_LA_CAR_CAPTURE
    const float rendered_steer=0.0f;
#else
    const float rendered_steer=car.steer;
#endif
    draw_wheel(-.95f, 1.55f, rendered_steer, .43f, .30f);
    draw_wheel( .95f, 1.55f, rendered_steer, .43f, .30f);
    draw_wheel(-.97f,-1.55f, 0.0f, .45f, .32f);
    draw_wheel( .97f,-1.55f, 0.0f, .45f, .32f);
    draw_car_mesh();
    draw_player_brake_lights();
}

static void draw_car_environment_reflections(void) {
    int material,i;
    for(material=DLA_MAT_PAINT;material<=DLA_MAT_GLASS;++material) {
        const int end=dla_car_material_ranges[material].first_face+
                      dla_car_material_ranges[material].face_count;
        for(i=dla_car_material_ranges[material].first_face;i<end;++i) {
            const dla_mesh_face_t *f=&dla_car_mesh.faces[i];
            if(!car_visible_faces[i]) continue;
            submit_triangle(&reflection_header,&car_render[f->a].projected,&car_render[f->b].projected,&car_render[f->c].projected,
                car_render[f->a].reflect_uv[0],car_render[f->a].reflect_uv[1],
                car_render[f->b].reflect_uv[0],car_render[f->b].reflect_uv[1],
                car_render[f->c].reflect_uv[0],car_render[f->c].reflect_uv[1],
                car_render[f->a].reflect_color,car_render[f->b].reflect_color,car_render[f->c].reflect_color);
        }
    }
}

static void draw_smoke(void) {
    int i;
    for(i=0;i<MAX_SMOKE;++i) {
        const smoke_t *particle=&smoke_pool[i];
        screen_point_t p;
        float life,age,radius,alpha,angle,rx,ry,u,v;
        uint32_t color;
        if(!particle->active || !project_world((vec3_t){particle->x,particle->y,particle->z},&p)) continue;
        life=clampf(particle->life/particle->max_life,0.0f,1.0f);
        age=1.0f-life;
        radius=clampf(particle->size*camera_focal*p.z,1.8f,54.0f);
        if(p.x+radius*1.42f<0 || p.x-radius*1.42f>SCREEN_W ||
           p.y+radius*1.42f<0 || p.y-radius*1.42f>SCREEN_H) continue;
        alpha=clampf(age*7.0f,0.0f,1.0f)*life*.64f;
        alpha*=clampf((180.0f-1.0f/p.z)/100.0f,0.0f,1.0f);
        if(alpha<.008f) continue;
        angle=(float)i*2.399963f+age*((i&1)?-.65f:.65f);
        rx=fcos(angle)*radius; ry=fsin(angle)*radius;
        u=(float)(i&1)*.5f+.004f;
        v=(float)((i>>1)&1)*.5f+.004f;
        color=pack_color(alpha,(color3_t){.70f-age*.12f,.73f-age*.11f,.80f-age*.10f});
        {
            const screen_point_t a={p.x-rx+ry,p.y-ry-rx,p.z,true};
            const screen_point_t b={p.x+rx+ry,p.y+ry-rx,p.z,true};
            const screen_point_t c={p.x-rx-ry,p.y-ry+rx,p.z,true};
            const screen_point_t d={p.x+rx-ry,p.y+ry+rx,p.z,true};
            submit_quad(&texture_headers[DLA_TEX_EFFECT_SMOKE],&a,&b,&c,&d,
                u,v,u+.492f,v,u,v+.492f,u+.492f,v+.492f,color,color,color,color);
        }
        QA_COUNT(smoke_particles);
    }
}

static void draw_exhaust_flames(void) {
    const color3_t orange={1.0f,.16f,.012f};
    const color3_t yellow={1.0f,.82f,.20f};
    float flash=0.0f;
    int i;
    if(game.exhaust_flash>0.0f) {
        static const float outlets[4]={-.255f,-.085f,.085f,.255f};
        const float level=game.exhaust_flash;
        const float end_z=-2.78f-level*.72f;
        const uint32_t hot=pack_color(level,yellow);
        const uint32_t clear=pack_color(0.0f,orange);
        for(i=0;i<4;++i) {
            const float x=outlets[i];
            draw_world_quad_colored(&additive_header,
                car_local_to_world(x-.060f,.160f,-2.485f),
                car_local_to_world(x+.060f,.160f,-2.485f),
                car_local_to_world(x-.020f,.160f,end_z),
                car_local_to_world(x+.020f,.160f,end_z),
                hot,hot,clear,clear);
            draw_world_quad_colored(&additive_header,
                car_local_to_world(x,.105f,-2.485f),
                car_local_to_world(x,.215f,-2.485f),
                car_local_to_world(x,.145f,end_z),
                car_local_to_world(x,.185f,end_z),
                hot,hot,clear,clear);
        }
    }
    for(i=0;i<MAX_EXHAUST_FLAMES;++i) {
        const exhaust_flame_t *flame=&exhaust_flame_pool[i];
        screen_point_t point;
        float life_ratio,radius;
        if(!flame->active||
           !project_world((vec3_t){flame->x,flame->y,flame->z},&point))
            continue;
        life_ratio=clampf(flame->life/flame->max_life,0.0f,1.0f);
        radius=clampf(flame->size*camera_focal*point.z,2.2f,20.0f);
        flash=fmaxf(flash,life_ratio);
        draw_disc(&additive_header,point.x,point.y,radius,
                  point.z+.00006f,6,
                  pack_color(.92f*life_ratio,orange),
                  pack_color(0.0f,orange));
        if(life_ratio>.34f)
            draw_disc(&additive_header,point.x,point.y,
                      radius*(.38f+.16f*life_ratio),point.z+.00008f,5,
                      pack_color(life_ratio,yellow),
                      pack_color(0.0f,yellow));
    }
    if(flash>0.0f)
        draw_world_disc(&additive_header,
            car_local_to_world(0.0f,.044f,-2.40f),1.0f+flash*.72f,8,
            pack_color(.22f*flash,(color3_t){1.0f,.22f,.015f}),
            pack_color(0.0f,(color3_t){1.0f,.04f,.005f}));
}

static uint16_t argb4444(unsigned a, unsigned r, unsigned g, unsigned b) {
    return (uint16_t)(((a&15)<<12)|((r&15)<<8)|((g&15)<<4)|(b&15));
}

static void hud_text(int x, int y, uint16_t color, const char *text) {
    if(x < 0 || y < 0 || x >= HUD_W || y >= HUD_H-BFONT_HEIGHT) return;
    bfont_draw_str_ex(hud_pixels + y*HUD_W + x, HUD_W, color, 0, 16, false, text);
}

static void hud_center(int y, uint16_t color, const char *text) {
    int x = (HUD_W - (int)strlen(text)*BFONT_THIN_WIDTH)/2;
    if(x < 0) x = 0;
    hud_text(x,y,color,text);
}

static void update_hud(bool connected) {
    char line[64];
    const float live_score = game.score + game.drift_chain * game.drift_multiplier;
    const uint16_t white = argb4444(15,15,15,15);
    const uint16_t cyan = argb4444(15,2,15,15);
    const uint16_t amber = argb4444(15,15,10,2);
    const uint16_t magenta = argb4444(15,15,3,12);
    const uint16_t district_hud=game.district==DISTRICT_COAST ? argb4444(15,3,15,13) :
        (game.district==DISTRICT_ARTS ? argb4444(15,15,7,2) :
        (game.district==DISTRICT_NEON ? argb4444(15,15,2,12) : amber));
    memset(hud_pixels,0,HUD_BYTES);
    if(game.mode == MODE_TITLE) {
        hud_center(24,cyan,"D R I F T   L O S   A N G E L E S");
        hud_center(54,white,"OPEN CITY  /  STREET DRIFT");
        hud_center(174,amber,"START OR A  -  DRIVE");
        hud_center(200,magenta,"X  -  DISTRICT TOUR");
        if(!connected) hud_center(226,magenta,"CONNECT A DREAMCAST CONTROLLER");
        else hud_center(226,white,"R GAS  L BRAKE  A HANDBRAKE  /  B EXIT");
    }
    else {
        /* The 512x256 HUD surface is an atlas.  Each group is composited into
           a separate arcade instrument pod at native 480p, giving speed,
           scoring and district identity real hierarchy instead of four lines
           of debug-looking text floating over the sky. */
        snprintf(line,sizeof(line),"%03d",(int)(fabsf(car.longitudinal)*2.23694f));
        hud_text(374,156,white,line);
        hud_text(374,184,amber,"MPH");
        snprintf(line,sizeof(line),"SCORE %07lu",(unsigned long)live_score);
        hud_text(8,8,amber,line);
        hud_text(310,8,district_hud,district_name(game.district));
        if((game.mode==MODE_PLAYING || game.mode==MODE_DEMO) &&
           game.drift_chain>0.0f) {
            snprintf(line,sizeof(line),"DRIFT %05lu x%.1f",
                (unsigned long)game.drift_chain,game.drift_multiplier);
            hud_text(8,36,magenta,line);
            snprintf(line,sizeof(line),"%04.1f SEC / %02d DEG",
                game.drift_duration,(int)game.drift_angle);
            hud_text(8,64,cyan,line);
        }
        if(game.mode == MODE_PAUSED) {
            hud_center(108,cyan,"P A U S E D");
            hud_center(134,white,"START RESUME  /  B TITLE");
        }
        if(game.mode==MODE_PLAYING) {
            if(car.clutch_kick_timer>.02f)
                hud_text(164,8,magenta,"CLUTCH KICK");
            else if(car.handbrake_lock>.10f)
                hud_text(176,8,amber,"HANDBRAKE");
            else if(car.burnout>.18f)
                hud_text(188,8,magenta,"BURNOUT");
        }
        if(game.mode==MODE_DEMO)
            hud_text(310,36,cyan,"DEMO RUN");
        if(game.district_banner>0.0f && game.mode!=MODE_PAUSED)
            hud_center(108,district_hud,district_name(game.district));
        if(!connected) hud_center(134,magenta,"CONTROLLER DISCONNECTED");
    }
    pvr_txr_load(hud_pixels,hud_texture,HUD_BYTES);
}

static void draw_hud_region(float x, float y, float width, float height,
                            float source_x, float source_y,
                            float source_width, float source_height) {
    const float u0=source_x/(float)HUD_W;
    const float v0=source_y/(float)HUD_H;
    const float u1=(source_x+source_width)/(float)HUD_W;
    const float v1=(source_y+source_height)/(float)HUD_H;
    const screen_point_t a={x,y,.992f,true},b={x+width,y,.992f,true};
    const screen_point_t c={x,y+height,.992f,true},d={x+width,y+height,.992f,true};
    const uint32_t white=pack_color(1.0f,(color3_t){1,1,1});
    submit_quad(&hud_texture_header,&a,&b,&c,&d,u0,v0,u1,v0,u0,v1,u1,v1,
                white,white,white,white);
}

static void draw_hud_texture(void) {
    if(game.mode==MODE_TITLE) {
        draw_hud_region(0,0,SCREEN_W,SCREEN_H,0,0,HUD_W,HUD_H);
        return;
    }
    {
        const color3_t accent=district_color(game.district);
        const uint32_t panel=pack_color(.72f,(color3_t){.008f,.014f,.040f});
        const uint32_t inset=pack_color(.38f,(color3_t){.025f,.040f,.095f});
        const uint32_t edge=pack_color(.92f,accent);
        const uint32_t chrome=pack_color(.72f,(color3_t){.62f,.72f,.88f});
        /* Score chain pod. */
        draw_rect(&hud_header,12,12,236,72,.986f,panel);
        draw_rect(&hud_header,17,17,226,62,.987f,inset);
        draw_rect(&hud_header,12,12,236,3,.989f,edge);
        draw_rect(&hud_header,12,82,236,2,.989f,chrome);
        draw_hud_region(18,17,224,64,0,0,300,88);

        /* District ribbon, isolated from the score so both stay readable. */
        draw_rect(&hud_header,260,12,176,54,.986f,panel);
        draw_rect(&hud_header,265,17,166,44,.987f,inset);
        draw_rect(&hud_header,260,12,176,3,.989f,edge);
        draw_hud_region(267,17,162,46,300,0,212,64);

        /* Speed pod is deliberately low and close to the car, like a late-90s
           arcade tach module, rather than another line in the top-left list. */
        draw_rect(&hud_header,12,392,112,72,.986f,panel);
        draw_rect(&hud_header,17,397,102,62,.987f,inset);
        draw_rect(&hud_header,12,392,112,3,.989f,edge);
        draw_rect(&hud_header,12,462,112,2,.989f,chrome);
        draw_hud_region(19,399,98,56,352,146,132,80);

        if(game.district_banner>0.0f && game.mode!=MODE_PAUSED) {
            const float alpha=clampf(game.district_banner,0.0f,1.0f)*.68f;
            draw_rect(&hud_header,106,108,428,34,.986f,
                      pack_color(alpha,(color3_t){.010f,.018f,.050f}));
            draw_rect(&hud_header,106,108,428,2,.989f,edge);
            draw_hud_region(112,110,416,30,0,100,512,42);
        }
    }
}

static void draw_minimap(void) {
    const float x=558.0f,y=18.0f,size=64.0f;
    const color3_t accent=district_color(game.district);
    int i;
    draw_rect(&hud_header,x-6,y-6,size+12,size+12,0.985f,
              pack_color(0.72f,(color3_t){0.008f,0.014f,0.04f}));
    draw_rect(&hud_header,x-3,y-3,size+6,size+6,0.986f,
              pack_color(0.34f,(color3_t){0.025f,0.04f,0.095f}));
    draw_rect(&hud_header,x-6,y-6,size+12,3,0.988f,pack_color(.92f,accent));
    for(i=0;i<4;++i) {
        const float p=x+6.0f+(float)i*19.0f;
        draw_rect(&hud_header,p,y,2.0f,size,0.986f,
                  pack_color(0.48f,accent));
        draw_rect(&hud_header,x,y+6.0f+(float)i*19.0f,size,2.0f,0.986f,
                  pack_color(0.48f,accent));
    }
    for(i=0;i<MAX_TRAFFIC;++i) {
        const float dx=traffic[i].x-car.x;
        const float dz=traffic[i].z-car.z;
        const float dot_x=x+size*.5f+dx*.37f;
        const float dot_y=y+size*.5f-dz*.37f;
        if(!traffic[i].active || fabsf(dx)>88.0f || fabsf(dz)>88.0f)
            continue;
        draw_disc(&hud_header,dot_x,dot_y,2.3f,0.989f,5,
                  pack_color(1.0f,(color3_t){1.0f,.64f,.12f}),
                  pack_color(1.0f,(color3_t){1.0f,.22f,.05f}));
    }
    draw_disc(&hud_header,x+size*.5f,y+size*.5f,5.0f,0.99f,6,
              pack_color(1.0f,(color3_t){1.0f,0.18f,0.60f}),
              pack_color(1.0f,(color3_t){0.25f,0.95f,1.0f}));
}

static void draw_speed_bar(void) {
    const float speed=clampf(fabsf(car.longitudinal)/82.0f,0.0f,1.0f);
    int segment;
    for(segment=0;segment<10;++segment) {
        const float threshold=(float)(segment+1)/10.0f;
        const color3_t active=segment<6 ? (color3_t){.08f,.86f,1.0f} :
                              (segment<8 ? (color3_t){1.0f,.66f,.08f} :
                                           (color3_t){1.0f,.10f,.30f});
        draw_rect(&hud_header,19.0f+(float)segment*9.7f,451,7.0f,3.0f,.994f,
                  speed>=threshold ? pack_color(.96f,active) :
                                     pack_color(.30f,(color3_t){.08f,.10f,.16f}));
    }
}

static void draw_drift_meter(void) {
    const float hold=clampf(game.drift_duration/12.0f,0.0f,1.0f);
    const float pulse=.78f+.22f*fsin(game.time*10.0f);
    draw_rect(&hud_header,180,468,278,7,0.986f,
              pack_color(.66f,(color3_t){0.01f,0.02f,0.06f}));
    if(game.drift_chain>0.0f)
        draw_rect(&hud_header,183,470,272*hold,3,0.988f,
                  pack_color(.94f,(color3_t){1.0f,.16f+.40f*hold,.65f*pulse}));
}

static void render_frame(bool connected, float dt) {
    /* The HUD is a shared dynamic texture, so finish the preceding frame
       before a possible CPU-to-VRAM update. */
    pvr_wait_ready();
#ifdef DRIFT_LA_VISUAL_QA
    memset(&render_qa,0,sizeof(render_qa));
#endif
    setup_camera(dt);
    {
        const dla_frustum_camera_t view={
            .x=camera_x,.y=camera_y,.z=camera_z,
            .sin_yaw=camera_sin_yaw,.cos_yaw=camera_cos_yaw,
            .sin_pitch=camera_sin_pitch,.cos_pitch=camera_cos_pitch,
            .sin_roll=camera_sin_roll,.cos_roll=camera_cos_roll,
            .focal=camera_focal,.center_x=SCREEN_CX,.center_y=SCREEN_CY,
            .width=SCREEN_W,.height=SCREEN_H,
            .near_z=NEAR_PLANE,.far_z=FAR_PLANE,.margin=72.0f
        };
        dla_frustum_setup(&scene_frustum,&view);
    }
    palm_draw_count=0;
    if(game.hud_timer <= 0.0f) {
        update_hud(connected);
        game.hud_timer = 0.10f;
    }
    dla_load_camera_rotation(camera_sin_yaw,camera_cos_yaw,
                             camera_sin_pitch,camera_cos_pitch,
                             camera_sin_roll,camera_cos_roll);
    pvr_set_bg_color(0.02f,0.02f,0.08f);
    pvr_scene_begin();
    begin_poly_list(PVR_LIST_OP_POLY);
    if(game.mode==MODE_TITLE)
        draw_title_art();
    else {
        draw_sky();
        draw_city();
#ifndef DRIFT_LA_CAR_CAPTURE
        draw_traffic();
#endif
        draw_car();
    }
    pvr_list_finish();

    begin_poly_list(PVR_LIST_PT_POLY);
    if(game.mode!=MODE_TITLE) draw_palm_foliage();
    pvr_list_finish();
    begin_poly_list(PVR_LIST_TR_POLY);
    if(game.mode==MODE_TITLE)
        draw_rect(&hud_header,0,306,SCREEN_W,174,.97f,
                  pack_color(.62f,(color3_t){.008f,.012f,.035f}));
    else {
        draw_sun();
        draw_city_lighting();
        draw_vehicle_shadows();
        draw_pedestrian_shadows();
        draw_car_environment_reflections();
        draw_vehicle_lighting();
        draw_skids();
#ifndef DRIFT_LA_CAR_CAPTURE
        draw_smoke();
        draw_exhaust_flames();
#endif
    }
#ifndef DRIFT_LA_CAR_CAPTURE
    draw_hud_texture();
    if(game.mode != MODE_TITLE) {
        draw_minimap();
        draw_speed_bar();
        draw_drift_meter();
    }
#endif
    pvr_list_finish();
    pvr_scene_finish();
#ifdef DRIFT_LA_VISUAL_QA
    if(render_qa.triangles>render_qa_peak.triangles)
        render_qa_peak.triangles=render_qa.triangles;
    if(render_qa.vertices>render_qa_peak.vertices)
        render_qa_peak.vertices=render_qa.vertices;
    if(render_qa.buildings>render_qa_peak.buildings)
        render_qa_peak.buildings=render_qa.buildings;
    if(render_qa.vehicles>render_qa_peak.vehicles)
        render_qa_peak.vehicles=render_qa.vehicles;
    if(render_qa.pedestrians>render_qa_peak.pedestrians)
        render_qa_peak.pedestrians=render_qa.pedestrians;
    if(render_qa.smoke_particles>render_qa_peak.smoke_particles)
        render_qa_peak.smoke_particles=render_qa.smoke_particles;
    if(render_qa.furnishing_clusters>render_qa_peak.furnishing_clusters)
        render_qa_peak.furnishing_clusters=render_qa.furnishing_clusters;
#endif
}

static void release_graphics(void) {
    int i;
    if(hud_texture) pvr_mem_free(hud_texture);
    hud_texture = NULL;
    if(hud_pixels) free(hud_pixels);
    hud_pixels = NULL;
    for(i=0;i<DLA_TEXTURE_COUNT;++i) {
        if(texture_vram[i]) pvr_mem_free(texture_vram[i]);
        texture_vram[i]=NULL;
    }
}

static int init_graphics(void) {
    pvr_init_params_t params=pvr_default_params;
    pvr_poly_cxt_t context;
    uint32_t texture_bytes=0;
    int i;
    vid_set_enabled(0);
    vid_set_mode(DM_640x480,PM_RGB565);
    vid_set_dithering(true);
    params.dma_enabled=false; /* Vertices are submitted directly through SQ. */
    params.vertex_buf_size=GRAPHICS_VERTEX_BUFFER_BYTES;
    params.opb_sizes[PVR_LIST_PT_POLY]=PVR_BINSIZE_16;
    if(pvr_init(&params)<0) {
        printf("Drift Los Angeles: PowerVR initialization failed.\n");
        vid_set_enabled(1);
        return -1;
    }
    memset(texture_vram,0,sizeof(texture_vram));
    for(i=0;i<DLA_TEXTURE_COUNT;++i) {
        const dla_texture_asset_t *asset=&dla_texture_assets[i];
        texture_vram[i]=pvr_mem_malloc(asset->byte_size);
        if(!texture_vram[i]) {
            printf("Drift Los Angeles: texture allocation failed at %d.\n",i);
            release_graphics();
            pvr_shutdown();
            vid_set_enabled(1);
            return -1;
        }
        pvr_txr_load(asset->pixels,texture_vram[i],asset->byte_size);
        texture_bytes+=asset->byte_size;
    }
    hud_pixels=aligned_alloc(32,HUD_BYTES);
    hud_texture=pvr_mem_malloc(HUD_BYTES);
    if(!hud_pixels || !hud_texture) {
        printf("Drift Los Angeles: HUD texture allocation failed.\n");
        release_graphics();
        pvr_shutdown();
        vid_set_enabled(1);
        return -1;
    }
    memset(hud_pixels,0,HUD_BYTES);
    pvr_txr_load(hud_pixels,hud_texture,HUD_BYTES);

    pvr_set_zclip(0.0000001f);
    PVR_SET(PVR_PT_ALPHA_REF,64); /* Retain thin, mip-filtered palm leaflets. */
    /* Violet marine haze keeps distant districts readable against the sunset
       instead of crushing whole towers into featureless black silhouettes. */
    pvr_fog_table_color(1.0f,0.18f,0.10f,0.24f);
    pvr_fog_table_linear(190.0f,420.0f);

    {
        const dla_texture_asset_t *asset=&dla_texture_assets[DLA_TEX_SKY_BACKDROP];
        pvr_poly_cxt_txr(&context,PVR_LIST_OP_POLY,PVR_TXRFMT_RGB565,
                         asset->width,asset->height,
                         texture_vram[DLA_TEX_SKY_BACKDROP],PVR_FILTER_BILINEAR);
        context.gen.shading=PVR_SHADE_GOURAUD;
        context.gen.culling=PVR_CULLING_NONE;
        context.gen.fog_type=PVR_FOG_DISABLE;
        context.depth.comparison=PVR_DEPTHCMP_ALWAYS;
        context.depth.write=PVR_DEPTHWRITE_DISABLE;
        context.txr.env=PVR_TXRENV_MODULATE;
        context.txr.uv_clamp=PVR_UVCLAMP_UV;
        pvr_poly_compile(&backdrop_header,&context);
    }

    pvr_poly_cxt_col(&context,PVR_LIST_OP_POLY);
    context.gen.shading=PVR_SHADE_GOURAUD;
    context.gen.culling=PVR_CULLING_NONE;
    context.gen.fog_type=PVR_FOG_TABLE;
    context.depth.comparison=PVR_DEPTHCMP_GREATER;
    context.depth.write=PVR_DEPTHWRITE_ENABLE;
    pvr_poly_compile(&world_header,&context);

    for(i=0;i<DLA_TEXTURE_COUNT;++i) {
        const dla_texture_asset_t *asset=&dla_texture_assets[i];
        pvr_poly_cxt_txr(&context,i==DLA_TEX_EFFECT_PALM?PVR_LIST_PT_POLY:(asset->alpha?PVR_LIST_TR_POLY:PVR_LIST_OP_POLY),
                         asset->alpha?PVR_TXRFMT_ARGB4444:PVR_TXRFMT_RGB565,
                         asset->width,asset->height,texture_vram[i],PVR_FILTER_BILINEAR);
        context.txr.mipmap=asset->mipmap;
        context.txr.mipmap_bias=PVR_MIPBIAS_NORMAL;
        context.gen.shading=PVR_SHADE_GOURAUD;
        context.gen.culling=PVR_CULLING_NONE;
        context.gen.fog_type=PVR_FOG_TABLE;
        context.depth.comparison=PVR_DEPTHCMP_GREATER;
        context.depth.write=PVR_DEPTHWRITE_ENABLE;
        context.txr.env=PVR_TXRENV_MODULATE;
        context.txr.uv_clamp=PVR_UVCLAMP_NONE;
        if(asset->alpha) {
            context.gen.alpha=true;
            context.gen.fog_type=PVR_FOG_DISABLE;
            context.depth.write=PVR_DEPTHWRITE_DISABLE;
            context.blend.src=PVR_BLEND_SRCALPHA;
            context.blend.dst=PVR_BLEND_INVSRCALPHA;
            context.txr.alpha=PVR_TXRALPHA_ENABLE;
            context.txr.env=PVR_TXRENV_MODULATEALPHA;
            context.txr.uv_clamp=PVR_UVCLAMP_UV;
        }
        if(i==DLA_TEX_EFFECT_LIGHT)
            context.blend.dst=PVR_BLEND_ONE;
        if(i==DLA_TEX_EFFECT_PALM) {
            context.gen.fog_type=PVR_FOG_TABLE;
            context.depth.write=PVR_DEPTHWRITE_ENABLE;
            context.blend.src=PVR_BLEND_ONE;
            context.blend.dst=PVR_BLEND_ZERO;
        }
        pvr_poly_compile(&texture_headers[i],&context);
    }

    {
        const dla_texture_asset_t *asset=&dla_texture_assets[DLA_TEX_TITLE_ART];
        pvr_poly_cxt_txr(&context,PVR_LIST_OP_POLY,PVR_TXRFMT_RGB565,
                         asset->width,asset->height,texture_vram[DLA_TEX_TITLE_ART],
                         PVR_FILTER_BILINEAR);
        context.gen.shading=PVR_SHADE_GOURAUD;
        context.gen.culling=PVR_CULLING_NONE;
        context.gen.fog_type=PVR_FOG_DISABLE;
        context.depth.comparison=PVR_DEPTHCMP_ALWAYS;
        context.depth.write=PVR_DEPTHWRITE_DISABLE;
        context.txr.env=PVR_TXRENV_MODULATE;
        context.txr.uv_clamp=PVR_UVCLAMP_UV;
        pvr_poly_compile(&title_header,&context);
    }

    {
        const dla_texture_asset_t *asset=&dla_texture_assets[DLA_TEX_CAR_REFLECTION];
        pvr_poly_cxt_txr(&context,PVR_LIST_TR_POLY,PVR_TXRFMT_RGB565,
            asset->width,asset->height,texture_vram[DLA_TEX_CAR_REFLECTION],PVR_FILTER_BILINEAR);
        context.gen.alpha=true;
        context.gen.culling=PVR_CULLING_NONE;
        context.depth.comparison=PVR_DEPTHCMP_GEQUAL;
        context.depth.write=PVR_DEPTHWRITE_DISABLE;
        context.blend.src=PVR_BLEND_SRCALPHA;
        context.blend.dst=PVR_BLEND_ONE;
        context.txr.env=PVR_TXRENV_MODULATEALPHA;
        context.txr.alpha=PVR_TXRALPHA_DISABLE;
        context.txr.uv_clamp=PVR_UVCLAMP_UV;
        pvr_poly_compile(&reflection_header,&context);
    }

    pvr_poly_cxt_col(&context,PVR_LIST_TR_POLY);
    context.gen.alpha=true;
    context.gen.shading=PVR_SHADE_GOURAUD;
    context.gen.culling=PVR_CULLING_NONE;
    context.gen.fog_type=PVR_FOG_DISABLE;
    context.depth.comparison=PVR_DEPTHCMP_GREATER;
    context.depth.write=PVR_DEPTHWRITE_DISABLE;
    context.blend.src=PVR_BLEND_SRCALPHA;
    context.blend.dst=PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&translucent_header,&context);
    context.blend.dst=PVR_BLEND_ONE;
    pvr_poly_compile(&additive_header,&context);
    context.depth.comparison=PVR_DEPTHCMP_ALWAYS;
    context.blend.dst=PVR_BLEND_INVSRCALPHA;
    pvr_poly_compile(&hud_header,&context);

    pvr_poly_cxt_txr(&context,PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB4444|PVR_TXRFMT_NONTWIDDLED,
                     HUD_W,HUD_H,hud_texture,PVR_FILTER_BILINEAR);
    context.gen.alpha=true;
    context.gen.shading=PVR_SHADE_GOURAUD;
    context.gen.culling=PVR_CULLING_NONE;
    context.gen.fog_type=PVR_FOG_DISABLE;
    context.depth.comparison=PVR_DEPTHCMP_ALWAYS;
    context.depth.write=PVR_DEPTHWRITE_DISABLE;
    context.blend.src=PVR_BLEND_SRCALPHA;
    context.blend.dst=PVR_BLEND_INVSRCALPHA;
    context.txr.alpha=PVR_TXRALPHA_ENABLE;
    context.txr.env=PVR_TXRENV_MODULATEALPHA;
    context.txr.uv_clamp=PVR_UVCLAMP_UV;
    pvr_poly_compile(&hud_texture_header,&context);

    graphics_texture_bytes=texture_bytes+HUD_BYTES;
    graphics_vram_free=pvr_mem_available();
    printf("Drift Los Angeles: PowerVR ready, %lu KiB textures, %lu KiB VRAM free.\n",
           (unsigned long)((texture_bytes+HUD_BYTES+1023)/1024),
           (unsigned long)(pvr_mem_available()/1024));
    vid_set_enabled(1);
    return 0;
}

int main(int argc, char **argv) {
    uint64_t previous_time;
#ifdef DRIFT_LA_VISUAL_QA
    uint64_t benchmark_start;
    uint32_t benchmark_frames=0;
#endif
    bool running=true;
#ifdef DRIFT_LA_PHYSICS_QA
    int physics_qa_phase=0;
    int physics_qa_exit=0;
    int burnout_smoke_start=0;
    float burnout_max_speed=0.0f;
    float burnout_max_wheel_delta=0.0f;
    float burnout_max_slip=0.0f;
    float donut_accumulated_yaw=0.0f;
    float donut_max_radius=0.0f;
    float donut_max_planar_speed=0.0f;
    float donut_max_yaw_rate=0.0f;
    float donut_max_slip=0.0f;
    float donut_slip_integral=0.0f;
    float donut_sample_time=0.0f;
#endif
#ifdef DRIFT_LA_AUTOTEST
    uint64_t audio_poll_total_us=0;
    uint64_t audio_poll_max_us=0;
    uint32_t audio_poll_count=0;
#endif
    (void)argc;
    (void)argv;
    printf("Drift Los Angeles booting. Native PowerVR renderer.\n");
    printf("Controls: R gas, L brake/reverse, stick steer, A handbrake, X clutch kick, B camera, Y reset, Start pause.\n");
    if(init_graphics()<0) return 1;
    memset(&game,0,sizeof(game));
    game.mode=MODE_TITLE;
    game.drift_multiplier=1.0f;
    reset_car();
#ifdef DRIFT_LA_SHOWCASE
    start_demo();
    printf("Drift Los Angeles showcase: cycling all four districts for capture.\n");
#elif defined(DRIFT_LA_AUTOTEST)
    start_run();
#ifdef DRIFT_LA_PHYSICS_QA
    memset(traffic,0,sizeof(traffic));
    burnout_smoke_start=smoke_cursor;
    printf("Drift Los Angeles physics QA: burnout and low-speed donut scenarios enabled.\n");
#elif defined(DRIFT_LA_COLLISION_QA)
    memset(traffic,0,sizeof(traffic));
    collision_qa_start();
    printf("Drift Los Angeles collision QA: lamppost, parked car and pedestrian scenarios enabled.\n");
#elif defined(DRIFT_LA_POWERTEST)
    printf("Drift Los Angeles power test: straight-line full-throttle run enabled.\n");
#else
    printf("Drift Los Angeles autotest: scripted throttle, steering, handbrake, and clutch kick enabled.\n");
#endif
#endif
    init_audio();
    previous_time=timer_us_gettime64();
#ifdef DRIFT_LA_VISUAL_QA
    benchmark_start=previous_time;
#endif
    while(running) {
        const uint64_t now=timer_us_gettime64();
        float dt=(float)(now-previous_time)*0.000001f;
        input_t input=poll_input();
        previous_time=now;
#ifdef DRIFT_LA_GEOMETRY_QA
        /* Reproduce simulation states regardless of renderer throughput. This
           validation mode is deliberately separate from the timed benchmark. */
        dt=1.0f/30.0f;
#else
        dt=clampf(dt,0.001f,0.050f);
#endif
#ifdef DRIFT_LA_SHOWCASE
        input.connected=true;
        input.buttons=0;
        input.pressed=0;
        input.steer=0.0f;
        input.throttle=0.0f;
        input.brake=0.0f;
#endif
#ifdef DRIFT_LA_AUTOTEST
        input.connected=true;
        input.buttons=0;
        input.pressed=0;
#ifdef DRIFT_LA_PHYSICS_QA
        if(physics_qa_phase==0&&game.time>=3.0f) {
            const int smoke_emissions=smoke_cursor-burnout_smoke_start;
            const bool pass=burnout_max_speed<1.8f&&
                burnout_max_wheel_delta>18.0f&&burnout_max_slip>.70f&&
                smoke_emissions>=30;
            printf("Drift Los Angeles physics QA: burnout %s -- chassis=%.2fm/s wheel-delta=%.2fm/s slip=%.2f smoke=%d.\n",
                   pass?"PASS":"FAIL",burnout_max_speed,
                   burnout_max_wheel_delta,burnout_max_slip,smoke_emissions);
            if(!pass) physics_qa_exit=1;
            physics_qa_phase=1;
            reset_car();
            memset(traffic,0,sizeof(traffic));
            memset(smoke_pool,0,sizeof(smoke_pool));
            memset(skid_pool,0,sizeof(skid_pool));
            game.smoke_timer=0.0f;
            game.skid_timer=0.0f;
            skid_contact_valid=false;
        }
        if(physics_qa_phase==0) {
            input.throttle=1.0f;
            input.brake=1.0f;
            input.steer=0.0f;
        }
        else {
            input.throttle=1.0f;
            input.brake=0.0f;
            input.steer=1.0f;
        }
#elif defined(DRIFT_LA_COLLISION_QA)
        collision_qa_input(&input);
#elif defined(DRIFT_LA_POWERTEST)
        input.throttle=1.0f;
        input.brake=0.0f;
        input.steer=0.0f;
#else
        /* Exercise the same trigger modulation expected from a player rather
           than hiding instability behind a constant scripted throttle. */
        input.throttle=.68f+.28f*(.5f+.5f*fsin(game.time*1.35f));
        input.brake=0.0f;
        input.steer=fsin(game.time*.72f)*.64f;
        if(fmodf(game.time,4.5f)>2.7f&&fmodf(game.time,4.5f)<2.96f)
            input.buttons|=CONT_A;
        if(fmodf(game.time,6.0f)>4.8f&&fmodf(game.time,6.0f)<5.25f)
            input.buttons|=CONT_X;
        input.pressed=input.buttons&~game.previous_buttons;
#endif
#endif
        running=update_game(&input,dt);
#ifdef DRIFT_LA_COLLISION_QA
        if(collision_qa_observe(dt)) running=false;
#endif
#ifdef DRIFT_LA_PHYSICS_QA
        if(physics_qa_phase==0) {
            burnout_max_speed=fmaxf(burnout_max_speed,
                                     fabsf(car.longitudinal));
            burnout_max_wheel_delta=fmaxf(burnout_max_wheel_delta,
                fabsf(car.rear_wheel_speed-car.longitudinal));
            burnout_max_slip=fmaxf(burnout_max_slip,car.rear_power_slip);
        }
        else {
            const float dx=car.x;
            const float dz=car.z-16.0f;
            const float planar_speed=sqrtf(car.longitudinal*car.longitudinal+
                                           car.lateral*car.lateral);
            donut_accumulated_yaw+=fabsf(car.yaw_rate)*dt;
            donut_max_radius=fmaxf(donut_max_radius,sqrtf(dx*dx+dz*dz));
            donut_max_planar_speed=fmaxf(donut_max_planar_speed,planar_speed);
            donut_max_yaw_rate=fmaxf(donut_max_yaw_rate,
                                     fabsf(car.yaw_rate));
            donut_max_slip=fmaxf(donut_max_slip,car.rear_power_slip);
            donut_slip_integral+=car.rear_power_slip*dt;
            donut_sample_time+=dt;
        }
#endif
        update_audio_controls(&input,dt);
        audio_update_recorded_voices();
        render_frame(input.connected,dt);
#ifdef DRIFT_LA_VISUAL_QA
        ++benchmark_frames;
#endif
#ifdef DRIFT_LA_AUTOTEST
        {
            const uint64_t audio_poll_start=timer_us_gettime64();
#endif
        if(audio_stream!=SND_STREAM_INVALID) snd_stream_poll(audio_stream);
#ifdef DRIFT_LA_AUTOTEST
            {
                const uint64_t elapsed=timer_us_gettime64()-audio_poll_start;
                audio_poll_total_us+=elapsed;
                if(elapsed>audio_poll_max_us) audio_poll_max_us=elapsed;
                ++audio_poll_count;
            }
        }
#endif
#ifdef DRIFT_LA_SHOWCASE
        {
            static int last_showcase_report=-1;
            const int second=(int)game.demo_time;
            if(second>0&&second%3==0&&second!=last_showcase_report) {
                pvr_stats_t stats;
                last_showcase_report=second;
                if(pvr_get_stats(&stats)==0)
#ifdef DRIFT_LA_VISUAL_QA
                    printf("Drift Los Angeles visual QA: t=%d district=%s fps=%.1f reg=%.2fms tri=%lu vtx=%lu bldg=%lu cars=%lu ped=%lu smoke=%lu furnish=%lu pvr=%luKiB.\n",
                           second,district_name(game.district),stats.frame_rate,
                           (double)stats.reg_last_time/1000000.0,
                           (unsigned long)render_qa.triangles,
                           (unsigned long)render_qa.vertices,
                           (unsigned long)render_qa.buildings,
                           (unsigned long)render_qa.vehicles,
                           (unsigned long)render_qa.pedestrians,
                           (unsigned long)render_qa.smoke_particles,
                           (unsigned long)render_qa.furnishing_clusters,
                           (unsigned long)(stats.vtx_buffer_used/1024));
#else
                    printf("Drift Los Angeles showcase: t=%d district=%s fps=%.1f reg=%.2fms.\n",
                           second,district_name(game.district),stats.frame_rate,
                           (double)stats.reg_last_time/1000000.0);
#endif
            }
#ifdef DRIFT_LA_VISUAL_QA
#ifndef DRIFT_LA_CAPTURE_SEGMENT
            if(game.demo_time>=60.0f) {
                const uint64_t elapsed=timer_us_gettime64()-benchmark_start;
#ifdef DRIFT_LA_GEOMETRY_QA
                printf("Drift Los Angeles geometry QA: complete; frames=%lu step_hz=30 simulated_seconds=%.6f total_tri=%llu total_vtx=%llu peak_tri=%lu peak_vtx=%lu elapsed_us=%llu.\n",
                    (unsigned long)benchmark_frames,(double)game.demo_time,
                    (unsigned long long)geometry_qa_triangles,
                    (unsigned long long)geometry_qa_vertices,
                    (unsigned long)render_qa_peak.triangles,
                    (unsigned long)render_qa_peak.vertices,
                    (unsigned long long)elapsed);
#else
                printf("Drift Los Angeles benchmark: frames=%lu elapsed_us=%llu average_fps=%.2f peak_tri=%lu peak_vtx=%lu texture_bytes=%lu vram_free=%lu vertex_buffer_bytes=%lu.\n",
                    (unsigned long)benchmark_frames,(unsigned long long)elapsed,
                    (double)benchmark_frames*1000000.0/(double)elapsed,
                    (unsigned long)render_qa_peak.triangles,(unsigned long)render_qa_peak.vertices,
                    (unsigned long)graphics_texture_bytes,(unsigned long)graphics_vram_free,
                    (unsigned long)GRAPHICS_VERTEX_BUFFER_BYTES);
#endif
                printf("Drift Los Angeles visual QA: complete; peak tri=%lu vtx=%lu bldg=%lu cars=%lu ped=%lu smoke=%lu furnish=%lu.\n",
                       (unsigned long)render_qa_peak.triangles,
                       (unsigned long)render_qa_peak.vertices,
                       (unsigned long)render_qa_peak.buildings,
                       (unsigned long)render_qa_peak.vehicles,
                       (unsigned long)render_qa_peak.pedestrians,
                       (unsigned long)render_qa_peak.smoke_particles,
                       (unsigned long)render_qa_peak.furnishing_clusters);
                running=false;
            }
#endif
#endif
        }
#endif
#ifdef DRIFT_LA_AUTOTEST
        {
            static int last_report=-1;
            const int second=(int)game.time;
            if(second>0&&second!=last_report&&
#ifdef DRIFT_LA_PHYSICS_QA
               true
#else
               second%3==0
#endif
              ) {
                pvr_stats_t stats;
                last_report=second;
                if(pvr_get_stats(&stats)==0)
#ifdef DRIFT_LA_PHYSICS_QA
                    printf("Drift Los Angeles physics QA: t=%d u=%.2f v=%.2f wheel=%.2f slip=%.2f yaw=%.2f heading=%.2f pos=%.2f,%.2f radius=%.2f fps=%.1f.\n",
                        second,car.longitudinal,car.lateral,
                        car.rear_wheel_speed,car.rear_power_slip,
                        car.yaw_rate,car.yaw,car.x,car.z,
                        sqrtf(car.x*car.x+
                                          (car.z-16.0f)*(car.z-16.0f)),
                        stats.frame_rate);
#else
                    printf("Drift Los Angeles autotest: t=%d speed=%.1fmph rpm=%.0f gear=%d tire=%.2f angle=%.1f score=%lu hb=%.2f kick=%.2f fps=%.1f reg=%.2fms audio=%.2f/%.2fms vtx=%luKiB.\n",
                        second,fabsf(car.longitudinal)*2.23694f,
                        audio_controls.rpm,audio_controls.gear,
                        audio_controls.rear_slip,game.drift_angle,
                        (unsigned long)game.score,car.handbrake_lock,
                        car.clutch_kick_timer,stats.frame_rate,
                        (double)stats.reg_last_time/1000000.0,
                        audio_poll_count?(double)audio_poll_total_us/
                            (double)audio_poll_count/1000.0:0.0,
                        (double)audio_poll_max_us/1000.0,
                        (unsigned long)(stats.vtx_buffer_used/1024));
#endif
                audio_poll_total_us=0;
                audio_poll_max_us=0;
                audio_poll_count=0;
            }
            if(game.time>=
#ifdef DRIFT_LA_PHYSICS_QA
               9.0f
#elif defined(DRIFT_LA_COLLISION_QA)
               30.0f
#elif defined(DRIFT_LA_POWERTEST)
               24.0f
#else
               12.0f
#endif
              ) {
#ifdef DRIFT_LA_PHYSICS_QA
                const float mean_slip=donut_sample_time>0.0f?
                    donut_slip_integral/donut_sample_time:0.0f;
                const bool pass=donut_accumulated_yaw>6.4f&&
                    donut_max_radius<16.5f&&donut_max_planar_speed<8.5f&&
                    donut_max_yaw_rate>.90f&&donut_max_slip>.80f&&
                    mean_slip>.75f;
                printf("Drift Los Angeles physics QA: donut %s -- rotation=%.2frad radius=%.2fm speed=%.2fm/s yaw-rate=%.2frad/s slip=%.2f/%.2f mean.\n",
                       pass?"PASS":"FAIL",donut_accumulated_yaw,
                       donut_max_radius,donut_max_planar_speed,
                       donut_max_yaw_rate,donut_max_slip,mean_slip);
                if(!pass) physics_qa_exit=1;
                printf("Drift Los Angeles physics QA: %s.\n",
                       physics_qa_exit==0?"ALL TESTS PASS":"FAILED");
#else
                printf("Drift Los Angeles autotest: completed scripted drive.\n");
#endif
                running=false;
            }
        }
#endif
    }
    pvr_wait_ready();
    shutdown_audio();
    release_graphics();
    pvr_shutdown();
    printf("Drift Los Angeles shutdown complete.\n");
#ifdef DRIFT_LA_PHYSICS_QA
    return physics_qa_exit;
#elif defined(DRIFT_LA_COLLISION_QA)
    return collision_qa.failures==0?0:1;
#else
    return 0;
#endif
}
