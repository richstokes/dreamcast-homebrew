/* Dial-up through the Dreamcast modem, normally answered by a DreamPi. The
   approach follows the DCVMU client: use the console's saved ISP profile
   when it has one, otherwise DreamPi's defaults, and never write to flash.

   KOS's dial and PPP calls block for up to a minute, so they run on their
   own thread while the frame loop keeps drawing and reading input. A cancel
   takes effect when the call in progress returns: a thread holding the modem
   or PPP library's locks must never be destroyed. */

#include "browser.h"

#include <dc/flashrom.h>
#include <dc/modem/modem.h>
#include <kos/net.h>
#include <kos/thread.h>
#include <ppp/ppp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { CONNECT_IDLE, CONNECT_RUNNING, CONNECT_DONE };

static kthread_t *worker;
static atomic_int state;
static atomic_int cancel;
static atomic_int result;
/* Each message is a string literal, so the frame loop may read it freely. */
static _Atomic(const char *) message;
static int modem_owned, ppp_owned, connected;

static int has_phone(const flashrom_ispcfg_t *cfg) {
    return (cfg->valid_fields & FLASHROM_ISP_PHONE1) && cfg->phone1[0];
}

/* Use a whole profile, rather than combining credentials from two browsers. */
static void load_settings(flashrom_ispcfg_t *cfg) {
    if(flashrom_get_pw_ispcfg(cfg) == 0 && has_phone(cfg)) {
        printf("browser: modem using the saved PlanetWeb ISP profile\n");
        return;
    }
    if(flashrom_get_ispcfg(cfg) == 0 && has_phone(cfg)) {
        printf("browser: modem using the saved DreamPassport ISP profile\n");
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->phone1, "555");
    printf("browser: modem using DreamPi defaults (no saved dial profile)\n");
}

/* KOS dials tones only: no AT commands, pauses or pulse dialling. Refuse
   what cannot be dialled rather than dial a different number. */
static int dial_number(const flashrom_ispcfg_t *cfg, char number[64]) {
    char raw[64];
    size_t i, n = 0;
    if(cfg->flags & FLASHROM_ISP_PULSE_DIAL) return -1;
    snprintf(raw, sizeof(raw), "%.*s%.*s",
             (cfg->flags & FLASHROM_ISP_DIAL_AREACODE) ? 3 : 0, cfg->p1_areacode,
             (int)sizeof(cfg->phone1), cfg->phone1);
    for(i = 0; raw[i]; ++i) {
        if(strchr("0123456789*#ABCD", raw[i])) number[n++] = raw[i];
        else if(!strchr(" -().", raw[i])) return -1;
    }
    number[n] = 0;
    return n ? 0 : -1;
}

static void hang_up(void) {
    /* PPP must stop before the modem's buffers are freed. */
    if(ppp_owned) {
        ppp_shutdown();
        ppp_owned = 0;
    }
    if(modem_owned) {
        modem_shutdown();
        modem_owned = 0;
    }
    connected = 0;
}

static int step(const char *text) {
    atomic_store(&message, text);
    return atomic_load(&cancel);
}

static const char *connect_link(void) {
    flashrom_ispcfg_t cfg;
    char number[64], login[30], password[21];
    unsigned char dns[4] = {0};
    netif_t *device;
    int blind, rate = 0, code;

    hang_up();
    if(step("finding modem")) return "Connection canceled";
    modem_owned = modem_init() != 0;
    if(!modem_owned)
        return "No network adapter or modem was found";

    load_settings(&cfg);
    if(dial_number(&cfg, number) < 0) {
        memset(&cfg, 0, sizeof(cfg));
        return "The saved dial settings need tone dialling and a plain number";
    }
    snprintf(login, sizeof(login), "%.*s", (int)sizeof(cfg.ppp_login),
             (cfg.valid_fields & FLASHROM_ISP_PPP_USER) ? cfg.ppp_login : "dream");
    snprintf(password, sizeof(password), "%.*s", (int)sizeof(cfg.ppp_passwd),
             (cfg.valid_fields & FLASHROM_ISP_PPP_PASS) ? cfg.ppp_passwd : "cast");
    blind = (cfg.flags & FLASHROM_ISP_BLIND_DIAL) != 0;
    /* Keep only the optional DNS fallback, not the profile's passwords. */
    if(cfg.valid_fields & FLASHROM_ISP_DNS) {
        memcpy(dns, cfg.dns[0], 4);
        if(!(dns[0] | dns[1] | dns[2] | dns[3])) memcpy(dns, cfg.dns[1], 4);
    }
    memset(&cfg, 0, sizeof(cfg));

    code = ppp_init();
    if(code == 0) {
        ppp_owned = 1;
        code = ppp_set_login(login, password);
    }
    memset(login, 0, sizeof(login));
    memset(password, 0, sizeof(password));
    if(code < 0) return "The modem could not be prepared";

    if(step("dialing (to 60 s)")) return "Connection canceled";
    code = ppp_modem_init(number, blind, &rate);
    if(code < 0) {
        /* ppp_modem_init has already shut the modem down. */
        modem_owned = 0;
        printf("browser: dialing failed (%d)\n", code);
        return code == -2 ? "No dial tone: check the DreamPi and the phone cable" :
               code == -3 ? "The number could not be dialed" :
                            "Nothing answered the call: check the DreamPi";
    }
    modem_owned = 1;
    printf("browser: modem carrier at %d bps\n", rate);

    if(step("signing in")) return "Connection canceled";
    code = ppp_connect();
    if(code < 0) {
        printf("browser: PPP failed (%d)\n", code);
        return "The connection refused the saved login";
    }
    device = net_default_dev;
    if(!device || strcmp(device->name, "ppp") ||
       !(device->ip_addr[0] | device->ip_addr[1] | device->ip_addr[2] |
         device->ip_addr[3]))
        return "The connection did not supply an address";
    if(!(device->dns[0] | device->dns[1] | device->dns[2] | device->dns[3]))
        memcpy(device->dns, dns, 4);
    if(!(device->dns[0] | device->dns[1] | device->dns[2] | device->dns[3]))
        return "The connection did not supply a DNS server";
    printf("browser: PPP ready, IP %u.%u.%u.%u DNS %u.%u.%u.%u\n",
           device->ip_addr[0], device->ip_addr[1], device->ip_addr[2],
           device->ip_addr[3], device->dns[0], device->dns[1], device->dns[2],
           device->dns[3]);
    if(atomic_load(&cancel)) return "Connection canceled";
    return NULL;
}

static void *connect_worker(void *unused) {
    const char *failure = connect_link();
    (void)unused;
    if(failure) {
        hang_up();
        atomic_store(&message, failure);
        printf("browser: no dial-up connection: %s\n", failure);
    } else {
        connected = 1;
        atomic_store(&message, "connected");
    }
    atomic_store(&result, failure ? -1 : 1);
    atomic_store_explicit(&state, CONNECT_DONE, memory_order_release);
    return NULL;
}

int modem_connect_start(void) {
    if(atomic_load(&state) != CONNECT_IDLE) return -1;
    atomic_store(&cancel, 0);
    atomic_store(&result, 0);
    atomic_store(&message, "finding modem");
    atomic_store(&state, CONNECT_RUNNING);
    worker = thd_create(0, connect_worker, NULL);
    if(!worker) {
        atomic_store(&state, CONNECT_IDLE);
        return -1;
    }
    return 0;
}

const char *modem_connect_status(void) {
    return atomic_load(&message);
}

int modem_connect_poll(const char **status) {
    *status = atomic_load(&message);
    if(atomic_load_explicit(&state, memory_order_acquire) != CONNECT_DONE) return 0;
    thd_join(worker, NULL);
    worker = NULL;
    atomic_store(&state, CONNECT_IDLE);
    return atomic_load(&result);
}

void modem_connect_cancel(void) {
    atomic_store(&cancel, 1);
}

int modem_in_use(void) {
    return connected;
}

int modem_link_up(void) {
    return connected && modem_is_connected() && net_default_dev &&
           !strcmp(net_default_dev->name, "ppp");
}

void modem_disconnect_link(void) {
    if(atomic_load(&state) != CONNECT_IDLE) {
        const char *ignored;
        atomic_store(&cancel, 1);
        while(!modem_connect_poll(&ignored)) thd_sleep(20);
    }
    hang_up();
}
