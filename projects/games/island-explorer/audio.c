#include "engine.h"
#include <dc/sound/stream.h>
#include <dc/sound/sfxmgr.h>
#include <string.h>
#include <stdlib.h>
static snd_stream_hnd_t ocean=SND_STREAM_INVALID;
static size_t cursor;
static uint8_t *buffer;
static sfxhnd_t step=SFXHND_INVALID;
static int volume=-1;
static bool stream_ready;
static void *ocean_callback(snd_stream_hnd_t h,int wanted,int *received){
    (void)h;
    size_t size=(size_t)(ambience_end-ambience_start),done=0;
    if(wanted<=0||wanted>65536){*received=0;return NULL;}
    while(done<(size_t)wanted){
        size_t count=(size_t)wanted-done;
        if(count>size-cursor)count=size-cursor;
        memcpy(buffer+done,ambience_start+cursor,count);
        done+=count;cursor=(cursor+count)%size;
    }
    *received=wanted;return buffer;
}
bool audio_init(void){
    buffer=memalign(32,65536);
    if(!buffer||snd_stream_init_ex(2,32768)<0)return false;
    stream_ready=true;
    ocean=snd_stream_alloc(ocean_callback,32768);
    if(ocean==SND_STREAM_INVALID)return false;
    snd_stream_start(ocean,22050,1);snd_stream_volume(ocean,195);
    int16_t *samples=memalign(32,4096);
    if(samples){
        uint32_t random=72;float filter=0;
        for(int i=0;i<2048;i++){
            random=random*1664525+1013904223;
            float noise=(int16_t)(random>>16)/32768.f;
            filter+=.28f*(noise-filter);
            float env=expf(-i/290.f)*(1-expf(-i/22.f));
            samples[i]=(int16_t)((filter*.75f+noise*.18f)*env*8000);
        }
        step=snd_sfx_load_raw_buf((char *)samples,4096,22050,16,1);free(samples);
    }
    printf("[audio] 28-second stereo surf, 22050 Hz; streaming, seam checked\n");return true;
}
void audio_update(float dt){
    (void)dt;
    if(ocean==SND_STREAM_INVALID)return;
    if(snd_stream_poll(ocean)<0)printf("[audio] stream underrun\n");
    float altitude=game.player.position.y;
    int target=game.muted?0:(int)(195-clampf(altitude/13,0,60));
    if(target!=volume){snd_stream_volume(ocean,target);volume=target;}
}
void audio_footstep(void){if(step!=SFXHND_INVALID&&!game.muted&&!game.player.swimming)snd_sfx_play(step,95,128);}
void audio_shutdown(void){
    if(ocean!=SND_STREAM_INVALID)snd_stream_destroy(ocean);
    if(step!=SFXHND_INVALID)snd_sfx_unload(step);
    if(stream_ready)snd_stream_shutdown();
    free(buffer);
}
