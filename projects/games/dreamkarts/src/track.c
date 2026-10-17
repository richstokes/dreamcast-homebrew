/* Track layouts.

   Emerald Beach: an anticlockwise oval. The front straight runs east along
   z = -R (the resort side), the back straight runs west along z = +R on a
   boardwalk beside the sea, joined by two 180-degree left-handers.

   Coral Crossing: a figure 8. Two loops of radius F8_R, centred on the x
   axis, are joined by straight legs that cross at the origin. Leg A runs
   north-east into the clockwise eastern loop and leaps the crossing off a
   ramp; leg B runs north-west underneath it into the anticlockwise western
   loop. The start line is on leg B. */
#include "track.h"
#include <stddef.h>

Track track;

const char *const track_names[TRACK_COUNT] = {"EMERALD BEACH", "CORAL CROSSING"};
static const char *const track_blurbs[TRACK_COUNT] = {"SEASIDE OVAL", "FIGURE 8 WITH A BIG JUMP"};

#define STRAIGHT_HALF 85.0f
#define TURN_R        55.0f
#define START_X       20.0f

#define F8_R      55.0f
#define F8_BETA   0.6632f   /* leg angle from the x axis (38 degrees) */
#define F8_START  28.0f     /* start line, metres into leg B */

static float f8_c, f8_l, f8_arc, f8_u0;   /* loop centre x, half leg, arc, start offset */

/* Emerald Beach centreline at lap distance u (start/finish line at u = 0). */
static V3 oval_centreline(float u, V3 *tangent) {
    const float R = TURN_R, H = STRAIGHT_HALF;
    const float seg1 = H - START_X;          /* front straight to turn 1 */
    const float arc = DK_PI * R;
    const float back = 2.0f * H;
    if(u < seg1) {
        *tangent = v3(1, 0, 0);
        return v3(START_X + u, 0, -R);
    }
    u -= seg1;
    if(u < arc) {
        float phi = -DK_PI * 0.5f + u / R;
        *tangent = v3(-sinf(phi), 0, cosf(phi));
        return v3(H + R * cosf(phi), 0, R * sinf(phi));
    }
    u -= arc;
    if(u < back) {
        *tangent = v3(-1, 0, 0);
        return v3(H - u, 0, R);
    }
    u -= back;
    if(u < arc) {
        float phi = DK_PI * 0.5f + u / R;
        *tangent = v3(-sinf(phi), 0, cosf(phi));
        return v3(-H + R * cosf(phi), 0, R * sinf(phi));
    }
    u -= arc;
    *tangent = v3(1, 0, 0);
    return v3(-H + u, 0, -R);
}

/* Figure 8 centreline at distance u measured from the crossing on leg A. */
static V3 f8_canonical(float u, V3 *tangent) {
    const float R = F8_R, L = f8_l, cb = cosf(F8_BETA), sb = sinf(F8_BETA);
    if(u < L) {                                   /* leg A, crossing to east loop */
        *tangent = v3(cb, 0, sb);
        return v3(u * cb, 0, u * sb);
    }
    u -= L;
    if(u < f8_arc) {                              /* east loop, clockwise */
        float phi = DK_PI * 0.5f + F8_BETA - u / R;
        *tangent = v3(sinf(phi), 0, -cosf(phi));
        return v3(f8_c + R * cosf(phi), 0, R * sinf(phi));
    }
    u -= f8_arc;
    if(u < 2.0f * L) {                            /* leg B, under the jump */
        float a = u - L;
        *tangent = v3(-cb, 0, sb);
        return v3(-a * cb, 0, a * sb);
    }
    u -= 2.0f * L;
    if(u < f8_arc) {                              /* west loop, anticlockwise */
        float phi = DK_PI * 0.5f - F8_BETA + u / R;
        *tangent = v3(-sinf(phi), 0, cosf(phi));
        return v3(-f8_c + R * cosf(phi), 0, R * sinf(phi));
    }
    u -= f8_arc;
    float a = u - L;                              /* leg A, back to the crossing */
    *tangent = v3(cb, 0, sb);
    return v3(a * cb, 0, a * sb);
}

static V3 centreline(float s, V3 *tangent) {
    if(track.id == TRACK_FIGURE8) {
        float u = s + f8_u0;
        if(u >= track.length) u -= track.length;
        return f8_canonical(u, tangent);
    }
    return oval_centreline(s, tangent);
}

static void init_oval(float *turnk) {
    const float R = TURN_R, H = STRAIGHT_HALF;
    const float arc = DK_PI * R;
    float turn1_start = H - START_X;
    float back_start = turn1_start + arc;
    float turn2_start = back_start + 2.0f * H;
    float front_start = turn2_start + arc;

    track.length = 2.0f * (2.0f * H) + 2.0f * arc;
    track.ds = track.length / TRACK_SAMPLES;
    track.back_start = back_start;
    track.turn2_start = turn2_start;

    for(int i = 0; i < TRACK_SAMPLES; i++) {
        TrackSample *t = &track.smp[i];
        t->s = i * track.ds;
        t->p = centreline(t->s, &t->t);
        t->r = v3(t->t.z, 0, -t->t.x);
        bool in_turn = (t->s >= turn1_start && t->s < back_start) ||
                       (t->s >= turn2_start && t->s < front_start);
        t->curv = in_turn ? -1.0f / R : 0.0f;
        turnk[i] = in_turn ? -1.0f : 0.0f;
        t->section = (t->s > back_start + 12.0f && t->s < turn2_start - 12.0f)
            ? SEC_BOARDWALK : SEC_ASPHALT;
        /* The boardwalk is a railed pier over the sea: its outer rail closes
           in over 30 m so wall contact never jumps. */
        float into = fminf(t->s - (back_start + 4.0f), (turn2_start - 4.0f) - t->s);
        float w = clampf(into / 30.0f, 0.0f, 1.0f);
        w = w * w * (3.0f - 2.0f * w);
        t->half = t->section == SEC_BOARDWALK ? 9.6f : 8.0f;
        t->curb = t->section == SEC_BOARDWALK ? 0.0f : 1.6f;
        t->wall_in = 16.5f;
        t->wall_out = lerpf(21.0f, 11.2f, w);
        t->shore = w;
    }

    /* Sonic-style dash panels. */
    float mid_back = back_start + H;
    track.npads = 3;
    track.pads[0] = (DashPad){mid_back - 2.5f, mid_back + 2.5f, -1.8f, 1.8f, 1.1f};
    track.pads[1] = (DashPad){front_start + 32.0f, front_start + 37.0f, 2.2f, 5.8f, 1.1f};
    track.pads[2] = (DashPad){back_start - 30.0f, back_start - 25.0f, -5.8f, -2.2f, 1.1f};
}

static float smoothstep01(float x) {
    x = clampf(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

static void init_figure8(float *turnk) {
    f8_c = F8_R / sinf(F8_BETA);
    f8_l = f8_c * cosf(F8_BETA);
    f8_arc = (DK_PI + 2.0f * F8_BETA) * F8_R;
    f8_u0 = f8_l + f8_arc + F8_START;
    track.length = 4.0f * f8_l + 2.0f * f8_arc;
    track.ds = track.length / TRACK_SAMPLES;
    track.back_start = track.turn2_start = -1.0f;

    /* The ramp: a 30 m kicker to a 4 m lip, a 32 m gap over leg B and a
       landing deck that eases back down to the ground. */
    float cross_a = track.length - f8_u0;          /* leg A crossing */
    float cross_b = f8_l - F8_START;               /* leg B crossing */
    track.has_jump = true;
    track.jump = (Jump){cross_a, -46.0f, -16.0f, 16.0f, 54.0f, 4.0f, 3.0f};

    for(int i = 0; i < TRACK_SAMPLES; i++) {
        TrackSample *t = &track.smp[i];
        t->s = i * track.ds;
        t->p = centreline(t->s, &t->t);
        t->r = v3(t->t.z, 0, -t->t.x);
        float u = t->s + f8_u0;
        if(u >= track.length) u -= track.length;
        float loop = 0;
        if(u >= f8_l && u < f8_l + f8_arc) loop = 1.0f;                  /* east: right turn */
        else if(u >= 3.0f * f8_l + f8_arc && u < 3.0f * f8_l + 2.0f * f8_arc) loop = -1.0f;
        t->curv = loop / F8_R;
        turnk[i] = loop;
        t->section = SEC_ASPHALT;
        t->half = 8.0f;
        t->curb = 1.6f;
        /* Barriers close in where leg B passes the ramp, and become the
           ramp's own rails on leg A. */
        float w = 12.5f;
        float ab = fabsf(track_delta(cross_b, t->s));
        w = lerpf(11.0f, w, smoothstep01((ab - 30.0f) / 15.0f));
        float aa = track_delta(cross_a, t->s);
        float ramp_w = fmaxf(smoothstep01((-58.0f - aa) / 10.0f), smoothstep01((aa - 54.0f) / 10.0f));
        if(aa > -80.0f && aa < 80.0f) w = lerpf(10.2f, w, ramp_w);
        t->wall_in = t->wall_out = w;
        t->shore = 0;
    }

    track.npads = 2;
    /* Full-width jump pad: a long boost so every class clears the gap. */
    track.pads[0] = (DashPad){track_wrap(cross_a - 58.0f), track_wrap(cross_a - 53.0f), -7.6f, 7.6f, 2.2f};
    track.pads[1] = (DashPad){track_wrap(cross_b + 30.0f), track_wrap(cross_b + 35.0f), 2.2f, 5.8f, 1.1f};
}

void track_init(int id) {
    memset(&track, 0, sizeof(track));
    track.id = id = (id % TRACK_COUNT + TRACK_COUNT) % TRACK_COUNT;
    track.name = track_names[id];
    track.blurb = track_blurbs[id];
    float turnk[TRACK_SAMPLES];
    if(id == TRACK_FIGURE8) init_figure8(turnk);
    else init_oval(turnk);

    /* Racing line: swing wide before a turn, hug the inside through it and
       drift back out on exit. Two box blurs of the left- and right-turn
       indicators (left turns put the outside at +d). */
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        float near_l = 0, in_l = 0, near_r = 0, in_r = 0;
        int wide = (int)(55.0f / track.ds), tight = (int)(14.0f / track.ds);
        for(int k = -wide; k <= wide; k++) {
            float v = turnk[(i + k + TRACK_SAMPLES) % TRACK_SAMPLES];
            near_l += fmaxf(-v, 0);
            near_r += fmaxf(v, 0);
        }
        for(int k = -tight; k <= tight; k++) {
            float v = turnk[(i + k + TRACK_SAMPLES) % TRACK_SAMPLES];
            in_l += fmaxf(-v, 0);
            in_r += fmaxf(v, 0);
        }
        float nw = 1.0f / (float)(2 * wide + 1), tw = 1.0f / (float)(2 * tight + 1);
        float line = 5.0f * clampf(2.0f * near_l * nw, 0, 1) - 10.5f * in_l * tw -
                     5.0f * clampf(2.0f * near_r * nw, 0, 1) + 10.5f * in_r * tw;
        /* Straight down the middle over the jump. */
        float a = track_jump_pos(track.smp[i].s);
        line *= smoothstep01((fabsf(a) - 45.0f) / 15.0f);
        track.smp[i].line = clampf(line, -5.5f, 5.5f);
    }

    printf("DreamKarts track: %s, lap %.1f m, %d samples\n", track.name, (double)track.length, TRACK_SAMPLES);
}

float track_jump_pos(float s) {
    if(!track.has_jump) return 1e9f;
    return track_delta(track.jump.cross, s);
}

bool track_in_gap(float s) {
    float a = track_jump_pos(s);
    return a > track.jump.lip && a < track.jump.land;
}

float track_height(float s) {
    if(!track.has_jump) return 0.0f;
    const Jump *j = &track.jump;
    float a = track_delta(j->cross, s);
    if(a >= j->ramp && a <= j->lip) {
        float x = (a - j->ramp) / (j->lip - j->ramp);
        return j->lip_h * x * x * x;
    }
    if(a >= j->land && a <= j->foot)
        return j->land_h * (1.0f - smoothstep01((a - j->land) / (j->foot - j->land)));
    return 0.0f;
}

static float sample_lerp_param(float s, int *i0) {
    s = track_wrap(s);
    float f = s / track.ds;
    int i = (int)f;
    if(i >= TRACK_SAMPLES) i = TRACK_SAMPLES - 1;
    *i0 = i;
    return f - (float)i;
}

V3 track_point(float s, float d) {
    V3 t;
    V3 p = centreline(track_wrap(s), &t);
    V3 r = v3(t.z, 0, -t.x);
    return vmad(p, r, d);
}

V3 track_tangent(float s) {
    V3 t;
    centreline(track_wrap(s), &t);
    return t;
}

float track_curvature(float s) {
    int i;
    float f = sample_lerp_param(s, &i);
    int j = (i + 1) % TRACK_SAMPLES;
    return lerpf(track.smp[i].curv, track.smp[j].curv, f);
}

float track_line(float s) {
    int i;
    float f = sample_lerp_param(s, &i);
    int j = (i + 1) % TRACK_SAMPLES;
    return lerpf(track.smp[i].line, track.smp[j].line, f);
}

int track_section(float s) {
    int i;
    sample_lerp_param(s, &i);
    return track.smp[i].section;
}

static float tp_lerp(const TrackPos *tp, size_t field) {
    const TrackSample *a = &track.smp[tp->idx];
    const TrackSample *b = &track.smp[(tp->idx + 1) % TRACK_SAMPLES];
    float t = clampf((track_delta(a->s, tp->s)) / track.ds, 0, 1);
    float fa = *(const float *)((const char *)a + field), fb = *(const float *)((const char *)b + field);
    return lerpf(fa, fb, t);
}

float track_half_at(const TrackPos *tp) { return tp_lerp(tp, offsetof(TrackSample, half)); }
float track_wall_in_at(const TrackPos *tp) { return tp_lerp(tp, offsetof(TrackSample, wall_in)); }
float track_wall_out_at(const TrackPos *tp) { return tp_lerp(tp, offsetof(TrackSample, wall_out)); }

int track_surface(const TrackPos *tp) {
    float a = fabsf(tp->d);
    float half = track_half_at(tp);
    if(a <= half) return SURF_ROAD;
    if(a <= half + track.smp[tp->idx].curb) return SURF_CURB;
    return SURF_SAND;
}

int track_dash_pad(float s, float d) {
    for(int i = 0; i < track.npads; i++) {
        const DashPad *p = &track.pads[i];
        float ds = track_delta(p->s0, s);
        if(ds >= 0 && ds <= track_delta(p->s0, p->s1) && d >= p->d0 && d <= p->d1)
            return i;
    }
    return -1;
}

void track_locate(V3 p, int hint, TrackPos *out) {
    int first = 0, count = TRACK_SAMPLES;
    if(hint >= 0) {
        first = hint - 6;
        count = 13;
    }
    float best = 1e30f, best_t = 0;
    int best_i = 0;
    for(int k = 0; k < count; k++) {
        int i = (first + k + TRACK_SAMPLES) % TRACK_SAMPLES;
        const TrackSample *a = &track.smp[i];
        const TrackSample *b = &track.smp[(i + 1) % TRACK_SAMPLES];
        float sx = b->p.x - a->p.x, sz = b->p.z - a->p.z;
        float px = p.x - a->p.x, pz = p.z - a->p.z;
        float t = clampf((px * sx + pz * sz) / (sx * sx + sz * sz), 0.0f, 1.0f);
        float qx = px - sx * t, qz = pz - sz * t;
        float d2 = qx * qx + qz * qz;
        if(d2 < best) {
            best = d2;
            best_i = i;
            best_t = t;
        }
    }
    const TrackSample *a = &track.smp[best_i];
    const TrackSample *b = &track.smp[(best_i + 1) % TRACK_SAMPLES];
    out->idx = best_i;
    out->s = track_wrap(a->s + best_t * track.ds);
    out->center = vlerp(a->p, b->p, best_t);
    out->tangent = vnorm(vlerp(a->t, b->t, best_t));
    out->right = v3(out->tangent.z, 0, -out->tangent.x);
    V3 rel = vsub(p, out->center);
    out->d = rel.x * out->right.x + rel.z * out->right.z;
}
