/* DreamKarts audio: CD-DA music, PCM effects and kart loops. */
#ifndef DK_AUDIO_H
#define DK_AUDIO_H

#include "kart.h"
#include "sound_assets.h"
#include "music_tracks.h"

/* Finds the music's audio tracks on the disc; audio_init does it if this
   hasn't. It resets the GD-ROM drive, and on the console all five VMU read
   timeouts seen while that overlapped the save worker came during it. */
void audio_disc_init(void);
void audio_init(void);
void audio_shutdown(void);
void audio_update(float dt);
/* The title screen's song (the theme), unless it is already playing. */
void audio_music_title(void);
/* A race start: the next of the race songs' shuffle (the theme stays on
   the title screen). Every change of song fades the one playing out first
   (~0.4 s), then fades the new one in. */
void audio_music_start(void);
/* The pause menu's NEXT SONG: the next of that shuffle. */
void audio_music_next(void);
/* Sound test: play a song from its start. */
void audio_music_play(int song);
/* The song playing now; its elapsed and total seconds (either may be NULL). */
int audio_music_now(float *secs, float *len);
bool audio_music_playing(void);
void audio_music_target(int volume);     /* fades toward 0..255 */
/* The music switch: off fades the music out and pauses the disc. */
void audio_music_enable(bool on);
bool audio_music_enabled(void);
/* The volume setting, 0 (off) to SAVE_VOLUME_MAX: 3 dB a step under the
   scene's level. */
void audio_music_volume(int volume);
#ifdef DK_AUTOTEST
/* Prints how the disc's music went: tracks found, songs started and ended. */
void audio_autotest_report(void);
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
