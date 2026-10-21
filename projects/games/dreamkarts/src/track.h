/* DreamKarts tracks: centrelines, road heights, surfaces and queries. */
#ifndef DK_TRACK_H
#define DK_TRACK_H

#include "dk.h"

#define TRACK_SAMPLES 256

enum { SURF_ROAD, SURF_CURB, SURF_SAND };
enum { SEC_ASPHALT, SEC_BOARDWALK };
enum { TRACK_BEACH, TRACK_FIGURE8, TRACK_HARBOUR, TRACK_TEMPLE, TRACK_SKYWAY, TRACK_CYBERSTONE, TRACK_CANYON, TRACK_COUNT };
/* TrackSample.open: a side with no barrier, where karts can fall off. */
enum { OPEN_LEFT = 1, OPEN_RIGHT = 2 };

typedef struct {
    V3 p;            /* centreline point (y = 0; road height in track_height) */
    V3 t;            /* unit tangent (race direction) */
    V3 r;            /* unit right */
    float s;         /* distance along the lap */
    float curv;      /* signed curvature, negative = left turn */
    float line;      /* AI racing-line lateral offset */
    float half;      /* drivable road half width (curbs start here) */
    float curb;      /* rumble strip width (0 on the boardwalk) */
    float wall_in;   /* inner barrier distance (positive) */
    float wall_out;  /* outer barrier distance */
    float shore;     /* 0 = beach behind the outer wall, 1 = open water */
    float bank;      /* road height gained per metre to the right (banked turns) */
    uint8_t section; /* SEC_* road surface style */
    uint8_t open;    /* OPEN_* sides without a barrier */
} TrackSample;

typedef struct {
    float s0, s1;    /* along-track extent */
    float d0, d1;    /* lateral extent */
    float boost;     /* boost seconds */
} DashPad;

#define MAX_DASH_PADS 4

/* An invisible band across the road where a power-up emerald can sit, in
   one of ITEM_SLOTS slots across its width (items.c). */
typedef struct {
    float s;         /* lap distance */
} ItemBand;

#define MAX_ITEM_BANDS 4

/* A ramp jump: the road climbs to a lip, opens over a gap (another stretch
   of track passes beneath) and comes down a landing deck. */
typedef struct {
    float cross;     /* lap distance of the gap centre */
    float ramp, lip; /* ramp foot and lip, metres from the gap centre (< 0) */
    float land, foot;/* landing edge and deck foot, metres from the centre (> 0) */
    float lip_h, land_h;
} Jump;

typedef struct {
    int id;
    const char *name, *blurb;
    TrackSample smp[TRACK_SAMPLES];
    float length, ds;
    float back_start, turn2_start;   /* Emerald Beach boardwalk straight */
    DashPad pads[MAX_DASH_PADS];
    int npads;
    ItemBand bands[MAX_ITEM_BANDS];
    int nbands;
    bool has_jump;
    Jump jump;
    bool has_bridge;     /* a viaduct: lap distance of its centre, half span */
    float bridge_s, bridge_half;
    bool in_sky;         /* nothing below the road: off an open edge, karts fall */
} Track;

typedef struct {
    int idx;         /* segment start sample */
    float s;         /* distance along the lap, 0..length */
    float d;         /* lateral offset, + = right/outward */
    V3 center, tangent, right;
} TrackPos;

extern Track track;

extern const char *const track_names[TRACK_COUNT];

void track_init(int id);
/* Lap distance where segment i of a straight-and-arc layout (Palm Harbour,
   Temple Hill, Neon Skyway, Cyberstone, Spiral Canyon) begins. */
float track_seg_s(int i);
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
/* Road surface height at lap distance s (0 on the floor under a jump gap). */
float track_height(float s);
/* Banking (height per metre to the right) at lap distance s. */
float track_bank(float s);
/* Road surface height at lap distance s and lateral offset d, banking
   included. */
static inline float track_road_y(float s, float d) { return track_height(s) + track_bank(s) * d; }
/* True when a kart at tp has gone over an open edge (no road below). */
bool track_off_edge(const TrackPos *tp);
/* Signed distance from the jump's gap centre, or a large value off it. */
float track_jump_pos(float s);
/* True over the open gap of a jump, where there is no road. */
bool track_in_gap(float s);
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
