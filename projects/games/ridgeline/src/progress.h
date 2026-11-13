/* Ridgeline progress: what a drive adds to the save (viewpoints found, the
   trail log's stamps), which trucks that has earned, and the extreme
   trail's run, gate to gate. */
#ifndef RL_PROGRESS_H
#define RL_PROGRESS_H

#include "rl.h"
#include "unlocks.h"

/* ---- A drive ---- */

void progress_drive_start(void);
/* Each driving frame: the extreme trail's gates, places and clock. */
void progress_update(float dt);
/* The player asked for a recover (Y held, or the pause menu). On the
   extreme trail that is back to the last rest passed; anywhere it ends a
   clean run. Returns false if the truck is still to be recovered the
   usual way (truck_recover). */
bool progress_recover(void);
/* The player jumped to a viewpoint: no stamps from this drive, and an
   extreme trail's run is over. */
void progress_jumped(void);
/* Viewpoint i of the loaded area was found; true if for the first time ever. */
bool progress_view(int i);
/* At the summit, first: was the extreme trail driven on the way (it is
   finished here if the summit is its top gate)? Then msg is the summit's
   line, "BY WIDOWMAKER 4:12.3", and the way up is WAY_EXTREME. */
bool progress_summit_trail(char *msg, int size);
/* The summit: by `way` (WAY_*, records.h, or -1 after a jump). Fills msg
   with the stamps it earned, "TRAIL LOG: TOP, CLEAN" ("" for none). */
void progress_summit(int way, char *msg, int size);

/* On the extreme trail between its gates, and its clock in seconds. */
bool progress_trail_on(void);
float progress_trail_clock(void);
/* Driven to its end this drive (the summit is then by it). */
bool progress_trail_done(void);
/* Still clean: no recover this drive. */
bool progress_clean(void);

/* ---- What has been done ---- */

/* The save's, as unlocks.h wants it. */
void progress_done(Done *d);
bool progress_unlocked(int model);
/* The nearest unlocked truck to model, going dir (1 or -1; 0: model itself if it is). */
int progress_next_unlocked(int model, int dir);
/* A locked truck's two lines for the truck select: what it wants, and
   how far along that is ("" when there is nothing to count). */
void progress_need(int model, const char **need, char *count, int size);
/* An area's log: stamps of 3 a way over 4 ways, viewpoints, and all of
   it as a percentage. */
int progress_area_percent(int area);
/* The title's code: every truck unlocked, or locked again. */
void progress_cheat_toggle(void);

#endif
