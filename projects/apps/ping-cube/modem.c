/*
 * Dial-up link for Ping Cube.
 *
 * Used only when KOS found no Ethernet adapter at startup: the BBA and modem
 * share the expansion port, so the modem is never probed while a BBA is in
 * use. DreamPi (or a similar PPP answering setup) must already be configured;
 * the dial number and PPP login come from the console's saved browser ISP
 * profile, falling back to the KOS DreamPi example's defaults.
 *
 * KOS's dial and PPP calls block for up to a minute, so they run on a worker
 * thread while the cube keeps rendering. Cancellation takes effect between
 * SDK calls; the worker is never killed while it holds modem or PPP locks.
 */

#include "ping-cube.h"

#include <kos.h>
#include <dc/flashrom.h>
#include <dc/modem/modem.h>
#include <ppp/ppp.h>

#include <stdio.h>
#include <string.h>

static kthread_t *worker;
static volatile modem_stage_t stage = MODEM_STAGE_IDLE;
static const char *volatile failure = "";
static volatile int cancel_requested;
static volatile int worker_running;
static volatile int connection_rate;
static int modem_owned;
static int ppp_owned;

static int has_phone(const flashrom_ispcfg_t *cfg) {
    return (cfg->valid_fields & FLASHROM_ISP_PHONE1) && cfg->phone1[0];
}

/* Use one whole profile rather than mixing fields from two browsers. Flash
   is only read, never written. */
static void load_settings(flashrom_ispcfg_t *cfg) {
    if(flashrom_get_pw_ispcfg(cfg) == 0 && has_phone(cfg)) {
        printf("Ping Cube: modem using saved PlanetWeb ISP profile\n");
        return;
    }

    if(flashrom_get_ispcfg(cfg) == 0 && has_phone(cfg)) {
        printf("Ping Cube: modem using saved DreamPassport ISP profile\n");
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->phone1, "555");
    printf("Ping Cube: modem using DreamPi defaults (no saved profile)\n");
}

/* KOS dials DTMF digits only: no AT commands, pauses or pulse dialing.
   Refuse unsupported syntax instead of dialing a different number. */
static int dial_number(const flashrom_ispcfg_t *cfg, char number[64]) {
    char raw[64];
    size_t length = 0;
    size_t i;

    if(cfg->flags & FLASHROM_ISP_PULSE_DIAL)
        return -1;

    snprintf(raw, sizeof(raw), "%.*s%.*s",
             (cfg->flags & FLASHROM_ISP_DIAL_AREACODE) ? 3 : 0,
             cfg->p1_areacode,
             (int)sizeof(cfg->phone1), cfg->phone1);

    for(i = 0; raw[i]; ++i) {
        if(strchr("0123456789*#ABCD", raw[i]))
            number[length++] = raw[i];
        else if(!strchr(" -().", raw[i]))
            return -1;
    }

    number[length] = '\0';
    return length ? 0 : -1;
}

static int has_address(const uint8_t address[4]) {
    return address[0] | address[1] | address[2] | address[3];
}

static void hang_up(void) {
    /* Requires the ppp lifecycle patch (see README): stop and join the PPP
       receive thread before the modem buffers are freed. */
    if(ppp_owned) {
        ppp_shutdown();
        ppp_owned = 0;
    }

    if(modem_owned) {
        modem_shutdown();
        modem_owned = 0;
    }
}

static void *modem_worker(void *unused) {
    flashrom_ispcfg_t cfg;
    char number[64];
    char login[30];
    char password[21];
    uint8_t fallback_dns[4] = {0, 0, 0, 0};
    int blind;
    int rate = 0;
    int result;
    netif_t *netif;

    (void)unused;

    if(!modem_init()) {
        failure = "NO BBA OR MODEM";
        goto failed;
    }
    modem_owned = 1;

    if(cancel_requested)
        goto canceled;

    load_settings(&cfg);
    if(dial_number(&cfg, number) < 0) {
        memset(&cfg, 0, sizeof(cfg));
        failure = "BAD DIAL SETTINGS";
        goto failed;
    }

    snprintf(login, sizeof(login), "%.*s", (int)sizeof(cfg.ppp_login),
             (cfg.valid_fields & FLASHROM_ISP_PPP_USER) ? cfg.ppp_login
                                                         : "dream");
    snprintf(password, sizeof(password), "%.*s", (int)sizeof(cfg.ppp_passwd),
             (cfg.valid_fields & FLASHROM_ISP_PPP_PASS) ? cfg.ppp_passwd
                                                         : "cast");
    blind = !!(cfg.flags & FLASHROM_ISP_BLIND_DIAL);

    /* Keep only the optional DNS fallback, not the profile's passwords. */
    if(cfg.valid_fields & FLASHROM_ISP_DNS) {
        memcpy(fallback_dns, cfg.dns[0], sizeof(fallback_dns));
        if(!has_address(fallback_dns))
            memcpy(fallback_dns, cfg.dns[1], sizeof(fallback_dns));
    }
    memset(&cfg, 0, sizeof(cfg));

    result = ppp_init();
    if(result == 0) {
        ppp_owned = 1;
        result = ppp_set_login(login, password);
    }
    memset(login, 0, sizeof(login));
    memset(password, 0, sizeof(password));

    if(result < 0) {
        failure = "PPP SETUP FAILED";
        goto failed;
    }

    if(cancel_requested)
        goto canceled;

    stage = MODEM_STAGE_DIALING;
    printf("Ping Cube: dialing (%s)\n", blind ? "blind" : "wait for tone");
    result = ppp_modem_init(number, blind, &rate);
    if(result < 0) {
        failure = result == -2 ? "NO DIAL TONE" :
                  result == -3 ? "DIAL FAILED" : "NO CARRIER";
        goto failed;
    }

    connection_rate = rate;
    printf("Ping Cube: modem carrier %d bps\n", rate);

    if(cancel_requested)
        goto canceled;

    stage = MODEM_STAGE_NEGOTIATING;
    if(ppp_connect() < 0) {
        failure = "PPP FAILED";
        goto failed;
    }

    netif = net_default_dev;
    if(!netif || strcmp(netif->name, "ppp") || !has_address(netif->ip_addr)) {
        failure = "NO PPP ADDRESS";
        goto failed;
    }

    if(!has_address(netif->dns))
        memcpy(netif->dns, fallback_dns, sizeof(fallback_dns));

    printf("Ping Cube: PPP up, IP %u.%u.%u.%u peer %u.%u.%u.%u\n",
           netif->ip_addr[0], netif->ip_addr[1],
           netif->ip_addr[2], netif->ip_addr[3],
           netif->gateway[0], netif->gateway[1],
           netif->gateway[2], netif->gateway[3]);

    stage = MODEM_STAGE_READY;
    worker_running = 0;
    return NULL;

canceled:
    failure = "CANCELED";
failed:
    printf("Ping Cube: modem link failed: %s\n", failure);
    hang_up();
    stage = MODEM_STAGE_FAILED;
    worker_running = 0;
    return NULL;
}

int modem_link_start(void) {
    stage = MODEM_STAGE_DETECTING;
    worker_running = 1;
    worker = thd_create(0, modem_worker, NULL);

    if(!worker) {
        failure = "NO WORKER THREAD";
        stage = MODEM_STAGE_FAILED;
        worker_running = 0;
        return -1;
    }

    return 0;
}

modem_stage_t modem_link_stage(void) {
    return stage;
}

const char *modem_link_failure(void) {
    return failure;
}

int modem_link_rate(void) {
    return connection_rate;
}

void modem_link_cancel(void) {
    cancel_requested = 1;
}

int modem_link_busy(void) {
    return worker_running;
}

void modem_link_shutdown(void) {
    if(worker) {
        thd_join(worker, NULL);
        worker = NULL;
    }

    hang_up();
}
