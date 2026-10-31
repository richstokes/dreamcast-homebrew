/* DreamKarts Free Roam: no laps, no places, the Night Meet lot to drive
   around with whoever is there. The lot is a flat island inside its ring
   road (track.h TRACK_LOT), and what keeps a kart off the buildings, poles
   and planters on it lives here; world_lot.c builds them and registers
   each as a solid as it goes. */
#ifndef DK_ROAM_H
#define DK_ROAM_H

#include "kart.h"

/* The lot's plan, shared by the scenery (world_lot.c) and the grid: the
   parking bays run in double rows east to west, LOT_ROW_HALF metres deep
   each side of a row's centre line, LOT_BAY_W wide, from -LOT_ROW_HX to
   LOT_ROW_HX; the rows' centre lines are at LOT_ROW_Z. */
#define LOT_ROWS     4
#define LOT_ROW_HALF 5.0f
#define LOT_BAY_W    2.5f
#define LOT_ROW_HX   100.0f
extern const float lot_row_z[LOT_ROWS];
/* Lamp poles stand on each row's centre line, LOT_POLE_STEP apart. */
#define LOT_POLE_STEP 50.0f

/* Solids on the lot: an upright box about c, half extents hx across and
   hz along its yaw, or a disc of radius r; top is the height a kart must
   clear to pass over it. world_lot.c clears the list and registers each
   obstacle as it builds it. */
void roam_solids_clear(void);
void roam_solid_box(V3 c, float hx, float hz, float yaw, float top);
void roam_solid_disc(V3 c, float r, float top);
/* Keep a kart off them: pushed out and bounced as the barriers bounce it
   (kart.c), with the same EV_WALL for the thud. */
void roam_kart_solids(Kart *k);
/* The lot's walls, in place of the ring road's barriers in kart.c: the
   ring's barrier (two-sided, with the gaps at the ramps), the decks'
   parapets where they stand high enough, and the solids. Call after
   track_locate. */
void roam_kart_walls(Kart *k);
/* p is inside a solid, or within margin metres of one. */
bool roam_solid(V3 p, float margin);
/* Line up the karts: the humans nose-in in parking bays by the
   clubhouse, the CPUs spaced out round the ring road, cruising. Online
   every kart is somebody's and gets the bay of its grid slot. */
void roam_place_karts(void);
/* A bay by the clubhouse (0 and 1 the humans' offline, one per grid slot
   online): its centre and the heading the kart parks facing. */
V3 roam_bay(int bay, float *heading);

#endif
