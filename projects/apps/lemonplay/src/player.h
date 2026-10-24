/* Plays one ZeroConfigDLNA --dreamcast-mode stream (MPEG-1 video and 16-bit
   PCM audio in AVI) full screen, with seeking, pause and an on-screen bar. */
#ifndef LEMONPLAY_PLAYER_H
#define LEMONPLAY_PLAYER_H

#include "upnp.h"

#include <stddef.h>

typedef enum {
    PLAY_FINISHED,   /* reached the end: play the next item */
    PLAY_NEXT,       /* the viewer skipped forward */
    PLAY_PREVIOUS,   /* the viewer skipped back */
    PLAY_STOPPED,    /* the viewer pressed B */
    PLAY_RESET,      /* the viewer held A+B+X+Y+Start */
    PLAY_FAILED      /* see the error text */
} play_result_t;

/* Appended to stream requests (benchmarks try other server profiles). */
extern const char *player_extra_query;

play_result_t player_play(const entry_t *entry, double start_seconds, char *error, size_t error_size);

#endif
