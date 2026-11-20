/* The bot that plays the kaiju (the title's attract mode, and the test
   builds), and the test builds' log and screenshots. */
#ifndef KL_AUTOTEST_H
#define KL_AUTOTEST_H

#include "kl.h"

/* A frame of the bot's play: the nearest building, then everything it has. */
void bot_ctl(Ctl *c, float dt);
#ifdef KL_AUTOTEST
void autotest_boot(void);
void autotest_frame(float dt_real);
#endif

#endif
