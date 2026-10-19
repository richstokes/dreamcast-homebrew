/* Track layouts.

   Emerald Beach: an anticlockwise oval. The front straight runs east along
   z = -R (the resort side), the back straight runs west along z = +R on a
   boardwalk beside the sea, joined by two 180-degree left-handers.

   Coral Crossing: a figure 8. Two loops of radius F8_R, centred on the x
   axis, are joined by straight legs that cross at the origin. Leg A runs
   north-east into the clockwise eastern loop and leaps the crossing off a
   ramp; leg B runs north-west underneath it into the anticlockwise western
   loop. The start line is on leg B.

   Palm Harbour: a twisty anticlockwise circuit built from straights and
   arcs. Out of turn 1 a fast right-hand sweeper climbs to a long left onto
   the seafront, where a right-left chicane jinks toward the water before
   the beach straight. A left hairpin then drops into the infield, a right
   hairpin doubles back, and two lefts bring it round onto the main
   straight along the grandstand.

   Temple Hill: a jungle hill climb. The main straight runs west along the
   beach; two hairpin switchbacks climb the hillside to a summit ridge with
   a hump that throws karts into the air, then descending esses lead onto
   a stone viaduct over a sea inlet and a last left down to the line.

   Neon Skyway: a neon road through a city in the night sky, above the
   clouds. The
   main straight runs east into a banked left; the waves roll north (no
   barriers: fall off and you are dropped back on); a banked right-hand
   loop climbs 11 m and flies over its own approach, heading west over a
   crest into the plunge and a jump across a gap in the road. A banked
   left hairpin, a dip through the infield, a tight right hairpin and two
   lefts bring it back to the line.

   Cyberstone: Neon Skyway's road and city on a layout after Silverstone's
   National circuit, run clockwise. The start straight runs east, climbing
   into Copse, a fast right; the road crests and drops 11 m down to the
   Maggotts kink and the tight right at Becketts at the bottom of the
   valley. The National straight then climbs 17 m back across the infield
   to Brooklands, a left at the summit, and Luffield, the long banked right
   that spirals down to the fast Woodcote right onto the line. */
#include "track.h"
#include <stddef.h>

Track track;

const char *const track_names[TRACK_COUNT] = {"EMERALD BEACH", "CORAL CROSSING", "PALM HARBOUR", "TEMPLE HILL",
                                              "NEON SKYWAY", "CYBERSTONE"};
static const char *const track_blurbs[TRACK_COUNT] = {"SEASIDE OVAL", "FIGURE 8 WITH A BIG JUMP",
                                                      "HAIRPINS AND A CHICANE", "SWITCHBACKS AND A VIADUCT",
                                                      "A NIGHT RIDE ABOVE THE CITY",
                                                      "HILLS AND A BANKED HELIX"};

#define STRAIGHT_HALF 85.0f
#define TURN_R        55.0f
#define START_X       20.0f

#define F8_R      55.0f
#define F8_BETA   0.6632f   /* leg angle from the x axis (38 degrees) */
#define F8_START  28.0f     /* start line, metres into leg B */

static float f8_c, f8_l, f8_arc, f8_u0;   /* loop centre x, half leg, arc, start offset */

/* Palm Harbour, Temple Hill, Neon Skyway and Cyberstone are laid out from straights
   (radius 0) and arcs (degrees, + = right) end to end from an origin. Two
   straights are solved at init so the lap closes. An arc may be banked:
   its outside rises bank metres per metre from the centreline. */
typedef struct {
    float r, deg, len, bank;
} Seg;

#define MAX_SEGS 20
typedef struct {
    Seg *seg;
    int n;
    float heading;          /* initial heading, 0 = north */
    V3 origin;
    float start;            /* start line, metres into segment 0 */
    int solve_a, solve_b;
    float s0[MAX_SEGS + 1], h0[MAX_SEGS + 1];
    V3 p0[MAX_SEGS + 1];
} Layout;

static Seg hb_seg[] = {
    {0, 0, 91.6f},        /*  0 main straight (solved) */
    {30.0f, -90, 0},      /*  1 turn 1, now north */
    {0, 0, 24.0f},
    {46.0f, 60, 0},       /*  3 fast right sweeper */
    {0, 0, 14.0f},
    {24.0f, -150, 0},     /*  5 long left onto the seafront, now west */
    {0, 0, 34.0f},
    {16.0f, 60, 0},       /*  7 chicane: right, toward the sea */
    {0, 0, 6.0f},
    {16.0f, -60, 0},      /*  9 chicane: left, west again */
    {0, 0, 80.0f},        /* 10 beach straight */
    {28.0f, -180, 0},     /* 11 left hairpin into the infield, now east */
    {0, 0, 50.0f},
    {19.0f, 180, 0},      /* 13 right hairpin, now west */
    {0, 0, 54.0f},
    {26.0f, -90, 0},      /* 15 left, now south */
    {0, 0, 16.8f},        /* 16 (solved) */
    {30.0f, -90, 0},      /* 17 left onto the main straight */
};
#define HB_CHICANE 7      /* first chicane segment */

static Seg th_seg[] = {
    {0, 0, 159.7f},       /*  0 main straight along the beach, west (solved) */
    {35.0f, -90, 0},      /*  1 turn 1, now south */
    {0, 0, 30.0f},
    {25.0f, -90, 0},      /*  3 left at the foot of the hill, now east */
    {0, 0, 90.0f},        /*  4 first climb */
    {17.0f, 180, 0},      /*  5 right hairpin, now west */
    {0, 0, 70.0f},        /*  6 second climb */
    {17.0f, -180, 0},     /*  7 left hairpin, now east */
    {0, 0, 130.0f},       /*  8 summit ridge, over the hump */
    {26.0f, -90, 0},      /*  9 left off the summit, now north */
    {0, 0, 14.0f},
    {18.0f, 70, 0},       /* 11 descending esses: right */
    {18.0f, -70, 0},      /* 12 and left */
    {0, 0, 54.2f},        /* 13 over the viaduct (solved) */
    {30.0f, -90, 0},      /* 14 last left onto the main straight */
};

static Seg sk_seg[] = {
    {0, 0, 120.0f, 0},        /*  0 main straight, east (solved) */
    {40.0f, -90, 0, 0.20f},   /*  1 banked left, now north */
    {0, 0, 134.0f, 0},        /*  2 the waves (solved) */
    {28.0f, 270, 0, 0.16f},   /*  3 banked loop, climbing, now west */
    {0, 0, 235.0f, 0},        /*  4 over the crossing, the plunge and the jump */
    {25.0f, -180, 0, 0.14f},  /*  5 banked left hairpin, now east */
    {0, 0, 90.0f, 0},         /*  6 the infield dip */
    {18.0f, 180, 0, 0.10f},   /*  7 right hairpin, now west */
    {0, 0, 55.0f, 0},         /*  8 */
    {22.0f, -90, 0, 0.10f},   /*  9 left, now south */
    {0, 0, 4.0f, 0},
    {34.0f, -90, 0, 0.16f},   /* 11 banked left onto the main straight */
};

static Seg cs_seg[] = {
    {0, 0, 277.4f, 0},        /*  0 start straight, east (solved) */
    {34.0f, 100, 0, 0.08f},   /*  1 Copse, now south */
    {0, 0, 145.0f, 0},        /*  2 over the crest and down the drop */
    {40.0f, -30, 0, 0.06f},   /*  3 Maggotts kink */
    {0, 0, 8.0f, 0},
    {20.0f, 145, 0, 0.10f},   /*  5 Becketts, now north-west */
    {0, 0, 296.4f, 0},        /*  6 the National straight, climbing (solved) */
    {24.0f, -80, 0, 0},       /*  7 Brooklands, at the summit */
    {0, 0, 10.0f, 0},
    {30.0f, 190, 0, 0.22f},   /*  9 Luffield, banked and spiralling down */
    {0, 0, 30.0f, 0},
    {60.0f, 35, 0, 0.06f},    /* 11 Woodcote onto the start straight */
};

#define NSEGS(a) ((int)(sizeof(a) / sizeof(a[0])))
static Layout hb = {hb_seg, NSEGS(hb_seg), DK_PI * 0.5f, {-69.4f, 0, -98.8f}, 38.0f, 0, 16, {0}, {0}, {{0}}};
static Layout th = {th_seg, NSEGS(th_seg), -DK_PI * 0.5f, {79.0f, 0, 60.0f}, 40.0f, 0, 13, {0}, {0}, {{0}}};
static Layout sk = {sk_seg, NSEGS(sk_seg), DK_PI * 0.5f, {-72.0f, 0, -100.0f}, 40.0f, 0, 2, {0}, {0}, {{0}}};
static Layout cs = {cs_seg, NSEGS(cs_seg), DK_PI * 0.5f, {-120.0f, 0, 119.0f}, 40.0f, 0, 6, {0}, {0}, {{0}}};
static Layout *lay = &hb;

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

/* Point u metres into layout segment i (heading 0 = north). */
static V3 lay_seg_point(const Layout *l, int i, float u, float *heading) {
    const Seg *g = &l->seg[i];
    float h0 = l->h0[i];
    V3 p0 = l->p0[i];
    if(g->r == 0) {
        *heading = h0;
        return v3(p0.x + sinf(h0) * u, 0, p0.z + cosf(h0) * u);
    }
    float k = (g->deg > 0 ? 1.0f : -1.0f) / g->r;
    float h = h0 + k * u;
    *heading = h;
    return v3(p0.x + (cosf(h0) - cosf(h)) / k, 0, p0.z + (sinf(h) - sinf(h0)) / k);
}

/* Lay the segments end to end from the origin. */
static void lay_walk(Layout *l) {
    float h = l->heading, s = 0;
    V3 p = l->origin;
    for(int i = 0; i < l->n; i++) {
        Seg *g = &l->seg[i];
        if(g->r > 0) g->len = g->r * fabsf(g->deg) * (DK_PI / 180.0f);
        l->s0[i] = s;
        l->h0[i] = h;
        l->p0[i] = p;
        p = lay_seg_point(l, i, g->len, &h);
        s += g->len;
    }
    l->s0[l->n] = s;
    l->h0[l->n] = h;
    l->p0[l->n] = p;
}

/* Close the lap: the end point is linear in the two solved straights'
   lengths, so one 2x2 solve lands it on the origin. */
static void lay_close(Layout *l) {
    const int a = l->solve_a, b = l->solve_b;
    lay_walk(l);
    float ex = l->p0[l->n].x - l->origin.x, ez = l->p0[l->n].z - l->origin.z;
    float ax = sinf(l->h0[a]), az = cosf(l->h0[a]), bx = sinf(l->h0[b]), bz = cosf(l->h0[b]);
    float det = ax * bz - bx * az;
    float da = (-ex * bz + bx * ez) / det, db = (-ax * ez + ex * az) / det;
    l->seg[a].len += da;
    l->seg[b].len += db;
    lay_walk(l);
}

/* Layout distance (from the origin) at lap distance s. */
static float lay_u(const Layout *l, float s) {
    float u = s + l->start;
    if(u >= l->s0[l->n]) u -= l->s0[l->n];
    return u;
}

static int lay_seg_at(const Layout *l, float u) {
    int i = 0;
    while(i + 1 < l->n && u >= l->s0[i + 1]) i++;
    return i;
}

static V3 lay_centreline(const Layout *l, float s, V3 *tangent) {
    float u = lay_u(l, s);
    int i = lay_seg_at(l, u);
    float h;
    V3 p = lay_seg_point(l, i, u - l->s0[i], &h);
    *tangent = v3(sinf(h), 0, cosf(h));
    return p;
}

float track_seg_s(int i) {
    return track_wrap(lay->s0[i] - lay->start);
}

static V3 centreline(float s, V3 *tangent) {
    if(track.id == TRACK_HARBOUR || track.id == TRACK_TEMPLE || track.id == TRACK_SKYWAY ||
       track.id == TRACK_CYBERSTONE)
        return lay_centreline(lay, s, tangent);
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

/* Centreline, curvature and default widths for a straight-and-arc layout. */
static void init_layout(Layout *l, float *turnk) {
    lay = l;
    lay_close(l);
    track.length = l->s0[l->n];
    track.ds = track.length / TRACK_SAMPLES;
    track.back_start = track.turn2_start = -1.0f;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        TrackSample *t = &track.smp[i];
        t->s = i * track.ds;
        t->p = centreline(t->s, &t->t);
        t->r = v3(t->t.z, 0, -t->t.x);
        const Seg *g = &l->seg[lay_seg_at(l, lay_u(l, t->s))];
        float k = g->r > 0 ? (g->deg > 0 ? 1.0f : -1.0f) / g->r : 0.0f;
        t->curv = k;
        turnk[i] = k > 0 ? 1.0f : k < 0 ? -1.0f : 0.0f;
        t->section = SEC_ASPHALT;
        t->half = 8.0f;
        t->curb = 1.6f;
        t->wall_in = t->wall_out = 12.5f;
        t->shore = 0;
    }
}

static void init_harbour(float *turnk) {
    init_layout(&hb, turnk);
    /* Barriers pinch in through the chicane so it can't be cut. */
    float ch0 = track_seg_s(HB_CHICANE), ch1 = track_seg_s(HB_CHICANE + 3);
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        TrackSample *t = &track.smp[i];
        float into = fminf(track_delta(ch0 - 10.0f, t->s), track_delta(t->s, ch1 + 10.0f));
        t->wall_in = t->wall_out = lerpf(12.5f, 10.6f, smoothstep01(into / 12.0f));
    }

    /* Dash panels: the seafront run-in, the beach straight's sea side and
       the infield run between the hairpins. */
    float sea = track_seg_s(6), beach = track_seg_s(10), infield = track_seg_s(12);
    track.npads = 3;
    track.pads[0] = (DashPad){sea + 8.0f, sea + 13.0f, -5.8f, -2.2f, 1.1f};
    track.pads[1] = (DashPad){beach + 38.0f, beach + 43.0f, 2.2f, 5.8f, 1.1f};
    track.pads[2] = (DashPad){infield + 22.0f, infield + 27.0f, -1.8f, 1.8f, 1.1f};
}

/* Temple Hill's road height: knots (layout distance, metres) eased with
   smoothsteps, so the slope is continuous, plus the summit hump. The
   first knot is set at init to the last one, a lap earlier. */
static float th_knots[][2] = {
    {0.0f, 7.5f}, {5.0f, 0.0f}, {240.0f, 0.0f}, {290.0f, 1.0f}, {370.0f, 9.0f}, {430.0f, 10.0f},
    {500.0f, 17.0f}, {550.0f, 18.0f}, {690.0f, 18.0f}, {778.0f, 7.5f}, {822.0f, 7.5f},
};
#define TH_KNOTS NSEGS(th_knots)
#define TH_HUMP_U   640.0f     /* hump crest, layout distance */
#define TH_HUMP_LEN 18.0f
#define TH_HUMP_H   1.6f
#define TH_BRIDGE_U 800.0f     /* viaduct centre, layout distance */

static float th_height(float s) {
    float u = lay_u(&th, s);
    if(u >= th_knots[TH_KNOTS - 1][0]) u -= th.s0[th.n];
    int i = 0;
    while(i + 2 < TH_KNOTS && u >= th_knots[i + 1][0]) i++;
    float t = (u - th_knots[i][0]) / (th_knots[i + 1][0] - th_knots[i][0]);
    float h = lerpf(th_knots[i][1], th_knots[i + 1][1], smoothstep01(t));
    float x = (u - TH_HUMP_U) / TH_HUMP_LEN;
    if(x > -0.5f && x < 0.5f) h += TH_HUMP_H * 0.5f * (1.0f + cosf(x * DK_TAU));
    return h;
}

static void init_temple(float *turnk) {
    init_layout(&th, turnk);
    th_knots[0][0] = th_knots[TH_KNOTS - 1][0] - track.length;
    track.has_bridge = true;
    track.bridge_s = track_wrap(TH_BRIDGE_U - th.start);
    track.bridge_half = 24.0f;

    /* Dash panels: the second climb, the run-up to the summit hump (a
       boost there means bigger air) and the main straight. */
    float climb = track_seg_s(6);
    track.npads = 3;
    track.pads[0] = (DashPad){climb + 30.0f, climb + 35.0f, -5.8f, -2.2f, 1.1f};
    track.pads[1] = (DashPad){track_wrap(TH_HUMP_U - th.start - 40.0f), track_wrap(TH_HUMP_U - th.start - 35.0f),
                              -1.8f, 1.8f, 1.1f};
    track.pads[2] = (DashPad){70.0f, 75.0f, 2.2f, 5.8f, 1.1f};
}

/* A road in the sky: glass rails on the turns, open edges over the
   layout-distance ranges given, and each arc's banking eased over about
   30 m each way. */
static float sk_bank[TRACK_SAMPLES];

static void sky_rails_and_banking(const Layout *l, const float (*open_u)[2], int nopen) {
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        TrackSample *t = &track.smp[i];
        float u = lay_u(l, t->s);
        const Seg *g = &l->seg[lay_seg_at(l, u)];
        bool open = false;
        for(int k = 0; k < nopen; k++)
            if(u >= open_u[k][0] && u < open_u[k][1]) open = true;
        t->open = open ? OPEN_LEFT | OPEN_RIGHT : 0;
        t->wall_in = t->wall_out = open ? 60.0f : 10.2f;
        /* Banking: outside up, eased in and out below. */
        sk_bank[i] = g->r > 0 ? (g->deg > 0 ? -g->bank : g->bank) : 0.0f;
    }
    const int ease = 4;
    for(int i = 0; i < TRACK_SAMPLES; i++) {
        float sum = 0;
        for(int k = -ease; k <= ease; k++) sum += sk_bank[(i + k + TRACK_SAMPLES) % TRACK_SAMPLES];
        track.smp[i].bank = sum / (float)(2 * ease + 1);
    }
}

/* Neon Skyway's road height: knots in layout distance eased with
   smoothsteps (the loop's climb, the plunge, the infield dip), the waves,
   the crest hump over the crossing and the jump's ramp and deck. */
#define SK_KNOTS    9
#define SK_WAVES    3
#define SK_WAVE_LEN 26.0f
#define SK_WAVE_H   1.8f
#define SK_TOP      11.0f      /* the loop's exit and the crossing deck */
static float sk_knots[SK_KNOTS][2];
static float sk_waves_u, sk_hump_u;

static float sk_height(float s) {
    float u = lay_u(&sk, s);
    int i = 0;
    while(i + 2 < SK_KNOTS && u >= sk_knots[i + 1][0]) i++;
    float t = (u - sk_knots[i][0]) / (sk_knots[i + 1][0] - sk_knots[i][0]);
    float h = lerpf(sk_knots[i][1], sk_knots[i + 1][1], smoothstep01(t));
    float w = (u - sk_waves_u) / SK_WAVE_LEN;
    if(w > 0 && w < SK_WAVES) h += SK_WAVE_H * 0.5f * (1.0f - cosf(w * DK_TAU));
    float x = (u - sk_hump_u) / 16.0f;
    if(x > -0.5f && x < 0.5f) h += 1.3f * 0.5f * (1.0f + cosf(x * DK_TAU));
    return h;
}

static void init_skyway(float *turnk) {
    init_layout(&sk, turnk);
    track.in_sky = true;
    const float *u0 = sk.s0;
    float L = track.length;
    /* Flat to the loop, which climbs to the crossing deck; the plunge after
       the crest; a 3 m dip through the infield. */
    const float knots[SK_KNOTS][2] = {
        {0.0f, 0.0f}, {u0[3], 0.0f}, {u0[4], SK_TOP}, {u0[4] + 40.0f, SK_TOP}, {u0[4] + 90.0f, 0.0f},
        {u0[6], 0.0f}, {(u0[6] + u0[7]) * 0.5f, -3.0f}, {u0[7], 0.0f}, {L, 0.0f},
    };
    memcpy(sk_knots, knots, sizeof(knots));
    sk_waves_u = u0[2] + 18.0f;
    sk_hump_u = u0[4] + 34.0f;

    /* The jump: its ramp starts at the foot of the plunge, its full-width
       pad on the plunge itself; the deck ends well before the hairpin. */
    track.has_jump = true;
    track.jump = (Jump){track_wrap(u0[4] + 136.0f - sk.start), -46.0f, -16.0f, 16.0f, 54.0f, 4.0f, 3.0f};
    float cross = track.jump.cross;
    track.npads = 3;
    track.pads[0] = (DashPad){track_wrap(cross - 58.0f), track_wrap(cross - 53.0f), -7.6f, 7.6f, 2.2f};
    /* The infield dip and the main straight. */
    float dip = track_seg_s(6);
    track.pads[1] = (DashPad){dip + 30.0f, dip + 35.0f, -1.8f, 1.8f, 1.1f};
    track.pads[2] = (DashPad){50.0f, 55.0f, 2.2f, 5.8f, 1.1f};

    /* Barriers sit close in (glass rails on the turns, the loop and the
       jump); the straights are open, with nothing between you and the sky. */
    const float open_u[][2] = {
        {u0[0] + 8.0f, u0[1] - 8.0f}, {u0[2] + 8.0f, u0[3] - 8.0f}, {u0[6] + 8.0f, u0[7] - 8.0f},
        {u0[8] + 8.0f, u0[9] - 8.0f},
    };
    sky_rails_and_banking(&sk, open_u, NSEGS(open_u));
}

/* Cyberstone's road height: knots in layout distance eased with
   smoothsteps. Up into Copse, over the crest and down the drop to the
   valley at Becketts, up the National straight (over two gentle rollers)
   to the summit at Brooklands, then down round Luffield and Woodcote. */
#define CS_KNOTS    10
#define CS_ROLL_LEN 44.0f
#define CS_ROLL_H   1.2f
static float cs_knots[CS_KNOTS][2];
static float cs_roll_u;

static float cs_height(float s) {
    float u = lay_u(&cs, s);
    int i = 0;
    while(i + 2 < CS_KNOTS && u >= cs_knots[i + 1][0]) i++;
    float t = (u - cs_knots[i][0]) / (cs_knots[i + 1][0] - cs_knots[i][0]);
    float h = lerpf(cs_knots[i][1], cs_knots[i + 1][1], smoothstep01(t));
    float w = (u - cs_roll_u) / CS_ROLL_LEN;
    if(w > 0 && w < 2.0f) h += CS_ROLL_H * 0.5f * (1.0f - cosf(w * DK_TAU));
    return h;
}

static void init_cyberstone(float *turnk) {
    init_layout(&cs, turnk);
    track.in_sky = true;
    const float *u0 = cs.s0;
    float L = track.length;
    const float knots[CS_KNOTS][2] = {
        {0.0f, 0.0f}, {u0[1] - 70.0f, 0.0f}, {u0[2] + 12.0f, 6.0f}, {u0[2] + 40.0f, 6.0f}, {u0[3] + 12.0f, -5.0f},
        {u0[6] + 25.0f, -5.0f}, {u0[7] - 6.0f, 12.0f}, {u0[9] + 6.0f, 12.0f}, {u0[11] + 6.0f, 1.5f}, {L, 0.0f},
    };
    memcpy(cs_knots, knots, sizeof(knots));
    cs_roll_u = u0[6] + 70.0f;

    /* Dash panels: the start straight, the foot of the drop and the
       National straight's climb. */
    track.npads = 3;
    track.pads[0] = (DashPad){60.0f, 65.0f, -5.8f, -2.2f, 1.1f};
    float drop = track_seg_s(2);
    track.pads[1] = (DashPad){drop + 96.0f, drop + 101.0f, 2.2f, 5.8f, 1.1f};
    float nat = track_seg_s(6);
    track.pads[2] = (DashPad){nat + 190.0f, nat + 195.0f, -1.8f, 1.8f, 1.1f};

    /* Open straights, railed turns. */
    const float open_u[][2] = {
        {u0[0] + 8.0f, u0[1] - 8.0f}, {u0[2] + 8.0f, u0[3] - 8.0f}, {u0[6] + 8.0f, u0[7] - 8.0f},
    };
    sky_rails_and_banking(&cs, open_u, NSEGS(open_u));
}

void track_init(int id) {
    memset(&track, 0, sizeof(track));
    track.id = id = (id % TRACK_COUNT + TRACK_COUNT) % TRACK_COUNT;
    track.name = track_names[id];
    track.blurb = track_blurbs[id];
    float turnk[TRACK_SAMPLES];
    if(id == TRACK_FIGURE8) init_figure8(turnk);
    else if(id == TRACK_HARBOUR) init_harbour(turnk);
    else if(id == TRACK_TEMPLE) init_temple(turnk);
    else if(id == TRACK_SKYWAY) init_skyway(turnk);
    else if(id == TRACK_CYBERSTONE) init_cyberstone(turnk);
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

    /* Release builds log only the first build: under dcload-ip each printf
       stalls the console with IRQs off. */
#ifdef DK_AUTOTEST
    bool log = true;
#else
    static bool logged;
    bool log = !logged;
    logged = true;
#endif
    if(log)
        printf("DreamKarts track: %s, lap %.1f m, %d samples\n", track.name, (double)track.length, TRACK_SAMPLES);
}

static float sample_lerp_param(float s, int *i0);

float track_jump_pos(float s) {
    if(!track.has_jump) return 1e9f;
    return track_delta(track.jump.cross, s);
}

bool track_in_gap(float s) {
    float a = track_jump_pos(s);
    return a > track.jump.lip && a < track.jump.land;
}

float track_height(float s) {
    if(track.id == TRACK_TEMPLE) return th_height(s);
    if(track.id == TRACK_CYBERSTONE) return cs_height(s);
    float base = track.id == TRACK_SKYWAY ? sk_height(s) : 0.0f;
    if(!track.has_jump) return base;
    const Jump *j = &track.jump;
    float a = track_delta(j->cross, s);
    if(a >= j->ramp && a <= j->lip) {
        float x = (a - j->ramp) / (j->lip - j->ramp);
        return base + j->lip_h * x * x * x;
    }
    if(a >= j->land && a <= j->foot)
        return base + j->land_h * (1.0f - smoothstep01((a - j->land) / (j->foot - j->land)));
    return base;
}

float track_bank(float s) {
    if(!track.in_sky) return 0.0f;
    int i;
    float f = sample_lerp_param(s, &i);
    return lerpf(track.smp[i].bank, track.smp[(i + 1) % TRACK_SAMPLES].bank, f);
}

bool track_off_edge(const TrackPos *tp) {
    if(!track.in_sky) return false;
    if(track.has_jump && track_in_gap(tp->s)) return true;
    uint8_t open = track.smp[tp->idx].open;
    float edge = track.smp[tp->idx].half + track.smp[tp->idx].curb + 0.5f;
    return (tp->d > edge && (open & OPEN_RIGHT)) || (tp->d < -edge && (open & OPEN_LEFT));
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
    /* The nearest chord picks the span; s comes from projecting along the
       tangent blended across it. A plain chord projection stalls on the
       outside of each sample joint and skips ahead on the inside, which off
       the centreline of a climbing bend steps the road height every
       sample, like a staircase. */
    const TrackSample *a = NULL, *b = NULL;
    for(int step = 0; step < 3; step++) {
        a = &track.smp[best_i];
        b = &track.smp[(best_i + 1) % TRACK_SAMPLES];
        float sx = b->p.x - a->p.x, sz = b->p.z - a->p.z;
        float tx = b->t.x - a->t.x, tz = b->t.z - a->t.z;
        float px = p.x - a->p.x, pz = p.z - a->p.z;
        /* (p - lerp(a, b, t)) . lerp(ta, tb, t) = c0 + c1 t + c2 t^2 = 0 */
        float c0 = px * a->t.x + pz * a->t.z;
        float c1 = px * tx + pz * tz - (sx * a->t.x + sz * a->t.z);
        float c2 = -(sx * tx + sz * tz);
        float disc = c1 * c1 - 4.0f * c0 * c2;
        float den = disc > 0 ? -c1 + sqrtf(disc) : -c1;
        float t = den > 1e-4f ? 2.0f * c0 / den : (px * sx + pz * sz) / (sx * sx + sz * sz);
        if(t < 0 && step < 2) {
            best_i = (best_i + TRACK_SAMPLES - 1) % TRACK_SAMPLES;
            continue;
        }
        if(t > 1 && step < 2) {
            best_i = (best_i + 1) % TRACK_SAMPLES;
            continue;
        }
        best_t = clampf(t, 0.0f, 1.0f);
        break;
    }
    out->idx = best_i;
    out->s = track_wrap(a->s + best_t * track.ds);
    out->center = vlerp(a->p, b->p, best_t);
    out->tangent = vnorm(vlerp(a->t, b->t, best_t));
    out->right = v3(out->tangent.z, 0, -out->tangent.x);
    V3 rel = vsub(p, out->center);
    out->d = rel.x * out->right.x + rel.z * out->right.z;
}
