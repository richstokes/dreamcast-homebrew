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

   Physics run at a fixed 60 Hz step. Velocity follows the heading through a
   grip blend, so slides drift outward naturally while the body is drawn
   yawed into the turn. */
#include "kart.h"

Kart karts[MAX_KARTS];
int kart_count;

const KartColour kart_colours[COLOUR_COUNT] = {
    {"CLASSIC", 0, 0, 0, 0xff8cc8f0},                          /* red kart, sky-blue Chao */
    {"BUBBLES", 0xff2c6ee6, 0xffffa8d0, 0xffff5a96, 0xffffa8d0},
    {"MANGO", 0xffff8c14, 0xffffd25a, 0xffff8c28, 0xffffd25a},
    {"CORAL", 0xff9646dc, 0xffff8c78, 0xffffdc78, 0xffff8c78},
    {"PEBBLE", 0xff28b450, 0xffe6ebf5, 0xff96c8ff, 0xffe6ebf5},
    {"MIDNIGHT", 0xff32323c, 0xff6e5a96, 0xffe63c50, 0xff6e5a96},
    {"HERO", 0xfff0f0f4, 0xfffaf4e0, 0xffffc830, 0xffffe890},
    {"LIME", 0xffffd020, 0xff82e07a, 0xff2fa84f, 0xff82e07a},
};

void kart_apply_colour(Kart *k, int colour) {
    const KartColour *c = &kart_colours[(colour % COLOUR_COUNT + COLOUR_COUNT) % COLOUR_COUNT];
    k->paint1 = c->paint1;
    k->chao = c->chao;
    k->chao_tip = c->tip;
    k->ui_color = c->ui;
}

bool kart_is_human(int index) {
    for(int p = 0; p < game.num_players; p++)
        if(game.pl[p].kart == index) return true;
    return false;
}

const CCParams cc_params[CC_COUNT] = {
    {17.5f}, {21.5f}, {25.5f}
};

#define GRAVITY          30.0f
#define HOP_VY           4.2f
#define CHARGE_JUMP_VY   7.6f
#define KART_RADIUS      0.95f
#define DRIFT_MIN_SPEED  7.0f
#define TURN_MAX         1.75f
/* Slide radii for full inward stick, neutral and full counter-steer. */
#define DRIFT_R_TIGHT    13.0f
#define DRIFT_R_NEUTRAL  24.0f
#define DRIFT_R_WIDE     75.0f
#define STAGE1           0.85f
#define STAGE2           1.90f
#define STAGE3           3.10f
#define BOOST_TOP_MUL    1.22f

float kart_top_speed(const Kart *k) {
    float top = cc_params[game.cc].top_speed;
    if(k->cpu) {
        /* Rubber-band against the leading human. */
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
    const char *name = k->name;
    bool cpu = k->cpu;
    float skill = k->skill, lane = k->lane;
    memset(k, 0, sizeof(*k));
    k->paint1 = p1; k->paint2 = p2; k->chao = ch; k->chao_tip = tip; k->ui_color = ui;
    k->name = name;
    k->cpu = cpu; k->skill = skill; k->lane = lane;
    k->pos = track_point(s, d);
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
            if(k->lap >= 2 && k->lap - 2 < RACE_LAPS) {
                float t = game.race_time - k->lap_start;
                k->lap_times[k->lap - 2] = t;
                if(k->best_lap == 0 || t < k->best_lap) k->best_lap = t;
            }
            k->lap_start = game.race_time;
            if(k->lap > RACE_LAPS && !k->finished) {
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

void kart_update(Kart *k, const KartInput *in, float dt, bool can_drive) {
    k->events = 0;
    float top = kart_top_speed(k);
    bool btn = in->drift && can_drive;
    bool pressed = btn && !k->prev_drift_btn;
    bool released = !btn && k->prev_drift_btn;
    k->prev_drift_btn = btn;

    /* Steering input is smoothed so digital input still feels analogue. */
    float steer_in = can_drive ? clampf(in->steer, -1, 1) : 0;
    k->steer = approach(k->steer, steer_in, 7.5f * dt);

    if(k->burnout_t > 0) {
        k->burnout_t -= dt;
        k->speed = 0;
        k->vel = v3(0, 0, 0);
    }

    /* ---- Drift button: hop, slide, charge jump ---- */
    if(pressed && k->grounded && k->burnout_t <= 0) {
        k->vy = HOP_VY;
        k->grounded = false;
        k->air_t = 0;
        k->drift = DRIFT_PENDING;
        k->pending_t = 0.16f;
        k->events |= EV_HOP;
        k->squash_v -= 3.0f;
    }
    if(released) {
        if(k->drift == DRIFT_CHARGE && k->charge_jump_t > 0.45f && k->grounded) {
            k->vy = CHARGE_JUMP_VY;
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
    if(k->drift == DRIFT_ACTIVE && (k->speed < DRIFT_MIN_SPEED || (in->brake && !in->accel)))
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
    if(k->grounded && can_drive && k->burnout_t <= 0) {
        bool gas = in->accel || k->boost_t > 0;
        if(gas && !(in->brake && !in->accel && k->boost_t <= 0)) {
            if(k->speed < target) {
                float rate = k->boost_t > 0 ? 3.0f : 0.85f;
                k->speed += ((target - k->speed) * rate + 1.2f) * dt;
                if(k->speed > target) k->speed = target;
            }
            else {
                k->speed = approach(k->speed, target, (sand ? 16.0f : 5.0f) * dt);
            }
        }
        else if(in->brake) {
            if(k->speed > 0.3f) k->speed = approach(k->speed, 0, 22.0f * dt);
            else k->speed = approach(k->speed, -6.0f, 9.0f * dt);
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
        if(!k->grounded) rate *= 1.25f;           /* hop turns */
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

    /* ---- Vertical ---- */
    if(!k->grounded) {
        k->air_t += dt;
        k->vy -= GRAVITY * dt;
        k->pos.y += k->vy * dt;
        if(k->pos.y <= 0) {
            float impact = -k->vy;
            k->pos.y = 0;
            k->vy = 0;
            k->grounded = true;
            k->squash_v -= impact * 0.9f;
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
    }

    /* ---- Track position, walls ---- */
    float prev_s = k->tp.s;
    track_locate(k->pos, k->tp.idx, &k->tp);
    float lim_out = track_wall_out_at(&k->tp) - KART_RADIUS, lim_in = -(track_wall_in_at(&k->tp) - KART_RADIUS);
    if(k->tp.d > lim_out || k->tp.d < lim_in) {
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
    if(k->bump_t > 0) k->bump_t -= dt;
    update_lap(k, prev_s);

    /* ---- Dash panels ---- */
    int pad = k->grounded ? track_dash_pad(k->tp.s, k->tp.d) : -1;
    if(pad >= 0 && pad != k->last_pad) {
        kart_start_boost(k, 1.1f, 1.3f);
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
    float lean = k->drift == DRIFT_ACTIVE ? -(float)k->drift_dir * 0.10f : -k->steer * 0.05f * clampf(spd / top, 0, 1);
    k->roll += (lean - k->roll) * smooth_k(8.0f, dt);
    float pitch_t = (in->accel && k->speed < top * 0.7f && can_drive ? -0.035f : 0) +
                    (in->brake && k->speed > 1 ? 0.05f : 0) + (k->boost_t > 0 ? -0.05f : 0);
    k->pitch += (pitch_t - k->pitch) * smooth_k(6.0f, dt);
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
}

void karts_collide(void) {
    for(int i = 0; i < kart_count; i++) {
        for(int j = i + 1; j < kart_count; j++) {
            Kart *a = &karts[i], *b = &karts[j];
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
                }
            }
        }
    }
}

/* CPU driver: pure pursuit toward a point on the racing line. The
   required path curvature is mapped onto the stick, through the slide
   radii while drifting, so CPUs drift and counter-steer like a player. */
void ai_drive(Kart *k, KartInput *in, float dt) {
    memset(in, 0, sizeof(*in));
    float s = k->tp.s, spd = fabsf(k->speed);
    float top = kart_top_speed(k);
    float look = 6.0f + spd * 0.45f;
    float td = clampf(track_line(s + look) + k->lane, -6.0f, 6.0f);
    V3 tp = track_point(s + look, td);
    float dx = tp.x - k->pos.x, dz = tp.z - k->pos.z;
    float dist = fmaxf(sqrtf(dx * dx + dz * dz), 1.0f);
    float alpha = wrap_angle(atan2f(dx, dz) - k->heading);
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

    float c_ahead = track_curvature(s + 4.0f + spd * 0.3f);
    float c_now = track_curvature(s + 2.0f);
    if(k->ai_drift_hold > 0) k->ai_drift_hold -= dt;
    if(!k->ai_drift_btn) {
        if(fabsf(c_ahead) > 0.004f && fabsf(track_curvature(s + 35.0f)) > 0.004f && spd > 12.0f &&
           k->grounded && k->drift == DRIFT_NONE && k->ai_drift_hold <= 0)
            k->ai_drift_btn = true;
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
        }
    }
    in->drift = k->ai_drift_btn;
}

uint32_t dk_rand(void) {
    static uint32_t state = 0x2026dcu;
    state = state * 1664525u + 1013904223u;
    return state >> 8;
}
