/* DreamKarts controller vibration. Game code asks for short pulses and
   per-step holds; once a frame the strongest request goes to the Jump Pack
   in that player's own controller, and only when it changes.

   Packs read the effect timing fields differently (and Flycast differently
   again), so every effect runs continuously and the game stops it itself.
   A held level is re-sent each second to stay ahead of the pack's
   auto-stop timer. */
#include "rumble.h"
#include <dc/maple/purupuru.h>

typedef struct {
    int pulse_pow, pulse_freq;
    float pulse_t;
    int hold_pow, hold_freq;
    float hold_t;
    int sent_pow, sent_freq;
    float since_send;
    maple_device_t *dev;
} Rumble;

static Rumble rb[MAX_PLAYERS];
int rumble_sends;

/* The pack plugged into the player's controller, if any. */
static maple_device_t *find_pack(int player) {
    int port = input_pad(player)->port;
    if(port < 0) return NULL;
    for(int u = 1; u < MAPLE_UNIT_COUNT; u++) {
        maple_device_t *dev = maple_enum_dev(port, u);
        if(dev && (dev->info.functions & MAPLE_FUNC_PURUPURU)) return dev;
    }
    return NULL;
}

static bool pack_send(maple_device_t *dev, int power, int freq) {
    purupuru_effect_t e = {.motor = 1};
    if(power > 0) {
        e.cont = true;
        e.fpow = (uint32_t)(power > 7 ? 7 : power);
        e.freq = (uint8_t)clampf((float)freq, 4, 59);
    }
    if(purupuru_rumble(dev, &e) != MAPLE_EOK) return false;   /* busy: retry next frame */
    rumble_sends++;
    return true;
}

void rumble_pulse(int player, int power, int freq, float seconds) {
    Rumble *r = &rb[player & 1];
    if(r->pulse_t > 0 && power < r->pulse_pow) return;
    if(r->pulse_t > 0 && power == r->pulse_pow && seconds < r->pulse_t) return;
    r->pulse_pow = power;
    r->pulse_freq = freq;
    r->pulse_t = seconds;
}

void rumble_hold(int player, int power, int freq) {
    Rumble *r = &rb[player & 1];
    if(r->hold_t > 0 && power < r->hold_pow) return;
    r->hold_pow = power;
    r->hold_freq = freq;
    r->hold_t = 0.1f;   /* outlives a frame with no simulation step */
}

void rumble_kart(int player, const Kart *k) {
    uint32_t ev = k->events;
    if(ev & EV_FINISH) rumble_pulse(player, 3, 40, 0.35f);
    if(k->finished) return;   /* the CPU drives a finished kart home */
    if(ev & EV_HOP) rumble_pulse(player, 2, 42, 0.07f);
    if(ev & EV_CHARGE_JUMP) rumble_pulse(player, 4, 30, 0.12f);
    if(ev & EV_LAND) rumble_pulse(player, (int)clampf(1.0f + k->land_v * 0.5f, 2, 6), 22, 0.1f);
    if(ev & EV_DRIFT_START) rumble_pulse(player, 1, 42, 0.06f);
    if(ev & EV_CHARGE1) rumble_pulse(player, 2, 46, 0.08f);
    if(ev & EV_CHARGE2) rumble_pulse(player, 3, 46, 0.08f);
    if(ev & EV_CHARGE3) rumble_pulse(player, 4, 46, 0.1f);
    if(ev & EV_BOOST) rumble_pulse(player, 3 + k->boost_stage, 34, 0.2f + 0.12f * (float)k->boost_stage);
    if(ev & EV_DASH) rumble_pulse(player, 5, 34, 0.3f);
    if(ev & EV_KART_BUMP) rumble_pulse(player, 4, 22, 0.15f);
    if(ev & EV_WALL) rumble_pulse(player, (int)clampf(3.0f + k->wall_v * 0.3f, 4, 7), 18, 0.22f);

    /* Rumble strips rattle faster and harder with speed; sand grinds. */
    float spd = fabsf(k->speed);
    if(k->grounded && k->surface == SURF_CURB && spd > 3.0f) {
        float f = clampf(spd / kart_top_speed(k), 0, 1);
        rumble_hold(player, 2 + (int)(f * 2.4f), 12 + 4 * (int)(f * 4.0f));
    }
    else if(k->grounded && k->surface == SURF_SAND && spd > 4.0f && k->boost_t <= 0) {
        rumble_hold(player, 1, 9);
    }
    if(k->burnout_t > 0) rumble_hold(player, 2, 10);
}

void rumble_update(float dt, bool active) {
    for(int p = 0; p < MAX_PLAYERS; p++) {
        Rumble *r = &rb[p];
        maple_device_t *dev = find_pack(p);
        if(dev != r->dev) {
            if(!r->dev != !dev) printf("DreamKarts rumble: player %d jump pack %s\n", p + 1, dev ? "found" : "removed");
            r->dev = dev;
            r->sent_pow = -1;   /* unknown state on a new pack */
        }
        r->since_send += dt;
        if(r->pulse_t > 0) r->pulse_t -= dt;
        if(r->hold_t > 0) r->hold_t -= dt;
        if(!active || p >= game.num_players) r->pulse_t = r->hold_t = 0;

        int pow = 0, freq = 0;
        if(r->pulse_t > 0) { pow = r->pulse_pow; freq = r->pulse_freq; }
        if(r->hold_t > 0 && r->hold_pow > pow) { pow = r->hold_pow; freq = r->hold_freq; }
#ifdef DK_RUMBLE_TRACE
        /* Logs requested levels with or without a pack attached. */
        static int traced[MAX_PLAYERS][2];
        if(pow != traced[p][0] || (pow > 0 && freq != traced[p][1])) {
            printf("DK RUMBLE p%d t=%.2f pow=%d freq=%d\n", p + 1, (double)game.race_time, pow, freq);
            traced[p][0] = pow;
            traced[p][1] = freq;
        }
#endif
        if(!dev) continue;
        bool change = pow != r->sent_pow || (pow > 0 && freq != r->sent_freq && r->since_send > 0.1f);
        bool refresh = pow > 0 && r->since_send > 1.0f;
        if((change || refresh) && pack_send(dev, pow, freq)) {
            r->sent_pow = pow;
            r->sent_freq = freq;
            r->since_send = 0;
        }
    }
}

void rumble_shutdown(void) {
    for(int p = 0; p < MAX_PLAYERS; p++)
        if(rb[p].dev && rb[p].sent_pow != 0) pack_send(rb[p].dev, 0, 0);
}
