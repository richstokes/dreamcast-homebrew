#ifndef PING_CUBE_H
#define PING_CUBE_H

#include <stdint.h>

/* Dial-up link stages. The worker thread publishes these; the render thread
   only reads them, so it keeps drawing while KOS's blocking calls run. */
typedef enum modem_stage {
    MODEM_STAGE_IDLE,
    MODEM_STAGE_DETECTING,
    MODEM_STAGE_DIALING,
    MODEM_STAGE_NEGOTIATING,
    MODEM_STAGE_READY,
    MODEM_STAGE_FAILED
} modem_stage_t;

/* Start detecting, dialing and negotiating PPP in a background thread. It
   first hangs up any previous connection, so the same call redials. Only
   call it while modem_link_busy() is zero. Returns <0 if the worker could not
   be started. */
int modem_link_start(void);

modem_stage_t modem_link_stage(void);

/* Short (at most 20 characters) reason for MODEM_STAGE_FAILED. */
const char *modem_link_failure(void);

/* Carrier rate in bits per second once dialing succeeded, otherwise 0. */
int modem_link_rate(void);

/* Nonzero when the last attempt failed in a way that redialing may fix
   (no dial tone, no carrier, PPP failure), rather than missing hardware or
   unusable dial settings. */
int modem_link_retryable(void);

/* Nonzero once an established link has lost carrier or left PPP's network
   phase. */
int modem_link_dropped(void);

/* Ask the worker to stop after the current SDK call returns. */
void modem_link_cancel(void);

/* Nonzero while the worker thread is still running. */
int modem_link_busy(void);

/* Join the worker and hang up. Only call once modem_link_busy() is zero. */
void modem_link_shutdown(void);

/* VMU LCD: latency history for the graph, newest last. -1 marks a timeout. */
void vmu_graph_reset(void);
void vmu_graph_push(int32_t latency_ms);

/* Compose the VMU screen and send it to every attached VMU when it changed.
   floor_ms is the smallest full-scale value of the graph. */
void vmu_display_draw(const char *title, const char *headline,
                      const char *unit, int floor_ms, uint64_t now_ms);

/* Blank every VMU screen (on exit). */
void vmu_display_clear(void);

#endif
