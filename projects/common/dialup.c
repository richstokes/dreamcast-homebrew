/* Dial-up through the Dreamcast modem and PPP (see dialup.h). */
#include "dialup.h"

#include <kos.h>
#include <dc/flashrom.h>
#include <dc/modem/modem.h>
#include <ppp/ppp.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static kthread_t *worker;
static volatile int worker_running;
static volatile int cancel_requested;
static volatile dialup_stage_t stage = DIALUP_IDLE;
static volatile dialup_error_t last_error = DIALUP_OK;
static volatile int connection_rate;
static volatile int link_monitored;
static volatile int link_lost;
static int modem_owned;
static int ppp_owned;
static int sdk_result;
static const char *log_name;
static int connect_flags;

static void say(const char *format, ...) {
    char line[112];
    va_list args;

    if(!log_name)
        return;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    /* One printf per line: under dcload-ip each is a network round trip. */
    printf("%s: %s\n", log_name, line);
}

/* memset of a dead local can be optimized away; these must really clear. */
static void wipe(void *buffer, size_t length) {
    volatile unsigned char *bytes = buffer;

    while(length--)
        *bytes++ = 0;
}

static int has_address(const uint8_t address[4]) {
    return address[0] | address[1] | address[2] | address[3];
}

/* A do-nothing PPP "protocol" whose only job is to hear phase changes. PPP
   protocol 0 is reserved, so no packet is ever delivered to it. */
static int monitor_input(ppp_protocol_t *self, const uint8_t *buf,
                         size_t length) {
    (void)self;
    (void)buf;
    (void)length;
    return 0;
}

static void monitor_enter_phase(ppp_protocol_t *self, int old_phase,
                                int new_phase) {
    (void)self;
    (void)old_phase;
    if(link_monitored && (new_phase == PPP_PHASE_TERMINATE ||
                          new_phase == PPP_PHASE_DEAD))
        link_lost = 1;
}

static ppp_protocol_t link_monitor = {
    .name = "dialup-monitor",
    .code = 0,
    .input = monitor_input,
    .enter_phase = monitor_enter_phase
};

static int has_phone(const flashrom_ispcfg_t *cfg) {
    return (cfg->valid_fields & FLASHROM_ISP_PHONE1) && cfg->phone1[0];
}

static void load_settings(flashrom_ispcfg_t *cfg) {
    if(flashrom_get_pw_ispcfg(cfg) == 0 && has_phone(cfg)) {
        say("modem using the saved PlanetWeb ISP profile");
        return;
    }
    if(flashrom_get_ispcfg(cfg) == 0 && has_phone(cfg)) {
        say("modem using the saved DreamPassport ISP profile");
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->phone1, "555");
    say("modem using DreamPi defaults (no saved dial profile)");
}

static int dial_number(const flashrom_ispcfg_t *cfg, char number[64]) {
    char raw[64];
    size_t length = 0;
    size_t i;

    if(cfg->flags & FLASHROM_ISP_PULSE_DIAL)
        return -1;
    snprintf(raw, sizeof(raw), "%.*s%.*s",
             (cfg->flags & FLASHROM_ISP_DIAL_AREACODE) ? 3 : 0,
             cfg->p1_areacode, (int)sizeof(cfg->phone1), cfg->phone1);
    for(i = 0; raw[i]; ++i) {
        if(strchr("0123456789*#ABCD", raw[i]))
            number[length++] = raw[i];
        else if(!strchr(" -().", raw[i]))
            return -1;
    }
    number[length] = '\0';
    return length ? 0 : -1;
}

static void hang_up(void) {
    link_monitored = 0;
    /* PPP must stop before the modem's buffers are freed. The shutdown also
       unregisters link_monitor. */
    if(ppp_owned) {
        ppp_shutdown();
        ppp_owned = 0;
    }
    if(modem_owned) {
        modem_shutdown();
        modem_owned = 0;
    }
}

static const char *error_name(dialup_error_t error) {
    switch(error) {
        case DIALUP_OK: return "connected";
        case DIALUP_ERR_NO_MODEM: return "no modem found";
        case DIALUP_ERR_SETTINGS: return "the dial settings need tone dialing and a plain number";
        case DIALUP_ERR_PPP_SETUP: return "PPP setup failed";
        case DIALUP_ERR_NO_DIAL_TONE: return "no dial tone";
        case DIALUP_ERR_DIAL: return "could not dial";
        case DIALUP_ERR_NO_ANSWER: return "no answer";
        case DIALUP_ERR_PPP: return "PPP negotiation failed";
        case DIALUP_ERR_NO_ADDRESS: return "PPP gave no IP address";
        case DIALUP_ERR_NO_DNS: return "no DNS server";
        case DIALUP_ERR_CANCELED: return "canceled";
        case DIALUP_ERR_THREAD: return "no worker thread";
    }
    return "unknown error";
}

static dialup_error_t connect_link(void) {
    flashrom_ispcfg_t cfg;
    char number[64];
    char login[30];
    char password[21];
    uint8_t fallback_dns[4] = {0, 0, 0, 0};
    int blind;
    int rate = 0;
    netif_t *netif;

    /* A redial starts from whatever the previous call left behind. */
    hang_up();
    link_lost = 0;
    connection_rate = 0;
    sdk_result = 0;

    if(cancel_requested)
        return DIALUP_ERR_CANCELED;
    if(!modem_init())
        return DIALUP_ERR_NO_MODEM;
    modem_owned = 1;
    if(cancel_requested)
        return DIALUP_ERR_CANCELED;

    load_settings(&cfg);
    if(dial_number(&cfg, number) < 0) {
        wipe(&cfg, sizeof(cfg));
        return DIALUP_ERR_SETTINGS;
    }
    snprintf(login, sizeof(login), "%.*s", (int)sizeof(cfg.ppp_login),
             (cfg.valid_fields & FLASHROM_ISP_PPP_USER) ? cfg.ppp_login : "dream");
    snprintf(password, sizeof(password), "%.*s", (int)sizeof(cfg.ppp_passwd),
             (cfg.valid_fields & FLASHROM_ISP_PPP_PASS) ? cfg.ppp_passwd : "cast");
    blind = !!(cfg.flags & FLASHROM_ISP_BLIND_DIAL);
    /* Keep only the optional DNS fallback, not the profile's passwords. */
    if(cfg.valid_fields & FLASHROM_ISP_DNS) {
        memcpy(fallback_dns, cfg.dns[0], sizeof(fallback_dns));
        if(!has_address(fallback_dns))
            memcpy(fallback_dns, cfg.dns[1], sizeof(fallback_dns));
    }
    wipe(&cfg, sizeof(cfg));

    sdk_result = ppp_init();
    if(sdk_result == 0) {
        ppp_owned = 1;
        sdk_result = ppp_set_login(login, password);
    }
    if(sdk_result == 0)
        sdk_result = ppp_add_protocol(&link_monitor);
    wipe(login, sizeof(login));
    wipe(password, sizeof(password));
    if(sdk_result < 0)
        return DIALUP_ERR_PPP_SETUP;
    if(cancel_requested)
        return DIALUP_ERR_CANCELED;

    stage = DIALUP_DIALING;
    say("dialing (%s)", blind ? "blind" : "waiting for a dial tone");
    sdk_result = ppp_modem_init(number, blind, &rate);
    if(sdk_result < 0) {
        /* ppp_modem_init has already shut the modem down; modem_shutdown
           is safe to repeat. */
        return sdk_result == -1 ? DIALUP_ERR_NO_MODEM :
               sdk_result == -2 ? DIALUP_ERR_NO_DIAL_TONE :
               sdk_result == -3 ? DIALUP_ERR_DIAL : DIALUP_ERR_NO_ANSWER;
    }
    connection_rate = rate;
    say("modem carrier %d bps; negotiating PPP", rate);
    if(cancel_requested)
        return DIALUP_ERR_CANCELED;

    stage = DIALUP_NEGOTIATING;
    sdk_result = ppp_connect();
    if(sdk_result < 0)
        return DIALUP_ERR_PPP;
    netif = net_default_dev;
    if(!netif || strcmp(netif->name, "ppp") || !has_address(netif->ip_addr))
        return DIALUP_ERR_NO_ADDRESS;
    if(!has_address(netif->dns))
        memcpy(netif->dns, fallback_dns, sizeof(fallback_dns));
    if((connect_flags & DIALUP_REQUIRE_DNS) && !has_address(netif->dns))
        return DIALUP_ERR_NO_DNS;
    if(cancel_requested)
        return DIALUP_ERR_CANCELED;

    say("PPP ready, IP %u.%u.%u.%u DNS %u.%u.%u.%u",
        netif->ip_addr[0], netif->ip_addr[1], netif->ip_addr[2], netif->ip_addr[3],
        netif->dns[0], netif->dns[1], netif->dns[2], netif->dns[3]);
    link_lost = 0;
    link_monitored = 1;
    return DIALUP_OK;
}

static dialup_error_t finish(dialup_error_t error) {
    if(error != DIALUP_OK) {
        if(sdk_result)
            say("dial-up failed: %s (SDK result %d)", error_name(error), sdk_result);
        else
            say("dial-up failed: %s", error_name(error));
        hang_up();
    }
    last_error = error;
    stage = error == DIALUP_OK ? DIALUP_READY : DIALUP_FAILED;
    return error;
}

static void prepare(const char *name, int flags) {
    log_name = name;
    connect_flags = flags;
    cancel_requested = 0;
    last_error = DIALUP_OK;
    stage = DIALUP_DETECTING;
}

static void *dial_worker(void *unused) {
    (void)unused;
    finish(connect_link());
    worker_running = 0;
    return NULL;
}

int dialup_start(const char *name, int flags) {
    if(worker_running)
        return -1;
    /* The previous worker has already returned; reap it before replacing it. */
    if(worker) {
        thd_join(worker, NULL);
        worker = NULL;
    }
    prepare(name, flags);
    worker_running = 1;
    worker = thd_create(0, dial_worker, NULL);
    if(!worker) {
        worker_running = 0;
        sdk_result = 0;
        finish(DIALUP_ERR_THREAD);
        return -1;
    }
    return 0;
}

dialup_error_t dialup_connect(const char *name, int flags,
                              int (*progress)(dialup_stage_t stage)) {
    uint64_t next_update = 0;

    if(worker_running)
        return DIALUP_ERR_THREAD;
    prepare(name, flags);
    if(!progress)
        return finish(connect_link());
    if(progress(DIALUP_DETECTING)) {
        sdk_result = 0;
        return finish(DIALUP_ERR_CANCELED);
    }
    if(dialup_start(name, flags) < 0)
        return last_error;
    while(worker_running) {
        uint64_t now = timer_ms_gettime64();

        if(now >= next_update) {
            if(progress(stage))
                cancel_requested = 1;
            next_update = now + 100;
        }
        thd_sleep(10);
    }
    thd_join(worker, NULL);
    worker = NULL;
    return last_error;
}

dialup_stage_t dialup_stage(void) {
    return stage;
}

dialup_error_t dialup_error(void) {
    return last_error;
}

int dialup_retryable(dialup_error_t error) {
    return error == DIALUP_ERR_NO_DIAL_TONE || error == DIALUP_ERR_DIAL ||
           error == DIALUP_ERR_NO_ANSWER || error == DIALUP_ERR_PPP ||
           error == DIALUP_ERR_NO_ADDRESS || error == DIALUP_ERR_NO_DNS;
}

int dialup_rate(void) {
    return connection_rate;
}

int dialup_dropped(void) {
    if(stage != DIALUP_READY || worker_running)
        return 0;
    return link_lost || !modem_is_connected();
}

int dialup_online(void) {
    return stage == DIALUP_READY && !dialup_dropped() && net_default_dev &&
           !strcmp(net_default_dev->name, "ppp");
}

void dialup_cancel(void) {
    cancel_requested = 1;
}

int dialup_busy(void) {
    return worker_running;
}

void dialup_hang_up(void) {
    if(worker) {
        cancel_requested = 1;
        thd_join(worker, NULL);
        worker = NULL;
    }
    hang_up();
    connection_rate = 0;
    last_error = DIALUP_OK;
    stage = DIALUP_IDLE;
}
