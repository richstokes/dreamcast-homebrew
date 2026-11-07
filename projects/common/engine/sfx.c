/* Engine: sound effects (sfx.h). */
#include "sfx.h"
#include "engine_config.h"
#include <kos.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/aica_comm.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

static sfxhnd_t *handles;
static const eng_sfx_asset_t *table;
static int count;
static bool ready;

bool sfx_init(const uint8_t *pak, const eng_sfx_asset_t *assets, int n) {
    if(snd_init() < 0) {
        printf(ENG_NAME " sfx: sound init failed; running silently\n");
        return false;
    }
    handles = calloc(n, sizeof(sfxhnd_t));
    if(!handles) {
        printf(ENG_NAME " sfx: out of memory for %d effects; running silently\n", n);
        snd_shutdown();
        return false;
    }
    table = assets;
    count = n;
    int loaded = 0;
    for(int i = 0; i < n; i++) {
        const eng_sfx_asset_t *a = &assets[i];
        handles[i] = snd_sfx_load_raw_buf((char *)(pak + a->offset), a->bytes, a->rate, 16, 1);
        if(handles[i] == SFXHND_INVALID) printf(ENG_NAME " sfx: effect %d failed to load\n", i);
        else loaded++;
    }
    ready = true;
    printf(ENG_NAME " sfx: %d/%d effects, %lu KiB AICA RAM free\n", loaded, n,
           (unsigned long)(snd_mem_available() / 1024));
    return true;
}

void sfx_shutdown(void) {
    if(!ready) return;
    snd_sfx_stop_all();
    snd_sfx_unload_all();
    snd_shutdown();
    free(handles);
    handles = NULL;
    ready = false;
}

bool sfx_ready(void) {
    return ready;
}

void sfx_play(int id, int vol, int pan, float rate) {
    if(!ready || id < 0 || id >= count || handles[id] == SFXHND_INVALID || vol <= 0) return;
    sfx_play_data_t d;
    memset(&d, 0, sizeof(d));
    d.chn = -1;
    d.idx = handles[id];
    d.vol = vol > 255 ? 255 : vol;
    d.pan = pan < 0 ? 0 : pan > 255 ? 255 : pan;
    d.freq = (int)(table[id].rate * rate);
    snd_sfx_play_ex(&d);
}

float sfx_listen(V3 p, const Camera *c, float width, int *pan) {
    V3 d = vsub(p, c->pos);
    float dist = vlen(d);
    float side = dist > 0.1f ? vdot(d, c->right) / dist : 0;
    *pan = (int)clampf(128.0f + side * width, 0, 255);
    return dist;
}

bool sfx_voice_start(SfxVoice *v, int id) {
    v->channel = -1;
    v->sfx = id;
    if(!ready || id < 0 || id >= count || handles[id] == SFXHND_INVALID) return false;
    int chn = snd_sfx_chn_alloc();
    if(chn < 0) {
        printf(ENG_NAME " sfx: no free channel for a voice\n");
        return false;
    }
    sfx_play_data_t d;
    memset(&d, 0, sizeof(d));
    d.chn = chn;
    d.idx = handles[id];
    d.vol = 0;
    d.pan = 128;
    d.loop = 1;
    d.freq = (int)table[id].rate;
    d.loopstart = 0;
    d.loopend = table[id].bytes / 2;
    v->channel = snd_sfx_play_ex(&d);
    if(v->channel < 0) {
        snd_sfx_chn_free(chn);
        return false;
    }
    v->last_freq = v->last_vol = v->last_pan = -1;
    return true;
}

void sfx_voice_set(SfxVoice *v, float rate, int vol, int pan) {
    if(v->channel < 0) return;
    int freq = (int)(table[v->sfx].rate * rate);
    if(freq < 1000) freq = 1000;
    if(freq > 88000) freq = 88000;
    /* No command for a tiny pitch change. 16 Hz is ~5 cents at the lowest
       pitch a voice gets to (an 11 kHz loop at half speed), under what a
       glide shows; 64 Hz stepped audibly there. */
    freq = (freq + 8) & ~15;
    vol = vol < 0 ? 0 : vol > 255 ? 255 : vol;
    pan = pan < 0 ? 0 : pan > 255 ? 255 : pan;
    uint32_t what = 0;
    if(freq != v->last_freq) what |= AICA_CH_UPDATE_SET_FREQ;
    if(vol != v->last_vol) what |= AICA_CH_UPDATE_SET_VOL;
    if(pan != v->last_pan && vol > 0) what |= AICA_CH_UPDATE_SET_PAN;
    if(!what) return;
    AICA_CMDSTR_CHANNEL(packet, cmd, chan);
    cmd->cmd = AICA_CMD_CHAN;
    cmd->timestamp = 0;
    cmd->size = AICA_CMDSTR_CHANNEL_SIZE;
    cmd->cmd_id = (uint32_t)v->channel;
    chan->cmd = AICA_CH_CMD_UPDATE | what;
    chan->freq = (uint32_t)freq;
    chan->vol = (uint32_t)vol;
    chan->pan = (uint32_t)pan;
    snd_sh4_to_aica(packet, cmd->size);
    v->last_freq = freq;
    v->last_vol = vol;
    if(what & AICA_CH_UPDATE_SET_PAN) v->last_pan = pan;
}

void sfx_voice_stop(SfxVoice *v) {
    if(v->channel < 0) return;
    snd_sfx_stop(v->channel);
    snd_sfx_chn_free(v->channel);
    v->channel = -1;
}

void sfx_batch_begin(void) {
    if(ready) snd_sh4_to_aica_stop();
}

void sfx_batch_end(void) {
    if(ready) snd_sh4_to_aica_start();
}
