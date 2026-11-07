/* Ridgeline audio. */
#ifndef RL_AUDIO_H
#define RL_AUDIO_H

#include "rl.h"

void audio_init(void);
/* Out on the mountain: the engines start, the loops come up. */
void audio_drive_start(void);
void audio_drive_stop(void);
void audio_update(float dt);
void audio_sfx(int id, int vol);

#endif
