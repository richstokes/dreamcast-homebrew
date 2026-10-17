/* DreamKarts audio.

   Music is one stereo ADPCM loop streamed from RAM. Effects live in AICA
   RAM as 16-bit mono PCM.

   The player's engine (two crossfaded loops), tyre squeal, spark crackle and
   sand rumble are looping hardware voices. Each one owns a channel reserved
   with snd_sfx_chn_alloc(): snd_sfx_play() hands out channels round-robin
   and skips only reserved ones, so an unreserved loop is soon replaced by a
   one-shot, after which its pitch updates retune the wrong sound. Pitch and
   volume then change in place with AICA channel UPDATE commands (the
   hardware pitch register), smoothed per frame and sent only on change. */
#include "audio.h"
#include "render.h"
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <dc/sound/aica_comm.h>

#define MUSIC_BUFFER_BYTES 16384

extern const uint8_t dk_sfx_pak[];
extern const uint8_t dk_music_adpcm[];
extern const uint8_t dk_music_adpcm_end[];

static sfxhnd_t sfx[DK_SFX_COUNT];
static bool ready;
static snd_stream_hnd_t music = SND_STREAM_INVALID;
static size_t music_cursor, music_bytes;
static float music_volume;
static int music_target;
static bool music_playing;
static float music_rate = 1.0f;
static uint8_t wrap_buffer[MUSIC_BUFFER_BYTES * 2] __attribute__((aligned(32)));

typedef struct {
    int channel;
    int last_freq, last_vol;
    float vol, rate;
} Voice;

enum { V_ENGINE_LOW, V_ENGINE_HIGH, V_DRIFT, V_SPARKS, V_OFFROAD, V_COUNT };
static Voice voices[V_COUNT];
static const int voice_sfx[V_COUNT] = {
    DK_SFX_ENGINE_LOW, DK_SFX_ENGINE_HIGH, DK_SFX_DRIFT, DK_SFX_SPARKS, DK_SFX_OFFROAD
};
static bool voices_started;

static void *music_callback(snd_stream_hnd_t hnd, int req, int *got) {
    (void)hnd;
    size_t want = (size_t)req;
    if(music_bytes == 0 || req <= 0) {
        *got = 0;
        return NULL;
    }
    if(want > sizeof(wrap_buffer)) want = sizeof(wrap_buffer);
    size_t left = music_bytes - music_cursor;
    if(want <= left) {
        const uint8_t *p = dk_music_adpcm + music_cursor;
        music_cursor += want;
        if(music_cursor >= music_bytes) music_cursor = 0;
        *got = (int)want;
        return (void *)p;
    }
    memcpy(wrap_buffer, dk_music_adpcm + music_cursor, left);
    memcpy(wrap_buffer + left, dk_music_adpcm, want - left);
    music_cursor = want - left;
    *got = (int)want;
    return wrap_buffer;
}

void audio_init(void) {
    if(snd_stream_init_ex(2, MUSIC_BUFFER_BYTES) < 0) {
        printf("DreamKarts audio: stream init failed; running silently\n");
        return;
    }
    ready = true;
    int loaded = 0;
    for(int i = 0; i < DK_SFX_COUNT; i++) {
        const dk_sfx_asset_t *a = &dk_sfx_assets[i];
        sfx[i] = snd_sfx_load_raw_buf((char *)(dk_sfx_pak + a->offset), a->bytes, DK_SFX_RATE, 16, 1);
        if(sfx[i] == SFXHND_INVALID) printf("DreamKarts audio: effect %d failed to load\n", i);
        else loaded++;
    }
    music_bytes = (size_t)(dk_music_adpcm_end - dk_music_adpcm);
    music = snd_stream_alloc(music_callback, MUSIC_BUFFER_BYTES);
    for(int i = 0; i < V_COUNT; i++) voices[i].channel = -1;
    printf("DreamKarts audio: %d/%d effects, music %lu bytes, %lu KiB AICA RAM free\n",
           loaded, DK_SFX_COUNT, (unsigned long)music_bytes, (unsigned long)(snd_mem_available() / 1024));
}

void audio_shutdown(void) {
    if(!ready) return;
    if(music != SND_STREAM_INVALID) {
        snd_stream_stop(music);
        snd_stream_destroy(music);
    }
    /* snd_sfx_stop_all() skips reserved channels; stop the loops here. */
    for(int i = 0; i < V_COUNT; i++)
        if(voices[i].channel >= 0) {
            snd_sfx_stop(voices[i].channel);
            snd_sfx_chn_free(voices[i].channel);
            voices[i].channel = -1;
        }
    snd_sfx_stop_all();
    snd_sfx_unload_all();
    snd_stream_shutdown();
    ready = false;
}

void audio_music_start(void) {
    if(!ready || music == SND_STREAM_INVALID) return;
    if(music_playing) snd_stream_stop(music);
    music_cursor = 0;
    music_rate = 1.0f;
    music_volume = 0;
    snd_stream_start_adpcm(music, DK_MUSIC_RATE, 1);
    snd_stream_volume(music, 0);
    music_playing = true;
}

void audio_music_rate(float mul) {
    if(!ready || !music_playing || mul == music_rate) return;
    music_rate = mul;
    snd_stream_stop(music);
    snd_stream_start_adpcm(music, (uint32_t)(DK_MUSIC_RATE * mul), 1);
    snd_stream_volume(music, (int)music_volume);
}

void audio_music_target(int volume) {
    music_target = volume;
}

void audio_update(float dt) {
    if(!ready || music == SND_STREAM_INVALID || !music_playing) return;
    if(music_volume != (float)music_target) {
        float step = 200.0f * dt;
        music_volume = approach(music_volume, (float)music_target, step);
        snd_stream_volume(music, (int)music_volume);
    }
    snd_stream_poll(music);
}

void audio_sfx_pitch(int id, int vol, int pan, float rate) {
    if(!ready || id < 0 || id >= DK_SFX_COUNT || sfx[id] == SFXHND_INVALID || vol <= 0) return;
    sfx_play_data_t d;
    memset(&d, 0, sizeof(d));
    d.chn = -1;
    d.idx = sfx[id];
    d.vol = vol > 255 ? 255 : vol;
    d.pan = pan;
    d.freq = (int)(DK_SFX_RATE * rate);
    int chn = snd_sfx_play_ex(&d);
    for(int i = 0; i < V_COUNT; i++)
        if(chn >= 0 && chn == voices[i].channel)
            printf("DreamKarts audio: BUG one-shot %d took loop channel %d\n", id, chn);
}

void audio_sfx(int id, int vol, int pan) {
    audio_sfx_pitch(id, vol, pan, 1.0f);
}

static void start_voices(void) {
    if(voices_started || !ready) return;
    for(int i = 0; i < V_COUNT; i++) {
        int id = voice_sfx[i];
        if(sfx[id] == SFXHND_INVALID) continue;
        int chn = snd_sfx_chn_alloc();
        if(chn < 0) {
            printf("DreamKarts audio: no free channel for loop %d\n", i);
            continue;
        }
        sfx_play_data_t d;
        memset(&d, 0, sizeof(d));
        d.chn = chn;
        d.idx = sfx[id];
        d.vol = 0;
        d.pan = 128;
        d.loop = 1;
        d.freq = DK_SFX_RATE;
        d.loopstart = 0;
        d.loopend = dk_sfx_assets[id].bytes / 2;
        voices[i].channel = snd_sfx_play_ex(&d);
        if(voices[i].channel < 0) snd_sfx_chn_free(chn);
        voices[i].last_freq = voices[i].last_vol = -1;
    }
    voices_started = true;
    printf("DreamKarts audio: loop voices on reserved channels %d %d %d %d %d\n", voices[0].channel,
           voices[1].channel, voices[2].channel, voices[3].channel, voices[4].channel);
}

static void voice_set(Voice *v, int freq, int vol) {
    if(v->channel < 0) return;
    if(freq == v->last_freq && vol == v->last_vol) return;
    AICA_CMDSTR_CHANNEL(packet, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = (uint32_t)v->channel;
    chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ | AICA_CH_UPDATE_SET_VOL;
    chan->freq = (uint32_t)freq;
    chan->vol = (uint32_t)vol;
    snd_sh4_to_aica(packet, cmd->size);
    v->last_freq = freq;
    v->last_vol = vol;
}

void audio_kart_loops(const Kart *k, float dt) {
    if(!ready) return;
    if(k) start_voices();
    if(!voices_started) return;
    float target[V_COUNT] = {0};
    float rate[V_COUNT] = {1, 1, 1, 1, 1};
    if(k) {
        float top = cc_params[CC_150].top_speed;
        float n = clampf(fabsf(k->speed) / top, 0, 1.35f);
        bool boost = k->boost_t > 0;
        float rev = n + (boost ? 0.15f : 0) + (k->burnout_t > 0 ? 0.6f : 0) + (!k->grounded ? 0.08f : 0);
        float hi = clampf((rev - 0.25f) / 0.5f, 0, 1);
        target[V_ENGINE_LOW] = 175.0f * (1.0f - hi * 0.85f) + 25.0f;
        target[V_ENGINE_HIGH] = 150.0f * hi;
        rate[V_ENGINE_LOW] = 0.70f + rev * 1.1f;
        rate[V_ENGINE_HIGH] = 0.70f + rev * 0.75f;
        if(k->drift == DRIFT_ACTIVE && k->grounded) {
            target[V_DRIFT] = 120.0f;
            rate[V_DRIFT] = 0.92f + 0.12f * k->steer * (float)k->drift_dir;
            target[V_SPARKS] = k->stage == 0 ? 50.0f : 90.0f + 15.0f * k->stage;
            rate[V_SPARKS] = 0.9f + 0.12f * k->stage;
        }
        if(k->surface == SURF_SAND && k->grounded && fabsf(k->speed) > 2.0f)
            target[V_OFFROAD] = 70.0f + 80.0f * clampf(fabsf(k->speed) / 15.0f, 0, 1);
    }
    snd_sh4_to_aica_stop();
    for(int i = 0; i < V_COUNT; i++) {
        Voice *v = &voices[i];
        float speed = (i == V_DRIFT || i == V_SPARKS) ? 900.0f : 500.0f;
        v->vol = approach(v->vol, target[i], speed * dt);
        /* Glide pitch like a real engine instead of stepping on boosts. */
        if(v->rate <= 0) v->rate = rate[i];
        v->rate += (rate[i] - v->rate) * smooth_k(i <= V_ENGINE_HIGH ? 7.0f : 14.0f, dt);
        int freq = (int)(DK_SFX_RATE * clampf(v->rate, 0.3f, 2.6f));
        freq = (freq + 32) & ~63;   /* avoid a command for tiny pitch changes */
        voice_set(v, freq, (int)v->vol);
    }
    snd_sh4_to_aica_start();
}

void audio_kart_events(const Kart *k, bool is_player) {
    uint32_t ev = k->events;
    if(!ev) return;
    int vol = 255, pan = 128;
    if(!is_player) {
        V3 d = vsub(k->pos, r_cam.pos);
        float dist = vlen(d);
        if(dist > 60.0f) return;
        vol = (int)(200.0f * clampf(1.0f - dist / 60.0f, 0, 1));
        float side = vdot(vnorm(d), r_cam.right);
        pan = (int)clampf(128.0f + side * 100.0f, 0, 255);
        /* Rivals only make the big noises. */
        ev &= EV_BOOST | EV_DASH | EV_WALL | EV_KART_BUMP;
    }
    if(ev & EV_HOP) audio_sfx(DK_SFX_HOP, vol * 150 / 255, pan);
    if(ev & EV_LAND) audio_sfx(DK_SFX_LAND, vol * 170 / 255, pan);
    if(ev & EV_CHARGE_JUMP) audio_sfx_pitch(DK_SFX_HOP, vol * 200 / 255, pan, 0.75f);
    if(ev & EV_CHARGE1) audio_sfx(DK_SFX_CHARGE1, vol * 150 / 255, pan);
    if(ev & EV_CHARGE2) audio_sfx(DK_SFX_CHARGE2, vol * 150 / 255, pan);
    if(ev & EV_CHARGE3) audio_sfx(DK_SFX_CHARGE3, vol * 165 / 255, pan);
    if(ev & (EV_BOOST | EV_ROCKET)) audio_sfx(DK_SFX_BOOST, vol * 220 / 255, pan);
    if(ev & EV_DASH) audio_sfx(DK_SFX_DASH, vol * 210 / 255, pan);
    if(ev & EV_WALL) audio_sfx(DK_SFX_BUMP, vol * 210 / 255, pan);
    if(ev & EV_KART_BUMP) audio_sfx(DK_SFX_KART_BUMP, vol * 190 / 255, pan);
}
