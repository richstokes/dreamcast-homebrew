/* Engine: the voiceover (voice.h). */
#include "voice.h"
#include "engine_config.h"
#include <kos.h>
#include <dc/sound/stream.h>
#include <stdio.h>
#include <string.h>

/* The stream's ring in AICA RAM. KOS fills half of it at a time, when half
   has played, and all of it when a line starts. */
#define RING_BYTES 32768
#define HALF_BYTES (RING_BYTES / 2)

static snd_stream_hnd_t stream = SND_STREAM_INVALID;
static const uint8_t *line;           /* what is left of the line */
static uint32_t line_left;
static uint32_t quiet;                /* bytes of silence sent since it ran out */
static bool saying;
/* The line's last part, padded with silence, and then silence alone. */
static uint8_t tail[HALF_BYTES] __attribute__((aligned(32)));

static void *feed(snd_stream_hnd_t hnd, int want, int *got) {
    (void)hnd;
    if(want > HALF_BYTES) want = HALF_BYTES;
    *got = want;
    if(line_left >= (uint32_t)want) {
        const uint8_t *at = line;
        line += want;
        line_left -= (uint32_t)want;
        return (void *)at;
    }
    /* Past the end the ring must still be written, or it plays round again. */
    memset(tail, 0, sizeof(tail));
    memcpy(tail, line, line_left);
    quiet += (uint32_t)want - line_left;
    line_left = 0;
    return tail;
}

bool voice_init(void) {
    if(snd_stream_init_ex(1, 0) < 0) return false;
    stream = snd_stream_alloc(feed, RING_BYTES);
    if(stream == SND_STREAM_INVALID) printf(ENG_NAME " voice: no stream; no voiceover\n");
    return stream != SND_STREAM_INVALID;
}

void voice_shutdown(void) {
    if(stream == SND_STREAM_INVALID) return;
    snd_stream_destroy(stream);
    stream = SND_STREAM_INVALID;
    saying = false;
}

void voice_say(const void *pcm, uint32_t bytes, uint32_t rate, int vol) {
    if(stream == SND_STREAM_INVALID || vol <= 0) return;
    if(saying) snd_stream_stop(stream);
    line = pcm;
    line_left = bytes & ~31u;
    quiet = 0;
    saying = true;
    snd_stream_start(stream, rate, 0);
    snd_stream_volume(stream, vol > 255 ? 255 : vol);
}

void voice_update(void) {
    if(!saying) return;
    snd_stream_poll(stream);
    /* A whole ring of silence: nothing of the line is left to hear. */
    if(quiet >= RING_BYTES) {
        snd_stream_stop(stream);
        saying = false;
    }
}
