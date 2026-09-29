/* Tiny synthesized UI clicks, kept resident while browsing the client. */
#include "client.h"
#include <kos.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define CLICK_RATE 22050
#define CLICK_SAMPLES 1024
#define PI 3.14159265f

static sfxhnd_t click = SFXHND_INVALID;
static int sound_ready, channel = -1;
static uint64_t last_click;

void client_menu_audio_shutdown(void) {
    if(!sound_ready) return;
    if(channel >= 0) {
        snd_sfx_stop(channel);
        snd_sfx_chn_free(channel);
        channel = -1;
    }
    if(click != SFXHND_INVALID) {
        snd_sfx_unload(click);
        click = SFXHND_INVALID;
    }
    snd_shutdown();
    sound_ready = 0;
}

void client_menu_audio_init(void) {
    if(sound_ready) return;
    if(snd_init() < 0) {
        printf("dcvmu: menu audio unavailable; continuing silently\n");
        return;
    }
    sound_ready = 1;
    last_click = 0;
    const size_t bytes = CLICK_SAMPLES * sizeof(int16_t);
    int16_t *pcm = aligned_alloc(32, bytes);
    if(!pcm) goto failed;
    uint32_t noise = 0x4d5655;
    for(int i = 0; i < CLICK_SAMPLES; ++i) {
        float t = (float)i / CLICK_RATE;
        float decay = 1.0f - (float)i / (CLICK_SAMPLES - 1);
        float attack = fminf(t / 0.001f, 1.0f);
        noise = noise * 1664525u + 1013904223u;
        float hiss = (float)(noise >> 16) / 32767.5f - 1.0f;
        float tone = sinf(2 * PI * (1300 * t - 7000 * t * t));
        pcm[i] = (int16_t)(8000 * attack * decay * decay * decay * decay *
                           (0.85f * tone + 0.15f * hiss));
    }
    click = snd_sfx_load_raw_buf((char *)pcm, bytes, CLICK_RATE, 16, 1);
    free(pcm);
    if(click == SFXHND_INVALID) goto failed;
    channel = snd_sfx_chn_alloc();
    if(channel < 0) goto failed;
    printf("dcvmu: menu audio ready\n");
    return;
failed:
    printf("dcvmu: menu click unavailable; continuing silently\n");
    client_menu_audio_shutdown();
}

void client_menu_sound(menu_sound_t sound) {
    if(!sound_ready || click == SFXHND_INVALID || channel < 0) return;
    uint64_t now = timer_ms_gettime64();
    /* Coalesce fast key repeats. A selection/back action is always immediate.
       Reusing one reserved channel also prevents a pile-up of overlapping clicks. */
    if(sound == MENU_MOVE && last_click && now - last_click < 35) return;
    sfx_play_data_t data = {
        .chn = channel, .idx = click, .pan = 128,
        .vol = sound == MENU_MOVE ? 128 : 160,
        .freq = sound == MENU_SELECT ? 28665 : sound == MENU_BACK ? 16538 : CLICK_RATE
    };
    snd_sfx_play_ex(&data);
    last_click = now;
}
