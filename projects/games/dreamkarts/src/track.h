/* DreamKarts tracks: centrelines, road heights, surfaces and queries. */
#ifndef DK_TRACK_H
#define DK_TRACK_H

#include "dk.h"

#define TRACK_SAMPLES 256

enum { SURF_ROAD, SURF_CURB, SURF_SAND };
enum { SEC_ASPHALT, SEC_BOARDWALK };
enum { TRACK_BEACH, TRACK_FIGURE8, TRACK_HARBOUR, TRACK_TEMPLE, TRACK_SKYWAY, TRACK_CYBERSTONE, TRACK_CANYON, TRACK_TSUDOKEN,
       TRACK_APPLE, TRACK_LOT, TRACK_SF, TRACK_COUNT };
/* Tracks the race menus do not offer (the track select, the Grand Prix),
   kept in the code and in the online protocol's numbering: Cyberstone is
   retired for now, and the Night Meet lot is Free Roam's (the mode loads
   it itself offline; online the lobby's picker offers it as a Free Roam
   session, track_step_online). */
#define TRACK_DISABLED ((1u << TRACK_CYBERSTONE) | (1u << TRACK_LOT))
/* Free Roam's lot (roam.h): the whole flat island inside its ring road is
   drivable, and nothing counts laps there. Keyed on the track so an online
   lobby could one day pick it too. */
/* The lot: the ring road's centreline is a rounded rectangle of these half
   extents (metres, about the origin), corners of LOT_CORNER_R, run
   anticlockwise so the lot is on its left (negative d). */
#define LOT_HX       150.0f
#define LOT_HZ       95.0f
#define LOT_CORNER_R 36.0f
#define LOT_WALL     12.5f   /* the ring's outer barrier, beside the road */
#define LOT_WALL_OUT 38.0f   /* where it runs along the deck, outside it, so the ramps and the ground under the deck are inside */
/* The island's edges: quays along the north and the sides, the mainland
   south of LOT_SHORE_Z either side, and a line short of the fog. */
#define LOT_ISLAND_X 230.0f
#define LOT_QUAY_Z   118.0f
#define LOT_SHORE_Z  (-60.0f)
#define LOT_SOUTH_Z  (-180.0f)
/* The expressway deck down the west side and along the south, outside the
   barrier between these lateral offsets, LOT_DECK_Y up, coming down to
   the ground over LOT_DECK_RAMP at each end; along it the ring's barrier
   moves out beyond it (lot_wall_d), so the ramps' feet, the strip beside
   the road and the ground under the deck are all inside the fence. The
   overpass crosses the
   lot's north end at z = LOT_OVER_Z, LOT_OVER_HW either side, flat at
   LOT_OVER_Y between +-LOT_OVER_FLAT and ramping to the ground at
   +-LOT_OVER_FOOT, on an earth embankment (banked both sides like the
   ramps') for LOT_OVER_EMB from each foot and on pillars between. Both
   are road: track_road_y gives their height. */
#define LOT_DECK_D0   18.0f
#define LOT_DECK_D1   34.0f
#define LOT_DECK_Y    11.5f
#define LOT_DECK_RAMP 90.0f
/* The ramps: a single lane leaving the road's outer edge at each foot
   (between LOT_RAMP_D0 and LOT_RAMP_D1), moving out and widening into
   the deck as it climbs; the lane stands on an earth embankment (its
   banks LOT_RAMP_BATTER out per metre of height) for LOT_RAMP_EMB
   metres, then on pillars. */
#define LOT_RAMP_D0   9.6f
#define LOT_RAMP_D1   17.6f
#define LOT_RAMP_EMB  45.0f
#define LOT_RAMP_BATTER 0.8f
#define LOT_OVER_Z    62.0f
#define LOT_OVER_Y    11.0f
#define LOT_OVER_HW   7.0f
#define LOT_OVER_FLAT 60.0f
#define LOT_OVER_FOOT 135.0f
#define LOT_OVER_EMB  45.0f
/* The deck's height at lap distance s, its lane's edges there (the ramp
   lane at a foot, the full deck once up) and whether s is along it at
   all; whether the deck there stands on its ramp's embankment (no
   driving under it); the overpass's height at x and whether it stands
   on its embankment there. */
float lot_deck_y(float s);
float lot_deck_d0(float s);
float lot_deck_d1(float s);
bool lot_in_deck(float s);
bool lot_deck_banked(float s);
/* On a ramp's embankment: metres in from its end wall under the deck
   (where the span begins), with span_dir the way along the track the
   span lies (+1 ahead, -1 behind); negative off the embankment. */
float lot_deck_emb_depth(float s, float *span_dir);
float lot_over_y(float x);
bool lot_over_banked(float x);
/* The ring's barrier: LOT_WALL from the road round the lap, moving out
   with each ramp to the fence at LOT_WALL_OUT beyond the deck and back. */
float lot_wall_d(float s);
/* Two stretches of the deck with no parapet on the lot side: drive off
   and drop to the ground, just for fun. */
bool lot_deck_gap(float s);
/* Which level the next height queries are for, on Night Meet: a kart's
   own height, so the overpass counts only for a kart up on it and the
   ground stays the ground for one driving under. Each kart sets it
   before its physics, shadow and effects. */
void track_probe_height(float y);
static inline bool track_enabled(int id) { return !((TRACK_DISABLED >> id) & 1u); }
/* What the online lobby's leader may pick: the courses, and the lot. */
static inline bool track_online_enabled(int id) { return track_enabled(id) || id == TRACK_LOT; }
/* The next enabled track from id, dir (+1 or -1) along the list, wrapping;
   and how many there are. */
int track_step(int id, int dir);
int track_step_online(int id, int dir);   /* the lobby leader's picker: the lot too */
int track_enabled_count(void);
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

#define MAX_DASH_PADS 6

/* An invisible band across the road where a power-up emerald can sit, in
   one of ITEM_SLOTS slots across its width (items.c). */
typedef struct {
    float s;         /* lap distance */
    float d;         /* the slots' centre across the road: 0 on the road; Night Meet puts bands out in the lot and up on the deck */
} ItemBand;

#define MAX_ITEM_BANDS 8

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
    bool fine_bends;     /* the road mesh adds stations through tight bends (Apple Valley's hairpins) */
} Track;

typedef struct {
    int idx;         /* segment start sample */
    float s;         /* distance along the lap, 0..length */
    float d;         /* lateral offset, + = right/outward */
    V3 center, tangent, right;
} TrackPos;

extern Track track;

static inline bool track_is_roam(void) { return track.id == TRACK_LOT; }

/* Per track, the textures only its scenes draw, ended by -1 (track_tex.c):
   the engine renderer's texture sets (engine_assets.h). */
extern const int16_t *const track_textures[];
extern const char *const track_names[TRACK_COUNT];

void track_init(int id);
/* Lap distance where segment i of a straight-and-arc layout (Palm Harbour,
   Temple Hill, Neon Skyway, Cyberstone, Spiral Canyon, Tsudoken Drift,
   Apple Valley) begins. */
float track_seg_s(int i);
/* The layout's segments: how many, and each one's arc radius (0 on a
   straight) and signed degrees (+ = right). */
int track_seg_count(void);
float track_seg_radius(int i);
float track_seg_deg(int i);
/* Tsudoken Drift's back straight, the one with the jump. */
#define TK_BACK_SEG 16
/* Apple Valley: the road's half width (narrower than the other tracks'
   8 m), and the long top straight. */
#define AV_HALF     6.5f
#define AV_TOP_SEG  24
/* Bay City Hills: the road's half width (Apple Valley's), the segments of
   the pier, the Hyde Street climb, the crooked street's switchbacks (first
   and last) and the plunge, and the quay's height. */
#define SF_HALF       6.5f
#define SF_PIER_SEG   1
#define SF_PIER_END   5
#define SF_HYDE_SEG   12
#define SF_CROOK_SEG  15
#define SF_CROOK_END  23
#define SF_PLUNGE_SEG 30
#define SF_QUAY_Y     1.2f
#define SF_PIER_RAIL_H 0.55f   /* the pier's low edge, so the water and the sea lions show over it */
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
   included (on the Night Meet lot: the deck and the overpass too). */
float track_road_y(float s, float d);
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
