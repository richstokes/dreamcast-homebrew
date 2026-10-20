/* DreamKarts Grand Prix: every course back to back against the same five
   rivals, points for each finishing place and the standings between races. */
#ifndef DK_GP_H
#define DK_GP_H

#include "dk.h"
#include "track.h"

typedef struct {
    bool active;                        /* a Grand Prix is running (offline only) */
    int race;                           /* races finished so far */
    int count;                          /* races in the cup */
    int order[TRACK_COUNT];             /* the courses, in cup order */
    int points[MAX_KARTS];              /* totals, by kart index */
    int gained[MAX_KARTS];              /* points from the last race */
    int place[MAX_KARTS];               /* finishing place in the last race */
    uint8_t history[TRACK_COUNT][MAX_KARTS]; /* finishing place in each race */
    int rank[MAX_KARTS];                /* kart indices, leader first */
    int page;                           /* results screen: 0 the race, 1 the standings */
    bool quit_prompt;
} GrandPrix;

extern GrandPrix gp;

/* Begin the cup on its first course (from the race setup menu). */
void gp_start(void);
/* Award the finished race's points and rank the field, before the results
   screen opens. */
void gp_race_done(void);
/* True once the last race's points are in: the standings are final. */
bool gp_over(void);
/* Grid slot for a kart in the next race: by the standings, leader in front. */
int gp_grid_slot(int kart);
/* A kart's standing, 1-based. */
int gp_standing(int kart);
/* The results screen of a Grand Prix: the race's points, then the standings. */
void gp_results_update(void);
void gp_draw_results(void);

#endif
