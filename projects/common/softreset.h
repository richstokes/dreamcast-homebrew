/* The Dreamcast soft reset every retail game honours: A+B+X+Y+Start held
   together on any one controller goes back to the title screen, and on the
   title screen leaves the game for the BIOS menu.

   Call softreset_poll() once a frame, before the game reads its own input,
   saying whether the title screen is up. It reads every controller itself,
   so it works whatever the game's own input layer does with the buttons.

   - SOFTRESET_TITLE: put the game back on its title screen
   - SOFTRESET_QUIT: shut the game down and return from main(); the exit
     path is already set, so KOS goes to the BIOS menu (or hands the
     console back to dcload when that loaded us)

   A game's own way out (an Exit item on the title screen) calls
   softreset_leave() before returning from main(), for the same exit.

   The reset fires once when the fifth button goes down, so the hand still
   on the buttons when the title comes back does not quit straight away.
   Players press the buttons one after another, and A or Start alone often
   leaves the title (to a menu, or a new game); a combo finished within
   SOFTRESET_GRACE frames of leaving the title still counts as made on it.

   Test builds: -DSOFTRESET_TEST=N fakes the combo N seconds after boot and
   again 4 s later, for scripted runs in Flycast. */
#ifndef DC_SOFTRESET_H
#define DC_SOFTRESET_H

#include <stdbool.h>
#include <stdio.h>
#include <kos.h>
#include <dc/fs_dcload.h>

#define SOFTRESET_GRACE 45

enum { SOFTRESET_NONE, SOFTRESET_TITLE, SOFTRESET_QUIT };

/* Returning from main() goes back to whatever loaded us: dcload, or from a
   disc nothing useful (a black screen). Leave for the BIOS menu instead. */
static inline void softreset_leave(void) {
    printf("Soft reset: leaving for the %s\n",
           dcload_type == DCLOAD_TYPE_NONE ? "BIOS menu" : "dcload loader");
    if(dcload_type == DCLOAD_TYPE_NONE) arch_set_exit_path(ARCH_EXIT_MENU);
}

static inline int softreset_poll(bool on_title) {
    static bool was_held[MAPLE_PORT_COUNT];
    static int since_title = SOFTRESET_GRACE;
    bool fired = false;

    if(on_title) since_title = 0;
    else if(since_title < SOFTRESET_GRACE) since_title++;

    for(int port = 0; port < MAPLE_PORT_COUNT; port++) {
        maple_device_t *dev = maple_enum_dev(port, 0);
        cont_state_t *st = dev && (dev->info.functions & MAPLE_FUNC_CONTROLLER) ?
                           (cont_state_t *)maple_dev_status(dev) : NULL;
        bool held = st && (st->buttons & CONT_RESET_BUTTONS) == CONT_RESET_BUTTONS;
        if(held && !was_held[port]) fired = true;
        was_held[port] = held;
    }
#ifdef SOFTRESET_TEST
    {
        static int fakes;
        uint64_t at = (uint64_t)(SOFTRESET_TEST) * 1000 + (uint64_t)fakes * 4000;
        if(fakes < 2 && timer_ms_gettime64() >= at) {
            fakes++;
            fired = true;
        }
    }
#endif
    if(!fired) return SOFTRESET_NONE;
    if(since_title < SOFTRESET_GRACE) {
        softreset_leave();
        return SOFTRESET_QUIT;
    }
    printf("Soft reset: back to the title screen\n");
    return SOFTRESET_TITLE;
}

#endif
