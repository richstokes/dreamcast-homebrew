/* DreamKarts online multiplayer: menus and lobby races (see online.c). */
#ifndef DK_ONLINE_H
#define DK_ONLINE_H

#include "dk.h"
#include "kart.h"
#include "net.h"

void online_init(void);
/* From the main menu: name entry, then connect. */
void online_enter(void);
/* Once a frame while game.online: network, and following the lobby. */
void online_tick(void);
void online_update(float dt);
void online_camera(Camera *cam, float dt);
void online_draw(void);

/* Online races. */
float online_countdown(void);
float online_race_time(void);
/* Remote karts from the network, and ours out to it. */
void online_remote_karts(float dt);
/* A kart as it goes over the wire. */
void online_pack_kart(const Kart *k, NetKart *out);
/* Mid-race Start opens a leave prompt; true when the player left. */
bool online_race_input(void);
void online_draw_race(void);
void online_draw_results(void);
const char *online_name(void);
#ifdef DK_AT_ONLINE
void online_autotest(float dt);
#endif

#endif
