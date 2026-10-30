/* DreamKarts' scripted builds (autotest.c). Each piece is declared only
   with the -D flags that build it, as main.c only calls it then. */
#ifndef DK_AUTOTEST_H
#define DK_AUTOTEST_H

#include "dk.h"
#include "kart.h"

#ifdef DK_AUTOTEST
extern int at_skip;   /* frames to leave out of timing after slow I/O */
void at_kart_input(Kart *k, int hp, KartInput *in, float dt);
void at_race_step(void);
void at_player_events(int pi);
void at_frame(float dt_real, float vbl_dt, int steps);
#endif
#ifdef DK_AT_FIXED
bool at_hold_frame(void);   /* true while a screenshot holds the frame still */
#endif
#ifdef DK_CAPTURE
void take_shot(const char *name);
void capture_script(void);   /* each physics step, before the update */
#endif
#if defined(DK_HW) && defined(DK_CAPTURE)
bool probe_step(const uint32_t *last_us);   /* true while probing; last_us: the phase times */
#endif
#ifdef DK_AT_VIEWS
void view_tour(void) __attribute__((noreturn));
#endif
#ifdef DK_AT_CAMTOUR
void camera_tour(void) __attribute__((noreturn));
#endif
#ifdef DK_RECORD
void record_races(uint64_t t0);
#endif

/* main.c's, for the tours and the recorder. */
#if defined(DK_AT_VIEWS) || defined(DK_AT_CAMTOUR)
void game_render(void);
#endif
#ifdef DK_RECORD
void game_boot_screen(uint64_t t0, float progress, const char *what);
void game_countdown_step(float dt);
void game_race_step(float dt);
#endif

#endif
