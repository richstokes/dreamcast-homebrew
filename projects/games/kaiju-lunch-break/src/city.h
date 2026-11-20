/* The city: a grid of blocks on an island, every building of which comes
   down. Buildings are boxes in up to three tiers with an ink shell and a
   halftone shadow on the ground; tall ones topple like trees and flatten
   what they land on, the rest sink into their own dust.

   The big ones come apart first. Each is a stack of bands of four chunks
   (one, if it is slim); a hit knocks out the chunks nearest it, which fly
   off as pieces of their own, and whatever that leaves hanging drops. The
   building stands until a whole band has gone or less than half is left. */
#ifndef KL_CITY_H
#define KL_CITY_H

#include "kl.h"

#ifndef CITY_N
#define CITY_N 11                 /* blocks a side: odd, 15 at most (a building keeps its block in a byte) */
#endif
#define CITY_BLOCK 64.0f
#define CITY_ROAD 20.0f
#define CITY_PITCH (CITY_BLOCK + CITY_ROAD)
#define CITY_HALF ((CITY_N * CITY_PITCH + CITY_ROAD) * 0.5f)
#define CITY_ROADS (CITY_N + 1)
#define CITY_SHORE (CITY_HALF + 46.0f)   /* where the sand meets the sea */
/* The centre line of road i, east-west or north-south alike. */
static inline float city_road(int i) { return -CITY_HALF + CITY_ROAD * 0.5f + (float)i * CITY_PITCH; }

enum { B_INTACT, B_TOPPLE, B_CRUMBLE, B_RUBBLE };
enum { BF_EXPLOSIVE = 1, BF_LANDMARK = 2, BF_BURN = 4, BF_DONUT = 8, BF_NOSHADOW = 16 };
enum { PROP_NONE, PROP_WTANK, PROP_DONUT };

#define B_STRIPS 26
#define CITY_BANDS 24
typedef struct {
    float x, z, hw, hd, h;      /* footprint centre and half sizes, height */
    float hp, hp_max;
    uint32_t value;             /* thousands of dollars */
    uint32_t tint;
    uint8_t state, style, flags, prop;
    uint8_t block, depth, src, nstrips;
    uint32_t v0;                /* its vertices in the pool */
    uint16_t nv, vcap;
    struct { uint16_t off; uint8_t n, kind; } strip[B_STRIPS];
    float prop_x, prop_y, prop_z;
    /* Coming down. */
    float t, dur, ang, angv, dx, dz, px, pz, len, shake, fuse, smoke;
    float h0;                   /* its height when it stood */
    /* In chunks (bands 0: all of a piece). */
    uint8_t bands, cols, carved, ntiers;
    uint8_t cells, cells_left;
    float band_h, carry;
    float tier_y[3], tier_k[3];   /* each tier's top, and its footprint against the base's */
    uint8_t cell[CITY_BANDS];     /* a bit a chunk: bit 0 of the pair is east, bit 1 north */
} Building;
extern Building *city_b;
extern int city_nb;

bool city_init(void);
void city_build(uint32_t seed);
void city_update(float dt);
void city_draw_sky(void);
void city_draw_op(void);
void city_draw_pt(void);
/* The sun and clouds, in the punch-through list. */
void city_draw_sky_pt(void);

/* The standing building over (x, z), or -1. */
int city_at(float x, float z);
void city_damage(int i, float dmg, V3 dir, int src);
/* The same, landing at a point: where a building in chunks loses them. */
void city_damage_at(int i, float dmg, V3 dir, int src, V3 at);
/* Every standing building within r of c takes dmg (falling to half at the
   rim) at c's height, pushed away from c. Returns how many it brought down. */
int city_damage_circle(V3 c, float r, float dmg, int src);
/* A beam from o along d (unit): every building it passes through below
   its roof takes dmg. Returns how many it touched, and the distance to
   the nearest in *first (len if none). */
int city_damage_beam(V3 o, V3 d, float len, float dmg, int src, float *first);
/* Keep a body of this radius, its feet at y, out of the buildings: short
   ones under it are flattened (anything under plough metres, in a charge),
   the rest push back and take ram (if it is walking into them along
   move). True when something pushed. */
bool city_collide(V3 *pos, float radius, float y, V3 move, float ram, float plough);
/* The giant donut within r of p: eaten. True if there was one. */
bool city_eat(V3 p, float r);
/* Tear the nearest chunk within reach of at out of a building, to carry:
   its handle, or -1. It follows city_chunk_hold until city_chunk_throw. */
int city_rip(V3 at, float reach);
void city_chunk_hold(int handle, V3 p);
void city_chunk_throw(int handle, V3 v);
/* Pull up the nearest tree within r of p, to carry: its handle, or -1. */
int city_grab_tree(V3 p, float r);
void city_tree_hold(int handle, V3 p);
void city_tree_throw(int handle, V3 v);
/* A wave's front, r0 to r1 from c: what it reaches takes dmg at street level. */
void city_damage_ring(V3 c, float r0, float r1, float dmg, int src);
/* Trees within r of p go over. */
void city_trample(V3 p, float r);
int city_standing(void);
int city_total(void);
/* The tallest standing building's index within r of p that is at least
   min_h tall, or -1: the autotest's next target. */
int city_nearest(V3 p, float min_h);

#endif
