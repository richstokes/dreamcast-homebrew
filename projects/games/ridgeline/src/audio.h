/* Ridgeline audio. */
#ifndef RL_AUDIO_H
#define RL_AUDIO_H

#include "rl.h"

void audio_init(void);
/* The room the sounds were linked into, free once audio_init has copied
   them to the sound chip: 32-byte aligned, *bytes long. The level's grid
   lives there (main.c), which is what lets two areas fit in 16 MB. */
void *audio_spent_pak(uint32_t *bytes);
/* Out on the mountain: the engines start, the loops come up. */
void audio_drive_start(void);
void audio_drive_stop(void);
void audio_update(float dt);
/* Whose engine we are in (demo mode cuts between trucks); 0 on a drive start. */
void audio_listen(int ti);
void audio_sfx(int id, int vol);

#endif
