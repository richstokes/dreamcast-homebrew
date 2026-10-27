/* DreamKarts kart handling, tuned toward modern Mario Kart:

   - Press the drift button for a short hop. Landing while steering starts a
     powerslide in that direction (a short grace window lets the stick
     choose just after touchdown).
   - While sliding, the stick changes the slide radius: into the turn is
     tight, counter-steering is wide. The slide holds until release.
   - Sliding charges a mini-turbo through blue, orange and purple sparks;
     releasing fires a boost whose length depends on the stage reached.
   - Holding the button without steering charges a jump (Mario Kart World's
     charge jump); release to spring into the air.
   - Pressing the button again in the air, while still climbing with time
     to finish it, barrel rolls the kart: a boost on landing.

   Physics step once per displayed frame, at the measured vblank period
   (see main.c). Velocity follows the heading through a
   grip blend, so slides drift outward naturally while the body is drawn
   yawed into the turn. */
#include "kart.h"
#include "items.h"
#include "save.h"

Kart karts[MAX_KARTS];
int kart_count;
int kart_jumps, kart_rescues, kart_falls;

const KartColour kart_colours[COLOUR_COUNT] = {
    {"CLASSIC", 0, 0, 0, 0, 0xff8cc8f0},                          /* red kart, sky-blue Chao */
    {"HAWK", 0xff2c6ee6, 0, 0xffffa8d0, 0xffff5a96, 0xffffa8d0},
    {"MANGO", 0xffff8c14, 0, 0xffffd25a, 0xffff8c28, 0xffffd25a},
    {"CORAL", 0xff9646dc, 0, 0xffff8c78, 0xffffdc78, 0xffff8c78},
    {"PEBBLE", 0xff28b450, 0, 0xffe6ebf5, 0xff96c8ff, 0xffe6ebf5},
    {"MIDNIGHT", 0xff32323c, 0, 0xff6e5a96, 0xffe63c50, 0xff6e5a96},
    {"HERO", 0xfff0f0f4, 0, 0xfffaf4e0, 0xffffc830, 0xffffe890},
    {"LIME", 0xffffd020, 0, 0xff82e07a, 0xff2fa84f, 0xff82e07a},
};

/* The Customize screen's palette. The first and twelfth are the model's own
   red and white, so the default custom kart is the classic one. */
const PaletteColour kart_palette[PALETTE_COUNT] = {
    {"RED", 0xffe8161e, 0xff8c0a14},
    {"ORANGE", 0xffff8c14, 0xffc85a00},
    {"YELLOW", 0xffffd020, 0xffe08a10},
    {"LIME", 0xff82e07a, 0xff2fa84f},
    {"GREEN", 0xff28b450, 0xff146e32},
    {"TEAL", 0xff20c8b4, 0xff128c82},
    {"SKY", 0xff8cc8f0, 0xff4a8cd2},
    {"BLUE", 0xff2c6ee6, 0xff1a3c9c},
    {"PURPLE", 0xff9646dc, 0xff5a2096},
    {"PINK", 0xffffa8d0, 0xffff5a96},
    {"CORAL", 0xffff8c78, 0xffffdc78},
    {"WHITE", 0xfff0f0f4, 0xff96c8ff},
    {"CREAM", 0xfffaf4e0, 0xffffc830},
    {"SILVER", 0xffa8acb8, 0xff6e7280},
    {"GUNMETAL", 0xff4a4e58, 0xff2a2c32},
    {"BLACK", 0xff32323c, 0xff6e5a96},
};
const KartCustom kart_custom_default = {0, 11, 6};

static int palette_index(int i) {
    return (i % PALETTE_COUNT + PALETTE_COUNT) % PALETTE_COUNT;
}

KartColour kart_colour_look(int colour, const KartCustom *custom) {
    if(colour == COLOUR_CUSTOM) {
        if(!custom) custom = &save.custom;
        KartColour c = {"CUSTOM", kart_palette[palette_index(custom->body)].rgb,
                        kart_palette[palette_index(custom->trim)].rgb, kart_palette[palette_index(custom->chao)].rgb,
                        kart_palette[palette_index(custom->chao)].tip, kart_palette[palette_index(custom->chao)].rgb};
        return c;
    }
    return kart_colours[(colour % COLOUR_COUNT + COLOUR_COUNT) % COLOUR_COUNT];
}

int kart_voice(int colour, const KartCustom *custom) {
    if(colour == COLOUR_CUSTOM) {
        if(!custom) custom = &save.custom;
        return palette_index(custom->chao) % COLOUR_COUNT;
    }
    return (colour % COLOUR_COUNT + COLOUR_COUNT) % COLOUR_COUNT;
}

void kart_apply_look(Kart *k, int colour, const KartCustom *custom) {
    KartColour c = kart_colour_look(colour, custom);
    k->paint1 = c.paint1;
    k->paint2 = c.paint2;
    k->chao = c.chao;
    k->chao_tip = c.tip;
    k->ui_color = c.ui;
    k->colour = colour == COLOUR_CUSTOM ? COLOUR_CUSTOM : (colour % COLOUR_COUNT + COLOUR_COUNT) % COLOUR_COUNT;
    k->voice = kart_voice(colour, custom);
}

bool kart_is_human(int index) {
    for(int p = 0; p < game.num_players; p++)
        if(game.pl[p].kart == index) return true;
    return false;
}

#define WAVE_LEN   1.7f   /* seconds a wave lasts, lowering the arm included */
#define ARM_LOWER  0.25f  /* the arm starts back to the wheel this long before the end */

void kart_wave(Kart *k) {
    if(kart_cheering(k)) return;
    /* Arms take turns, right first; a press mid-wave hands it to the other
       arm without restarting the wave. */
    k->wave_left = k->waves++ & 1;
    if(k->wave_t <= ARM_LOWER) k->events |= EV_WAVE;
    k->wave_t = WAVE_LEN;
}

bool kart_cheering(const Kart *k) {
    return k->finished && k->place == 1 && kart_count > 1;
}

/* Arms ease up off the wheel and back; models.c turns arm_up and emote_bob
   into the pose (a wave or fist pumps). */
static void emote_update(Kart *k, const KartInput *in, float dt) {
    if(in && in->wave) kart_wave(k);
    if(k->wave_back_t > 0 && (k->wave_back_t -= dt) <= 0) {
        k->wave_back_t = 0;
        kart_wave(k);
    }
    bool cheer = kart_cheering(k);
    k->cheer_t = cheer ? k->cheer_t + dt : 0;
    if(cheer) k->wave_t = 0;
    k->wave_t = fmaxf(k->wave_t - dt, 0);
    /* The right arm also goes up to hold a used emerald overhead. */
    bool wave = k->wave_t > ARM_LOWER;
    float up_l = cheer || (wave && k->wave_left) ? 1.0f : 0.0f;
    float up_r = cheer || (wave && !k->wave_left) || k->raise_t > 0 ? 1.0f : 0.0f;
    k->arm_up[0] = approach(k->arm_up[0], up_l, 4.5f * dt);
    k->arm_up[1] = approach(k->arm_up[1], up_r, 4.5f * dt);
}

const CCParams cc_params[CC_COUNT] = {
    {19.25f}, {23.65f}, {28.05f}
};

#define GRAVITY          30.0f
#define HOP_VY           4.5f   /* ~0.34 m peak, 0.30 s in the air */
#define CHARGE_JUMP_VY   7.6f
#define KART_RADIUS      0.95f
#define DRIFT_MIN_SPEED  7.0f
#define TURN_MAX         1.75f
/* Slide radii for full inward stick, neutral and full counter-steer. Full
   counter-steer still curves in, so a slide always runs out of road: it
   can't be held down a straight to charge a purple boost. A bigger wide
   radius means more opposite lock: at 150cc full lock still turns in at
   21 deg/s with 75 m, 15 with 110, 13 with 120, 11 with 140. 75 m couldn't
   open the line enough on the long sweepers; 140 felt right in a first
   test and 120 a touch strong after a longer play, so the change is
   staggered for players used to the old handling and may still go up. */
#define DRIFT_R_TIGHT    13.0f
#define DRIFT_R_NEUTRAL  24.0f
#define DRIFT_R_WIDE     110.0f
#define DRIFT_EASE       0.35f  /* seconds for a new slide to reach full lock */
#define STAGE1           0.85f
#define STAGE2           1.90f
#define STAGE3           3.10f
#define BOOST_TOP_MUL    1.22f
/* Ramp jumps launch at a fixed climb rate under lighter gravity, so the
   flight lasts the same at every class; the jump pad sets the distance.
   The climb rate scales with the class speeds and gravity with their
   square, so the arcs keep their shape if the speeds change. */
#define JUMP_VY          9.9f
#define JUMP_GRAVITY     13.31f
#define JUMP_MIN_SPEED   23.1f
/* The jump pad's kick varies per hit by this factor, so some karts sail
   further than others. JUMP_MIN_SPEED still guarantees the gap: at 23.1 m/s
   the 1.58 s flight covers ~37 m of the 32 m gap; the strongest 150cc kick
   touches down ~50 m past the gap centre, on the 54 m landing deck. */
#define JUMP_PAD_MIN     0.92f
#define JUMP_PAD_MAX     1.10f
/* A barrel roll flicked in the air takes TRICK_LEN, so the flick only
   counts while the kart is still climbing with at least that long left
   before the road comes back: the first half of a ramp jump's arc (a hop
   or a charge jump is over too soon, and nothing counts over an open edge
   in the sky). Touching down after one pays TRICK_BOOST seconds of boost. */
#define TRICK_LEN        0.6f
#define TRICK_BOOST      2.0f

float kart_top_speed(const Kart *k) {
    float top = cc_params[game.cc].top_speed;
    if(k->cpu && !karts[game.pl[0].kart].cpu) {
        /* Rubber-band against the leading human (none in a recorded
           all-CPU demo race). */
        float lead = karts[game.pl[0].kart].progress;
        if(game.num_players > 1) lead = fmaxf(lead, karts[game.pl[1].kart].progress);
        float behind = lead - k->progress;
        top *= k->skill * (1.0f + clampf(behind / 140.0f, -0.07f, 0.09f));
    }
    return top;
}

void kart_place_on_grid(Kart *k, int slot) {
    int row = slot / 2, col = slot % 2;
    float s = -7.0f - row * 7.5f - col * 3.5f;
    float d = col ? 3.6f : -3.6f;
    uint32_t p1 = k->paint1, p2 = k->paint2, ch = k->chao, tip = k->chao_tip, ui = k->ui_color;
    int colour = k->colour, voice = k->voice;
    const char *name = k->name;
    bool cpu = k->cpu, remote = k->remote;
    int net_slot = k->net_slot;
    float skill = k->skill, lane = k->lane;
    memset(k, 0, sizeof(*k));
    k->remote = remote;
    k->net_slot = net_slot;
    k->paint1 = p1; k->paint2 = p2; k->chao = ch; k->chao_tip = tip; k->ui_color = ui;
    k->colour = colour;
    k->voice = voice;
    k->name = name;
    k->cpu = cpu; k->skill = skill; k->lane = lane;
    k->pos = track_point(s, d);
    k->pos.y = k->gnd = track_road_y(s, d);
    V3 t = track_tangent(s);
    k->heading = atan2f(t.x, t.z);
    k->grounded = true;
    k->lap = 0;
    k->checkpoint = true;
    k->last_pad = -1;
    k->best_lap = 0;
    track_locate(k->pos, -1, &k->tp);
    k->progress = k->tp.s;
}

void kart_start_boost(Kart *k, float seconds, float kick) {
    float top = kart_top_speed(k);
    if(seconds > k->boost_t) {
        k->boost_t = seconds;
        k->boost_len = seconds;
    }
    if(k->speed < top * kick) k->speed = top * kick;
    k->boost_mul = BOOST_TOP_MUL;
}

static void trick_cancel(Kart *k) {
    k->tricked = false;
    k->trick_t = 0;
    k->trick_a = 0;
}

/* The barrel roll: one full turn toward the steered side, a touch quicker
   through the middle. */
static void trick_animate(Kart *k, float dt) {
    if(k->trick_t <= 0) return;
    k->trick_t += dt;
    float f = fminf(k->trick_t / TRICK_LEN, 1.0f);
    f = 0.65f * f + 0.35f * f * f * (3.0f - 2.0f * f);
    k->trick_a = -(float)k->trick_dir * DK_TAU * f;
    if(k->trick_t >= TRICK_LEN) {
        k->trick_t = 0;
        k->trick_a = 0;
    }
}

void kart_remote_trick(Kart *k, bool left) {
    k->trick_t = 1e-3f;
    k->trick_dir = left ? -1 : 1;
    k->events |= EV_TRICK;
}

static void end_drift(Kart *k, bool award) {
    k->boost_stage = k->drift == DRIFT_ACTIVE && award ? k->stage : 0;
    if(k->drift == DRIFT_ACTIVE && award && k->stage > 0) {
        static const float lengths[4] = {0, 0.45f, 0.95f, 1.55f};
        kart_start_boost(k, lengths[k->stage], 1.08f + 0.04f * k->stage);
        k->events |= EV_BOOST;
    }
    else if(k->drift == DRIFT_ACTIVE) {
        k->events |= EV_DRIFT_CANCEL;
    }
    k->drift = DRIFT_NONE;
    k->drift_dir = 0;
    k->charge = 0;
    k->stage = 0;
    k->drift_t = 0;
    k->charge_jump_t = 0;
}

void kart_spin_out(Kart *k, int dir) {
    if(k->drift != DRIFT_NONE) end_drift(k, false);
    k->spin_t = SPIN_LEN;
    k->spin_dir = dir < 0 ? -1 : 1;
    k->speed *= SPIN_SPEED_MUL;
    k->boost_t = 0;
    trick_cancel(k);
    k->charge_jump_t = 0;
    k->events |= EV_SPIN;
}

/* The body whips round during a spin-out, slowing to a stop as the
   controls come back. */
static float spin_yaw(const Kart *k) {
    float f = 1.0f - k->spin_t / SPIN_LEN;
    f = f * f * (3.0f - 2.0f * f);
    return (float)k->spin_dir * f * DK_TAU;
}

static void start_drift(Kart *k, int dir) {
    k->drift = DRIFT_ACTIVE;
    k->drift_dir = dir;
    k->drift_t = 0;
    k->charge = 0;
    k->stage = 0;
    k->events |= EV_DRIFT_START;
}

static void update_lap(Kart *k, float prev_s) {
    float L = track.length, s = k->tp.s;
    if(s > L * 0.4f && s < L * 0.6f) k->checkpoint = true;
    if(prev_s > L * 0.75f && s < L * 0.25f) {
        if(k->checkpoint) {
            k->checkpoint = false;
            k->lap++;
            if(k->lap >= 2 && k->lap - 2 < game.laps) {
                float t = game.race_time - k->lap_start;
                k->lap_times[k->lap - 2] = t;
                if(k->best_lap == 0 || t < k->best_lap) k->best_lap = t;
            }
            k->lap_start = game.race_time;
            if(k->lap > game.laps && !k->finished) {
                k->finished = true;
                k->finish_time = game.race_time;
                k->events |= EV_FINISH;
            }
            else if(k->lap >= 2) {
                k->events |= EV_LAP;
            }
        }
    }
    else if(prev_s < L * 0.25f && s > L * 0.75f) {
        k->lap--;
        k->checkpoint = true;
    }
    k->progress = (float)k->lap * L + s;
}

/* Put a kart that fell into a jump's gap back on the landing deck. */
static void rescue_from_gap(Kart *k) {
    float s = track_wrap(track.jump.cross + track.jump.land + 3.0f);
    float d = clampf(k->tp.d, -6.0f, 6.0f);
    k->pos = track_point(s, d);
    k->pos.y = k->gnd = track_road_y(s, d);
    V3 t = track_tangent(s);
    k->heading = atan2f(t.x, t.z);
    k->speed = fmaxf(k->speed * 0.5f, 8.0f);
    k->vel = vscale(heading_fwd(k->heading), k->speed);
    k->vy = 0;
    k->grounded = true;
    k->big_air = false;
    trick_cancel(k);
    track_locate(k->pos, (int)(s / track.ds), &k->tp);
    k->progress = (float)k->lap * track.length + k->tp.s;
    kart_rescues++;
}

/* Fell off an open edge in the sky: dropped back onto the road where it
   went over, from a few metres up, at a standstill. */
#define FALL_DEPTH 10.0f     /* metres below the road before the rescue */
static void rescue_from_fall(Kart *k) {
    if(k->drift != DRIFT_NONE) end_drift(k, false);
    float s = k->tp.s, d = clampf(k->tp.d, -4.0f, 4.0f);
    k->pos = track_point(s, d);
    k->gnd = track_road_y(s, d);
    k->pos.y = k->gnd + 2.5f;
    V3 t = track_tangent(s);
    k->heading = atan2f(t.x, t.z);
    k->speed = 0;
    k->vel = v3(0, 0, 0);
    k->vy = 0;
    k->grounded = false;
    k->big_air = false;
    k->air_t = 0;
    k->boost_t = 0;
    k->over_edge = false;
    trick_cancel(k);
    track_locate(k->pos, k->tp.idx, &k->tp);
    kart_falls++;
}

/* Road height, ramp launches, flight and landings. When grounded the kart
   rides the road and vy tracks its slope, so leaving an edge (or hopping)
   carries the climb rate into the air. Off an open edge in the sky there
   is no ground at all. */
static void kart_vertical(Kart *k, bool btn, float prev_s, float prev_d, float dt) {
    bool no_road = track_off_edge(&k->tp);
    float road = track_road_y(k->tp.s, k->tp.d);
    float ground = no_road ? -1e4f : road;
    if(no_road && !k->over_edge && k->pos.y < road + 0.5f && !(track.has_jump && track_in_gap(k->tp.s))) {
        k->over_edge = true;
        k->events |= EV_FALL;
    }
    if(track.has_jump && k->speed > 0) {
        float a0 = track_jump_pos(prev_s), a1 = track_jump_pos(k->tp.s);
        float lip_h = track.jump.lip_h;
        bool near_deck = k->grounded || (k->pos.y > lip_h - 0.6f && k->pos.y < lip_h + 2.0f);
        if(a0 <= track.jump.lip && a1 > track.jump.lip && a1 - a0 < 5.0f && near_deck) {
            k->grounded = false;
            k->big_air = true;
            k->air_t = 0;
            k->pos.y = fmaxf(k->pos.y, lip_h);
            k->vy = JUMP_VY;
            if(k->speed < JUMP_MIN_SPEED) k->speed = JUMP_MIN_SPEED;
            /* Carry the full speed into the flight along the travel line. */
            float hv = sqrtf(k->vel.x * k->vel.x + k->vel.z * k->vel.z);
            V3 dir = hv > 1.0f ? vscale(k->vel, 1.0f / hv) : heading_fwd(k->heading);
            if(hv < k->speed) k->vel = vscale(dir, k->speed);
            if(k->drift == DRIFT_ACTIVE) end_drift(k, true);
            else k->drift = DRIFT_NONE;
            k->events |= EV_JUMP;
            kart_jumps++;
#if defined(DK_AUTOTEST) && !defined(DK_HW)
            printf("DK JUMP kart %d lip %.1f m/s\n", (int)(k - karts), (double)k->speed);
#endif
        }
    }
    if(k->grounded) {
        /* Over a crest the road can fall away faster than gravity pulls
           the kart down: it flies, carrying its climb rate. */
        bool crest = (ground - k->pos.y) / dt < k->vy - GRAVITY * dt && k->speed > 8.0f;
        if(ground < k->pos.y - 0.5f || crest) {
            k->grounded = false;      /* drove off an edge or a crest */
            k->air_t = 0;
        }
        else {
            k->vy = (ground - k->pos.y) / dt;
            k->pos.y = k->gnd = ground;
            return;
        }
    }
    k->air_t += dt;
    k->vy -= (k->big_air ? JUMP_GRAVITY : GRAVITY) * dt;
#ifdef DK_AIR_TRACE
    if(k == &karts[game.player] && !k->big_air && k->pos.y - ground > 0.5f)
        printf("DK AIR s %.0f up %.2f t %.2f spd %.1f\n", (double)k->tp.s, (double)(k->pos.y - ground), (double)k->air_t, (double)k->speed);
#endif
    float prev_y = k->pos.y;
    k->pos.y += k->vy * dt;
    if(track.in_sky && k->pos.y < road - FALL_DEPTH) {
        if(track.has_jump && track_in_gap(k->tp.s)) rescue_from_gap(k);
        else rescue_from_fall(k);
        return;
    }
    if(k->pos.y > ground) return;
    /* Already under the road (fell off and drifted back below it). */
    if(track.in_sky && prev_y < ground - 0.3f) return;
    if(track.has_jump && track_in_gap(k->tp.s)) {
        rescue_from_gap(k);
        k->events |= EV_LAND;
        k->land_v = 3.0f;
        return;
    }
    /* Impact relative to the road, which may itself be falling away. Its
       rate comes from where the kart was on it last frame: off the
       centreline of a bend s runs faster or slower than the kart, and a
       rate from speed alone lands it too slowly on the inside of a
       descending bend, where the next frame reads a crest and it hops
       again, over and over. */
    float road_vy = (ground - track_road_y(prev_s, prev_d)) / dt;
    float impact = road_vy - k->vy;
#ifdef DK_HOP_TRACE
    if(k->air_t < 0.2f)
        printf("DK MICROHOP kart %d s %.1f d %.1f air %.3f impact %.2f\n", (int)(k - karts), (double)k->tp.s,
               (double)k->tp.d, (double)k->air_t, (double)impact);
#endif
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    if(k->big_air)
        printf("DK LAND kart %d %.1f m past the gap centre, air %.2f s\n", (int)(k - karts),
               (double)track_jump_pos(k->tp.s), (double)k->air_t);
#endif
    k->pos.y = k->gnd = ground;
    k->vy = road_vy;
    k->grounded = true;
    k->big_air = false;
    k->squash_v -= impact * 0.9f;
    if(k->tricked) {
        /* Landed a barrel roll (cut short if the road came early). */
        trick_cancel(k);
        k->boost_stage = 3;
        kart_start_boost(k, TRICK_BOOST, 1.12f);
        k->events |= EV_BOOST | EV_TRICK_BOOST;
#if defined(DK_AUTOTEST) && !defined(DK_HW)
        printf("DK TRICK LAND kart %d s %.0f air %.2f s\n", (int)(k - karts), (double)k->tp.s, (double)k->air_t);
#endif
    }
    if(impact > 2.5f) {
        k->events |= EV_LAND;
        k->land_v = impact;
    }
    if(k->drift == DRIFT_PENDING && btn) {
        if(fabsf(k->steer) > 0.3f && k->speed > DRIFT_MIN_SPEED)
            start_drift(k, k->steer < 0 ? -1 : 1);
    }
    else if(k->drift == DRIFT_PENDING) {
        k->drift = DRIFT_NONE;
    }
}

/* The chassis pitch and roll that sit the wheels on the road: its slope
   along the lap and banking across it, taken along the kart's drawn
   forward and right axes (a drifting kart is drawn yawed across the
   road, so some of the banking becomes pitch and some slope becomes
   roll). */
static void road_tilt(const Kart *k, float yaw, float *pitch, float *roll) {
    /* Along the lap at the kart's own offset: off the centreline a bank
       easing in or out is a slope too. */
    float slope = track_road_y(k->tp.s + 0.5f, k->tp.d) - track_road_y(k->tp.s - 0.5f, k->tp.d);
    float bank = track_bank(k->tp.s);
    V3 f = heading_fwd(yaw), r = heading_right(yaw);
    *pitch = atanf(slope * vdot(f, k->tp.tangent) + bank * vdot(f, k->tp.right));
    *roll = atanf(slope * vdot(r, k->tp.tangent) + bank * vdot(r, k->tp.right));
}

/* An online rival: its physics ran on another console. Take its state from
   the network and animate the wheels, slide, lean and suspension here. */
void kart_remote_update(Kart *k, V3 pos, float heading, float speed, float steer, int drift_dir, int stage,
                        bool boosting, bool grounded, bool skid, float dt) {
    k->events = 0;
    k->pos = pos;
    k->heading = heading;
    k->speed = speed;
    k->vel = vscale(heading_fwd(heading), speed);
    k->steer = steer;
    k->grounded = grounded;
    k->skid = skid;   /* braking hard: fx lays its skid marks */
    if(grounded) k->gnd = pos.y;
    k->drift = drift_dir ? DRIFT_ACTIVE : DRIFT_NONE;
    if(drift_dir) k->drift_dir = drift_dir;
    k->stage = stage;
    k->boost_t = boosting ? fmaxf(k->boost_t, 0.15f) : fmaxf(k->boost_t - dt, 0.0f);
    track_locate(k->pos, k->tp.idx, &k->tp);
    if(fabsf(k->tp.d) > 30.0f) track_locate(k->pos, -1, &k->tp);   /* jumped: search the whole lap */
    k->surface = track_surface(&k->tp);

    float spd = fabsf(speed), top = cc_params[game.cc].top_speed;
    float yaw_target = k->drift == DRIFT_ACTIVE ? (float)k->drift_dir * (0.40f + 0.12f * steer * (float)k->drift_dir)
                                                : steer * 0.10f * clampf(spd / 8.0f, 0, 1);
    k->vis_yaw += (yaw_target - k->vis_yaw) * smooth_k(9.0f, dt);
    if(k->spin_t > 0) {
        k->spin_t -= dt;
        k->vis_yaw = k->spin_t > 0 ? spin_yaw(k) : 0;
        if(k->spin_t < 0) k->spin_t = 0;
    }
    float lean = k->drift == DRIFT_ACTIVE ? -(float)k->drift_dir * 0.10f : -steer * 0.05f * clampf(spd / top, 0, 1);
    k->lean += (lean - k->lean) * smooth_k(8.0f, dt);
    k->nose += ((k->boost_t > 0 ? -0.05f : 0.0f) - k->nose) * smooth_k(6.0f, dt);
    float pitch_t = 0, roll_t = 0;
    if(grounded) road_tilt(k, heading + k->vis_yaw, &pitch_t, &roll_t);
    trick_animate(k, dt);
    /* On the road the chassis follows it closely (a slow follow sinks the
       tyres through every dip); in the air it settles gently. */
    k->roll += (roll_t - k->roll) * smooth_k(grounded ? 25.0f : 8.0f, dt);
    k->pitch += (pitch_t - k->pitch) * smooth_k(grounded ? 25.0f : 6.0f, dt);
    k->squash_v += (-k->squash * 160.0f - k->squash_v * 14.0f) * dt;
    k->squash += k->squash_v * dt;
    k->squash = clampf(k->squash, -0.35f, 0.35f);
    k->wheel_spin += speed / 0.23f * dt;
    if(k->wheel_spin > DK_TAU) k->wheel_spin -= DK_TAU * floorf(k->wheel_spin / DK_TAU);
    if(k->wheel_spin < 0) k->wheel_spin += DK_TAU * ceilf(-k->wheel_spin / DK_TAU);
    float wheel_target = k->drift == DRIFT_ACTIVE ? -(float)k->drift_dir * 0.25f + steer * 0.3f : steer * 0.45f;
    k->steer_vis += (wheel_target - k->steer_vis) * smooth_k(12.0f, dt);
    k->head_tilt += ((k->drift == DRIFT_ACTIVE ? (float)k->drift_dir * 0.22f : steer * 0.15f) - k->head_tilt) *
                    smooth_k(6.0f, dt);
    k->emote_bob += dt * 3.2f;
    emote_update(k, NULL, dt);
}

void kart_update(Kart *k, const KartInput *in, float dt, bool can_drive) {
    k->events = 0;
    float top = kart_top_speed(k);
    /* Spun out: no steering, throttle or drift until it stops. */
    static const KartInput dead = {0};
    bool spinning = k->spin_t > 0;
    if(spinning) {
        k->spin_t -= dt;
        if(k->spin_t <= 0) {
            k->spin_t = 0;
            k->vis_yaw = 0;
        }
        in = &dead;
    }
    bool btn = in->drift && can_drive;
    bool pressed = btn && !k->prev_drift_btn;
    bool released = !btn && k->prev_drift_btn;
    k->prev_drift_btn = btn;

    /* Steering input is smoothed so digital input still feels analogue. */
    float steer_in = can_drive ? clampf(in->steer, -1, 1) : 0;
    k->steer = approach(k->steer, steer_in, 7.5f * dt);

    /* Accelerate and brake held at a standstill: a burnout on the spot,
       the tyres spinning until either is let go. */
    if(can_drive && k->grounded && in->accel && in->brake && fabsf(k->speed) < 1.0f && k->drift == DRIFT_NONE) {
        k->rev_t += dt;
        k->burnout_t = fmaxf(k->burnout_t, 0.1f);
    }
    else {
        k->rev_t = 0;
    }
    if(k->burnout_t > 0) {
        k->burnout_t -= dt;
        k->speed = 0;
        k->vel = v3(0, 0, 0);
    }

    /* ---- Drift button: hop, slide, charge jump ---- */
    if(pressed && k->grounded && k->burnout_t <= 0) {
        k->vy += HOP_VY;
        k->grounded = false;
        k->air_t = 0;
        k->drift = DRIFT_PENDING;
        k->pending_t = 0.16f;
        k->events |= EV_HOP;
        k->squash_v -= 3.0f;
    }
    else if(pressed && !k->grounded && !k->tricked && !k->over_edge && k->vy > 0 &&
            (k->big_air || !track_off_edge(&k->tp))) {
        /* Flicked in the air: a barrel roll, if the kart is still climbing
           with time to finish it before touchdown (a ramp flight always
           has the road ahead, even over Neon Skyway's open gap; off a
           side edge in the sky there is nothing to land on). The button
           stays live for a slide on landing, as after a hop. */
        float g = k->big_air ? JUMP_GRAVITY : GRAVITY;
        float h = fmaxf(k->pos.y - track_road_y(k->tp.s, k->tp.d), 0.0f);
        float left = (k->vy + sqrtf(k->vy * k->vy + 2.0f * g * h)) / g;
        if(left >= TRICK_LEN + 0.05f) {
            k->tricked = true;
            k->trick_t = 1e-3f;
            k->trick_dir = k->steer < -0.2f ? -1 : 1;
            k->drift = DRIFT_PENDING;
            k->pending_t = 0.16f;
            k->events |= EV_TRICK;
#if defined(DK_AUTOTEST) && !defined(DK_HW)
            printf("DK TRICK kart %d s %.0f air %.2f vy %.1f left %.2f\n", (int)(k - karts), (double)k->tp.s,
                   (double)k->air_t, (double)k->vy, (double)left);
#endif
        }
    }
    if(released) {
        if(k->drift == DRIFT_CHARGE && k->charge_jump_t > 0.45f && k->grounded) {
            k->vy += CHARGE_JUMP_VY;
            k->grounded = false;
            k->air_t = 0;
            k->events |= EV_CHARGE_JUMP;
            k->squash_v -= 5.0f;
            k->drift = DRIFT_NONE;
            k->charge_jump_t = 0;
        }
        else {
            end_drift(k, true);
        }
    }
    if(k->drift == DRIFT_ACTIVE && (k->speed < DRIFT_MIN_SPEED || in->brake))
        end_drift(k, false);

    /* Grounded states waiting for the stick to choose a direction. */
    if(k->grounded && btn) {
        if(k->drift == DRIFT_PENDING) {
            if(fabsf(k->steer) > 0.3f && k->speed > DRIFT_MIN_SPEED)
                start_drift(k, k->steer < 0 ? -1 : 1);
            else if((k->pending_t -= dt) <= 0) {
                k->drift = DRIFT_CHARGE;
                k->charge_jump_t = 0;
            }
        }
        else if(k->drift == DRIFT_CHARGE) {
            k->charge_jump_t += dt;
            if(fabsf(steer_in) > 0.55f && k->speed > DRIFT_MIN_SPEED)
                start_drift(k, steer_in < 0 ? -1 : 1);
        }
    }

    /* ---- Track surface ---- */
    k->surface = track_surface(&k->tp);
    bool sand = k->surface == SURF_SAND;
    float surf_mul = sand ? 0.52f : (k->surface == SURF_CURB ? 0.97f : 1.0f);
    if(k->boost_t > 0) surf_mul = 1.0f;   /* boosts cut through sand */

    /* ---- Speed ---- */
    float target = top * surf_mul * (k->boost_t > 0 ? k->boost_mul : 1.0f);
    if(k->drift == DRIFT_ACTIVE) target *= 0.985f;
    if(k->drift == DRIFT_CHARGE) target *= 0.94f;
    k->skid = false;
    if(k->grounded && can_drive && k->burnout_t <= 0) {
        bool gas = in->accel || k->boost_t > 0;
        /* B brakes even with A held (and through a boost); reversing
           needs A released, so holding both at a standstill just stops. */
        if(in->brake) {
            k->skid = k->speed > 9.0f;
            if(k->speed > 0.3f) k->speed = approach(k->speed, 0, 22.0f * dt);
            else if(!in->accel) k->speed = approach(k->speed, -6.0f, 9.0f * dt);
            else k->speed = approach(k->speed, 0, 22.0f * dt);
        }
        else if(gas) {
            if(k->speed < target) {
                float rate = k->boost_t > 0 ? 3.0f : 0.85f;
                k->speed += ((target - k->speed) * rate + 1.2f) * dt;
                if(k->speed > target) k->speed = target;
            }
            else {
                k->speed = approach(k->speed, target, (sand ? 16.0f : 5.0f) * dt);
            }
        }
        else {
            k->speed = approach(k->speed, 0, (sand ? 9.0f : 4.5f) * dt);
            if(k->speed > target) k->speed = approach(k->speed, target, 6.0f * dt);
        }
    }
    else if(!can_drive) {
        k->speed = approach(k->speed, 0, 8.0f * dt);
    }

    /* ---- Heading ---- */
    float spd = fabsf(k->speed);
    if(k->drift == DRIFT_ACTIVE) {
        float kk = k->steer * (float)k->drift_dir;       /* +1 tight, -1 wide */
        float curv = kk >= 0 ? lerpf(1.0f / DRIFT_R_NEUTRAL, 1.0f / DRIFT_R_TIGHT, kk)
                             : lerpf(1.0f / DRIFT_R_NEUTRAL, 1.0f / DRIFT_R_WIDE, -kk);
        /* Ease in: the stick is usually still hard over from picking the
           direction, so start no tighter than neutral and tighten up over
           DRIFT_EASE seconds. Counter-steer for a wider line acts at once. */
        if(k->drift_t < DRIFT_EASE && kk > 0) {
            float e = k->drift_t / DRIFT_EASE;
            curv = lerpf(1.0f / DRIFT_R_NEUTRAL, curv, e * e * (3.0f - 2.0f * e));
        }
        float rate = clampf(spd * curv, 0, 2.1f);
        k->heading += (float)k->drift_dir * rate * dt;
        k->drift_t += dt;
        if(!sand && k->grounded) {
            float charge_rate = kk >= 0 ? lerpf(1.0f, 1.6f, kk) : lerpf(1.0f, 0.78f, -kk);
            k->charge += charge_rate * dt;
        }
        int stage = k->charge >= STAGE3 ? 3 : k->charge >= STAGE2 ? 2 : k->charge >= STAGE1 ? 1 : 0;
        if(stage > k->stage) {
            k->stage = stage;
            k->events |= stage == 1 ? EV_CHARGE1 : stage == 2 ? EV_CHARGE2 : EV_CHARGE3;
        }
    }
    else {
        float scale = clampf(spd / 6.0f, 0, 1) * (1.0f - 0.15f * clampf(spd / top, 0, 1));
        float rate = k->steer * TURN_MAX * scale;
        if(!k->grounded) rate *= k->big_air ? 0.45f : 1.05f;   /* hop turns, steadier in big air */
        if(k->speed < 0) rate = -rate;
        k->heading += rate * dt;
    }
    k->heading = wrap_angle(k->heading);

    /* ---- Velocity follows heading through grip ---- */
    V3 fwd = heading_fwd(k->heading);
    V3 want = vscale(fwd, k->speed);
    float grip = !k->grounded ? 0.8f : k->drift == DRIFT_ACTIVE ? 4.2f : sand ? 6.5f : 11.0f;
    float g = smooth_k(grip, dt);
    k->vel.x += (want.x - k->vel.x) * g;
    k->vel.z += (want.z - k->vel.z) * g;
    k->pos.x += k->vel.x * dt;
    k->pos.z += k->vel.z * dt;

    /* ---- Track position, walls ---- */
    float prev_s = k->tp.s, prev_d = k->tp.d;
    track_locate(k->pos, k->tp.idx, &k->tp);
    float lim_out = track_wall_out_at(&k->tp) - KART_RADIUS, lim_in = -(track_wall_in_at(&k->tp) - KART_RADIUS);
    bool below = track.in_sky && k->pos.y < track_road_y(k->tp.s, k->tp.d) - 1.0f;
    if(!below && (k->tp.d > lim_out || k->tp.d < lim_in)) {
        float over = k->tp.d > lim_out ? k->tp.d - lim_out : k->tp.d - lim_in;
        V3 n = k->tp.d > 0 ? k->tp.right : vscale(k->tp.right, -1);  /* points into the wall */
        k->pos.x -= k->tp.right.x * over;
        k->pos.z -= k->tp.right.z * over;
        float vn = k->vel.x * n.x + k->vel.z * n.z;
        if(vn > 0) {
            k->vel.x -= n.x * vn * 1.35f;
            k->vel.z -= n.z * vn * 1.35f;
            float frac = spd > 0.1f ? clampf(vn / spd, 0, 1) : 0;
            k->speed *= 1.0f - 0.75f * frac;
            /* Glance off: turn the heading toward the wall tangent. */
            V3 t = k->tp.tangent;
            float along = atan2f(t.x, t.z);
            if(k->speed < 0) along = wrap_angle(along + DK_PI);
            float diff = wrap_angle(along - k->heading);
            if(fabsf(diff) < 1.2f) k->heading += diff * 0.5f * frac;
            if(vn > 2.0f && k->bump_t <= 0) {
                k->events |= EV_WALL;
                k->wall_v = vn;
                k->bump_t = 0.35f;
                if(k->drift == DRIFT_ACTIVE && frac > 0.35f) end_drift(k, false);
            }
        }
        track_locate(k->pos, k->tp.idx, &k->tp);
    }
#if defined(DK_AUTOTEST) && !defined(DK_HW)
    if(fabsf(k->tp.d) > fmaxf(track_wall_out_at(&k->tp), track_wall_in_at(&k->tp)) + 0.3f)
        printf("DK ESCAPE kart %d s %.0f d %.1f y %.1f road %.1f below %d\n", (int)(k - karts), (double)k->tp.s, (double)k->tp.d,
               (double)k->pos.y, (double)track_road_y(k->tp.s, k->tp.d), (int)below);
#endif
    if(k->bump_t > 0) k->bump_t -= dt;
    update_lap(k, prev_s);
    kart_vertical(k, btn, prev_s, prev_d, dt);

    /* ---- Dash panels ---- */
    int pad = k->grounded ? track_dash_pad(k->tp.s, k->tp.d) : -1;
    if(pad >= 0 && pad != k->last_pad) {
        /* The pad feeding a ramp gives a slightly random push. */
        float lead = track.has_jump ? track_delta(track.pads[pad].s1, track.jump.cross + track.jump.lip) : -1.0f;
        float power = 1.0f;
        if(lead > 0 && lead < 80.0f) power = JUMP_PAD_MIN + (JUMP_PAD_MAX - JUMP_PAD_MIN) * frand();
        kart_start_boost(k, track.pads[pad].boost, 1.3f * power);
        k->boost_mul = BOOST_TOP_MUL * power;
        k->events |= EV_DASH;
    }
    k->last_pad = pad;

    if(k->boost_t > 0) {
        k->boost_t -= dt;
        if(k->boost_t <= 0) k->boost_t = 0;
    }

    /* ---- Presentation ---- */
    float yaw_target = 0;
    if(k->drift == DRIFT_ACTIVE)
        yaw_target = (float)k->drift_dir * (0.40f + 0.12f * k->steer * (float)k->drift_dir);
    else
        yaw_target = k->steer * 0.10f * clampf(spd / 8.0f, 0, 1);
    k->vis_yaw += (yaw_target - k->vis_yaw) * smooth_k(9.0f, dt);
    if(k->spin_t > 0) k->vis_yaw = spin_yaw(k);
    /* The body leans on its springs in corners and pitches under power
       and braking; the chassis (and the wheels) follow the road's banking
       and slope, or the flight path in big air. */
    float lean = k->drift == DRIFT_ACTIVE ? -(float)k->drift_dir * 0.10f : -k->steer * 0.05f * clampf(spd / top, 0, 1);
    k->lean += (lean - k->lean) * smooth_k(8.0f, dt);
    float nose_t = (in->accel && k->speed < top * 0.7f && can_drive ? -0.035f : 0) +
                   (in->brake && k->speed > 1 ? 0.05f : 0) + (k->boost_t > 0 ? -0.05f : 0);
    if(k->rev_t > 0) nose_t -= 0.05f;    /* the rear squats */
    k->nose += (nose_t - k->nose) * smooth_k(6.0f, dt);
    float pitch_t = 0, roll_t = 0;
    if(k->grounded) road_tilt(k, k->heading + k->vis_yaw, &pitch_t, &roll_t);
    else if(k->big_air) pitch_t = atan2f(k->vy, fmaxf(spd, 1.0f)) * 0.6f;
    trick_animate(k, dt);
    /* On the road the chassis follows it closely (a slow follow sinks the
       tyres through every dip); in the air it settles gently. */
    k->roll += (roll_t - k->roll) * smooth_k(k->grounded ? 25.0f : 8.0f, dt);
    k->pitch += (pitch_t - k->pitch) * smooth_k(k->grounded ? 25.0f : 6.0f, dt);
    if(k->rev_t > 0) {
        /* The body shakes on its springs as the tyres fight for grip. */
        float shake = clampf(k->rev_t * 2.0f, 0, 1);
        k->lean += 0.03f * shake * sinf(k->rev_t * 53.0f);
        k->vis_yaw += 0.02f * shake * sinf(k->rev_t * 31.0f);
    }
    /* Suspension spring for hop/landing squash. */
    k->squash_v += (-k->squash * 160.0f - k->squash_v * 14.0f) * dt;
    k->squash += k->squash_v * dt;
    k->squash = clampf(k->squash, -0.35f, 0.35f);
    k->wheel_spin += k->speed / 0.23f * dt;
    if(k->burnout_t > 0) k->wheel_spin += 40.0f * dt;
    if(k->wheel_spin > DK_TAU) k->wheel_spin -= DK_TAU * floorf(k->wheel_spin / DK_TAU);
    if(k->wheel_spin < 0) k->wheel_spin += DK_TAU * ceilf(-k->wheel_spin / DK_TAU);
    float wheel_target = k->drift == DRIFT_ACTIVE ? -(float)k->drift_dir * 0.25f + k->steer * 0.3f : k->steer * 0.45f;
    k->steer_vis += (wheel_target - k->steer_vis) * smooth_k(12.0f, dt);
    k->head_tilt += ((k->drift == DRIFT_ACTIVE ? (float)k->drift_dir * 0.22f : k->steer * 0.15f) - k->head_tilt) * smooth_k(6.0f, dt);
    k->emote_bob += dt * 3.2f;
    emote_update(k, in, dt);
}

void karts_collide(void) {
    for(int i = 0; i < kart_count; i++) {
        for(int j = i + 1; j < kart_count; j++) {
            Kart *a = &karts[i], *b = &karts[j];
            if(a->gone || b->gone) continue;
            float dx = b->pos.x - a->pos.x, dz = b->pos.z - a->pos.z;
            float d2 = dx * dx + dz * dz;
            const float min = KART_RADIUS * 2.0f;
            if(d2 >= min * min || d2 < 1e-6f) continue;
            if(fabsf(a->pos.y - b->pos.y) > 1.0f) continue;
            float d = sqrtf(d2), push = (min - d) * 0.5f;
            float nx = dx / d, nz = dz / d;
            a->pos.x -= nx * push; a->pos.z -= nz * push;
            b->pos.x += nx * push; b->pos.z += nz * push;
            float rv = (b->vel.x - a->vel.x) * nx + (b->vel.z - a->vel.z) * nz;
            if(rv < 0) {
                float imp = -rv * 0.6f;
                a->vel.x -= nx * imp; a->vel.z -= nz * imp;
                b->vel.x += nx * imp; b->vel.z += nz * imp;
                if(-rv > 1.5f) {
                    a->events |= EV_KART_BUMP;
                    b->events |= EV_KART_BUMP;
                    a->bump_with = j;
                    b->bump_with = i;
                }
            }
        }
    }
}

/* CPU personalities, by kart index, so the pack doesn't drive as one:
   look-ahead (short is twitchy, long is smooth), how far (m) the line
   sways about the kart's lane and how many times a lap (a whole number,
   so it meets itself at the line), how far (m) before or after the others
   it hops into a drift, and its odds of taking a bend on grip instead. */
static struct { float look, sway, sways, lead, grip; } ai_style[MAX_KARTS] = {
    {1.00f, 0.9f, 3, 4.0f, 0.10f}, {0.88f, 1.6f, 5, 11.0f, 0.05f}, {1.15f, 0.6f, 2, -3.0f, 0.25f},
    {0.95f, 1.3f, 4, 7.0f, 0.15f}, {1.08f, 1.8f, 3, 0.0f, 0.10f}, {0.92f, 1.0f, 6, -1.5f, 0.20f},
};
static float ai_react[MAX_KARTS];     /* drift reaction delay still to run */

/* How often a CPU barrel rolls off a ramp: nearly always while a human
   leads the race, rarely while the best-placed human is last, so the
   boosts help a struggling player catch up and punish one out in
   front (a mild rubber band, like kart_top_speed's). Half the time in
   an all-CPU demo race. */
static float ai_trick_odds(void) {
    if(karts[game.pl[0].kart].cpu) return 0.5f;
    int place = karts[game.pl[0].kart].place;
    if(game.num_players > 1 && karts[game.pl[1].kart].place < place) place = karts[game.pl[1].kart].place;
    float behind = kart_count > 1 ? (float)(place - 1) / (float)(kart_count - 1) : 0.0f;
    return lerpf(0.9f, 0.15f, behind);
}

/* CPU driver: pure pursuit toward a point on the racing line. The
   required path curvature is mapped onto the stick, through the slide
   radii while drifting, so CPUs drift and counter-steer like a player. */
void ai_drive(Kart *k, KartInput *in, float dt) {
    memset(in, 0, sizeof(*in));
    int idx = (int)(k - karts);
    /* The autotest drives the human kart: keep it on the neutral line. */
    bool styled = k->cpu && idx >= 0 && idx < MAX_KARTS;
    float look_k = styled ? ai_style[idx].look : 1.0f;
    float lead = styled ? ai_style[idx].lead : 0.0f;
    float s = k->tp.s, spd = fabsf(k->speed);
    float top = kart_top_speed(k);
    float look = (6.0f + spd * 0.45f) * look_k;
    float c_ahead = track_curvature(s + 4.0f + spd * 0.3f);
    /* A road narrower than the usual 8 m half (Apple Valley's) squeezes the
       lanes and weaves, and its tight bends get the extra care below. */
    float half = track_half_at(&k->tp), narrow = fminf(1.0f, half / 8.0f);
    bool tight = narrow < 1.0f;
    float lane = k->lane * narrow;
    if(styled) {
        /* A slow weave about the lane, straightened out through bends and
           on the run up to (and over) a jump. */
        float sway = ai_style[idx].sway * narrow * (1.0f - clampf(fabsf(c_ahead) * 120.0f, 0, 1));
        if(track.has_jump) {
            float a = track_jump_pos(s);
            if(a > -90.0f && a < track.jump.land + 10.0f) sway = 0;
        }
        lane += sway * sinf(s * (DK_TAU / track.length) * ai_style[idx].sways + (float)idx * 1.7f);
    }
    float edge = fminf(6.0f, half - 0.8f);
    float td = clampf(track_line(s + look) + lane, -edge, edge);
    td = items_ai_lane(k, s + look, td);
    V3 tp = track_point(s + look, td);
    float dx = tp.x - k->pos.x, dz = tp.z - k->pos.z;
    float dist = fmaxf(sqrtf(dx * dx + dz * dz), 1.0f);
    /* Aim from the way the kart is going. In a slide that runs 20-50 deg
       off the way it points, and aiming from the nose let the slides on
       Apple Valley's hairpins carry the CPUs (and the boost out of them)
       off the road. Elsewhere it also made them quicker, by 1-7 s a race,
       so it stays on the tight track until the difficulty is retuned. */
    float course = k->heading;
    if(tight && k->speed > 5.0f) course = atan2f(k->vel.x, k->vel.z);
    float alpha = wrap_angle(atan2f(dx, dz) - course);
    float c_des = 2.0f * sinf(alpha) / dist;          /* + = curve right */
    in->accel = true;
    if(k->drift == DRIFT_ACTIVE) {
        float cd = c_des * (float)k->drift_dir;
        const float cn = 1.0f / DRIFT_R_NEUTRAL, ct = 1.0f / DRIFT_R_TIGHT, cw = 1.0f / DRIFT_R_WIDE;
        float kk = cd >= cn ? (cd - cn) / (ct - cn) : -(cn - cd) / (cn - cw);
        in->steer = clampf(kk, -1, 1) * (float)k->drift_dir;
    }
    else {
        float scale = clampf(spd / 6.0f, 0, 1) * (1.0f - 0.15f * clampf(spd / top, 0, 1));
        float rate_max = fmaxf(TURN_MAX * scale, 0.1f);
        in->steer = clampf(c_des * spd / rate_max, -1, 1);
    }

    float c_now = track_curvature(s + 2.0f);
    if(k->ai_drift_hold > 0) k->ai_drift_hold -= dt;
    if(!k->ai_drift_btn) {
        float c_entry = lead != 0 ? track_curvature(s + 4.0f + spd * 0.3f + lead) : c_ahead;
        bool want = fabsf(c_entry) > 0.004f && fabsf(track_curvature(s + 35.0f + lead)) > 0.004f && spd > 12.0f &&
                    k->grounded && k->drift == DRIFT_NONE && k->ai_drift_hold <= 0;
        /* CPUs react after a beat, different every bend, so a pack doesn't
           hop into the same corner on the same frame; now and then one
           just grips its way round. */
        if(!styled) {
            if(want) k->ai_drift_btn = true;
        }
        else if(!want) ai_react[idx] = 0;
        else if(ai_react[idx] <= 0) {
            ai_react[idx] = 0.04f + (float)(dk_rand() & 255) * (0.20f / 255.0f);
            if((float)(dk_rand() & 255) < ai_style[idx].grip * 256.0f) {
                k->ai_drift_hold = 2.5f;
                ai_react[idx] = 0;
            }
        }
        else if((ai_react[idx] -= dt) <= 0) k->ai_drift_btn = true;
    }
    else {
        if(k->drift == DRIFT_PENDING || (k->drift == DRIFT_CHARGE && k->charge_jump_t < 0.2f))
            in->steer = c_ahead < 0 ? -0.34f : 0.34f;
        bool exit_turn = fabsf(c_now) < 0.002f && fabsf(c_ahead) < 0.002f;
        /* Bail out if even the widest slide turns too sharply. */
        bool too_tight = k->drift == DRIFT_ACTIVE && c_des * (float)k->drift_dir < -0.004f &&
                         k->drift_t > 0.9f;
        if(exit_turn || too_tight || k->drift == DRIFT_NONE ||
           (k->drift == DRIFT_CHARGE && k->charge_jump_t > 0.3f)) {
#ifdef DK_AI_TRACE
            if(k == &karts[game.player])
                printf("AI release: exit %d tight %d state %d t %.2f charge %.2f s %.0f d %.1f cdes %.4f\n", exit_turn,
                       too_tight, k->drift, (double)k->drift_t, (double)k->charge, (double)s, (double)k->tp.d,
                       (double)c_des);
#endif
            k->ai_drift_btn = false;
            k->ai_drift_hold = too_tight ? 0.35f : 0.6f;
            /* A tight track's next hairpin can be 20 m on: be ready for it. */
            if(tight)
                for(float x = 10.0f; x <= 30.0f; x += 5.0f)
                    if(fabsf(track_curvature(s + x)) > 1.0f / 30.0f) k->ai_drift_hold = 0.2f;
        }
    }
    in->drift = k->ai_drift_btn;
    /* Off a ramp a CPU lets go of the button and, with odds set by how
       the human is doing, flicks it again on the way up for a barrel
       roll. */
    if(k->grounded) {
        k->ai_trick_t = -1.0f;
    }
    else if(k->big_air) {
        if(k->ai_trick_t < 0) {
            bool skip = styled && frand() > ai_trick_odds();
            k->ai_trick_t = skip ? 1e9f : (styled ? 0.08f + frand() * 0.45f : 0.15f);
        }
        k->ai_drift_btn = false;
        in->drift = !k->tricked && k->air_t >= k->ai_trick_t;
    }
    items_ai(k, in, dt);
}

void kart_set_ai_style(int idx, float look, float sway, float sways, float lead, float grip) {
    if(idx < 0 || idx >= MAX_KARTS) return;
    ai_style[idx].look = look;
    ai_style[idx].sway = sway;
    ai_style[idx].sways = sways;
    ai_style[idx].lead = lead;
    ai_style[idx].grip = grip;
}

static uint32_t rand_state = 0x2026dcu;

uint32_t dk_rand(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return rand_state >> 8;
}

uint32_t dk_rand_peek(void) {
    return rand_state;
}

void dk_seed(uint32_t seed) {
    rand_state = seed;
}
