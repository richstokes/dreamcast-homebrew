/* DreamKarts audio: streamed ADPCM music, PCM effects and kart loops. */
#ifndef DK_AUDIO_H
#define DK_AUDIO_H

#include "kart.h"
#include "sound_assets.h"

/* Opens the music on the disc; audio_init does it if this hasn't. The
   first /cd access resets the GD-ROM drive, and on the console all five
   VMU read timeouts seen while it overlapped the save worker came during it. */
void audio_open_disc(void);
void audio_init(void);
void audio_shutdown(void);
void audio_update(float dt);
/* Starts the music: the theme on the first call (title screen), a random
   other song on each later one (a race); the playlist then plays on in order. */
void audio_music_start(void);
/* Sound test: play a song from its start straight away (the playlist then
   carries on in order). */
void audio_music_play(int song);
/* The song playing now; its elapsed and total seconds (either may be NULL). */
int audio_music_now(float *secs, float *len);
bool audio_music_playing(void);
float audio_music_rate_now(void);
void audio_music_target(int volume);     /* fades toward 0..255 */
/* The pause menu's music switch: off fades the music out whatever the target. */
void audio_music_enable(bool on);
bool audio_music_enabled(void);
/* Retune the stream to a new playback rate (final lap speed-up). */
void audio_music_rate(float mul);
#ifdef DK_AUTOTEST
/* Prints the rate change's cost and the music's byte rate either side of it. */
void audio_autotest_report(void);
/* The longest wait between two refills of the playing stream, how many
   waits ran past LATE_MS (its AICA buffer then replays stale data) and how
   many refills found the ring short of music (silence instead). */
#define AUDIO_FEED_LATE_MS 400
uint32_t audio_autotest_feed(int *late, int *dry);
#endif
void audio_sfx(int id, int vol, int pan);
/* Chao voices: a mood picks one of its clips (never the same one twice
   running), voiced at the pitch of a colour preset, so each Chao sounds
   like itself. */
enum { CHAO_HI, CHAO_OUCH };
void audio_chao(int mood, int colour, int vol, int pan);
void audio_sfx_pitch(int id, int vol, int pan, float rate);
/* Accelerate held on the grid (countdown) revs the engine freely; call each
   frame it applies, before audio_kart_loops(). */
void audio_kart_throttle(int slot, bool held);
/* Engine, tyre and spark loops for player slot 0/1 follow the given kart
   (NULL silences them). */
void audio_kart_loops(int slot, const Kart *k, float dt);
/* Spiral Canyon's biplane drone, a loop retuned every frame: volume
   0-255 (0 silences it), pan and playback rate. */
void audio_plane(int vol, int pan, float rate);
/* One-shot effects for a kart's events this step; distance attenuated. */
void audio_kart_events(const Kart *k, bool is_player);

#endif
