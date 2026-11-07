/* Ridgeline audio: engines, tyres, wind, the forest and creek, effects.

   The player's engine is eight looping voices: four recorded rpm points,
   each on load and off load (tools/build_audio.py). All eight start on the
   same frame and every frame each plays at rate rpm / its own rpm, so they
   all run the same number of engine cycles a second and stay in phase;
   only their volumes change, crossfading the two rpm points around the
   engine's speed (equal-power-ish, cos/sin to the 1.5, which kept the
   crossover within ~1.6 dB) and the throttle between on and off load.
   CPU trucks get the two nearest on-load points, quieter with distance. */
#include "audio.h"
#include "truck.h"
#include "terrain.h"
#include "engine/sfx.h"
#include "sound_assets.h"

extern const uint8_t rl_sfx_pak[];

static SfxVoice eng_v[2][RL_ENGINE_LOOPS];       /* player: on, off */
static SfxVoice cpu_v[MAX_TRUCKS][2];
static int cpu_lo[MAX_TRUCKS];
static SfxVoice horn_v, surf_v[5], wind_v, forest_v, creek_v;
static bool started;
static float load_s, master, start_t = -1, start_len;
static float surf_vol[5];
static int last_gear, last_mode;
static bool last_lights;
static float blowoff_cool;

static float bird_rand(void) {
    static uint32_t st = 0x6b1d5u;
    st = st * 1664525u + 1013904223u;
    return (float)(st >> 8 & 0xffff) / 65535.0f;
}

enum { SV_GRAVEL, SV_DIRT, SV_MUD, SV_WATER, SV_SCRUB };
static const int surf_sfx[5] = {RL_SFX_GRAVEL, RL_SFX_DIRT, RL_SFX_MUD, RL_SFX_WATER, RL_SFX_SCRUB};

void audio_init(void) {
    _Static_assert(sizeof(rl_sfx_asset_t) == sizeof(eng_sfx_asset_t), "sfx table layout");
    sfx_init(rl_sfx_pak, (const eng_sfx_asset_t *)rl_sfx_assets, RL_SFX_COUNT);
}

static void stop_all(void) {
    for(int l = 0; l < 2; l++)
        for(int k = 0; k < RL_ENGINE_LOOPS; k++) sfx_voice_stop(&eng_v[l][k]);
    for(int i = 0; i < MAX_TRUCKS; i++)
        for(int k = 0; k < 2; k++) sfx_voice_stop(&cpu_v[i][k]);
    sfx_voice_stop(&horn_v);
    for(int i = 0; i < 5; i++) sfx_voice_stop(&surf_v[i]);
    sfx_voice_stop(&wind_v);
    sfx_voice_stop(&forest_v);
    sfx_voice_stop(&creek_v);
    started = false;
}

void audio_drive_start(void) {
    if(!sfx_ready()) return;
    stop_all();
    const Truck *p = &trucks[0];
    const rl_engine_sound_t *es = &rl_engine_sounds[p->model];
    sfx_batch_begin();
    for(int k = 0; k < RL_ENGINE_LOOPS; k++) {
        sfx_voice_start(&eng_v[0][k], es->on[k]);
        sfx_voice_start(&eng_v[1][k], es->off[k]);
    }
    sfx_batch_end();
    for(int i = 1; i < game.ntrucks; i++) {
        cpu_lo[i] = -1;
        cpu_v[i][0].channel = cpu_v[i][1].channel = -1;
    }
    sfx_voice_start(&horn_v, rl_horn_sfx[p->model]);
    for(int i = 0; i < 5; i++) sfx_voice_start(&surf_v[i], surf_sfx[i]);
    sfx_voice_start(&wind_v, RL_SFX_WIND);
    sfx_voice_start(&forest_v, RL_SFX_FOREST);
    sfx_voice_start(&creek_v, RL_SFX_CREEK);
    int st = rl_start_sfx[p->model];
    sfx_play(st, 220, 128, 1.0f);
    start_len = (float)rl_sfx_assets[st].bytes / 2.0f / (float)rl_sfx_assets[st].rate;
    start_t = 0;
    master = 0;
    load_s = 0;
    last_gear = p->gear;
    last_mode = p->mode;
    last_lights = p->lights;
    started = true;
}

void audio_drive_stop(void) {
    stop_all();
}

void audio_sfx(int id, int vol) {
    sfx_play(id, vol, 128, 1.0f);
}

/* Pan and distance gain of a world point from the camera. */
static void place(V3 p, float range, int *pan, float *gain) {
    V3 d = vsub(p, r_cam.pos);
    float dist = vlen(d);
    float side = dist > 0.1f ? vdot(d, r_cam.right) / dist : 0;
    *pan = 128 + (int)(side * 90.0f);
    float k = clampf(1.0f - dist / range, 0, 1);
    *gain = k * k;
}

static void rpm_weights(const rl_engine_sound_t *es, float rpm, float *w) {
    for(int k = 0; k < RL_ENGINE_LOOPS; k++) w[k] = 0;
    if(rpm <= es->rpm[0]) {
        w[0] = 1;
        return;
    }
    for(int k = 0; k < RL_ENGINE_LOOPS - 1; k++) {
        if(rpm <= es->rpm[k + 1]) {
            float x = (rpm - es->rpm[k]) / (es->rpm[k + 1] - es->rpm[k]);
            w[k] = powf(cosf(x * ENG_PI * 0.5f), 1.5f);
            w[k + 1] = powf(sinf(x * ENG_PI * 0.5f), 1.5f);
            return;
        }
    }
    w[RL_ENGINE_LOOPS - 1] = 1;
}

void audio_update(float dt) {
    if(!started) return;
    Truck *p = &trucks[0];
    const rl_engine_sound_t *es = &rl_engine_sounds[p->model];
    bool paused = game.paused;
    /* The engine's loops fade in over the start sound's last moments. */
    if(start_t >= 0) {
        start_t += dt;
        master = clampf((start_t - (start_len - 0.45f)) / 0.4f, 0, 1);
        if(start_t > start_len) start_t = -1;
    }
    float m = paused ? 0.0f : master;
    if(p->stalled) m *= clampf(p->rpm / es->idle_rpm, 0, 1);
    load_s += (p->load - load_s) * smooth_k(8.0f, dt);
    float rpm = fmaxf(p->rpm, es->idle_rpm * 0.6f);
    float w[RL_ENGINE_LOOPS];
    rpm_weights(es, rpm, w);
    sfx_batch_begin();
    for(int k = 0; k < RL_ENGINE_LOOPS; k++) {
        float rate = rpm / es->rpm[k];
        sfx_voice_set(&eng_v[0][k], rate, (int)(215.0f * m * w[k] * load_s), 128);
        sfx_voice_set(&eng_v[1][k], rate, (int)(215.0f * m * w[k] * (1.0f - load_s)), 128);
    }
    /* CPU trucks: two on-load points each. */
    for(int i = 1; i < game.ntrucks; i++) {
        Truck *t = &trucks[i];
        const rl_engine_sound_t *ec = &rl_engine_sounds[t->model];
        float r = fmaxf(t->rpm, ec->idle_rpm * 0.6f);
        int lo = 0;
        while(lo < RL_ENGINE_LOOPS - 2 && r > ec->rpm[lo + 1]) lo++;
        if(lo != cpu_lo[i]) {
            sfx_voice_stop(&cpu_v[i][0]);
            sfx_voice_stop(&cpu_v[i][1]);
            sfx_voice_start(&cpu_v[i][0], ec->on[lo]);
            sfx_voice_start(&cpu_v[i][1], ec->on[lo + 1]);
            cpu_lo[i] = lo;
        }
        int pan;
        float gain;
        place(t->pos, 90.0f, &pan, &gain);
        float x = clampf((r - ec->rpm[lo]) / (ec->rpm[lo + 1] - ec->rpm[lo]), 0, 1);
        float v = paused ? 0.0f : 170.0f * gain * (0.6f + 0.4f * t->load);
        sfx_voice_set(&cpu_v[i][0], r / ec->rpm[lo], (int)(v * powf(cosf(x * 1.5708f), 1.5f)), pan);
        sfx_voice_set(&cpu_v[i][1], r / ec->rpm[lo + 1], (int)(v * powf(sinf(x * 1.5708f), 1.5f)), pan);
    }
    /* The horn, with a short ramp so it doesn't click. */
    static float horn = 0;
    horn = approach(horn, p->horn && !paused ? 1.0f : 0.0f, dt * 50.0f);
    sfx_voice_set(&horn_v, 1.0f, (int)(230.0f * horn), 128);
    /* Tyres on the ground: by surface, speed and slip, averaged over wheels. */
    float target[5] = {0};
    float spd = fabsf(p->speed);
    for(int i = 0; i < 4; i++) {
        const Wheel *wh = &p->wh[i];
        if(!wh->contact) continue;
        float roll = clampf(spd / 14.0f, 0, 1) * 0.25f;
        float slip = clampf(wh->slip / 6.0f, 0, 1) * 0.25f;
        if(wh->water > 0.08f) target[SV_WATER] += clampf(spd / 6.0f, 0.2f, 1.0f) * 0.3f;
        else if(wh->surf == SURF_MUD) target[SV_MUD] += roll + slip * 1.6f;
        else if(wh->surf == SURF_GRAVEL || wh->surf == SURF_SCREE) target[SV_GRAVEL] += roll + slip;
        else if(wh->surf == SURF_ROCK || wh->surf == SURF_WETROCK) target[SV_SCRUB] += slip * 1.4f;
        else target[SV_DIRT] += roll * 0.9f + slip;
    }
    target[SV_SCRUB] += clampf(p->scrape / 2.0f, 0, 1) * 0.6f;
    for(int i = 0; i < 5; i++) {
        surf_vol[i] = approach(surf_vol[i], clampf(target[i], 0, 1), dt * 3.0f);
        float rate = 0.8f + 0.4f * clampf(spd / 15.0f, 0, 1);
        sfx_voice_set(&surf_v[i], rate, (int)((paused ? 0 : 200.0f) * surf_vol[i]), 128);
    }
    float wind = clampf((spd - 6.0f) / 20.0f, 0, 1) + clampf((cam.pos.y - 120.0f) / 80.0f, 0, 0.4f);
    sfx_voice_set(&wind_v, 0.9f + 0.3f * clampf(spd / 25.0f, 0, 1), (int)((paused ? 0 : 150.0f) * wind), 128);
    /* The forest all round, the creek when near water. */
    float near_water = 0;
    for(int k = 0; k < 8; k++) {
        float a = k * ENG_TAU / 8;
        for(int r = 0; r < 3; r++) {
            float d = 6.0f + r * 14.0f;
            if(water_at(cam.pos.x + sinf(a) * d, cam.pos.z + cosf(a) * d) > -999.0f)
                near_water = fmaxf(near_water, 1.0f - r / 3.0f);
        }
    }
    float forest = 1.0f - clampf((cam.pos.y - 150.0f) / 60.0f, 0, 0.8f);
    sfx_voice_set(&forest_v, 1.0f, (int)((paused ? 20.0f : 70.0f) * forest), 128);
    sfx_voice_set(&creek_v, 1.0f, (int)((paused ? 0.0f : 150.0f) * near_water), 128);
    sfx_batch_end();
    /* Now and then a bird calls from somewhere about: one at a time, seconds
       apart (6-16 s), never the same call twice running. Their own
       generator, so they never shift the game's random sequence. */
    static float bird_t = 4.0f;
    static int last_bird = -1;
    if(!paused && (bird_t -= dt) <= 0) {
        int b = (int)(bird_rand() * RL_BIRDS) % RL_BIRDS;
        if(b == last_bird) b = (b + 1) % RL_BIRDS;
        last_bird = b;
        sfx_play(rl_bird_sfx[b], (int)((45.0f + 35.0f * bird_rand()) * forest), 40 + (int)(bird_rand() * 176.0f),
                 0.95f + 0.1f * bird_rand());
        bird_t = 6.0f + 10.0f * bird_rand();
    }

    /* Events. */
    if(!paused) {
        if(p->gear != last_gear) sfx_play(RL_SFX_SHIFT, 120, 128, 1.0f);
        if(p->mode != last_mode) sfx_play(RL_SFX_CLUNK, 220, 128, 1.0f);
        if(p->lights != last_lights) sfx_play(RL_SFX_CLICK, 200, 128, 1.0f);
        if(p->hit > 2.5f) sfx_play(RL_SFX_HIT, (int)clampf(p->hit * 40.0f, 60, 255), 128, 0.9f + 0.2f * frand());
        static float bump_cool;
        bump_cool -= dt;
        float jolt = 0;
        for(int i = 0; i < 4; i++) {
            float dv = (p->wh[i].comp - p->wh[i].comp_prev) * 240.0f;
            if(dv > jolt) jolt = dv;
        }
        if(jolt > 1.6f && bump_cool <= 0) {
            sfx_play(jolt > 3.0f ? RL_SFX_BUMP_HEAVY : RL_SFX_BUMP_LIGHT, (int)clampf(jolt * 50.0f, 60, 230), 128, 1.0f);
            bump_cool = 0.18f;
        }
        static float last_water;
        if(p->water > 0.15f && last_water <= 0.15f && spd > 2.0f) sfx_play(RL_SFX_SPLASH, 220, 128, 1.0f);
        last_water = p->water;
        /* The Kestrel's turbo sighs when the throttle snaps shut high up. */
        blowoff_cool -= dt;
        static float last_load;
        if(p->model == 1 && last_load > 0.7f && p->load < 0.2f && p->rpm > 2600.0f && blowoff_cool <= 0) {
            sfx_play(RL_SFX_BLOWOFF, 110, 128, 1.0f);
            blowoff_cool = 1.5f;
        }
        last_load = p->load;
    }
    last_gear = p->gear;
    last_mode = p->mode;
    last_lights = p->lights;
}
