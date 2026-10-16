/* Totally Bazaar audio: streamed looping ADPCM music plus PCM effects. */
#ifndef BAZAAR_AUDIO_H
#define BAZAAR_AUDIO_H

#include "sound_assets.h"

void audio_init(void);
void audio_shutdown(void);
void audio_update(float dt);
/* vol 0..255, pan 0 (left) .. 255 (right), 128 centre. */
void audio_play(int sfx, int vol, int pan);
void audio_play_pitched(int sfx, int vol, int pan, float rate_scale);
void audio_music_start(void);
void audio_music_target(int volume);   /* fades towards 0..255 */

#endif
