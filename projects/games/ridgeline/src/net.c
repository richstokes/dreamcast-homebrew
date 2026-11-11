/* Ridgeline online client.

   One non-blocking UDP socket. Nothing is acknowledged: the server resends
   the lobby state four times a second and this side resends what it wants
   (to join) until the state shows it, so a lost packet on a dial-up line
   only delays things.

   Trucks: each console sends its own 12 times a second; the server relays
   everyone's newest to everyone else on its own beat. Each sample is
   placed on our clock from the server's age for it, then spaced by the
   sender's own clock (in the packet) so network jitter does not show as
   speed wobble. Trucks are drawn far enough in the past that the next
   sample has normally arrived, interpolated between the two either side;
   that delay adapts to how late samples arrive. On a gap a truck carries
   on along its velocity, and when the real sample lands the difference
   fades out instead of snapping. (DreamKarts' net.c does the same for its
   karts; its comments say why each piece is there.) */
#include "net.h"
#include "netlink.h"
#include <kos.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>

#define HISTORY 8
#define SEND_MS 83           /* our truck goes out 12 times a second */
#define INTERP_MIN_MS 110    /* never draw remote trucks less than this far in the past */
#define INTERP_MAX_MS 400
#define INTERP_MARGIN_MS 25  /* on top of the latest sample arrival and one sample spacing */

typedef struct {
    uint64_t t;      /* local ms the state was current */
    NetTruck k;
} Sample;

static int state = NET_OFF;
static int sock = -1;
static struct sockaddr_in server;
static uint32_t token;
static char my_name[NET_NAME_LEN];
static uint64_t last_rx, last_hello, last_list, last_join, last_send, last_ping;
static uint32_t echo_ms;
static uint64_t echo_at;
static int reject_reason;
static bool update_available, want_list;
static const char *message = "";
static NetLobbyInfo lobbies[NET_LOBBIES];
static NetLobby lobby;
static int join_id = -1, join_model, join_livery;
static NetTruck my_truck;
static bool have_truck;
static bool received;       /* a packet arrived in this net_update */
static Sample history[NET_MAX_PLAYERS][HISTORY];
static int hist_n[NET_MAX_PLAYERS];
static uint8_t last_raw[NET_MAX_PLAYERS][NET_TRUCK_BYTES];
static bool have_raw[NET_MAX_PLAYERS];
static int64_t clk_off16[NET_MAX_PLAYERS];   /* local ms of the sender's clock zero, x16, smoothed */
static bool clk_known[NET_MAX_PLAYERS];
static uint64_t last_at[NET_MAX_PLAYERS];    /* the time each truck was last drawn at */
static V3 draw_err[NET_MAX_PLAYERS];         /* drawn minus network, fading */
static float lag_peak;
static float interp_ms = 150.0f;
static uint64_t step_base_ms, step_us;

uint64_t net_now(void) {
    return timer_ms_gettime64();
}

/* ---- little-endian packing (the SH-4 faults on unaligned access) ---- */

static void put16(uint8_t *p, uint32_t v) { p[0] = v & 255; p[1] = (v >> 8) & 255; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }
static void puts16(uint8_t *p, float v, float lim) { put16(p, (uint16_t)(int16_t)clampf(v, -lim, lim)); }
static float gets16(const uint8_t *p) { return (float)(int16_t)get16(p); }

static void send_packet(int type, const uint8_t *body, int len) {
    if(sock < 0) return;
    uint8_t b[64];
    uint64_t now = net_now();
    b[0] = 'R';
    b[1] = 'L';
    b[2] = RL_NET_PROTO;
    b[3] = (uint8_t)type;
    put32(b + 4, token);
    put32(b + 8, echo_ms);
    put16(b + 12, echo_at ? (uint32_t)(now - echo_at) : 0);
    put16(b + 14, RL_NET_BUILD);
    put16(b + 16, 0);
    if(len > (int)sizeof(b) - 18) return;
    if(len) memcpy(b + 18, body, len);
    sendto(sock, b, 18 + len, 0, (struct sockaddr *)&server, sizeof(server));
}

static uint32_t mix(uint32_t h, uint32_t v) {
    h = (h ^ v) * 16777619u;
    return h ^ (h >> 15);
}

/* The session token must not repeat between consoles; boot timing alone
   can (emulators boot the same way every time), so it mixes in when the
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
    h = mix(h, eng_rand());
    token = h ? h : 1;
}

static void open_socket(void) {
    if(sock >= 0) return;
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock < 0) return;
    fcntl(sock, F_SETFL, O_NONBLOCK);
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons(RL_NET_PORT);
    server.sin_addr.s_addr = netlink_server_ip();
}

static void forget_trucks(void) {
    memset(hist_n, 0, sizeof(hist_n));
    memset(have_raw, 0, sizeof(have_raw));
    memset(clk_known, 0, sizeof(clk_known));
    memset(last_at, 0, sizeof(last_at));
    memset(draw_err, 0, sizeof(draw_err));
}

void net_forget_slot(int slot) {
    if(slot < 0 || slot >= NET_MAX_PLAYERS) return;
    hist_n[slot] = 0;
    have_raw[slot] = clk_known[slot] = false;
    last_at[slot] = 0;
    draw_err[slot] = v3(0, 0, 0);
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
    lag_peak = 0;
    interp_ms = 150.0f;
    have_truck = false;
    forget_trucks();
    state = NET_LINK;
    netlink_set_server(RL_NET_HOST, RL_NET_FALLBACK_IP);
    netlink_start();
}

void net_end(void) {
    if(state == NET_LOBBY || state == NET_JOINING)
        for(int i = 0; i < 3; i++) send_packet(4, NULL, 0);
    state = NET_OFF;
    lobby.id = -1;
    join_id = -1;
}

int net_state(void) { return state; }
const char *net_message(void) { return state == NET_LINK ? netlink_status() : message; }
int net_reject_reason(void) { return reject_reason; }
bool net_update_available(void) { return update_available; }
void net_want_list(bool want) { want_list = want; }
const NetLobbyInfo *net_lobbies(void) { return lobbies; }
const NetLobby *net_lobby(void) { return &lobby; }

const NetPlayer *net_player(int slot) {
    for(int i = 0; i < lobby.count; i++)
        if(lobby.players[i].slot == slot) return &lobby.players[i];
    return NULL;
}

void net_join(int id, int model, int livery) {
    if(state != NET_BROWSE) return;
    join_id = id;
    join_model = model;
    join_livery = livery;
    message = "";
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
    have_truck = false;
    forget_trucks();
}

void net_set_truck(const NetTruck *t) {
    my_truck = *t;
    have_truck = true;
}

static void encode_truck(uint8_t *b, const NetTruck *t) {
    b[0] = t->flags;
    puts16(b + 1, t->pos.x * 64.0f, 32767);
    puts16(b + 3, t->pos.y * 64.0f, 32767);
    puts16(b + 5, t->pos.z * 64.0f, 32767);
    for(int i = 0; i < 4; i++) puts16(b + 7 + i * 2, t->q[i] * 32767.0f, 32767);
    puts16(b + 15, t->vel.x * 256.0f, 32767);
    puts16(b + 17, t->vel.y * 256.0f, 32767);
    puts16(b + 19, t->vel.z * 256.0f, 32767);
    b[21] = (uint8_t)(int8_t)clampf(t->steer * 160.0f, -127, 127);
    b[22] = (uint8_t)clampf(t->rpm / 32.0f, 0, 255);
    b[23] = (uint8_t)clampf(t->load * 255.0f, 0, 255);
    b[24] = (uint8_t)clampf(t->slip * 16.0f, 0, 255);
    b[25] = (uint8_t)(int8_t)clampf((float)t->gear, -1, 9);
    put32(b + 26, (uint32_t)net_now());
}

static void decode_truck(const uint8_t *b, NetTruck *t) {
    t->flags = b[0];
    t->pos = v3(gets16(b + 1) / 64.0f, gets16(b + 3) / 64.0f, gets16(b + 5) / 64.0f);
    float l = 0;
    for(int i = 0; i < 4; i++) {
        t->q[i] = gets16(b + 7 + i * 2) / 32767.0f;
        l += t->q[i] * t->q[i];
    }
    l = l > 1e-6f ? 1.0f / sqrtf(l) : 0;
    for(int i = 0; i < 4; i++) t->q[i] *= l;
    if(l == 0) t->q[3] = 1;
    t->vel = v3(gets16(b + 15) / 256.0f, gets16(b + 17) / 256.0f, gets16(b + 19) / 256.0f);
    t->steer = (float)(int8_t)b[21] / 160.0f;
    t->rpm = (float)b[22] * 32.0f;
    t->load = (float)b[23] / 255.0f;
    t->slip = (float)b[24] / 16.0f;
    t->gear = (int8_t)b[25];
}

static void copy_name(char *dst, const uint8_t *src) {
    memcpy(dst, src, NET_NAME_LEN);
    dst[NET_NAME_LEN - 1] = '\0';
    for(char *p = dst; *p; p++)
        if((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '?';
}

/* A remote truck's state at local time at_ms: between the two samples
   either side, or carried on past the newest along its velocity. */
static void sample(int slot, uint64_t at_ms, NetTruck *out) {
    const Sample *h = history[slot];
    int n = hist_n[slot];
    if(at_ms <= h[0].t) {
        *out = h[0].k;
        return;
    }
    for(int i = 0; i + 1 < n; i++) {
        if(at_ms <= h[i + 1].t) {
            const NetTruck *a = &h[i].k, *b = &h[i + 1].k;
            float t = (float)(at_ms - h[i].t) / (float)(h[i + 1].t - h[i].t);
            *out = *b;
            out->pos = vlerp(a->pos, b->pos, t);
            out->vel = vlerp(a->vel, b->vel, t);
            /* Quaternions: the shorter way round, then normalised. */
            float d = a->q[0] * b->q[0] + a->q[1] * b->q[1] + a->q[2] * b->q[2] + a->q[3] * b->q[3];
            float s = d < 0 ? -1.0f : 1.0f, l = 0;
            for(int k = 0; k < 4; k++) {
                out->q[k] = lerpf(a->q[k], b->q[k] * s, t);
                l += out->q[k] * out->q[k];
            }
            l = 1.0f / sqrtf(fmaxf(l, 1e-6f));
            for(int k = 0; k < 4; k++) out->q[k] *= l;
            out->steer = lerpf(a->steer, b->steer, t);
            out->rpm = lerpf(a->rpm, b->rpm, t);
            out->load = lerpf(a->load, b->load, t);
            return;
        }
    }
    const NetTruck *b = &h[n - 1].k;
    *out = *b;
    float dt = fminf((float)(at_ms - h[n - 1].t) * 0.001f, 0.3f);
    out->pos = vmad(b->pos, b->vel, dt);
}

/* A new sample moved where a truck is drawn (it had run on past the old
   newest one): keep drawing it where it was and fade the difference out.
   A jump of metres is a real teleport (a recovery) and is shown as one. */
static void absorb(int slot, const NetTruck *before, const NetTruck *after) {
    V3 d = vsub(before->pos, after->pos);
    if(fabsf(d.x) > 4.0f || fabsf(d.y) > 4.0f || fabsf(d.z) > 4.0f) {
        draw_err[slot] = v3(0, 0, 0);
        return;
    }
    draw_err[slot] = vadd(draw_err[slot], d);
}

static void take_truck(int slot, const uint8_t *e, uint64_t now) {
    const uint8_t *raw = e + 3;
    if(have_raw[slot] && memcmp(raw, last_raw[slot], NET_TRUCK_BYTES) == 0) return;   /* relayed again */
    memcpy(last_raw[slot], raw, NET_TRUCK_BYTES);
    have_raw[slot] = true;
    NetTruck k;
    decode_truck(raw, &k);
    /* When the state was current, by the server's age for it, then spaced
       by the sender's clock through a slow average of the two clocks'
       offset: jitter averages out, drift is followed. */
    uint64_t age = get16(e + 1);
    uint64_t t = now - (age < now ? age : now);
    uint32_t sc = get32(raw + 26);
    int64_t off16 = ((int64_t)t - (int64_t)sc) * 16;
    if(!clk_known[slot] || off16 - clk_off16[slot] > 500 * 16 || clk_off16[slot] - off16 > 500 * 16) {
        clk_off16[slot] = off16;
        clk_known[slot] = true;
    }
    else {
        clk_off16[slot] += (off16 - clk_off16[slot]) / 16;
    }
    t = (uint64_t)((int64_t)sc + clk_off16[slot] / 16);
    float lag = (float)((int64_t)now - (int64_t)t);
    if(lag > lag_peak) lag_peak = fminf(lag, lag_peak + 40.0f);
    Sample *h = history[slot];
    int n = hist_n[slot];
    if(n > 0 && t <= h[n - 1].t) return;   /* stale */
    bool ran_on = n > 0 && last_at[slot] > h[n - 1].t;
    NetTruck before;
    if(ran_on) sample(slot, last_at[slot], &before);
    if(n == HISTORY) {
        memmove(h, h + 1, sizeof(Sample) * (HISTORY - 1));
        n--;
    }
    h[n].t = t;
    h[n].k = k;
    hist_n[slot] = n + 1;
    if(ran_on) {
        NetTruck after;
        sample(slot, last_at[slot], &after);
        absorb(slot, &before, &after);
    }
}

static void take_lobby(const uint8_t *b, int n, uint64_t now) {
    if(n < 13 || (state != NET_JOINING && state != NET_LOBBY)) return;
    if(state == NET_JOINING && b[8] != join_id) return;
    NetLobby *l = &lobby;
    if(l->id != b[8] || l->you != b[9]) forget_trucks();
    l->id = b[8];
    l->you = b[9];
    l->hour = (float)get16(b + 10) / 2000.0f;
    int count = b[12];
    l->count = 0;
    for(int i = 0; i < count && i < NET_MAX_PLAYERS && 13 + (i + 1) * NET_PLAYER_BYTES <= n; i++) {
        const uint8_t *e = b + 13 + i * NET_PLAYER_BYTES;
        NetPlayer *p = &l->players[l->count++];
        p->slot = e[0] % NET_MAX_PLAYERS;
        copy_name(p->name, e + 1);
        p->model = e[12] % TRUCK_MODELS;
        p->livery = e[13] & 3;
        p->flags = e[14];
        p->ping = get16(e + 15);
    }
    (void)now;
    state = NET_LOBBY;
    message = "";
}

static void take_lobbies(const uint8_t *b, int n) {
    int o = 9;
    for(int i = 0; i < b[8] && i < NET_LOBBIES && o + 2 <= n; i++) {
        NetLobbyInfo *e = &lobbies[i];
        int count = b[o];
        e->hour = (float)b[o + 1] / 10.0f;
        o += 2;
        e->count = 0;
        for(int k = 0; k < count && o + 1 + NET_NAME_LEN <= n; k++, o += 1 + NET_NAME_LEN) {
            if(e->count >= NET_MAX_PLAYERS) continue;
            e->flags[e->count] = b[o];
            copy_name(e->names[e->count], b + o + 1);
            e->count++;
        }
    }
}

static void handle(const uint8_t *b, int n, uint64_t now) {
    if(n < 8 || b[0] != 'R' || b[1] != 'L') return;
    if(b[2] != RL_NET_PROTO) {
        /* A server on another protocol answers our HELLO with a REJECT
           stamped with its own protocol byte. The header and the reject
           body never move, so read it and say why. */
        if(b[3] != 65 || n < 9) return;
        last_rx = now;
        reject_reason = NREJ_TOO_OLD;
        message = "THIS VERSION IS TOO OLD TO PLAY ONLINE";
        state = NET_REJECTED;
        return;
    }
    last_rx = now;
    received = true;
    echo_ms = get32(b + 4);
    echo_at = now;
    switch(b[3]) {
        case 64:   /* WELCOME */
            if(n < 15) return;
            update_available = b[8] == 1;
            if(state == NET_HELLO) {
                state = NET_BROWSE;
                last_list = 0;
            }
            break;
        case 65:   /* REJECT */
            /* Stray TRUCKs reaching the server after an idle kick come
               back as session rejects: keep the first reason. */
            if(n < 9 || state == NET_REJECTED) return;
            if(b[8] == NREJ_SESSION) {   /* the server restarted: say hello again */
                state = NET_HELLO;
                last_hello = 0;
                lobby.id = -1;
                join_id = -1;
            }
            else if(b[8] == NREJ_LOBBY_FULL) {
                if(state == NET_JOINING) state = NET_BROWSE;
                message = "THAT LOBBY IS FULL";
            }
            else {
                reject_reason = b[8];
                message = b[8] == NREJ_TOO_OLD ? "THIS VERSION IS TOO OLD TO PLAY ONLINE"
                        : b[8] == NREJ_FULL ? "THE SERVER IS FULL"
                        : b[8] == NREJ_IDLE ? "IDLE FOR 10 MINUTES"
                        : "THE SERVER REFUSED THIS NAME";
                state = NET_REJECTED;
            }
            break;
        case 66:   /* LOBBIES */
            if(n >= 9) take_lobbies(b, n);
            break;
        case 67:   /* LOBBY */
            take_lobby(b, n, now);
            break;
        case 68: { /* TRUCKS */
            if(n < 9 || state != NET_LOBBY) return;
            const int per = 3 + NET_TRUCK_BYTES;
            for(int i = 0; i < b[8] && 9 + (i + 1) * per <= n; i++) {
                const uint8_t *e = b + 9 + i * per;
                int slot = e[0];
                if(slot < NET_MAX_PLAYERS && slot != lobby.you) take_truck(slot, e, now);
            }
            break;
        }
    }
}

void net_update(void) {
    uint64_t now = net_now();
    if(state == NET_OFF || state == NET_REJECTED) return;
    if(state == NET_LINK) {
        int ls = netlink_stage();
        if(ls == NETLINK_UP) {
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
        else if(ls == NETLINK_FAILED) {
            state = NET_LOST;
            message = netlink_status();
        }
        return;
    }
    if(netlink_stage() == NETLINK_FAILED) {
        state = NET_LOST;
        message = netlink_status();
        return;
    }

    uint8_t buf[1024];   /* a LOBBIES packet with every lobby full is ~760 bytes */
    received = false;
    for(int i = 0; i < 64; i++) {
        int n = recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL);
        if(n <= 0) break;
        handle(buf, n, now);
    }
    if(state == NET_LOST || state == NET_REJECTED) return;
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
                uint8_t b[3] = {(uint8_t)join_id, (uint8_t)join_model, (uint8_t)join_livery};
                send_packet(3, b, 3);
                last_join = now;
            }
            break;
        case NET_LOBBY:
            /* Not in a frame that received something: the next frame is
               soon enough, and keeps our sends clear of the adapter's
               receive path (a DreamKarts lesson: a send there sometimes
               double faulted KOS in Flycast). */
            if(have_truck && !received && now - last_send >= SEND_MS) {
                uint8_t b[NET_TRUCK_BYTES];
                encode_truck(b, &my_truck);
                send_packet(5, b, sizeof(b));
                last_send = now;
                last_ping = now;
            }
            break;
    }
    if(now - last_ping >= 1000 && (state == NET_JOINING || state == NET_LOBBY)) {
        last_ping = now;
        send_packet(7, NULL, 0);
    }
}

int net_interp_ms(void) {
    return (int)interp_ms;
}

uint64_t net_draw_time(float dt) {
    /* Far enough back that the sample after the drawn moment has normally
       arrived; the delay slews, and the peak fades so a one-off late
       packet does not hold the trucks back for long. */
    lag_peak = fmaxf(lag_peak - 25.0f * dt, 0.0f);
    float target = clampf(lag_peak + SEND_MS + INTERP_MARGIN_MS, INTERP_MIN_MS, INTERP_MAX_MS);
    interp_ms = approach(interp_ms, target, 60.0f * dt);
    /* In game time, a fixed step per physics step like our own truck and
       the camera, so a frame that runs two steps moves everything
       together; rebased only after a long stall. */
    uint64_t now = net_now();
    step_us += (uint64_t)(dt * 1000000.0f + 0.5f);
    uint64_t game_ms = step_base_ms + step_us / 1000;
    int64_t off = (int64_t)now - (int64_t)game_ms;
    if(off > 100 || off < -100) {
        step_base_ms = now;
        step_us = 0;
        game_ms = now;
    }
    return game_ms - (uint64_t)interp_ms;
}

bool net_remote_truck(int slot, uint64_t at_ms, NetTruck *out) {
    if(slot < 0 || slot >= NET_MAX_PLAYERS || hist_n[slot] == 0) return false;
    sample(slot, at_ms, out);
    float dt = at_ms > last_at[slot] ? fminf((float)(at_ms - last_at[slot]) * 0.001f, 0.1f) : 0.0f;
    last_at[slot] = at_ms;
    draw_err[slot] = vscale(draw_err[slot], 1.0f - smooth_k(10.0f, dt));
    out->pos = vadd(out->pos, draw_err[slot]);
    return true;
}
