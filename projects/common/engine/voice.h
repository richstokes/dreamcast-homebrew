/* Engine: a voiceover, one spoken line at a time, streamed from main memory.

   An effect in sfx.h lives in the AICA's 2 MB for as long as the game runs
   and can be 65534 samples at most. A line of speech is long, played now
   and then, and never over another line, so it stays in the pak the game
   links in and is fed through one KOS stream channel as it plays: 32 KiB of
   AICA RAM however many lines there are, at any length and sample rate. */
#ifndef ENG_VOICE_H
#define ENG_VOICE_H

#include <stdbool.h>
#include <stdint.h>

/* After sfx_init(). False (and silence) without a stream. */
bool voice_init(void);
void voice_shutdown(void);
/* Say a line: 16-bit mono PCM at a 32-byte aligned address, volume 0-255.
   It replaces a line still being said. */
void voice_say(const void *pcm, uint32_t bytes, uint32_t rate, int vol);
/* Once a frame: feeds the stream. A quarter of a second without a call
   (at 44.1 kHz) and the line stutters. */
void voice_update(void);

#endif
