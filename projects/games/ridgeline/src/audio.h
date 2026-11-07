/* Ridgeline audio. */
#ifndef RL_AUDIO_H
#define RL_AUDIO_H

#include "rl.h"

void audio_init(void);
/* Out on the mountain: the engines start, the loops come up. */
void audio_drive_start(void);
void audio_drive_stop(void);
void audio_update(float dt);
/* Whose engine we are in (demo mode cuts between trucks); 0 on a drive start. */
void audio_listen(int ti);
void audio_sfx(int id, int vol);

#endif
