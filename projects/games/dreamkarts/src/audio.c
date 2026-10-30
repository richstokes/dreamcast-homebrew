/* DreamKarts audio.

   Music is CD-DA: the songs are audio tracks on the disc, which the GD-ROM
   plays by itself into the AICA's CD input, so they cost the SH-4 nothing
   (no stream, no buffer, no decoder). The title screen plays the theme;
   races and the pause menu's NEXT SONG step through the rest in a shuffled
   order, reshuffled each time it runs out, and a song that ends on its own
   hands over to the next. A change of song fades the old one out before
   the drive is sent to the new one (which then fades in), so no song is
   cut off mid-note, and the seek happens in silence. The drive takes its orders from a jukebox thread
   (a GD-ROM command blocks for the seek, ~0.5 s on hardware), which also
   polls the drive to notice a song finishing; the main thread only fades
   the AICA's CD volume register. Under dcload there is normally no disc, so
   the drive is left alone and the music is silent (-DDK_DCLOAD_MUSIC plays
   whatever DreamKarts disc is in the tray). Effects live in AICA RAM as
   16-bit mono PCM.

   The player's engine (two crossfaded loops), tyre squeal, spark crackle,
   sand rumble and brake screech are looping hardware voices. Each one owns a channel reserved
   with snd_sfx_chn_alloc(): snd_sfx_play() hands out channels round-robin
   and skips only reserved ones, so an unreserved loop is soon replaced by a
   one-shot, after which its pitch updates retune the wrong sound. Pitch and
   volume then change in place with AICA channel UPDATE commands (the
   hardware pitch register), smoothed per frame and sent only on change. */
#include "audio.h"
#include "render.h"
#include "save.h"
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/cdrom.h>
#include <dc/fs_dcload.h>
#include <kos/thread.h>
#include <kos/mutex.h>
#include <dc/sound/aica_comm.h>
#include <dc/spu.h>
#include <arch/timer.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

extern const uint8_t dk_sfx_pak[];

static sfxhnd_t sfx[DK_SFX_COUNT];
static bool ready;

/* The disc's audio tracks, matched to the songs in music_tracks.h by
   order: music_tracks is how many the TOC had (0: no music). */
static int music_tracks;
static uint8_t music_track_no[DK_MUSIC_TRACKS];
static bool music_silent;                /* dcload: the drive is not touched */
/* What plays: the song (-1 none), its clock in seconds, and the shuffled
   order the next songs come in. */
static int music_song = -1;
static int music_next = -1;              /* the song waiting for the fade-out */
static float music_pos;
static uint8_t music_order[DK_MUSIC_TRACKS];
static int music_order_n, music_order_i;
static int music_starts, music_ends;     /* songs started and finished (autotest) */
/* Volume: 0..255 as the rest of the game sets it, faded per frame and
   written to the AICA's 4-bit CD level only when that changes. */
static float music_volume;
static int music_target;
static int music_user_vol = SAVE_VOLUME_DEFAULT;   /* the Settings slider */
static int cd_level = -1;
#define FADE_OUT_PER_S 600.0f    /* ~0.35 s from full before a change of song */
#define FADE_IN_PER_S 300.0f     /* ~0.7 s up to the scene's level */
static bool music_off;                   /* the music switch */
static bool music_paused;                /* ...and the drive paused for it */
/* Orders for the jukebox thread, a short queue under jb_mx (a NEXT SONG
   with the music off is a play then a pause); the drive's state as the
   thread last saw it. */
enum { JB_NONE, JB_PLAY, JB_PAUSE, JB_RESUME };
#define JB_QUEUE 4
static mutex_t jb_mx;
static struct { uint8_t cmd, track; } jb_queue[JB_QUEUE];
static int jb_head, jb_count;
static volatile bool jb_playing;         /* a song is going (or paused) in the drive */
static volatile bool jb_ended;           /* the drive stopped on its own: next song */
static volatile int jb_fails;
static volatile bool jb_quit;
static kthread_t *jukebox;

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

static void jb_post(int cmd, int track) {
    mutex_lock(&jb_mx);
    if(jb_count < JB_QUEUE) {
        jb_queue[(jb_head + jb_count) % JB_QUEUE].cmd = (uint8_t)cmd;
        jb_queue[(jb_head + jb_count) % JB_QUEUE].track = (uint8_t)track;
        jb_count++;
    }
    mutex_unlock(&jb_mx);
}

/* The jukebox: runs the drive. cdrom_cdda_play() returns once the drive
   has taken the order, after its seek; cdrom_get_status() is a quick
   syscall. Two seconds after a play the drive is given to have started,
   and from then a status of paused or standby means the track ran out
   (repeat 0); the main thread then queues the next one. */
static void *jukebox_thread(void *arg) {
    (void)arg;
    uint64_t started_us = 0, polled_us = 0;
    bool watching = false;
    while(!jb_quit) {
        mutex_lock(&jb_mx);
        int cmd = JB_NONE, track = 0;
        if(jb_count) {
            cmd = jb_queue[jb_head].cmd;
            track = jb_queue[jb_head].track;
            jb_head = (jb_head + 1) % JB_QUEUE;
            jb_count--;
        }
        mutex_unlock(&jb_mx);
        int err = 0;
        uint64_t now_before = timer_us_gettime64();
        (void)now_before;
        if(cmd == JB_PLAY) {
            err = cdrom_cdda_play((uint32_t)track, (uint32_t)track, 0, CDDA_TRACKS);
            started_us = timer_us_gettime64();
            jb_playing = err == 0;
            watching = err == 0;
            if(err) jb_fails++;
        }
        else if(cmd == JB_PAUSE) {
            err = cdrom_cdda_pause();
            watching = false;
        }
        else if(cmd == JB_RESUME) {
            err = cdrom_cdda_resume();
            started_us = timer_us_gettime64();
            watching = err == 0;
            if(err) jb_post(JB_PLAY, track);   /* the drive forgot the song (stopped while paused) */
        }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
        if(cmd != JB_NONE) {
            static const char *const names[] = {"", "play", "pause", "resume"};
            printf("DK JUKEBOX: %s %d -> %d (%lu ms)\n", names[cmd], track, err,
                   (unsigned long)((timer_us_gettime64() - now_before) / 1000));
        }
#endif
        uint64_t now = timer_us_gettime64();
        if(watching && now - started_us > 2000000 && now - polled_us > 500000) {
            polled_us = now;
            int status = -1;
            if(cdrom_get_status(&status, NULL) == 0 &&
               (status == CD_STATUS_PAUSED || status == CD_STATUS_STANDBY || status == CD_STATUS_OPEN ||
                status == CD_STATUS_NO_DISC)) {
                watching = false;
                jb_playing = false;
                jb_ended = true;
#if defined(DK_AUTOTEST) && !defined(DK_HW)
                printf("DK JUKEBOX: track ended (status %d)\n", status);
#endif
            }
        }
        thd_sleep(cmd == JB_NONE ? 50 : 1);
    }
    return NULL;
}

void audio_disc_init(void) {
    static bool done;
    if(done) return;
    done = true;
#ifndef DK_DCLOAD_MUSIC
    if(dcload_type != DCLOAD_TYPE_NONE) {
        music_silent = true;
        return;
    }
#endif
    int status = -1, type = -1;
    if(cdrom_get_status(&status, &type) < 0 || status == CD_STATUS_OPEN || status == CD_STATUS_NO_DISC) {
        printf("DreamKarts audio: no disc (status %d); no music\n", status);
        return;
    }
    cdrom_reinit();
    cd_toc_t toc;
    if(cdrom_read_toc(&toc, false) != ERR_OK) {
        printf("DreamKarts audio: no TOC; no music\n");
        return;
    }
    int first = (int)TOC_TRACK(toc.first), last = (int)TOC_TRACK(toc.last);
    for(int t = first; t <= last && t >= 1 && t <= 99 && music_tracks < DK_MUSIC_TRACKS; t++)
        if(!(TOC_CTRL(toc.entry[t - 1]) & 4)) music_track_no[music_tracks++] = (uint8_t)t;
    if(music_tracks != DK_MUSIC_TRACKS)
        printf("DreamKarts audio: %d CD-DA tracks on the disc, the build has %d songs\n", music_tracks, DK_MUSIC_TRACKS);
}

void audio_init(void) {
    if(snd_init() < 0) {
        printf("DreamKarts audio: sound init failed; running silently\n");
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
    audio_disc_init();
    spu_cdda_volume(0, 0);   /* songs fade in from silence */
    cd_level = 0;
    if(music_tracks) {
        mutex_init(&jb_mx, MUTEX_TYPE_NORMAL);
        kthread_attr_t attr = {.prio = PRIO_DEFAULT, .label = "dk-jukebox"};
        jukebox = thd_create_ex(&attr, jukebox_thread, NULL);
        if(!jukebox) {
            printf("DreamKarts audio: no jukebox thread; no music\n");
            music_tracks = 0;
        }
    }
    for(int p = 0; p < MAX_PLAYERS; p++)
        for(int i = 0; i < V_COUNT; i++) voice_sets[p][i].channel = -1;
    printf("DreamKarts audio: %d/%d effects, %d CD-DA tracks%s, %lu KiB AICA RAM free\n", loaded, DK_SFX_COUNT,
           music_tracks, music_silent ? " (silent under dcload)" : "", (unsigned long)(snd_mem_available() / 1024));
}

void audio_shutdown(void) {
    if(!ready) return;
    if(jukebox) {
        jb_quit = true;
        thd_join(jukebox, NULL);
        jukebox = NULL;
        if(jb_playing) cdrom_cdda_pause();   /* not on through the reboot to the menu */
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
    snd_shutdown();
    ready = false;
}

/* A fresh shuffle of every song, not opening with the one just played. */
static void music_reshuffle(void) {
    uint32_t r = (uint32_t)timer_us_gettime64() ^ 0x9e3779b9u;
    music_order_n = music_tracks;
    for(int i = 0; i < music_order_n; i++) music_order[i] = (uint8_t)i;
    for(int i = music_order_n - 1; i > 0; i--) {
        r ^= r << 13;
        r ^= r >> 17;
        r ^= r << 5;
        int j = (int)(r % (uint32_t)(i + 1));
        uint8_t t = music_order[i];
        music_order[i] = music_order[j];
        music_order[j] = t;
    }
    if(music_order_n > 1 && music_order[0] == music_song) {
        uint8_t t = music_order[0];
        music_order[0] = music_order[music_order_n - 1];
        music_order[music_order_n - 1] = t;
    }
    music_order_i = 0;
}

static int music_next_song(void) {
    if(music_order_i >= music_order_n) music_reshuffle();
    return music_order[music_order_i++];
}

/* Start a song now: the drive is told to play its track, the song's clock
   restarts and it fades in from silence. */
static void music_start_now(int song) {
    music_song = song;
    music_next = -1;
    music_pos = 0;
    music_starts++;
    jb_ended = false;
    jb_playing = true;
    music_paused = false;
    music_volume = 0;
    spu_cdda_volume(0, 0);
    cd_level = 0;
#ifndef DK_HW
    printf("DreamKarts audio: music opens on \"%s\" (track %d)\n", dk_music_title[song], music_track_no[song]);
#endif
    jb_post(JB_PLAY, music_track_no[song]);
}

/* Change song: straight away if nothing is audible, else once the one
   playing has faded out (audio_update). */
static void music_play_song(int song) {
    if(!ready || !music_tracks || song < 0 || song >= music_tracks) return;
    if(jb_playing && !music_paused && cd_level > 0) music_next = song;
    else music_start_now(song);
}

void audio_music_title(void) {
    int now = music_next >= 0 ? music_next : music_song;
    if(now == DK_MUSIC_TITLE_TRACK && jb_playing) return;
    music_play_song(DK_MUSIC_TITLE_TRACK);
}

void audio_music_start(void) {
    music_play_song(music_next_song());
}

void audio_music_next(void) {
    music_play_song(music_next_song());
}

void audio_music_play(int song) {
    music_play_song(song);
}

void audio_music_volume(int volume) {
    music_user_vol = volume < 0 ? 0 : volume > SAVE_VOLUME_MAX ? SAVE_VOLUME_MAX : volume;
}

int audio_music_now(float *secs, float *len) {
    int song = music_next >= 0 ? music_next : music_song < 0 ? 0 : music_song;
    float total = dk_music_secs[song];
    if(secs) *secs = music_song < 0 ? 0 : fminf(music_pos, total);
    if(len) *len = total;
    return song;
}

bool audio_music_playing(void) {
    return music_tracks > 0 && music_song >= 0;
}

void audio_music_target(int volume) {
    music_target = volume;
}

void audio_music_enable(bool on) {
    music_off = !on;
}

bool audio_music_enabled(void) {
    return !music_off;
}

/* The AICA's CD level is 4 bits of 3 dB steps (15 = full, 0 = off); the
   game's 0..255 is roughly linear amplitude, and the Settings slider takes
   a step off per notch under full. */
static int cd_level_for(float volume) {
    if(volume < 1.0f || music_user_vol <= 0) return 0;
    int steps = volume >= 255.0f ? 0 : (int)(20.0f * log10f(255.0f / volume) / 3.0f + 0.5f);
    steps += SAVE_VOLUME_MAX - music_user_vol;
    return steps >= 15 ? 1 : 15 - steps;
}

#ifdef DK_AUTOTEST
void audio_autotest_report(void) {
    printf("DK AUTOTEST: music: %d CD-DA tracks, %d songs started, %d ended, %d play failures\n", music_tracks,
           music_starts, music_ends, jb_fails);
}
#endif

void audio_update(float dt) {
    if(!ready || !music_tracks || music_song < 0) return;
    if(jb_ended) {
        jb_ended = false;
        music_ends++;
        if(jb_fails >= 3) {
            printf("DreamKarts audio: the drive will not play; giving up on the music\n");
            music_tracks = 0;
            return;
        }
        music_start_now(music_next >= 0 ? music_next : music_next_song());
    }
    if(jb_playing && !music_paused) music_pos += dt;
    /* Fading out ahead of a change of song, or toward the scene's level. */
    float target = music_off || music_next >= 0 ? 0.0f : (float)music_target;
    if(music_volume != target)
        music_volume = approach(music_volume, target, (target < music_volume ? FADE_OUT_PER_S : FADE_IN_PER_S) * dt);
    int level = cd_level_for(music_volume);
    if(level != cd_level) {
        cd_level = level;
        spu_cdda_volume(level, level);
    }
    if(music_next >= 0 && level == 0) {
#if defined(DK_AUTOTEST) && !defined(DK_HW)
        printf("DK JUKEBOX: faded out at %.1f s for the next song\n", (double)music_pos);
#endif
        music_start_now(music_next);
        return;
    }
    /* The switch: paused once faded out (the drive stops spinning), resumed
       as soon as it is back on. */
    if(music_off && !music_paused && level == 0 && jb_playing) {
        music_paused = true;
        jb_post(JB_PAUSE, 0);
    }
    else if(!music_off && music_paused) {
        music_paused = false;
        jb_post(JB_RESUME, music_track_no[music_song]);
    }
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
    if(ev & EV_TRICK) audio_sfx_pitch(DK_SFX_WHOOSH, vol * 190 / 255, pan, 1.45f);
    if(ev & EV_CHARGE1) audio_sfx(DK_SFX_CHARGE1, vol * 150 / 255, pan);
    if(ev & EV_CHARGE2) audio_sfx(DK_SFX_CHARGE2, vol * 150 / 255, pan);
    if(ev & EV_CHARGE3) audio_sfx(DK_SFX_CHARGE3, vol * 165 / 255, pan);
    if(ev & (EV_BOOST | EV_ROCKET)) audio_sfx(DK_SFX_BOOST, vol * 220 / 255, pan);
    if(ev & EV_DASH) audio_sfx(DK_SFX_DASH, vol * 210 / 255, pan);
    if(ev & EV_WALL) audio_sfx(DK_SFX_BUMP, vol * 210 / 255, pan);
    if(ev & EV_KART_BUMP) audio_sfx(DK_SFX_KART_BUMP, vol * 190 / 255, pan);
}
