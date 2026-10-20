/* DreamKarts lobby chat: the strip at the foot of the lobby room, the chat
   box it opens into, and typing on a Dreamcast keyboard or the on-screen
   keys (see chat.c). */
#ifndef DK_CHAT_H
#define DK_CHAT_H

#include "dk.h"

/* Joining a lobby: nothing heard, nothing typed, box closed. */
void chat_enter_lobby(void);
/* Once a frame in the lobby room. Returns true when the chat took this
   frame's buttons (the box is open, or it just opened or closed); can_say
   is false for a watcher, who reads but cannot type. */
bool chat_update(float dt, bool can_say);
bool chat_open(void);
/* The strip, or the open box, over the lobby room; and the button hint
   that goes with the open box (NULL when closed). */
void chat_draw(float t);
const char *chat_hint(void);
#ifdef DK_AUTOTEST
/* Test builds: buttons pressed next frame, and the on-screen key to move to. */
void chat_at_press(uint32_t btns);
bool chat_at_pick(const char *label);
#endif

#endif
