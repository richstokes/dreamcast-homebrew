/* Sound (audio.h).

   The music is CD-DA: audio tracks on the disc, which the GD-ROM plays by
   itself into the AICA's CD input, so it costs the SH-4 nothing. The songs
   play as a shuffle from the title on, and every lunch starts the next. The drive takes its orders from a jukebox
   thread (a GD-ROM command blocks for the seek, about half a second on the
   console, so never from the frame loop); the main thread only fades the
   AICA's CD volume. Under dcload there is normally no disc, so the drive
   is left alone and there is no music (-DKL_DCLOAD_MUSIC plays whatever
   Kaiju Lunch Break disc is in the tray). Effects live in AICA RAM; the
   voice lines are streamed from the pak in main memory, one at a time. */
#include "audio.h"
#include "score.h"
#include "save.h"
#include "music_tracks.h"
#include "engine/sfx.h"
#include "engine/voice.h"
#include <dc/cdrom.h>
#include <dc/fs_dcload.h>
#include <dc/spu.h>
#include <kos/thread.h>

extern const uint8_t kl_sfx_pak[];
float audio_beam, audio_heli, audio_siren, audio_crowd;

static SfxVoice v_beam, v_heli, v_siren, v_crowd;
static float l_beam, l_heli, l_siren, l_crowd;
static bool ready;

/* ---- The music ---- */
enum { JB_NONE, JB_PLAY, JB_PAUSE, JB_RESUME };
static kthread_t *jukebox;
static volatile int jb_order;            /* the next thing for the drive to do */
static volatile int jb_song;             /* ...and for a play, which song */
static volatile bool jb_playing, jb_ended, jb_quit;
static volatile int jb_status = -1;
/* The disc's audio tracks, matched to music_tracks.h by order. */
static int music_n;
static uint32_t music_fad[KL_MUSIC_TRACKS];
static uint8_t music_no[KL_MUSIC_TRACKS];
/* The shuffle: every song once before any comes round again. */
static uint8_t bag[KL_MUSIC_TRACKS];
static int bag_n, bag_i;
static uint32_t music_rng;
static int music_song = -1;              /* the one in the drive */
static int music_next = -1;              /* the one waiting for the fade-out */
static bool music_paused;
static float music_level;                /* 0-1, what the CD volume is at */
static int music_reg = -1;

/* The jukebox: runs the drive. Two seconds after a play the drive is
   taken to have started, and from then a status of paused or standby
   means the song ran out; the main thread then sends the next. */
static void *jukebox_thread(void *arg) {
    (void)arg;
    uint64_t started_us = 0, polled_us = 0;
    bool watching = false;
    while(!jb_quit) {
        int cmd = jb_order, song = jb_song, err = 0;
        jb_order = JB_NONE;
        if(cmd == JB_PLAY) {
            uint32_t first = music_fad[song], sectors = (uint32_t)(kl_music_secs[song] * 75.0f);
            if(first && sectors > 2) err = cdrom_cdda_play(first, first + sectors - 2, 0, CDDA_SECTORS);
            else err = cdrom_cdda_play(music_no[song], music_no[song], 0, CDDA_TRACKS);
            started_us = timer_us_gettime64();
            jb_playing = watching = err == 0;
        }
        else if(cmd == JB_PAUSE) {
            cdrom_cdda_pause();
            watching = false;
        }
        else if(cmd == JB_RESUME) {
            err = cdrom_cdda_resume();
            started_us = timer_us_gettime64();
            watching = err == 0;
            if(err) jb_order = JB_PLAY;   /* the drive forgot the song (stopped while paused) */
        }
        uint64_t now = timer_us_gettime64();
        if(watching && now - started_us > 2000000 && now - polled_us > 500000) {
            polled_us = now;
            int status = -1;
            if(cdrom_get_status(&status, NULL) == 0) jb_status = status;
            if(status >= 0 &&
               (status == CD_STATUS_PAUSED || status == CD_STATUS_STANDBY || status == CD_STATUS_OPEN || status == CD_STATUS_NO_DISC)) {
                watching = jb_playing = false;
                jb_ended = true;
            }
        }
        thd_sleep(cmd == JB_NONE ? 50 : 1);
    }
    return NULL;
}

static void music_init(void) {
    spu_cdda_volume(0, 0);   /* songs fade in from silence */
#ifndef KL_DCLOAD_MUSIC
    if(dcload_type != DCLOAD_TYPE_NONE) {
        printf("KL audio: no music under dcload\n");
        return;
    }
#endif
    int status = -1, type = -1;
    if(cdrom_get_status(&status, &type) < 0 || status == CD_STATUS_OPEN || status == CD_STATUS_NO_DISC) {
        printf("KL audio: no disc (status %d); no music\n", status);
        return;
    }
    cdrom_reinit();
    cd_toc_t toc;
    if(cdrom_read_toc(&toc, false) != ERR_OK) {
        printf("KL audio: no TOC; no music\n");
        return;
    }
    int first = (int)TOC_TRACK(toc.first), last = (int)TOC_TRACK(toc.last);
    for(int t = first; t <= last && t >= 1 && t <= 99 && music_n < KL_MUSIC_TRACKS; t++)
        if(!(TOC_CTRL(toc.entry[t - 1]) & 4)) {
            music_fad[music_n] = TOC_LBA(toc.entry[t - 1]);
            music_no[music_n++] = (uint8_t)t;
        }
    if(music_n) {
        kthread_attr_t attr = {.prio = PRIO_DEFAULT, .label = "kl-jukebox"};
        jukebox = thd_create_ex(&attr, jukebox_thread, NULL);
        if(!jukebox) music_n = 0;
    }
    printf("KL audio: %d CD-DA tracks on the disc, the build has %d songs\n", music_n, KL_MUSIC_TRACKS);
    audio_music_next();
}

/* The next song of the shuffle; the bag is refilled when it runs out,
   and never opens with the song just heard. */
void audio_music_next(void) {
    if(!music_n) return;
    if(bag_i >= bag_n) {
#ifdef KL_AUTOTEST
        if(!music_rng) music_rng = 0x4b4c;      /* the same order every run */
#else
        if(!music_rng) music_rng = (uint32_t)timer_us_gettime64() | 1;
#endif
        bag_n = music_n;
        bag_i = 0;
        for(int i = 0; i < bag_n; i++) bag[i] = (uint8_t)i;
        for(int i = bag_n - 1; i > 0; i--) {
            music_rng ^= music_rng << 13;
            music_rng ^= music_rng >> 17;
            music_rng ^= music_rng << 5;
            int j = (int)(music_rng % (uint32_t)(i + 1));
            uint8_t t = bag[i];
            bag[i] = bag[j];
            bag[j] = t;
        }
        int last = music_next >= 0 ? music_next : music_song;
        if(bag_n > 1 && bag[0] == last) {
            bag[0] = bag[1];
            bag[1] = (uint8_t)last;
        }
    }
    music_next = bag[bag_i++];
}

/* Once a frame: change song, pause it for the switch, and fade. A change
   fades the old song out before the drive is sent to the new one, so
   nothing is cut off mid-note and the seek happens in silence. */
static void music_update(float dt) {
    if(!music_n) return;
    bool want = !(save.options & SAVE_OPT_NO_MUSIC);
#ifdef KL_FILM
    want = false;   /* the edit lays the music under the cuts */
#endif
    if(jb_ended) {
        jb_ended = false;
        if(music_next < 0) audio_music_next();
    }
    bool changing = music_next >= 0;
    if(want && changing && music_level <= 0 && jb_order == JB_NONE) {
        jb_song = music_song = music_next;
        jb_order = JB_PLAY;
        music_next = -1;
        music_paused = false;
        changing = false;
    }
    else if(want && !changing && music_paused && jb_order == JB_NONE) {
        jb_order = JB_RESUME;
        music_paused = false;
    }
    /* Under the pause menu it drops back; switched off it fades right out,
       and only then is the drive paused. */
    float to = !want || changing || !jb_playing || music_paused ? 0 : game.paused ? 0.5f : 1.0f;
    music_level = approach(music_level, to, dt * 2.5f);
    if(!want && jb_playing && !music_paused && music_level <= 0 && jb_order == JB_NONE) {
        jb_order = JB_PAUSE;
        music_paused = true;
    }
    /* The register is four bits, 15 = full and 3 dB a step down, so the
       level is turned into decibels: 0.5 is -6 dB, two steps. Written when
       it changes. */
    int reg = music_level < 0.02f ? 0 : 15 - (int)(-20.0f * log10f(music_level) / 3.0f + 0.5f);    if(reg != music_reg) {
        music_reg = reg;
        spu_cdda_volume(reg, reg);
    }
}

void audio_init(void) {
    ready = sfx_init(kl_sfx_pak, (const eng_sfx_asset_t *)kl_sfx_assets, KL_SFX_VOICE_FIRST);
    if(!ready) return;
    voice_init();
    music_init();
    sfx_voice_start(&v_beam, KL_SFX_BEAM);
    sfx_voice_start(&v_heli, KL_SFX_HELI);
    sfx_voice_start(&v_siren, KL_SFX_SIREN);
    sfx_voice_start(&v_crowd, KL_SFX_CROWD);
}

void audio_music_state(int *status, int *volume, int *song) {
    *status = jb_status;
    *volume = music_reg;
    *song = music_song;
}

void audio_shutdown(void) {
    voice_shutdown();
    if(!jukebox) return;
    jb_quit = true;
    thd_join(jukebox, NULL);
    jukebox = NULL;
    spu_cdda_volume(0, 0);
    if(jb_playing) cdrom_cdda_pause();
}

/* Only the menus and the welcome make a sound outside a lunch (the title's kaiju is the bot's). */
static bool muted(int id) {
    return game.state == ST_TITLE && id != KL_SFX_MOVE && id != KL_SFX_OK && id != KL_SFX_WELCOME;
}

void audio_sfx_rate(int id, int vol, float rate) {
    if(!ready || muted(id)) return;
    if(id >= KL_SFX_VOICE_FIRST) voice_say(kl_sfx_pak + kl_sfx_assets[id].offset, kl_sfx_assets[id].bytes, kl_sfx_assets[id].rate, vol);
    else sfx_play(id, vol, 128, rate);
}

void audio_sfx(int id, int vol) {
    audio_sfx_rate(id, vol, 1.0f);
}

void audio_at(int id, V3 p, int vol) {
    if(!ready || muted(id)) return;
    int pan;
    float d = sfx_listen(p, &cam, 100.0f, &pan);
    float g = clampf(1.15f - d / 520.0f, 0.12f, 1.0f);
    sfx_play(id, (int)((float)vol * g), pan, 0.92f + frand() * 0.16f);
}

static void ease(float *level, float to, float dt, float rate) {
    *level = approach(*level, to, dt * rate);
}

void audio_update(float dt) {
    if(!ready) return;
    music_update(dt);
    voice_update();
    bool play = game.state == ST_PLAY && !game.paused && game.intro <= 0 && game.outro <= 0;
    ease(&l_beam, play ? audio_beam : 0, dt, 12);
    ease(&l_heli, play ? audio_heli : 0, dt, 3);
    ease(&l_siren, play ? audio_siren : 0, dt, 1.5f);
    ease(&l_crowd, play ? audio_crowd : 0, dt, 2);
    sfx_batch_begin();
    sfx_voice_set(&v_beam, 1, (int)(l_beam * 200), 128);
    sfx_voice_set(&v_heli, 1, (int)(l_heli * 110), 150);
    sfx_voice_set(&v_siren, 1, (int)(l_siren * 60), 100);
    sfx_voice_set(&v_crowd, 1, (int)(l_crowd * 70), 128);
    sfx_batch_end();
    audio_beam = 0;
}
