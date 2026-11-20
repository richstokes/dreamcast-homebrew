/* Engine: sound effects in AICA RAM, one-shots and looping voices.

   Effects are 16-bit mono PCM from a pak the game links in, with a table
   of {offset, bytes, loop, rate} (the layout every game's build_audio.py
   writes). One-shots go through KOS's sfx manager. Looping sounds whose
   pitch and volume follow the game (engines, tyres, wind) are voices:
   each owns a channel reserved with snd_sfx_chn_alloc(), since
   snd_sfx_play() hands channels out round-robin and skips only reserved
   ones, so an unreserved loop is soon replaced by a one-shot and its
   updates then retune the wrong sound (a DreamKarts bug). A voice's pitch,
   volume and pan change in place with AICA channel UPDATE commands, sent
   only when they change. */
#ifndef ENG_SFX_H
#define ENG_SFX_H

#include "vecmath.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct { uint32_t offset, bytes, loop, rate; } eng_sfx_asset_t;

/* Load every effect; false (and silence) without the sound system. */
bool sfx_init(const uint8_t *pak, const eng_sfx_asset_t *assets, int count);
void sfx_shutdown(void);
bool sfx_ready(void);
/* One-shot: volume 0-255, pan 0 (left) - 128 - 255 (right), playback
   rate relative to the effect's own. */
void sfx_play(int id, int vol, int pan, float rate);
/* Where a sound at p sits for a listener at camera c: returns its
   distance and sets the pan, 128 plus width per unit of the camera's
   right (clamped to 0-255). The gain curve is the game's. */
float sfx_listen(V3 p, const Camera *c, float width, int *pan);

typedef struct {
    int channel;        /* -1: not started */
    int sfx;
    int last_freq, last_vol, last_pan;
} SfxVoice;

/* Start effect id looping silently on a reserved channel. */
bool sfx_voice_start(SfxVoice *v, int id);
/* Retune: rate relative to the effect's own, volume 0-255, pan 0-255.
   Call between sfx_batch_begin/end when updating several at once. */
void sfx_voice_set(SfxVoice *v, float rate, int vol, int pan);
void sfx_voice_stop(SfxVoice *v);
/* Hold the AICA's command queue while a frame's voice updates go in. */
void sfx_batch_begin(void);
void sfx_batch_end(void);

#endif
