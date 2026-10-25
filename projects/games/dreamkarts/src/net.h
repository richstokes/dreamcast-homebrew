/* DreamKarts online client: talks to the lobby server over UDP (see
   server/PROTOCOL.md). Call net_update() once a frame while online. */
#ifndef DK_NET_H
#define DK_NET_H

#include "dk.h"
#include "items.h"
#include "kart.h"
#include "net_version.h"

#define NET_LOBBIES     10
#define NET_MAX_PLAYERS 6
#define NET_NAME_LEN    11   /* 10 characters and a NUL */
#define NET_CHAT_LEN    33   /* a chat line: 32 characters and a NUL */
#define NET_CHAT_KEEP   10   /* chat lines remembered */
#define NET_PLAYER_BYTES 23  /* a LOBBY packet's player entry */

enum { NET_OFF, NET_LINK, NET_HELLO, NET_BROWSE, NET_JOINING, NET_LOBBY, NET_REJECTED, NET_LOST };
enum { NLS_WAITING, NLS_COUNTDOWN, NLS_RACING, NLS_RESULTS };
enum { NREJ_TOO_OLD = 1, NREJ_FULL, NREJ_LOBBY_FULL, NREJ_BAD_NAME, NREJ_SESSION };
enum { NPF_LEADER = 1, NPF_RACING = 2, NPF_FINISHED = 4 };
enum { NKF_FINISHED = 1, NKF_GROUNDED = 2, NKF_BOOST = 4, NKF_DRIFT = 8, NKF_DRIFT_RIGHT = 16, NKF_WAVE = 32, NKF_WAVE_LEFT = 64,
       NKF_SKID = 128 };

enum { NLF_DEMO = 1 };   /* lobby flags: the server's bots race here (watch only) */
#define NET_SPECTATOR 255  /* NetLobby.you while watching */

typedef struct {
    uint8_t players, state, track;
    uint8_t watchers, flags;
    char leader[NET_NAME_LEN];
} NetLobbyInfo;

typedef struct {
    uint8_t slot;
    char name[NET_NAME_LEN];
    uint8_t colour, flags;      /* colour: a kart_colours[] preset or COLOUR_CUSTOM */
    uint16_t ping, points;
    uint8_t place, grid;
    KartCustom custom;          /* their own colours, meaningful with COLOUR_CUSTOM */
} NetPlayer;

typedef struct {
    int id, state, you, leader;
    int track, laps, cc, race_id;
    int items;              /* power-ups on in this lobby's races */
    uint64_t green_ms;      /* local timer_ms of the green light */
    uint64_t end_ms;        /* local timer_ms the race ends after the first finisher, or
                               results go back to the lobby (0 none) */
    uint8_t nonce;
    int watchers, flags;
    int count;
    NetPlayer players[NET_MAX_PLAYERS];
} NetLobby;

/* A lobby chat line as the server relayed it. */
typedef struct {
    uint8_t slot, colour, id;   /* slot 255: not a player */
    char name[NET_NAME_LEN];
    char text[NET_CHAT_LEN];
    uint64_t at;                /* local ms it arrived */
} NetChat;

/* Lobby chat. net_say() sends a line, resent until the server echoes it
   back (net_say_state() says how that went; one line at a time). The lines
   heard since joining this lobby are numbered from 0; the newest
   NET_CHAT_KEEP are kept, so net_chat(i) is valid for
   i >= net_chat_count() - NET_CHAT_KEEP. */
enum { NET_SAY_IDLE, NET_SAY_SENDING, NET_SAY_SENT, NET_SAY_FAILED };
bool net_say(const char *text);
int net_say_state(void);
int net_chat_count(void);
const NetChat *net_chat(int i);

/* Item byte (KART offset 33): the held item, a barrier up, spun out and
   whether the spear flew backwards. The sequence byte (offset 40) numbers
   this kart's spear in its low nibble and its mine in the high one, 0 for
   none. */
enum { NKI_ITEM_MASK = 7, NKI_SHIELD = 8, NKI_SPUN = 16, NKI_BACK = 32, NKI_SEQ_MASK = 15, NKI_MINE_SHIFT = 4 };

/* A kart as sent over the wire. */
typedef struct {
    uint8_t flags;
    float x, y, z, heading, speed, steer;
    int stage, lap;
    float dist, race_time;
    NetItems items;
} NetKart;

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
int net_latest_build(void);

/* Lobby browser. */
void net_want_list(bool want);
const NetLobbyInfo *net_lobbies(void);
void net_join(int id);
/* Watch lobby id instead: no seat, no kart, full or not. */
void net_spectate(int id);
bool net_spectating(void);
void net_leave(void);

/* Inside a lobby. */
const NetLobby *net_lobby(void);
const NetPlayer *net_me(void);
const NetPlayer *net_player(int slot);
/* What this console wants: everyone's colour (and their custom kart's
   colours, sent along for COLOUR_CUSTOM); the leader's race settings. */
void net_wish(int colour, const KartCustom *custom, int track, int laps, int cc, int items);
void net_press_start(void);
/* Drop out of the race being run (race_id), staying in the lobby: resent
   until the lobby state shows this console not racing. */
void net_retire(int race_id);

/* Racing: our kart for the next KART packet, and others' karts as they
   were at local time at_ms. Call net_draw_time() once a step for the
   moment to draw them at (a little in the past, so the samples either
   side have arrived); net_interp_ms() says how far back that is. */
void net_set_kart(const NetKart *k, int race_id);
uint64_t net_draw_time(float dt);
bool net_remote_kart(int slot, uint64_t at_ms, NetKart *out);
int net_interp_ms(void);
#ifdef DK_AUTOTEST
/* Test builds: a line about a slot's sample history for the serial log. */
const char *net_debug_sample(int slot);
#endif
/* The 29 wire bytes of a kart (KART offsets 19-47: flags to the mine). */
#define NET_KART_BYTES 29
void net_encode_kart(uint8_t *b, const NetKart *k);
uint64_t net_now(void);
/* This session's token (set once the link is up), and a name change before
   the server has said hello back (test builds). */
uint32_t net_token(void);
void net_rename(const char *name);

#endif
