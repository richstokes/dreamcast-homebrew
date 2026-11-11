/* Ridgeline online: up to four trucks on the mountain together (see
   online.c). */
#ifndef RL_ONLINE_H
#define RL_ONLINE_H

#include "rl.h"
#include "net.h"

/* A lobby is one area's: the first half Cascade Ridge's, the rest the
   desert's. Every console works it out from the lobby's number. */
static inline int online_lobby_area(int lobby) {
    return lobby < NET_LOBBIES / 2 ? AREA_CASCADE : AREA_MOAB;
}

void online_init(void);
/* From the title's main menu: the name entry, then connecting. */
void online_enter(void);
/* Once a frame while game.online: the network, and following the lobby. */
void online_tick(void);
/* ST_ONLINE: the name entry, connecting, the lobby browser, errors. */
void online_update(float dt);
void online_draw(void);

/* The truck select (ST_SELECT while online) joins the lobby picked on the
   browser with the truck and paint chosen. */
int online_lobby_pick(void);
void online_join(void);
bool online_joining(void);
/* Back from the select to the browser (cancelling a join). */
void online_back_to_browser(void);

/* Each driving step: the other trucks from the network, ours out to it. */
void online_drive_step(float dt);
/* The player reached the summit: their trophy, for the session. */
void online_summit(void);
/* The roster with trophies, name tags over the other trucks, and who
   came and went (translucent list, over the HUD). */
void online_draw_hud(void);
/* The pause menu's LEAVE LOBBY: back to the browser. */
void online_leave_lobby(void);
/* Offline, back to the title (soft reset). */
void online_quit(void);

#ifdef RL_AT_ONLINE
void online_autotest_boot(void);
void online_autotest(float dt);
#endif

#endif
