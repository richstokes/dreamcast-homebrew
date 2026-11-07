/* Ridgeline test builds (-DRL_AUTOTEST): the CPU drives the player up the
   mountain, the log gets frame times every 10 s, and capture builds
   (-DRL_CAPTURE) print RL SHOT lines for tools/capture.py. -DRL_AT_FXBENCH
   (make fxbench) is a short console bench of the tyre effects: a normal
   drive, then the player's wheels spinning in mud. Nothing here is in a
   release build. */
#ifndef RL_AUTOTEST_H
#define RL_AUTOTEST_H

#include "rl.h"

#ifdef RL_AUTOTEST
struct Truck;
void autotest_boot(void);
/* True when the test drives the player itself (the AI then sits out). */
bool autotest_controls(struct Truck *p, float dt);
int autotest_steps(int steps);
void autotest_frame(float dt_real);
void autotest_draw(void);
#endif

#endif
