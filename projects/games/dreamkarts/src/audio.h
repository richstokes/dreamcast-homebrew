/* DreamKarts audio: streamed ADPCM music, PCM effects and kart loops. */
#ifndef DK_AUDIO_H
#define DK_AUDIO_H

#include "kart.h"
#include "sound_assets.h"

void audio_init(void);
void audio_shutdown(void);
void audio_update(float dt);
void audio_music_start(void);
void audio_music_target(int volume);     /* fades toward 0..255 */
/* Restart the stream at a new playback rate (final lap speed-up). */
void audio_music_rate(float mul);
void audio_sfx(int id, int vol, int pan);
void audio_sfx_pitch(int id, int vol, int pan, float rate);
/* Engine, tyre and spark loops follow the given kart (NULL silences them). */
void audio_kart_loops(const Kart *k, float dt);
/* One-shot effects for a kart's events this step; distance attenuated. */
void audio_kart_events(const Kart *k, bool is_player);

#endif
