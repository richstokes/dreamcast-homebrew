/* DreamKarts audio.

   Music is a playlist of stereo ADPCM songs stored back to back as one
   seamless loop and streamed from the disc, so the songs play in order and the
   last flows into the first. The title screen opens on the theme; each race
   jumps to the start of a random other song. Effects live in AICA RAM as
   16-bit mono PCM.

   The player's engine (two crossfaded loops), tyre squeal, spark crackle,
   sand rumble and brake screech are looping hardware voices. Each one owns a channel reserved
   with snd_sfx_chn_alloc(): snd_sfx_play() hands out channels round-robin
   and skips only reserved ones, so an unreserved loop is soon replaced by a
   one-shot, after which its pitch updates retune the wrong sound. Pitch and
   volume then change in place with AICA channel UPDATE commands (the
   hardware pitch register), smoothed per frame and sent only on change.

   The final-lap music speed-up retunes the stream's two channels the same
   way. Restarting the stream instead refilled its whole AICA buffer
   synchronously (16.5 ms on hardware, a dropped vblank) and skipped ahead
   by the unplayed buffer. */
#include "audio.h"
#include "render.h"
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <kos/fs.h>
#include <kos/thread.h>
#include <kos/mutex.h>
#include <malloc.h>
#include <fcntl.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <dc/g2bus.h>
#include <stddef.h>

#define MUSIC_BUFFER_BYTES 16384
/* The KOS AICA driver's aica_channel_t[64] in sound RAM (arm/aica_cmd_iface.h). */
#define AICA_MEM_CHANNELS 0x020000

extern const uint8_t dk_sfx_pak[];
/* The music streams from the disc (the CDI, or under dcload the project
   directory mapped at /pc): 9 MB that linked into the executable left the
   console's 16 MB within a megabyte of full. A reader thread keeps a ring
   full ahead of the AICA stream, so a GD-ROM seek never stalls a frame;
   the stream's callback serves from the ring, silence if it ever runs
   dry. */
#define MUSIC_PATH      "/cd/music.adp"
#define RING_BYTES      (256 * 1024)
#define RING_CHUNK      (32 * 1024)
#define RING_PREFILL    (96 * 1024)      /* before a song starts */
static uint8_t *ring;
static file_t music_file = FILEHND_INVALID;
static mutex_t ring_mx;
static size_t ring_r, ring_w, ring_fill;     /* read index, write index, bytes waiting */
static volatile uint32_t seek_gen;           /* bumped with a new seek_pos; the reader restarts there */
static volatile size_t seek_pos;
static volatile bool reader_quit;
static bool music_pending;                   /* a song is set; the stream starts once the ring is primed */
/* Music from /pc (dcload) is read over the network with interrupts off,
   a frame or two lost about once a second, which spoils any timing on the
   console. There the stream runs on silence with no reader, unless the
   build asks for it (make hw MUSIC=1, -DDK_PC_MUSIC). */
static bool music_silent;

static sfxhnd_t sfx[DK_SFX_COUNT];
static bool ready;
static snd_stream_hnd_t music = SND_STREAM_INVALID;
static size_t music_cursor, music_bytes;
static float music_volume;
static int music_target;
static bool music_playing;
static bool music_off;
static float music_rate = 1.0f;
static int music_song;          /* song the last start opened with */
static int music_starts;
/* Playback clock for the sound test: frames into music.adpcm, advanced by
   real time at the current rate (integer frames plus a carried fraction,
   since float alone loses frames this far into the playlist). */
static uint32_t music_pos;
static float music_pos_frac;
static uint64_t music_clock_us;
static int music_chn[2] = { -1, -1 };
#ifdef DK_AUTOTEST
static uint32_t at_rate_us;
static size_t at_fed, at_fed_rate;      /* stream bytes fed: total, at the change */
static uint64_t at_t0, at_t_rate;
#endif
static uint8_t wrap_buffer[MUSIC_BUFFER_BYTES * 2] __attribute__((aligned(32)));

typedef struct {
    int channel;
    int last_freq, last_vol;
    float vol, rate;
} Voice;

enum { V_ENGINE_LOW, V_ENGINE_HIGH, V_DRIFT, V_SPARKS, V_OFFROAD, V_BRAKE, V_COUNT };
/* One set of loops per human player. */
static Voice voice_sets[MAX_PLAYERS][V_COUNT];
/* The biplane's drone: one more loop on its own reserved channel, started
   the first time it is heard and kept for the rest of the run. */
static Voice plane_voice = { -1, -1, -1, 0, 0 };
static int plane_pan = -1;
#define voices voice_sets[0]
static const int voice_sfx[V_COUNT] = {
    DK_SFX_ENGINE_LOW, DK_SFX_ENGINE_HIGH, DK_SFX_DRIFT, DK_SFX_SPARKS, DK_SFX_OFFROAD, DK_SFX_BRAKE
};
static bool voices_started;
/* Free revs on the grid: throttle held during the countdown blips the
   engine up, letting go lets it fall back; after GO it hands over to the
   speed-driven revs instead of dropping. */
static float grid_rev[MAX_PLAYERS], grid_flutter[MAX_PLAYERS];
static int grid_held[MAX_PLAYERS];  /* frames the last "held" report lasts */

void audio_kart_throttle(int slot, bool held) {
    /* A few frames' grace: a render frame can pass with no game step. */
    grid_held[slot & 1] = held ? 3 : 0;
}

static void *music_reader(void *arg) {
    (void)arg;
    uint32_t gen = 0;
    size_t pos = 0;
    while(!reader_quit) {
        bool seek = false;
        mutex_lock(&ring_mx);
        if(seek_gen != gen) {
            gen = seek_gen;
            pos = seek_pos;
            ring_r = ring_w = ring_fill = 0;
            seek = true;
        }
        size_t fill = ring_fill, w = ring_w;
        mutex_unlock(&ring_mx);
        if(fill + RING_CHUNK > RING_BYTES) {
            thd_sleep(8);
            continue;
        }
        if(seek) fs_seek(music_file, (off_t)pos, SEEK_SET);
        size_t n = RING_CHUNK;
        if(pos + n > music_bytes) n = music_bytes - pos;
        if(w + n > RING_BYTES) n = RING_BYTES - w;      /* a chunk never wraps the ring */
        ssize_t got = fs_read(music_file, ring + w, n);
        if(got <= 0) {
            /* A read error: back to the top of the playlist and try again. */
            pos = 0;
            fs_seek(music_file, 0, SEEK_SET);
            thd_sleep(20);
            continue;
        }
        pos += (size_t)got;
        if(pos >= music_bytes) {
            pos = 0;
            fs_seek(music_file, 0, SEEK_SET);   /* the last song flows into the first */
        }
        mutex_lock(&ring_mx);
        if(seek_gen == gen) {                   /* a seek while reading: the data is stale */
            ring_w = (w + (size_t)got) % RING_BYTES;
            ring_fill += (size_t)got;
        }
        mutex_unlock(&ring_mx);
    }
    return NULL;
}

static size_t ring_waiting(void) {
    mutex_lock(&ring_mx);
    size_t fill = ring_fill;
    mutex_unlock(&ring_mx);
    return fill;
}

static void *music_callback(snd_stream_hnd_t hnd, int req, int *got) {
    (void)hnd;
    size_t want = (size_t)req;
    if(music_bytes == 0 || req <= 0) {
        *got = 0;
        return NULL;
    }
    if(want > sizeof(wrap_buffer)) want = sizeof(wrap_buffer);
    mutex_lock(&ring_mx);
    size_t fill = ring_fill, r = ring_r;
    mutex_unlock(&ring_mx);
    if(fill < want) {
        *got = 0;              /* the stream plays silence for this block */
        return NULL;
    }
#ifdef DK_AUTOTEST
    at_fed += want;
#endif
    void *out;
    if(r + want <= RING_BYTES) {
        out = ring + r;
    }
    else {
        size_t first = RING_BYTES - r;
        memcpy(wrap_buffer, ring + r, first);
        memcpy(wrap_buffer + first, ring, want - first);
        out = wrap_buffer;
    }
    mutex_lock(&ring_mx);
    ring_r = (r + want) % RING_BYTES;
    ring_fill -= want;
    mutex_unlock(&ring_mx);
    *got = (int)want;
    return out;
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
        sfx[i] = snd_sfx_load_raw_buf((char *)(dk_sfx_pak + a->offset), a->bytes, a->rate, 16, 1);
        if(sfx[i] == SFXHND_INVALID) printf("DreamKarts audio: effect %d failed to load\n", i);
        else loaded++;
    }
    music_file = fs_open(MUSIC_PATH, O_RDONLY);
    if(music_file == FILEHND_INVALID) music_file = fs_open("/cd/MUSIC.ADP", O_RDONLY);
    /* Under dcload the project directory (make hw) or captures/hw (make
       hwbench, which copies the music there) is mapped at /pc. */
    bool from_pc = music_file == FILEHND_INVALID;
    if(music_file == FILEHND_INVALID) music_file = fs_open("/pc/disc/music.adp", O_RDONLY);
    if(music_file == FILEHND_INVALID) music_file = fs_open("/pc/music.adp", O_RDONLY);
#ifndef DK_PC_MUSIC
    music_silent = from_pc && music_file != FILEHND_INVALID;
#else
    (void)from_pc;
#endif
    if(music_file == FILEHND_INVALID) {
        printf("DreamKarts audio: no %s on the disc; no music\n", MUSIC_PATH);
        music_bytes = 0;
    }
    else {
        ssize_t total = fs_total(music_file);
        music_bytes = total > 0 ? (size_t)total : 0;
        if(music_bytes != DK_MUSIC_BYTES)
            printf("DreamKarts audio: %s is %lu bytes, the build expects %lu\n", MUSIC_PATH, (unsigned long)music_bytes,
                   (unsigned long)DK_MUSIC_BYTES);
        ring = memalign(32, RING_BYTES);
        if(!ring) music_bytes = 0;
        else {
            mutex_init(&ring_mx, MUTEX_TYPE_NORMAL);
            if(music_silent) printf("DreamKarts audio: music from /pc plays silent (make hw MUSIC=1 to hear it)\n");
            else thd_create(1, music_reader, NULL);
        }
    }
    /* snd_stream_alloc() takes the two lowest free channels and keeps them
       private; find out which ones by asking for them first. */
    music_chn[0] = snd_sfx_chn_alloc();
    music_chn[1] = snd_sfx_chn_alloc();
    if(music_chn[0] >= 0) snd_sfx_chn_free(music_chn[0]);
    if(music_chn[1] >= 0) snd_sfx_chn_free(music_chn[1]);
    music = music_bytes ? snd_stream_alloc(music_callback, MUSIC_BUFFER_BYTES) : SND_STREAM_INVALID;
    for(int p = 0; p < MAX_PLAYERS; p++)
        for(int i = 0; i < V_COUNT; i++) voice_sets[p][i].channel = -1;
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
    for(int p = 0; p < MAX_PLAYERS; p++)
        for(int i = 0; i < V_COUNT; i++) {
            Voice *v = &voice_sets[p][i];
            if(v->channel >= 0) {
                snd_sfx_stop(v->channel);
                snd_sfx_chn_free(v->channel);
                v->channel = -1;
            }
        }
    if(plane_voice.channel >= 0) {
        snd_sfx_stop(plane_voice.channel);
        snd_sfx_chn_free(plane_voice.channel);
        plane_voice.channel = -1;
    }
    snd_sfx_stop_all();
    snd_sfx_unload_all();
    snd_stream_shutdown();
    reader_quit = true;
    if(music_file != FILEHND_INVALID) {
        thd_sleep(30);   /* let a read in flight finish before the file closes */
        fs_close(music_file);
        music_file = FILEHND_INVALID;
    }
    ready = false;
}

/* The first start (the title screen) plays the theme, song 0; each later
   one (a race) picks a random song other than the last one opened with. */
static int pick_song(void) {
    if(music_starts++ == 0 || DK_MUSIC_SONG_COUNT < 2) return 0;
    uint32_t r = (uint32_t)timer_us_gettime64();
    r ^= r >> 13;
    r *= 0x5bd1e995u;
    r ^= r >> 15;
    int s = (int)(r % (DK_MUSIC_SONG_COUNT - 1));
    return s >= music_song ? s + 1 : s;
}

/* Restart the stream at the top of a song; fade in from silence or start
   straight at the current target volume. */
static void start_song(int song, bool fade) {
    if(music_playing) snd_stream_stop(music);
    music_song = song;
    music_cursor = music_pos = dk_music_song_start[song];
    music_pos_frac = 0;
#ifndef DK_HW
    printf("DreamKarts audio: music opens on \"%s\"\n", dk_music_song_title[song]);
#endif
    music_rate = 1.0f;
    music_volume = fade || music_off ? 0 : (float)music_target;
#ifdef DK_AUTOTEST
    at_fed = at_fed_rate = 0;
    at_t0 = timer_us_gettime64();
    at_t_rate = 0;
#endif
    /* Point the reader at the song; the stream starts from audio_update
       once the ring holds a few blocks, so a GD-ROM seek never underruns
       the first ones. */
    mutex_lock(&ring_mx);
    seek_pos = music_cursor;
    seek_gen++;
    mutex_unlock(&ring_mx);
    music_pending = true;
    music_clock_us = timer_us_gettime64();
    music_playing = true;
}

void audio_music_start(void) {
    if(!ready || music == SND_STREAM_INVALID) return;
    start_song(pick_song(), true);
}

void audio_music_play(int song) {
    if(!ready || music == SND_STREAM_INVALID || song < 0 || song >= DK_MUSIC_SONG_COUNT) return;
    music_starts++;
    start_song(song, false);
}

int audio_music_now(float *secs, float *len) {
    int song = 0;
    for(int i = 1; i < DK_MUSIC_SONG_COUNT; i++)
        if(music_pos >= dk_music_song_start[i]) song = i;
    uint32_t start = dk_music_song_start[song];
    uint32_t end = song + 1 < DK_MUSIC_SONG_COUNT ? dk_music_song_start[song + 1] : DK_MUSIC_BYTES;
    if(secs) *secs = music_playing ? (float)(music_pos - start) / DK_MUSIC_RATE : 0;
    if(len) *len = (float)(end - start) / DK_MUSIC_RATE;
    return song;
}

bool audio_music_playing(void) {
    return music_playing;
}

float audio_music_rate_now(void) {
    return music_rate;
}

/* The AICA driver's copy of the channel says whether it is still the
   music: a stereo ADPCM stream is the only ADPCM sound here. */
static bool music_chn_ok(int chn) {
    if(chn < 0) return false;
    uint32_t type = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CHANNELS +
                               (uint32_t)chn * sizeof(aica_channel_t) + offsetof(aica_channel_t, type));
    return type == AICA_SM_ADPCM_LS;
}

void audio_music_rate(float mul) {
    if(!ready || !music_playing || mul == music_rate) return;
#ifdef DK_AUTOTEST
    uint64_t t0 = timer_us_gettime64();
    if(mul > 1.0f) {
        at_fed_rate = at_fed;
        at_t_rate = t0;
    }
#endif
    music_rate = mul;
    uint32_t freq = (uint32_t)(DK_MUSIC_RATE * mul);
    bool in_place = music_chn_ok(music_chn[0]) && music_chn_ok(music_chn[1]);
    if(in_place) {
        AICA_CMDSTR_CHANNEL(packet, cmd, chan);
        cmd->cmd = AICA_CMD_CHAN;
        cmd->timestamp = 0;
        cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
        chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_FREQ;
        chan->freq = freq;
        snd_sh4_to_aica_stop();
        for(int i = 0; i < 2; i++) {
            cmd->cmd_id = (uint32_t)music_chn[i];
            snd_sh4_to_aica(packet, cmd->size);
        }
        snd_sh4_to_aica_start();
    }
    else {
        snd_stream_stop(music);
        snd_stream_start_adpcm(music, freq, 1);
        snd_stream_volume(music, (int)music_volume);
    }
#ifdef DK_AUTOTEST
    if(mul > 1.0f) at_rate_us = (uint32_t)(timer_us_gettime64() - t0);
#endif
    if(!in_place)
        printf("DreamKarts audio: music channels %d/%d not found; restarted stream\n", music_chn[0], music_chn[1]);
}

#ifdef DK_AUTOTEST
void audio_autotest_report(void) {
    uint64_t now = timer_us_gettime64();
    /* Stereo 4-bit ADPCM: bytes per second equal samples per second. The
       first fill is a whole buffer ahead, so skip it in the "before" rate. */
    size_t prefill = MUSIC_BUFFER_BYTES * 2;
    double before = at_t_rate > at_t0 && at_fed_rate > prefill ?
        (double)(at_fed_rate - prefill) * 1e6 / (double)(at_t_rate - at_t0) : 0;
    double after = at_t_rate && now > at_t_rate ?
        (double)(at_fed - at_fed_rate) * 1e6 / (double)(now - at_t_rate) : 0;
    printf("DK AUTOTEST: music speed-up took %luus, stream %.0f B/s before, %.0f B/s after (rate %d)\n",
           (unsigned long)at_rate_us, before, after, DK_MUSIC_RATE);
}
#endif

void audio_music_target(int volume) {
    music_target = volume;
}

void audio_music_enable(bool on) {
    music_off = !on;
}

bool audio_music_enabled(void) {
    return !music_off;
}

void audio_update(float dt) {
    if(!ready || music == SND_STREAM_INVALID || !music_playing) return;
    if(music_pending) {
        if(!music_silent && ring_waiting() < RING_PREFILL) {
            music_clock_us = timer_us_gettime64();   /* the song's clock starts with it */
            return;
        }
        music_pending = false;
        snd_stream_start_adpcm(music, DK_MUSIC_RATE, 1);
        snd_stream_volume(music, (int)music_volume);
    }
    uint64_t now = timer_us_gettime64();
    float adv = (float)(now - music_clock_us) * (DK_MUSIC_RATE * 1e-6f) * music_rate + music_pos_frac;
    music_clock_us = now;
    uint32_t whole = (uint32_t)adv;
    music_pos_frac = adv - (float)whole;
    music_pos = (music_pos + whole) % DK_MUSIC_BYTES;
    float target = music_off ? 0.0f : (float)music_target;
    if(music_volume != target) {
        float step = 200.0f * dt;
        music_volume = approach(music_volume, target, step);
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
    d.freq = (int)(dk_sfx_assets[id].rate * rate);
    int chn = snd_sfx_play_ex(&d);
    for(int i = 0; i < V_COUNT * MAX_PLAYERS; i++)
        if(chn >= 0 && chn == voice_sets[i / V_COUNT][i % V_COUNT].channel)
            printf("DreamKarts audio: BUG one-shot %d took loop channel %d\n", id, chn);
}

void audio_sfx(int id, int vol, int pan) {
    audio_sfx_pitch(id, vol, pan, 1.0f);
}

static void start_voices(void) {
    if(voices_started || !ready) return;
    for(int i = 0; i < V_COUNT * MAX_PLAYERS; i++) {
        Voice *v = &voice_sets[i / V_COUNT][i % V_COUNT];
        int id = voice_sfx[i % V_COUNT];
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
        v->channel = snd_sfx_play_ex(&d);
        if(v->channel < 0) snd_sfx_chn_free(chn);
        v->last_freq = v->last_vol = -1;
    }
    voices_started = true;
#ifdef DK_AUTOTEST   /* once per race: keep console builds quiet under dcload */
    printf("DreamKarts audio: loop voices on reserved channels %d-%d and %d-%d\n", voice_sets[0][0].channel,
           voice_sets[0][V_COUNT - 1].channel, voice_sets[1][0].channel, voice_sets[1][V_COUNT - 1].channel);
#endif
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

void audio_plane(int vol, int pan, float rate) {
    if(!ready || sfx[DK_SFX_PLANE] == SFXHND_INVALID) return;
    Voice *v = &plane_voice;
    if(v->channel < 0) {
        if(vol <= 0) return;
        int chn = snd_sfx_chn_alloc();
        if(chn < 0) return;
        sfx_play_data_t d;
        memset(&d, 0, sizeof(d));
        d.chn = chn;
        d.idx = sfx[DK_SFX_PLANE];
        d.vol = 0;
        d.pan = 128;
        d.loop = 1;
        d.freq = (int)dk_sfx_assets[DK_SFX_PLANE].rate;
        d.loopend = dk_sfx_assets[DK_SFX_PLANE].bytes / 2;
        v->channel = snd_sfx_play_ex(&d);
        if(v->channel < 0) {
            snd_sfx_chn_free(chn);
            return;
        }
        v->last_freq = v->last_vol = -1;
    }
    vol = vol < 0 ? 0 : vol > 255 ? 255 : vol;
    voice_set(v, (int)(dk_sfx_assets[DK_SFX_PLANE].rate * clampf(rate, 0.5f, 2.0f)), vol);
    if(vol > 0 && pan != plane_pan) {
        AICA_CMDSTR_CHANNEL(packet, cmd, chan);
        cmd->cmd = AICA_CMD_CHAN;
        cmd->timestamp = 0;
        cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
        cmd->cmd_id = (uint32_t)v->channel;
        chan->cmd = AICA_CH_CMD_UPDATE | AICA_CH_UPDATE_SET_PAN;
        chan->pan = (uint32_t)pan;
        snd_sh4_to_aica(packet, cmd->size);
        plane_pan = pan;
    }
}

void audio_kart_loops(int slot, const Kart *k, float dt) {
    if(!ready) return;
    if(k) start_voices();
    if(!voices_started) return;
    float target[V_COUNT] = {0};
    float rate[V_COUNT] = {1, 1, 1, 1, 1, 1};
    int gs = slot & 1;
    float g = grid_rev[gs];
    g = grid_held[gs] > 0 && k ? fminf(1.0f, g + dt * 2.6f) : fmaxf(0.0f, g - dt * 1.4f);
    grid_rev[gs] = k ? g : 0.0f;
    if(grid_held[gs] > 0) grid_held[gs]--;
    if(k) {
        float top = cc_params[CC_150].top_speed;
        float n = clampf(fabsf(k->speed) / top, 0, 1.35f);
        bool boost = k->boost_t > 0;
        float rev = n + (boost ? 0.15f : 0) + (k->burnout_t > 0 ? 0.6f : 0) + (!k->grounded ? 0.08f : 0);
        /* Eased so a blip leaps up, then settles. */
        if(g > 0) rev = fmaxf(rev, 1.05f * g * g * (3.0f - 2.0f * g));
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
        /* Hard braking: louder the faster it starts, and the pitch sags as
           the kart slows. */
        if(k->skid && k->grounded && k->surface != SURF_SAND) {
            float b = clampf((k->speed - 9.0f) / (top - 9.0f), 0, 1);
            target[V_BRAKE] = 90.0f + 80.0f * b;
            rate[V_BRAKE] = 0.86f + 0.18f * b;
        }
        /* Two engines at once: trim each so the mix doesn't swamp the music. */
        if(game.num_players > 1)
            for(int i = 0; i < V_COUNT; i++) target[i] *= 0.75f;
    }
    /* Held flat out on the grid, the rev limiter cuts in and out: a pitch
       stutter applied after the smoothing so it isn't averaged away. */
    float limiter = 1.0f;
    if(g >= 1.0f) {
        grid_flutter[gs] += dt;
        if(fmodf(grid_flutter[gs] * 11.0f, 1.0f) < 0.35f) limiter = 0.94f;
    }
    snd_sh4_to_aica_stop();
    for(int i = 0; i < V_COUNT; i++) {
        Voice *v = &voice_sets[slot & 1][i];
        float speed = (i == V_DRIFT || i == V_SPARKS || i == V_BRAKE) ? 900.0f : 500.0f;
        v->vol = approach(v->vol, target[i], speed * dt);
        /* Glide pitch like a real engine instead of stepping on boosts. */
        if(v->rate <= 0) v->rate = rate[i];
        v->rate += (rate[i] - v->rate) * smooth_k(i <= V_ENGINE_HIGH ? 7.0f : 14.0f, dt);
        int freq = (int)(DK_SFX_RATE * clampf(v->rate, 0.3f, 2.6f) * (i <= V_ENGINE_HIGH ? limiter : 1.0f));
        freq = (freq + 32) & ~63;   /* avoid a command for tiny pitch changes */
        voice_set(v, freq, (int)v->vol);
#ifdef DK_AT_BRAKE
        static int brake_on;
        if(i == V_BRAKE && slot == 0 && (v->vol > 0) != brake_on) {
            brake_on = v->vol > 0;
            printf("DK BRAKE %.2f %s speed %.1f target %.0f\n", game.race_time, brake_on ? "on" : "off",
                   k ? k->speed : 0.0f, target[i]);
        }
#endif
    }
    snd_sh4_to_aica_start();
}

/* Voice pitch by colour preset (CLASSIC, HAWK, MANGO, CORAL, PEBBLE,
   MIDNIGHT, HERO, LIME): the dark ones low, the bright ones squeaky. */
static const float chao_pitch[COLOUR_COUNT] = {1.0f, 1.12f, 1.05f, 0.96f, 0.9f, 0.85f, 1.07f, 1.17f};

/* Its own generator, so voices don't change the game's random sequence. */
static uint32_t voice_rand(void) {
    static uint32_t s = 0x2c4a0f1du;
    s = s * 1664525u + 1013904223u;
    return s >> 8;
}

void audio_chao(int mood, int colour, int vol, int pan) {
    static const int hi[] = {DK_SFX_CHAO, DK_SFX_CHAO_HEY, DK_SFX_CHAO_CHU, DK_SFX_CHAO_YAY};
    static const int ouch[] = {DK_SFX_CHAO_WAH, DK_SFX_CHAO_BUU, DK_SFX_CHAO_HYA};
    static int last[2] = {-1, -1};
    const int *set = mood == CHAO_OUCH ? ouch : hi;
    int n = mood == CHAO_OUCH ? 3 : 4, m = mood == CHAO_OUCH;
    int pick = (int)(voice_rand() % (uint32_t)(n - (last[m] >= 0)));
    if(last[m] >= 0 && pick >= last[m]) pick++;   /* skip the last clip */
    last[m] = pick;
    float pitch = chao_pitch[(colour % COLOUR_COUNT + COLOUR_COUNT) % COLOUR_COUNT] *
                  (0.97f + 0.06f * (float)(voice_rand() & 255) / 255.0f);
    audio_sfx_pitch(set[pick], vol, pan, pitch);
#ifdef DK_VOICE_TRACE
    printf("DK VOICE %.2f %s %s vol %d pitch %.2f\n", game.race_time, dk_sfx_label[set[pick]],
           kart_colours[(colour % COLOUR_COUNT + COLOUR_COUNT) % COLOUR_COUNT].name, vol, pitch);
#endif
}

/* Bumping into another kart: sometimes the Chao complains, often when a
   player is in the bump and now and then when two CPUs are. Each Chao
   waits a while before it speaks up again and voices start at least 1.2 s
   apart, so a scrum in the pack doesn't turn into a choir. */
static bool bump_voice(const Kart *k) {
    static uint64_t next_us[MAX_KARTS], any_us;
    int i = (int)(k - karts);
    uint64_t now = timer_us_gettime64();
    if(i < 0 || i >= MAX_KARTS || now < next_us[i] || now < any_us) return false;
    bool player = kart_is_human(i) || kart_is_human(k->bump_with);
    if(voice_rand() % 100 >= (player ? 50u : 12u)) {
        next_us[i] = now + 700000;   /* the same scrape won't get another roll */
        return false;
    }
    next_us[i] = now + 3500000;
    any_us = now + 1200000;
    return true;
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
        /* Rivals only make the big noises, and say hi when they wave. */
        ev &= EV_BOOST | EV_DASH | EV_WALL | EV_KART_BUMP | EV_WAVE | EV_FALL | EV_ITEM_GET | EV_SPEAR | EV_SPIN |
              EV_SHIELD_UP | EV_SHIELD_POP | EV_MINE | EV_MINE_HIT;
    }
    if(ev & EV_ITEM_GET) audio_sfx(DK_SFX_EMERALD, vol * 190 / 255, pan);
    if(ev & EV_SPEAR) audio_sfx(DK_SFX_SPEAR, vol * 215 / 255, pan);
    if(ev & EV_MINE) audio_sfx(DK_SFX_MINE, vol * 180 / 255, pan);
    if(ev & EV_MINE_HIT) audio_sfx(DK_SFX_MINE_HIT, vol * 235 / 255, pan);
    if(ev & EV_SPIN) {
        if(!(ev & EV_MINE_HIT)) audio_sfx(DK_SFX_SPEAR_HIT, vol * 225 / 255, pan);
        audio_chao(CHAO_OUCH, k->voice, vol * 210 / 255, pan);
    }
    if(ev & EV_SHIELD_UP) audio_sfx(DK_SFX_BARRIER, vol * 190 / 255, pan);
    if(ev & EV_SHIELD_POP) audio_sfx(DK_SFX_BARRIER_POP, vol * 215 / 255, pan);
    if(ev & EV_FALL) audio_chao(CHAO_OUCH, k->voice, vol * 220 / 255, pan);   /* over the edge */
    if(ev & EV_WAVE) audio_chao(CHAO_HI, k->voice, vol * 190 / 255, pan);
    if((ev & EV_KART_BUMP) && bump_voice(k)) audio_chao(CHAO_OUCH, k->voice, vol * 200 / 255, pan);
    if(ev & EV_HOP) audio_sfx(DK_SFX_HOP, vol * 150 / 255, pan);
    if(ev & EV_LAND) audio_sfx(DK_SFX_LAND, vol * 170 / 255, pan);
    if(ev & EV_JUMP) {
        audio_sfx(DK_SFX_WHOOSH, vol * 220 / 255, pan);
        audio_sfx(DK_SFX_CHAO_HAPPY, vol * 170 / 255, pan);
    }
    if(ev & EV_CHARGE_JUMP) audio_sfx_pitch(DK_SFX_HOP, vol * 200 / 255, pan, 0.75f);
    if(ev & EV_CHARGE1) audio_sfx(DK_SFX_CHARGE1, vol * 150 / 255, pan);
    if(ev & EV_CHARGE2) audio_sfx(DK_SFX_CHARGE2, vol * 150 / 255, pan);
    if(ev & EV_CHARGE3) audio_sfx(DK_SFX_CHARGE3, vol * 165 / 255, pan);
    if(ev & (EV_BOOST | EV_ROCKET)) audio_sfx(DK_SFX_BOOST, vol * 220 / 255, pan);
    if(ev & EV_DASH) audio_sfx(DK_SFX_DASH, vol * 210 / 255, pan);
    if(ev & EV_WALL) audio_sfx(DK_SFX_BUMP, vol * 210 / 255, pan);
    if(ev & EV_KART_BUMP) audio_sfx(DK_SFX_KART_BUMP, vol * 190 / 255, pan);
}
