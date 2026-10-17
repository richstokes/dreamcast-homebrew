/* DreamKarts track: Emerald Beach oval centreline, surfaces and queries. */
#ifndef DK_TRACK_H
#define DK_TRACK_H

#include "dk.h"

#define TRACK_SAMPLES 256

enum { SURF_ROAD, SURF_CURB, SURF_SAND };
enum { SEC_ASPHALT, SEC_BOARDWALK };

typedef struct {
    V3 p;            /* centreline point (y = 0) */
    V3 t;            /* unit tangent (race direction) */
    V3 r;            /* unit right (outward on this anticlockwise oval) */
    float s;         /* distance along the lap */
    float curv;      /* signed curvature, negative = left turn */
    float line;      /* AI racing-line lateral offset */
    float half;      /* drivable road half width (curbs start here) */
    float curb;      /* rumble strip width (0 on the boardwalk) */
    float wall_in;   /* inner barrier distance (positive) */
    float wall_out;  /* outer barrier distance */
    float shore;     /* 0 = beach behind the outer wall, 1 = open water */
    uint8_t section; /* SEC_* road surface style */
} TrackSample;

typedef struct {
    float s0, s1;    /* along-track extent */
    float d0, d1;    /* lateral extent */
} DashPad;

#define DASH_PAD_COUNT 3

typedef struct {
    TrackSample smp[TRACK_SAMPLES];
    float length, ds;
    float back_start, turn2_start;   /* lap distances of the boardwalk straight */
    DashPad pads[DASH_PAD_COUNT];
} Track;

typedef struct {
    int idx;         /* segment start sample */
    float s;         /* distance along the lap, 0..length */
    float d;         /* lateral offset, + = right/outward */
    V3 center, tangent, right;
} TrackPos;

extern Track track;

void track_init(void);
/* Locate p on the track. hint is the previous segment index or -1. */
void track_locate(V3 p, int hint, TrackPos *out);
/* World point at lap distance s (wrapped) and lateral offset d. */
V3 track_point(float s, float d);
/* Unit tangent at lap distance s. */
V3 track_tangent(float s);
float track_curvature(float s);
float track_line(float s);
int track_section(float s);
int track_surface(const TrackPos *tp);
/* Interpolated per-sample widths at a located position. */
float track_half_at(const TrackPos *tp);
float track_wall_in_at(const TrackPos *tp);
float track_wall_out_at(const TrackPos *tp);
int track_dash_pad(float s, float d);
static inline float track_wrap(float s) {
    while(s < 0) s += track.length;
    while(s >= track.length) s -= track.length;
    return s;
}
/* Signed along-track difference b - a in (-L/2, L/2]. */
static inline float track_delta(float a, float b) {
    float d = b - a;
    if(d > track.length * 0.5f) d -= track.length;
    if(d < -track.length * 0.5f) d += track.length;
    return d;
}

#endif
