/* Dial-up through the Dreamcast modem and PPP, normally answered by a
   DreamPi. The number and PPP login come from the console's saved PlanetWeb
   or DreamPassport ISP profile (one whole profile, never mixed; flash is only
   read), falling back to the DreamPi defaults: 555, login dream / cast.

   Only dial when KOS found no Ethernet adapter: the BBA and the modem share
   the expansion port, so probing the modem would reset a BBA in use.

   KOS's dial and PPP calls block for up to a minute. dialup_start() runs them
   on a worker thread for a frame loop to poll; dialup_connect() blocks, and
   can call back so the caller keeps drawing. A cancel takes effect when the
   SDK call in progress returns: a thread holding the modem or PPP library's
   locks is never killed.

   KOS dials tone digits only: no pulse dialing, pauses or AT commands, and
   the profile's outside-line and call-waiting prefixes are not dialed
   (DreamPi needs none). A profile KOS cannot dial fails rather than dialing
   a different number.

   Needs tools/kos/patches/kos/kos-ppp-lifecycle.patch (stock KOS 2.3's
   ppp_shutdown leaves its receive thread reading freed modem buffers) and
   -lppp. Build with projects/common/common.mk. */
#ifndef DC_DIALUP_H
#define DC_DIALUP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dialup_stage {
    DIALUP_IDLE,
    DIALUP_DETECTING,       /* finding the modem, reading the ISP profile */
    DIALUP_DIALING,         /* waiting for a dial tone, dialing, carrier */
    DIALUP_NEGOTIATING,     /* PPP login and addresses */
    DIALUP_READY,
    DIALUP_FAILED
} dialup_stage_t;

typedef enum dialup_error {
    DIALUP_OK,
    DIALUP_ERR_NO_MODEM,    /* no modem answered */
    DIALUP_ERR_SETTINGS,    /* pulse dialing, or a number KOS cannot dial */
    DIALUP_ERR_PPP_SETUP,   /* PPP would not start or take the login */
    DIALUP_ERR_NO_DIAL_TONE,
    DIALUP_ERR_DIAL,        /* the modem could not dial */
    DIALUP_ERR_NO_ANSWER,   /* no carrier within 60 s */
    DIALUP_ERR_PPP,         /* PPP negotiation or login failed */
    DIALUP_ERR_NO_ADDRESS,  /* PPP came up without an IP address */
    DIALUP_ERR_NO_DNS,      /* no DNS server, with DIALUP_REQUIRE_DNS */
    DIALUP_ERR_CANCELED,
    DIALUP_ERR_THREAD       /* the worker thread could not start */
} dialup_error_t;

/* Flags: fail when neither PPP nor the ISP profile gives a DNS server. */
#define DIALUP_REQUIRE_DNS 1

/* Hang up any previous call, then find the modem, dial and bring PPP up on
   a worker thread. log_name prefixes the few serial lines this prints (NULL
   prints nothing). Only call while dialup_busy() is zero. Returns <0 when
   the worker could not start. */
int dialup_start(const char *log_name, int flags);

/* The same, returning when connected or failed. Without a progress callback
   the SDK calls run on this thread. With one, they run on a worker while
   this thread calls progress(stage) about ten times a second; a nonzero
   return cancels. progress is called once before the modem is touched, so
   canceling then leaves the hardware alone. Only call while dialup_busy()
   is zero (otherwise it returns DIALUP_ERR_THREAD). */
dialup_error_t dialup_connect(const char *log_name, int flags,
                              int (*progress)(dialup_stage_t stage));

dialup_stage_t dialup_stage(void);
/* Why the last attempt failed (DIALUP_OK while connecting or connected). */
dialup_error_t dialup_error(void);
/* Nonzero when redialing might fix it: no dial tone, no carrier, PPP or
   address trouble, rather than missing hardware or unusable settings. */
int dialup_retryable(dialup_error_t error);
/* Carrier rate in bits per second once the modem connected, else 0. */
int dialup_rate(void);
/* Nonzero once an established call has lost carrier or PPP has left its
   network phase. The caller decides whether to redial. */
int dialup_dropped(void);
/* Connected and not dropped. */
int dialup_online(void);

/* Ask the worker to stop when the SDK call in progress returns. */
void dialup_cancel(void);
/* Nonzero while dialup_start()'s worker is running. */
int dialup_busy(void);
/* Cancel, wait for any worker (up to a minute mid-dial; a frame loop that
   must keep drawing cancels and waits for dialup_busy() to clear first),
   then hang up. Safe to call when nothing is connected. */
void dialup_hang_up(void);

#ifdef __cplusplus
}
#endif

#endif
