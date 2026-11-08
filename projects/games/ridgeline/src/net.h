/* Ridgeline online client: talks to the server over UDP (see
   server/PROTOCOL.md). Call net_update() once a frame while online. */
#ifndef RL_NET_H
#define RL_NET_H

#include "rl.h"
#include "net_version.h"

/* The server. Test builds can point at another one with
   EXTRA_CFLAGS=-DRL_NET_TEST_HOST=192.168.1.20 (an address or host name). */
#define RL_NET_STR2(x) #x
#define RL_NET_STR(x) RL_NET_STR2(x)
#ifdef RL_NET_TEST_HOST
#define RL_NET_HOST RL_NET_STR(RL_NET_TEST_HOST)
#endif
#ifndef RL_NET_HOST
#define RL_NET_HOST "dcvmu.com"
#endif
#ifndef RL_NET_FALLBACK_IP
#define RL_NET_FALLBACK_IP "143.110.224.215"   /* dcvmu.com, if DNS fails */
#endif
#ifndef RL_NET_PORT
#define RL_NET_PORT 7536
#endif

#define NET_LOBBIES      10
#define NET_MAX_PLAYERS  4
#define NET_NAME_LEN     11   /* 10 characters and a NUL */
#define NET_PLAYER_BYTES 17   /* a LOBBY packet's player entry */
#define NET_TRUCK_BYTES  30   /* a TRUCK body */

enum { NET_OFF, NET_LINK, NET_HELLO, NET_BROWSE, NET_JOINING, NET_LOBBY, NET_REJECTED, NET_LOST };
enum { NREJ_TOO_OLD = 1, NREJ_FULL, NREJ_LOBBY_FULL, NREJ_BAD_NAME, NREJ_SESSION };
enum { NPF_SUMMIT = 1 };   /* player flags: reached the summit this session */
/* TRUCK flags. */
enum { NTF_SUMMIT = 1, NTF_LIGHTS_SHIFT = 1, NTF_HORN = 8, NTF_BRAKE = 16, NTF_STALLED = 32 };

typedef struct {
    int count;
    float hour;
    char names[NET_MAX_PLAYERS][NET_NAME_LEN];
    uint8_t flags[NET_MAX_PLAYERS];
} NetLobbyInfo;

typedef struct {
    uint8_t slot;
    char name[NET_NAME_LEN];
    uint8_t model, livery, flags;
    uint16_t ping;
} NetPlayer;

typedef struct {
    int id, you;
    float hour;           /* the lobby's time of day */
    int count;
    NetPlayer players[NET_MAX_PLAYERS];
} NetLobby;

/* A truck as it goes over the wire. */
typedef struct {
    uint8_t flags;
    V3 pos, vel;
    float q[4];           /* x, y, z, w: the body axes right, up, forward */
    float steer, rpm, load, slip;
    int gear;
} NetTruck;

/* Go online as name: bring the link up, then say hello. */
void net_begin(const char *name);
/* Leave any lobby and stop talking (the link stays up for next time). */
void net_end(void);
void net_update(void);
int net_state(void);
/* Why the state is NET_REJECTED / NET_LOST, or link progress. */
const char *net_message(void);
int net_reject_reason(void);
bool net_update_available(void);

/* Lobby browser. */
void net_want_list(bool want);
const NetLobbyInfo *net_lobbies(void);
void net_join(int id, int model, int livery);
void net_leave(void);

/* Inside a lobby. */
const NetLobby *net_lobby(void);
const NetPlayer *net_player(int slot);

/* Our truck for the next TRUCK packet (12 a second). */
void net_set_truck(const NetTruck *t);
/* Others' trucks as they were at local time at_ms. Call net_draw_time()
   once a step for the moment to draw them at: a little in the past, so
   the samples either side have arrived. */
uint64_t net_draw_time(float dt);
bool net_remote_truck(int slot, uint64_t at_ms, NetTruck *out);
void net_forget_slot(int slot);
int net_interp_ms(void);
uint64_t net_now(void);

#endif
