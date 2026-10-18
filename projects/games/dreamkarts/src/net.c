/* DreamKarts online client.

   One non-blocking UDP socket. Nothing is acknowledged: the server resends
   lobby state four times a second and this side resends what it wants
   (join, colour, settings, start) until the state shows it, so a lost
   packet on a dial-up line only delays things.

   Remote karts: the server relays everyone's newest kart 12 times a
   second, on its own beat. Our KART packets are timed to reach it just
   before each relay, so they wait there as little as possible. Each
   relayed sample is placed on our clock from the server's age for it,
   then spaced by the sender's own race clock (in the packet) so network
   jitter does not show as speed wobble. Karts are drawn far enough in the
   past that the next sample has normally arrived (interpolated between
   the two either side); that delay adapts to how late samples arrive. On
   the rare gap a kart carries on along its curve, and when the real
   sample then lands, the difference fades out over a tenth of a second
   instead of snapping. */
#include "net.h"
#include "netlink.h"
#include <kos.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>

#define HISTORY 8
#define RELAY_MS 83          /* the server's kart relay interval */
#define SEND_LEAD_MS 50      /* aim our KART this long before the next relay, less our ping */
#define INTERP_MIN_MS 108    /* never draw remote karts less than this far in the past */
#define INTERP_MAX_MS 350
#define INTERP_MARGIN_MS 25  /* on top of the latest sample arrival and one sample spacing */

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
static bool spectating;
static int wish_colour, wish_track, wish_laps = 3, wish_cc = 1;
static uint8_t start_nonce;
static NetKart my_kart;
static int my_race_id;
static bool have_kart;
static Sample history[NET_MAX_PLAYERS][HISTORY];
static int hist_n[NET_MAX_PLAYERS];
static uint8_t last_raw[NET_MAX_PLAYERS][NET_KART_BYTES];   /* the newest sample's bytes: a relay repeat is skipped */
static bool have_raw[NET_MAX_PLAYERS];
static int64_t rt_off16[NET_MAX_PLAYERS];   /* local ms of the sender's race clock zero, x16, smoothed */
static bool rt_known[NET_MAX_PLAYERS];
static uint64_t last_at[NET_MAX_PLAYERS];   /* the time each kart was last drawn at */
static float draw_err[NET_MAX_PLAYERS][4];  /* x, y, z, heading: drawn minus network, fading */
static float lag_peak;      /* ms: how far behind our clock samples arrive, peak held */
static float interp_ms = 150.0f;   /* how far in the past remote karts are drawn */
static uint64_t step_base_ms, step_us;   /* game time: the clock at the last rebase, plus the steps since */
static uint64_t kart_due;   /* when the next KART goes out */
static bool kart_armed;     /* kart_due was set from a relay (not the fallback) */
static bool received;       /* a packet arrived in this net_update */
static bool green_locked;   /* green_ms is settled for this race */

static void forget_karts(void);

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
    lag_peak = 0;
    interp_ms = 150.0f;
    kart_due = 0;
    kart_armed = false;
    green_locked = false;
    forget_karts();
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
    spectating = false;
    state = NET_JOINING;
    last_join = 0;
}

void net_spectate(int id) {
    net_join(id);
    spectating = state == NET_JOINING;
}

bool net_spectating(void) {
    return spectating && (state == NET_JOINING || state == NET_LOBBY);
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

void net_encode_kart(uint8_t *b, const NetKart *k) {
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

static void forget_karts(void) {
    memset(hist_n, 0, sizeof(hist_n));
    memset(have_raw, 0, sizeof(have_raw));
    memset(rt_known, 0, sizeof(rt_known));
    memset(last_at, 0, sizeof(last_at));
    memset(draw_err, 0, sizeof(draw_err));
}

static float lerp_angle(float a, float b, float t) {
    return a + wrap_angle(b - a) * t;
}

/* A remote kart's network state at local time at_ms: between the two
   samples either side, or carried on past the newest one along the curve
   it was on. */
static void sample(int slot, uint64_t at_ms, NetKart *out) {
    const Sample *h = history[slot];
    int n = hist_n[slot];
    if(at_ms <= h[0].t) {
        *out = h[0].k;
        return;
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
            return;
        }
    }
    const NetKart *b = &h[n - 1].k;
    *out = *b;
    float dt = fminf((float)(at_ms - h[n - 1].t) * 0.001f, 0.3f);
    float yaw = 0, vy = 0;
    if(n >= 2) {
        float span = (float)(h[n - 1].t - h[n - 2].t) * 0.001f;
        if(span > 0.01f) {
            yaw = clampf(wrap_angle(b->heading - h[n - 2].k.heading) / span, -3.0f, 3.0f);
            if(!(b->flags & NKF_GROUNDED)) vy = clampf((b->y - h[n - 2].k.y) / span, -10.0f, 10.0f);
        }
    }
    float mid = b->heading + yaw * dt * 0.5f;
    out->x += sinf(mid) * b->speed * dt;
    out->z += cosf(mid) * b->speed * dt;
    out->y += vy * dt;
    out->heading = b->heading + yaw * dt;
}

/* A new sample moved where a kart is drawn (it had run on past the old
   newest one): keep drawing it where it was and fade the difference out.
   A jump of metres is a real teleport (a rescue) and is shown as one. */
static void absorb(int slot, const NetKart *before, const NetKart *after) {
    float *e = draw_err[slot];
    float dx = before->x - after->x, dy = before->y - after->y, dz = before->z - after->z;
    if(fabsf(dx) > 4.0f || fabsf(dy) > 4.0f || fabsf(dz) > 4.0f) {
        memset(e, 0, sizeof(draw_err[slot]));
        return;
    }
    e[0] += dx;
    e[1] += dy;
    e[2] += dz;
    e[3] = clampf(e[3] + wrap_angle(before->heading - after->heading), -1.0f, 1.0f);
}

static void take_kart(int slot, const uint8_t *e, uint64_t now) {
    const uint8_t *raw = e + 3;
    if(have_raw[slot] && memcmp(raw, last_raw[slot], NET_KART_BYTES) == 0) return;   /* relayed again: nothing new */
    memcpy(last_raw[slot], raw, NET_KART_BYTES);
    have_raw[slot] = true;
    NetKart k;
    decode_kart(raw, &k);
    /* When the state was current, by the server's age for it... */
    uint64_t age = get16(e + 1);
    uint64_t t = now - (age < now ? age : now);
    uint32_t rt = get32(raw + 17);   /* the sender's race clock then (0 before the lights) */
    if(rt > 0 && !(k.flags & NKF_FINISHED)) {
        /* ...then spaced by the sender's race clock, which is anchored to
           ours through a slow average: jitter averages out, clock drift
           is followed. */
        int64_t off16 = ((int64_t)t - (int64_t)rt) * 16;
        if(!rt_known[slot] || off16 - rt_off16[slot] > 500 * 16 || rt_off16[slot] - off16 > 500 * 16) {
            rt_off16[slot] = off16;
            rt_known[slot] = true;
        }
        else {
            rt_off16[slot] += (off16 - rt_off16[slot]) / 16;
        }
        t = (uint64_t)((int64_t)rt + rt_off16[slot] / 16);
    }
    /* A step at a time, so one very late sample does not hold every kart
       back for seconds; a line that stays slow still catches up fast. */
    float lag = (float)((int64_t)now - (int64_t)t);
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    if(lag > lag_peak + 100.0f)
        printf("DK ONLINE: slot %d sample %.0f ms late (age %u, race ms %u, peak %.0f)\n", slot, (double)lag,
               (unsigned)age, (unsigned)rt, (double)lag_peak);
#endif
    if(lag > lag_peak) lag_peak = fminf(lag, lag_peak + 40.0f);
    Sample *h = history[slot];
    int n = hist_n[slot];
    if(n > 0 && t <= h[n - 1].t) return;   /* stale */
    bool ran_on = n > 0 && last_at[slot] > h[n - 1].t;
    NetKart before;
    if(ran_on) sample(slot, last_at[slot], &before);
    if(n == HISTORY) {
        memmove(h, h + 1, sizeof(Sample) * (HISTORY - 1));
        n--;
    }
    h[n].t = t;
    h[n].k = k;
    hist_n[slot] = n + 1;
    if(ran_on) {
        NetKart after;
        sample(slot, last_at[slot], &after);
        absorb(slot, &before, &after);
    }
}

static void handle(const uint8_t *b, int n, uint64_t now) {
    if(n < 8 || b[0] != 'D' || b[1] != 'K' || b[2] != DK_NET_PROTO) return;
    last_rx = now;
    received = true;
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
            /* Build 4 servers add watchers and flags for every lobby. */
            for(int i = 0, o = 9 + b[8] * 14; i < b[8] && i < NET_LOBBIES && o + i * 2 + 2 <= n; i++) {
                lobbies[i].watchers = b[o + i * 2];
                lobbies[i].flags = b[o + i * 2 + 1];
            }
            break;
        case 67: { /* LOBBY */
            if(n < 22 || (state != NET_JOINING && state != NET_LOBBY)) return;
            if(state == NET_JOINING && (b[8] != join_id || (b[10] == NET_SPECTATOR) != spectating)) return;
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
            if(l->race_id != old_race) {
                forget_karts();
                green_locked = false;
            }
            if(l->state == NLS_COUNTDOWN || l->state == NLS_RACING) {
                /* The race clock: settled over the countdown, then held, so
                   lap times and the race clock sent with our kart do not
                   jump with every lobby packet. A long way out (a watcher
                   arriving mid-race, or a bad first ping) is taken as is. */
                uint64_t est = now + (int64_t)t - my_ping() / 2;
                int64_t d = (int64_t)est - (int64_t)l->green_ms;
                if(!green_locked || d > 150 || d < -150) {
                    l->green_ms = est;
                    green_locked = true;
                }
                else if(l->state == NLS_COUNTDOWN) {
                    l->green_ms += d / 4;
                }
            }
            else {
                green_locked = false;
            }
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
            int tail = 22 + count * 20;
            l->watchers = tail + 2 <= n ? b[tail] : 0;
            l->flags = tail + 2 <= n ? b[tail + 1] : 0;
            state = NET_LOBBY;
            message = "";
            break;
        }
        case 68: { /* RACE */
            if(n < 10 || state != NET_LOBBY || b[8] != lobby.race_id) return;
            /* The server relays on a steady beat: send our next kart so it
               lands shortly before the next one (or a later one, on a
               slow line), not just after. A send already lined up from
               the last relay stands: with a long lead it falls due around
               the time this packet arrives, and re-arming it every time
               would push it back for good. */
            if(!kart_armed) {
                int lead = (SEND_LEAD_MS - my_ping()) % RELAY_MS;
                if(lead < 0) lead += RELAY_MS;
                uint64_t want = now + lead;
                if(want < last_kart + 40) want += RELAY_MS;   /* not twice in quick succession */
                kart_due = want;
                kart_armed = true;
            }
            int count = b[9];
            for(int i = 0; i < count && 10 + i * 24 + 24 <= n; i++) {
                const uint8_t *e = b + 10 + i * 24;
                int slot = e[0];
                if(slot < NET_MAX_PLAYERS && slot != lobby.you) take_kart(slot, e, now);
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
    received = false;
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
                uint8_t b[2] = {(uint8_t)join_id, spectating ? 1 : 0};   /* bit0: spectate */
                send_packet(3, b, 2);
                last_join = now;
            }
            break;
        case NET_LOBBY: {
            if(spectating) break;   /* watchers only ping */
            const NetPlayer *me = net_me();
            bool racing = me && (me->flags & NPF_RACING) && lobby.state != NLS_WAITING;
            /* Not in a frame that received something: the next frame is
               soon enough, and keeps our sends clear of the adapter's
               receive path. */
            if(racing && have_kart && my_race_id == lobby.race_id && !received) {
                if(now >= kart_due) {
                    uint8_t b[26];
                    b[0] = (uint8_t)lobby.race_id;
                    net_encode_kart(b + 1, &my_kart);
                    put32(b + 22, (uint32_t)now);
                    send_packet(6, b, 26);
                    last_kart = now;
                    kart_armed = false;
                    kart_due = now + RELAY_MS + 25;   /* only if no relay arrives to set it */
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
        if(state == NET_JOINING || (state == NET_LOBBY && spectating)) send_packet(7, NULL, 0);
    }
}

int net_interp_ms(void) {
    return (int)interp_ms;
}

uint64_t net_draw_time(float dt) {
    /* Far enough back that the sample after the drawn moment has normally
       arrived: the latest arrival seen lately, plus one sample spacing and
       a margin. The delay slews, so a change is a slight speed change
       rather than a jump; the peak fades so a one-off late packet does not
       hold the karts back for long. */
    lag_peak = fmaxf(lag_peak - 25.0f * dt, 0.0f);
    float target = clampf(lag_peak + RELAY_MS + INTERP_MARGIN_MS, INTERP_MIN_MS, INTERP_MAX_MS);
    interp_ms = approach(interp_ms, target, 60.0f * dt);
    /* Measured in game time, which moves a fixed step per physics step
       like our own kart and the camera do, so a frame that runs two steps
       (or none) moves everything together: on the wall clock the rivals
       would hop a step against the camera. The main loop keeps game time
       within a step of the clock; only a stall long enough to be dropped
       there needs a rebase. */
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

#ifdef DK_AUTOTEST
const char *net_debug_sample(int slot) {
    static char line[96];
    int n = hist_n[slot];
    const Sample *h = history[slot];
    snprintf(line, sizeof(line), "[hist %d newest %ld ms ago, prev gap %ld, err %.2f %.2f, lag peak %.0f]", n,
             n ? (long)((int64_t)last_at[slot] - (int64_t)h[n - 1].t) : 0L,
             n >= 2 ? (long)(h[n - 1].t - h[n - 2].t) : 0L, (double)draw_err[slot][0], (double)draw_err[slot][2],
             (double)lag_peak);
    return line;
}
#endif

bool net_remote_kart(int slot, uint64_t at_ms, NetKart *out) {
    if(slot < 0 || slot >= NET_MAX_PLAYERS || hist_n[slot] == 0) return false;
    sample(slot, at_ms, out);
    float dt = at_ms > last_at[slot] ? fminf((float)(at_ms - last_at[slot]) * 0.001f, 0.1f) : 0.0f;
    last_at[slot] = at_ms;
    float *e = draw_err[slot];
    float keep = 1.0f - smooth_k(10.0f, dt);
    for(int i = 0; i < 4; i++) e[i] *= keep;
    out->x += e[0];
    out->y += e[1];
    out->z += e[2];
    out->heading += e[3];
    return true;
}
