#ifndef PING_CUBE_H
#define PING_CUBE_H

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

/* Start detecting, dialing and negotiating PPP in a background thread.
   Returns <0 if the worker could not be started. */
int modem_link_start(void);

modem_stage_t modem_link_stage(void);

/* Short (at most 20 characters) reason for MODEM_STAGE_FAILED. */
const char *modem_link_failure(void);

/* Carrier rate in bits per second once dialing succeeded, otherwise 0. */
int modem_link_rate(void);

/* Ask the worker to stop after the current SDK call returns. */
void modem_link_cancel(void);

/* Nonzero while the worker thread is still running. */
int modem_link_busy(void);

/* Join the worker and hang up. Only call once modem_link_busy() is zero. */
void modem_link_shutdown(void);

#endif
