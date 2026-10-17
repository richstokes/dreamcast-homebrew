/* DreamKarts audio: streamed ADPCM music, PCM effects and kart loops. */
#ifndef DK_AUDIO_H
#define DK_AUDIO_H

#include "kart.h"
#include "sound_assets.h"

void audio_init(void);
void audio_shutdown(void);
void audio_update(float dt);
/* Starts the music: the theme on the first call (title screen), a random
   other song on each later one (a race); the playlist then plays on in order. */
void audio_music_start(void);
void audio_music_target(int volume);     /* fades toward 0..255 */
/* The pause menu's music switch: off fades the music out whatever the target. */
void audio_music_enable(bool on);
bool audio_music_enabled(void);
/* Retune the stream to a new playback rate (final lap speed-up). */
void audio_music_rate(float mul);
#ifdef DK_AUTOTEST
/* Prints the rate change's cost and the music's byte rate either side of it. */
void audio_autotest_report(void);
#endif
void audio_sfx(int id, int vol, int pan);
void audio_sfx_pitch(int id, int vol, int pan, float rate);
/* Engine, tyre and spark loops for player slot 0/1 follow the given kart
   (NULL silences them). */
void audio_kart_loops(int slot, const Kart *k, float dt);
/* One-shot effects for a kart's events this step; distance attenuated. */
void audio_kart_events(const Kart *k, bool is_player);

#endif
