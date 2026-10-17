/* DreamKarts online client.

   One non-blocking UDP socket. Nothing is acknowledged: the server resends
   lobby state four times a second and this side resends what it wants
   (join, colour, settings, start) until the state shows it, so a lost
   packet on a dial-up line only delays things. Remote karts arrive at
   12 Hz and are drawn about 100 ms in the past, interpolated between the
   two samples either side. */
#include "net.h"
#include "netlink.h"
#include <kos.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>

#define HISTORY 8

typedef struct {
    uint64_t t;      /* local ms the state was current */
    NetKart k;
} Sample;

static int state = NET_OFF;
static int sock = -1;
static struct sockaddr_in server;
static uint32_t token;
static char my_name[NET_NAME_LEN];
static uint64_t last_rx, last_hello, last_list, last_join, last_wish, last_kart, last_ping;
static uint32_t echo_ms;
static uint64_t echo_at;
static int reject_reason;
static int latest_build, min_build;
static bool update_available, want_list;
static const char *message = "";
static NetLobbyInfo lobbies[NET_LOBBIES];
static NetLobby lobby;
static int join_id = -1;
static int wish_colour, wish_track, wish_laps = 3, wish_cc = 1;
static uint8_t start_nonce;
static NetKart my_kart;
static int my_race_id;
static bool have_kart;
static Sample history[NET_MAX_PLAYERS][HISTORY];
static int hist_n[NET_MAX_PLAYERS];

uint64_t net_now(void) {
    return timer_ms_gettime64();
}

/* ---- little-endian packing (the SH-4 faults on unaligned access) ---- */

static void put16(uint8_t *p, uint32_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

static void send_packet(int type, const uint8_t *body, int len) {
    if(sock < 0) return;
    uint8_t b[64];
    uint64_t now = net_now();
    b[0] = 'D';
    b[1] = 'K';
    b[2] = DK_NET_PROTO;
    b[3] = (uint8_t)type;
    put32(b + 4, token);
    put32(b + 8, echo_ms);
    put16(b + 12, echo_at ? (uint32_t)(now - echo_at) : 0);
    put16(b + 14, DK_NET_BUILD);
    put16(b + 16, 0);
    if(len > (int)sizeof(b) - 18) return;
    if(len) memcpy(b + 18, body, len);
    sendto(sock, b, 18 + len, 0, (struct sockaddr *)&server, sizeof(server));
}

static uint32_t mix(uint32_t h, uint32_t v) {
    h = (h ^ v) * 16777619u;
    return h ^ (h >> 15);
}

/* The session token identifies this console to the server, so it must not
   repeat between consoles. Boot timing alone can (emulators boot the same
   way every time), so it is made once the link is up, mixing in when the
   network answered, the clock, the adapter and the name. */
static void make_token(void) {
    uint64_t us = timer_us_gettime64();
    uint32_t h = 2166136261u;
    h = mix(h, (uint32_t)us);
    h = mix(h, (uint32_t)(us >> 32));
    h = mix(h, (uint32_t)rtc_unix_secs());
    netif_t *n = net_default_dev;
    if(n)
        for(int i = 0; i < 6; i++) h = mix(h, n->mac_addr[i] | (i < 4 ? n->ip_addr[i] << 8 : 0));
    for(const char *c = my_name; *c; c++) h = mix(h, (uint8_t)*c);
    h = mix(h, dk_rand());
    token = h ? h : 1;
}

uint32_t net_token(void) {
    return token;
}

void net_rename(const char *name) {
    memset(my_name, 0, sizeof(my_name));
    strncpy(my_name, name, NET_NAME_LEN - 1);
}

static void open_socket(void) {
    if(sock >= 0) return;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock < 0) return;
    fcntl(sock, F_SETFL, O_NONBLOCK);
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(DK_NET_PORT);
    server.sin_addr.s_addr = link_server_ip();
}

void net_begin(const char *name) {
    memset(my_name, 0, sizeof(my_name));
    strncpy(my_name, name, NET_NAME_LEN - 1);
    reject_reason = 0;
    message = "";
    join_id = -1;
    memset(&lobby, 0, sizeof(lobby));
    lobby.id = -1;
    echo_ms = 0;
    echo_at = 0;
    state = NET_LINK;
    link_start();
}

void net_end(void) {
    if(state == NET_LOBBY || state == NET_JOINING)
        for(int i = 0; i < 3; i++) send_packet(4, NULL, 0);
    state = NET_OFF;
    lobby.id = -1;
    join_id = -1;
}

int net_state(void) { return state; }
const char *net_message(void) { return state == NET_LINK ? link_status() : message; }
int net_reject_reason(void) { return reject_reason; }
bool net_update_available(void) { return update_available; }
int net_latest_build(void) { return latest_build; }
void net_want_list(bool want) { want_list = want; }
const NetLobbyInfo *net_lobbies(void) { return lobbies; }
const NetLobby *net_lobby(void) { return &lobby; }

const NetPlayer *net_player(int slot) {
    for(int i = 0; i < lobby.count; i++)
        if(lobby.players[i].slot == slot) return &lobby.players[i];
    return NULL;
}

const NetPlayer *net_me(void) {
    return state == NET_LOBBY ? net_player(lobby.you) : NULL;
}

void net_join(int id) {
    if(state != NET_BROWSE) return;
    join_id = id;
    state = NET_JOINING;
    last_join = 0;
}

void net_leave(void) {
    if(state != NET_LOBBY && state != NET_JOINING) return;
    for(int i = 0; i < 3; i++) send_packet(4, NULL, 0);
    state = NET_BROWSE;
    lobby.id = -1;
    join_id = -1;
    last_list = 0;
}

void net_wish(int colour, int track, int laps, int cc) {
    bool changed = colour != wish_colour || track != wish_track || laps != wish_laps || cc != wish_cc;
    wish_colour = colour;
    wish_track = track;
    wish_laps = laps;
    wish_cc = cc;
    if(changed) last_wish = 0;   /* send now */
}

void net_press_start(void) {
    start_nonce++;
    last_wish = 0;
}

void net_set_kart(const NetKart *k, int race_id) {
    my_kart = *k;
    my_race_id = race_id;
    have_kart = true;
}

static void encode_kart(uint8_t *b, const NetKart *k) {
    b[0] = k->flags;
    put16(b + 1, (uint16_t)(int16_t)clampf(k->x * 16.0f, -32767, 32767));
    put16(b + 3, (uint16_t)(int16_t)clampf(k->y * 64.0f, -32767, 32767));
    put16(b + 5, (uint16_t)(int16_t)clampf(k->z * 16.0f, -32767, 32767));
    float h = k->heading;
    while(h < 0) h += DK_TAU;
    while(h >= DK_TAU) h -= DK_TAU;
    put16(b + 7, (uint16_t)(h * (65536.0f / DK_TAU)));
    put16(b + 9, (uint16_t)(int16_t)clampf(k->speed * 64.0f, -32767, 32767));
    b[11] = (uint8_t)(int8_t)clampf(k->steer * 127.0f, -127, 127);
    b[12] = (uint8_t)k->stage;
    b[13] = (uint8_t)clampf((float)k->lap, 0, 255);
    b[14] = 0;
    put16(b + 15, (uint16_t)clampf(k->dist * 16.0f, 0, 65535));
    put32(b + 17, (uint32_t)(fmaxf(k->race_time, 0) * 1000.0f));
}

static void decode_kart(const uint8_t *b, NetKart *k) {
    k->flags = b[0];
    k->x = (float)(int16_t)get16(b + 1) / 16.0f;
    k->y = (float)(int16_t)get16(b + 3) / 64.0f;
    k->z = (float)(int16_t)get16(b + 5) / 16.0f;
    k->heading = (float)get16(b + 7) * (DK_TAU / 65536.0f);
    k->speed = (float)(int16_t)get16(b + 9) / 64.0f;
    k->steer = (float)(int8_t)b[11] / 127.0f;
    k->stage = b[12];
    k->lap = b[13];
    k->dist = (float)get16(b + 15) / 16.0f;
    k->race_time = (float)get32(b + 17) / 1000.0f;
}

static void copy_name(char *dst, const uint8_t *src) {
    memcpy(dst, src, NET_NAME_LEN);
    dst[NET_NAME_LEN - 1] = '\0';
}

static int my_ping(void) {
    const NetPlayer *me = net_player(lobby.you);
    return me ? me->ping : 0;
}

static void handle(const uint8_t *b, int n, uint64_t now) {
    if(n < 8 || b[0] != 'D' || b[1] != 'K' || b[2] != DK_NET_PROTO) return;
    last_rx = now;
    echo_ms = get32(b + 4);
    echo_at = now;
    switch(b[3]) {
        case 64:   /* WELCOME */
            if(n < 15) return;
            update_available = b[8] == 1;
            latest_build = get16(b + 9);
            min_build = get16(b + 11);
            if(state == NET_HELLO) {
                state = NET_BROWSE;
                last_list = 0;
            }
            break;
        case 65:   /* REJECT */
            if(n < 9) return;
            if(n >= 13) {
                latest_build = get16(b + 9);
                min_build = get16(b + 11);
            }
            if(b[8] == NREJ_SESSION) {   /* the server restarted: say hello again */
                state = NET_HELLO;
                last_hello = 0;
            }
            else if(b[8] == NREJ_LOBBY_FULL) {
                if(state == NET_JOINING) state = NET_BROWSE;
                message = "THAT LOBBY IS FULL";
            }
            else {
                reject_reason = b[8];
                message = b[8] == NREJ_TOO_OLD ? "THIS VERSION IS TOO OLD TO PLAY ONLINE"
                        : b[8] == NREJ_FULL ? "THE SERVER IS FULL" : "THE SERVER REFUSED THIS NAME";
                state = NET_REJECTED;
            }
            break;
        case 66:   /* LOBBIES */
            if(n < 9) return;
            for(int i = 0; i < b[8] && i < NET_LOBBIES && 9 + i * 14 + 14 <= n; i++) {
                const uint8_t *e = b + 9 + i * 14;
                lobbies[i].players = e[0];
                lobbies[i].state = e[1];
                lobbies[i].track = e[2];
                copy_name(lobbies[i].leader, e + 3);
            }
            break;
        case 67: { /* LOBBY */
            if(n < 22 || (state != NET_JOINING && state != NET_LOBBY)) return;
            if(state == NET_JOINING && b[8] != join_id) return;
            NetLobby *l = &lobby;
            int old_race = l->race_id;
            l->id = b[8];
            l->state = b[9];
            l->you = b[10];
            l->leader = b[11] == 255 ? -1 : b[11];
            l->track = b[12];
            l->laps = b[13];
            l->cc = b[14];
            l->race_id = b[15];
            int32_t t = (int32_t)get32(b + 16);
            if(l->state == NLS_COUNTDOWN || l->state == NLS_RACING)
                l->green_ms = now + (int64_t)t - my_ping() / 2;
            l->nonce = b[20];
            int count = b[21];
            l->count = 0;
            for(int i = 0; i < count && i < NET_MAX_PLAYERS && 22 + i * 20 + 20 <= n; i++) {
                const uint8_t *e = b + 22 + i * 20;
                NetPlayer *p = &l->players[l->count++];
                p->slot = e[0];
                copy_name(p->name, e + 1);
                p->colour = e[12];
                p->flags = e[13];
                p->ping = get16(e + 14);
                p->points = get16(e + 16);
                p->place = e[18];
                p->grid = e[19];
            }
            if(l->race_id != old_race) memset(hist_n, 0, sizeof(hist_n));
            state = NET_LOBBY;
            message = "";
            break;
        }
        case 68: { /* RACE */
            if(n < 10 || state != NET_LOBBY || b[8] != lobby.race_id) return;
            int count = b[9];
            for(int i = 0; i < count && 10 + i * 24 + 24 <= n; i++) {
                const uint8_t *e = b + 10 + i * 24;
                int slot = e[0];
                if(slot >= NET_MAX_PLAYERS || slot == lobby.you) continue;
                uint64_t t = now - get16(e + 1) - my_ping() / 2;
                Sample *h = history[slot];
                if(hist_n[slot] > 0 && t <= h[hist_n[slot] - 1].t) continue;   /* duplicate or stale */
                if(hist_n[slot] == HISTORY) {
                    memmove(h, h + 1, sizeof(Sample) * (HISTORY - 1));
                    hist_n[slot]--;
                }
                h[hist_n[slot]].t = t;
                decode_kart(e + 3, &h[hist_n[slot]].k);
                hist_n[slot]++;
            }
            break;
        }
    }
}

void net_update(void) {
    uint64_t now = net_now();
    if(state == NET_OFF || state == NET_REJECTED) return;
    if(state == NET_LINK) {
        int ls = link_stage();
        if(ls == LINK_UP) {
            open_socket();
            if(sock < 0) {
                state = NET_LOST;
                message = "COULD NOT OPEN A NETWORK SOCKET";
                return;
            }
            make_token();
            state = NET_HELLO;
            last_hello = 0;
            last_rx = now;
        }
        else if(ls == LINK_FAILED) {
            state = NET_LOST;
            message = link_status();
        }
        return;
    }
    if(link_stage() == LINK_FAILED) {
        state = NET_LOST;
        message = link_status();
        return;
    }

    uint8_t buf[512];
    for(int i = 0; i < 64; i++) {
        int n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        if(n <= 0) break;
        handle(buf, n, now);
    }
    if(state == NET_LOST) return;
    if(now - last_rx > (state == NET_HELLO ? 12000u : 6000u)) {
        state = NET_LOST;
        message = "LOST CONNECTION TO THE SERVER";
        return;
    }

    switch(state) {
        case NET_HELLO:
            if(now - last_hello >= 500) {
                send_packet(1, (const uint8_t *)my_name, NET_NAME_LEN);
                last_hello = now;
            }
            break;
        case NET_BROWSE:
            if(now - last_list >= (want_list ? 500u : 1000u)) {
                send_packet(want_list ? 2 : 7, NULL, 0);
                last_list = now;
            }
            break;
        case NET_JOINING:
            if(now - last_join >= 500) {
                uint8_t id = (uint8_t)join_id;
                send_packet(3, &id, 1);
                last_join = now;
            }
            break;
        case NET_LOBBY: {
            const NetPlayer *me = net_me();
            bool racing = me && (me->flags & NPF_RACING) && lobby.state != NLS_WAITING;
            if(racing && have_kart && my_race_id == lobby.race_id) {
                if(now - last_kart >= 83) {
                    uint8_t b[26];
                    b[0] = (uint8_t)lobby.race_id;
                    encode_kart(b + 1, &my_kart);
                    put32(b + 22, (uint32_t)now);
                    send_packet(6, b, 26);
                    last_kart = now;
                }
            }
            if(now - last_wish >= 250) {
                uint8_t b[5] = {(uint8_t)wish_colour, (uint8_t)wish_track, (uint8_t)wish_laps, (uint8_t)wish_cc,
                                start_nonce};
                send_packet(5, b, 5);
                last_wish = now;
            }
            break;
        }
    }
    if(now - last_ping >= 1000 && state != NET_HELLO) {
        last_ping = now;
        if(state == NET_JOINING) send_packet(7, NULL, 0);
    }
}

static float lerp_angle(float a, float b, float t) {
    return a + wrap_angle(b - a) * t;
}

bool net_remote_kart(int slot, uint64_t at_ms, NetKart *out) {
    if(slot < 0 || slot >= NET_MAX_PLAYERS || hist_n[slot] == 0) return false;
    const Sample *h = history[slot];
    int n = hist_n[slot];
    if(at_ms <= h[0].t) {
        *out = h[0].k;
        return true;
    }
    for(int i = 0; i + 1 < n; i++) {
        if(at_ms <= h[i + 1].t) {
            const NetKart *a = &h[i].k, *b = &h[i + 1].k;
            float t = (float)(at_ms - h[i].t) / (float)(h[i + 1].t - h[i].t);
            *out = *b;
            out->x = lerpf(a->x, b->x, t);
            out->y = lerpf(a->y, b->y, t);
            out->z = lerpf(a->z, b->z, t);
            out->heading = lerp_angle(a->heading, b->heading, t);
            out->speed = lerpf(a->speed, b->speed, t);
            out->steer = lerpf(a->steer, b->steer, t);
            return true;
        }
    }
    /* Past the newest sample: carry on along its heading for a moment. */
    *out = h[n - 1].k;
    float dt = fminf((float)(at_ms - h[n - 1].t) * 0.001f, 0.25f);
    out->x += sinf(out->heading) * out->speed * dt;
    out->z += cosf(out->heading) * out->speed * dt;
    return true;
}
