/* Sound: one-shots placed in the world, the looping voices, and the music,
   which is CD audio tracks on the disc. */
#ifndef KL_AUDIO_H
#define KL_AUDIO_H

#include "kl.h"

void audio_init(void);
/* Before leaving: the drive must not play on through the reboot to the menu. */
void audio_shutdown(void);
void audio_update(float dt);
void audio_sfx(int id, int vol);
void audio_sfx_rate(int id, int vol, float rate);
/* A sound at p: quieter with distance from the kaiju's camera, panned. */
void audio_at(int id, V3 p, int vol);
/* The loops the game feeds each frame: 0-1. */
/* For the test builds' log: the drive's status as the jukebox last read it
   (-1: none yet; 3 is playing in Flycast), the CD volume register and the
   song in the drive. */
void audio_music_state(int *status, int *volume, int *song);
/* On to the next song of the shuffle (a lunch starting). */
void audio_music_next(void);
extern float audio_beam, audio_heli, audio_siren, audio_crowd;

#endif
