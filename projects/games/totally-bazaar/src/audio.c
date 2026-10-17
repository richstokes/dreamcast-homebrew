/* Totally Bazaar audio. */
#include "audio.h"

#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <string.h>

#define MUSIC_BUFFER_BYTES 16384

static sfxhnd_t sfx[JP_SFX_COUNT];
static bool audio_ready;
static snd_stream_hnd_t music = SND_STREAM_INVALID;
static size_t music_cursor;
static size_t music_bytes;
static float music_volume;
static int music_target;
static uint8_t wrap_buffer[MUSIC_BUFFER_BYTES * 2] __attribute__((aligned(32)));

static void *music_callback(snd_stream_hnd_t hnd, int req, int *got) {
    size_t want = (size_t)req;
    size_t left;
    (void)hnd;
    if(music_bytes == 0 || req <= 0) {
        *got = 0;
        return NULL;
    }
    if(want > sizeof(wrap_buffer))
        want = sizeof(wrap_buffer);
    left = music_bytes - music_cursor;
    if(want <= left) {
        const uint8_t *p = jp_music_adpcm + music_cursor;
        music_cursor += want;
        if(music_cursor >= music_bytes)
            music_cursor = 0;
        *got = (int)want;
        return (void *)p;
    }
    /* Seam: stitch the tail and the head into one contiguous block. */
    memcpy(wrap_buffer, jp_music_adpcm + music_cursor, left);
    memcpy(wrap_buffer + left, jp_music_adpcm, want - left);
    music_cursor = want - left;
    *got = (int)want;
    return wrap_buffer;
}

void audio_init(void) {
    int i;
    if(snd_stream_init_ex(2, MUSIC_BUFFER_BYTES) < 0) {
        printf("Totally Bazaar audio: stream init failed; running silently.\n");
        return;
    }
    audio_ready = true;
    for(i = 0; i < JP_SFX_COUNT; ++i) {
        const jp_sfx_asset_t *a = &jp_sfx_assets[i];
        sfx[i] = snd_sfx_load_raw_buf((char *)(jp_sfx_pak + a->offset),
                                      a->bytes, JP_SFX_RATE, 16, 1);
        if(sfx[i] == SFXHND_INVALID)
            printf("Totally Bazaar audio: effect %d failed to load.\n", i);
    }
    music_bytes = (size_t)(jp_music_adpcm_end - jp_music_adpcm);
    music = snd_stream_alloc(music_callback, MUSIC_BUFFER_BYTES);
    printf("Totally Bazaar audio: %d effects, music %lu bytes, stream %d, "
           "%lu KiB AICA RAM free.\n", JP_SFX_COUNT,
           (unsigned long)music_bytes, music,
           (unsigned long)(snd_mem_available() / 1024u));
}

void audio_shutdown(void) {
    if(!audio_ready)
        return;
    if(music != SND_STREAM_INVALID) {
        snd_stream_stop(music);
        snd_stream_destroy(music);
    }
    snd_sfx_unload_all();
    snd_stream_shutdown();
    audio_ready = false;
}

void audio_music_start(void) {
    if(!audio_ready || music == SND_STREAM_INVALID)
        return;
    /* The loop opens mid-crossfade from the last track; start where the
     * first track plays alone (twice its crossfade midpoint), 32-byte aligned. */
    music_cursor = (jp_music_track_start[0] * 2u) & ~31u;
    music_volume = 0.0f;
    snd_stream_start_adpcm(music, JP_MUSIC_RATE, 1);
    snd_stream_volume(music, 0);
}

void audio_music_target(int volume) {
    music_target = volume;
}

void audio_update(float dt) {
    if(!audio_ready || music == SND_STREAM_INVALID)
        return;
    if(music_volume != (float)music_target) {
        float step = 160.0f * dt;
        if(music_volume < music_target) {
            music_volume += step;
            if(music_volume > music_target)
                music_volume = (float)music_target;
        }
        else {
            music_volume -= step;
            if(music_volume < music_target)
                music_volume = (float)music_target;
        }
        snd_stream_volume(music, (int)music_volume);
    }
    snd_stream_poll(music);
}

int audio_music_track(void) {
    int i, track = JP_MUSIC_TRACKS - 1;   /* before the first start: wrap fade */
    for(i = 0; i < JP_MUSIC_TRACKS; ++i)
        if(music_cursor >= jp_music_track_start[i])
            track = i;
    return track;
}

void audio_play_pitched(int id, int vol, int pan, float rate_scale) {
    sfx_play_data_t d;
    if(!audio_ready || id < 0 || id >= JP_SFX_COUNT || sfx[id] == SFXHND_INVALID)
        return;
    memset(&d, 0, sizeof(d));
    d.chn = -1;
    d.idx = sfx[id];
    d.vol = vol;
    d.pan = pan;
    d.freq = (int)(JP_SFX_RATE * rate_scale);
    snd_sfx_play_ex(&d);
}

void audio_play(int id, int vol, int pan) {
    if(!audio_ready || id < 0 || id >= JP_SFX_COUNT || sfx[id] == SFXHND_INVALID)
        return;
    snd_sfx_play(sfx[id], vol, pan);
}
