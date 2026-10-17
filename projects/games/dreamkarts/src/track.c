/* Emerald Beach: an anticlockwise oval. The front straight runs east along
   z = -R (the resort side), the back straight runs west along z = +R on a
   boardwalk beside the sea, joined by two 180-degree left-handers. */
#include "track.h"
#include <stddef.h>

Track track;

#define STRAIGHT_HALF 85.0f
#define TURN_R        55.0f
#define START_X       20.0f

/* Centreline position at lap distance u (start/finish line at u = 0). */
static V3 centreline(float u, V3 *tangent) {
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

void track_init(void) {
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

    float turnk[TRACK_SAMPLES];
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        TrackSample *t = &track.smp[i];
        t->s = i * track.ds;
        t->p = centreline(t->s, &t->t);
        t->r = v3(t->t.z, 0, -t->t.x);
        bool in_turn = (t->s >= turn1_start && t->s < back_start) ||
                       (t->s >= turn2_start && t->s < front_start);
        t->curv = in_turn ? -1.0f / R : 0.0f;
        turnk[i] = in_turn ? 1.0f : 0.0f;
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

    /* Racing line: swing wide before a turn, hug the inside through it and
       drift back out on exit. Two box blurs of the turn indicator. */
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        float near = 0, in = 0;
        int wide = (int)(55.0f / track.ds), tight = (int)(14.0f / track.ds);
        for(int k = -wide; k <= wide; k++)
            near += turnk[(i + k + TRACK_SAMPLES) % TRACK_SAMPLES];
        for(int k = -tight; k <= tight; k++)
            in += turnk[(i + k + TRACK_SAMPLES) % TRACK_SAMPLES];
        near /= (float)(2 * wide + 1);
        in /= (float)(2 * tight + 1);
        track.smp[i].line = clampf(5.0f * clampf(2.0f * near, 0, 1) - 10.5f * in, -5.5f, 5.0f);
    }

    /* Sonic-style dash panels. */
    float mid_back = back_start + H;
    track.pads[0] = (DashPad){mid_back - 2.5f, mid_back + 2.5f, -1.8f, 1.8f};
    track.pads[1] = (DashPad){front_start + 32.0f, front_start + 37.0f, 2.2f, 5.8f};
    track.pads[2] = (DashPad){back_start - 30.0f, back_start - 25.0f, -5.8f, -2.2f};

    printf("DreamKarts track: Emerald Beach oval, lap %.1f m, %d samples\n",
           (double)track.length, TRACK_SAMPLES);
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
    for(int i = 0; i < DASH_PAD_COUNT; i++) {
        const DashPad *p = &track.pads[i];
        float ds = track_delta(p->s0, s);
        if(ds >= 0 && ds <= p->s1 - p->s0 && d >= p->d0 && d <= p->d1)
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
