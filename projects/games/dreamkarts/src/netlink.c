/* DreamKarts network link: the Broadband/LAN adapter, or the modem dialling
   a DreamPi.

   Nothing touches the network until the player picks Online, so normal
   boots stay fast and never wait on an unplugged adapter. Then a worker
   thread starts KOS's network stack (arch_init_net also hands dcload-ip's
   console over to KOS when the game was loaded that way), and if no
   Ethernet adapter came up, dials with the modem: the number and PPP login
   come from the console's saved PlanetWeb or DreamPassport ISP profile,
   falling back to DreamPi's defaults (555, dream/cast). Dialling and PPP
   block for up to a minute, so they never run on the render thread.

   Adapted from Ping Cube's modem.c; needs the libppp lifecycle patch in
   projects/dcvmu.com/client/patches (applied to the SDK and in CI). */
#include "netlink.h"
#include <kos.h>
#include <dc/flashrom.h>
#include <dc/modem/modem.h>
#include <dc/net/broadband_adapter.h>
#include <dc/net/lan_adapter.h>
#include <ppp/ppp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>

extern void arch_init_net(void);

static kthread_t *worker;
static volatile int stage = LINK_OFF;
static volatile int worker_running;
static volatile int link_lost, link_monitored;
static volatile bool modem_link;
static const char *volatile failure = "";
static char status_buf[48];
static bool stack_up, modem_owned, ppp_owned;
static uint32_t server_ip;   /* network byte order */

static int monitor_input(ppp_protocol_t *self, const uint8_t *buf, size_t length) {
    (void)self;
    (void)buf;
    (void)length;
    return 0;
}

static void monitor_enter_phase(ppp_protocol_t *self, int old_phase, int new_phase) {
    (void)self;
    (void)old_phase;
    if(link_monitored && (new_phase == PPP_PHASE_TERMINATE || new_phase == PPP_PHASE_DEAD)) link_lost = 1;
}

static ppp_protocol_t link_monitor = {
    .name = "dreamkarts-monitor", .code = 0, .input = monitor_input, .enter_phase = monitor_enter_phase
};

static bool has_address(const uint8_t a[4]) {
    return a[0] | a[1] | a[2] | a[3];
}

static bool has_phone(const flashrom_ispcfg_t *cfg) {
    return (cfg->valid_fields & FLASHROM_ISP_PHONE1) && cfg->phone1[0];
}

static void load_settings(flashrom_ispcfg_t *cfg) {
    if(flashrom_get_pw_ispcfg(cfg) == 0 && has_phone(cfg)) return;
    if(flashrom_get_ispcfg(cfg) == 0 && has_phone(cfg)) return;
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->phone1, "555");
}

/* KOS dials DTMF digits only; refuse anything it would misdial. */
static int dial_number(const flashrom_ispcfg_t *cfg, char number[64]) {
    char raw[64];
    size_t n = 0;
    if(cfg->flags & FLASHROM_ISP_PULSE_DIAL) return -1;
    snprintf(raw, sizeof(raw), "%.*s%.*s", (cfg->flags & FLASHROM_ISP_DIAL_AREACODE) ? 3 : 0, cfg->p1_areacode,
             (int)sizeof(cfg->phone1), cfg->phone1);
    for(size_t i = 0; raw[i]; i++) {
        if(strchr("0123456789*#ABCD", raw[i])) number[n++] = raw[i];
        else if(!strchr(" -().", raw[i])) return -1;
    }
    number[n] = '\0';
    return n ? 0 : -1;
}

static void hang_up(void) {
    link_monitored = 0;
    if(ppp_owned) {
        ppp_shutdown();
        ppp_owned = false;
    }
    if(modem_owned) {
        modem_shutdown();
        modem_owned = false;
    }
}

/* Dial the DreamPi and bring PPP up. Returns false with failure set. */
static bool dial(void) {
    flashrom_ispcfg_t cfg;
    char number[64], login[30], password[21];
    uint8_t fallback_dns[4] = {0, 0, 0, 0};
    int rate = 0;
    if(!modem_init()) {
        failure = "NO BROADBAND ADAPTER OR MODEM";
        return false;
    }
    modem_owned = true;
    load_settings(&cfg);
    if(dial_number(&cfg, number) < 0) {
        memset(&cfg, 0, sizeof(cfg));
        failure = "UNSUPPORTED DIAL SETTINGS";
        return false;
    }
    snprintf(login, sizeof(login), "%.*s", (int)sizeof(cfg.ppp_login),
             (cfg.valid_fields & FLASHROM_ISP_PPP_USER) ? cfg.ppp_login : "dream");
    snprintf(password, sizeof(password), "%.*s", (int)sizeof(cfg.ppp_passwd),
             (cfg.valid_fields & FLASHROM_ISP_PPP_PASS) ? cfg.ppp_passwd : "cast");
    int blind = !!(cfg.flags & FLASHROM_ISP_BLIND_DIAL);
    if(cfg.valid_fields & FLASHROM_ISP_DNS) {
        memcpy(fallback_dns, cfg.dns[0], 4);
        if(!has_address(fallback_dns)) memcpy(fallback_dns, cfg.dns[1], 4);
    }
    memset(&cfg, 0, sizeof(cfg));

    int r = ppp_init();
    if(r == 0) {
        ppp_owned = true;
        r = ppp_set_login(login, password);
    }
    if(r == 0) r = ppp_add_protocol(&link_monitor);
    memset(login, 0, sizeof(login));
    memset(password, 0, sizeof(password));
    if(r < 0) {
        failure = "PPP SETUP FAILED";
        return false;
    }
    stage = LINK_DIALING;
    r = ppp_modem_init(number, blind, &rate);
    if(r < 0) {
        failure = r == -2 ? "NO DIAL TONE" : r == -3 ? "DIAL FAILED" : "NO ANSWER FROM DREAMPI";
        return false;
    }
    stage = LINK_NEGOTIATING;
    if(ppp_connect() < 0) {
        failure = "PPP CONNECTION FAILED";
        return false;
    }
    netif_t *n = net_default_dev;
    if(!n || strcmp(n->name, "ppp") || !has_address(n->ip_addr)) {
        failure = "NO ADDRESS FROM DREAMPI";
        return false;
    }
    if(!has_address(n->dns)) memcpy(n->dns, fallback_dns, 4);
    link_lost = 0;
    link_monitored = 1;
    snprintf(status_buf, sizeof(status_buf), "DIAL-UP %d BPS", rate);
    return true;
}

static void *link_worker(void *unused) {
    (void)unused;
    link_lost = 0;
    failure = "";
    if(!stack_up) {
        /* The adapter drivers are only registered with INIT_NET, which the
           game leaves off so it boots without probing the network. */
        bba_init();
        la_init();
        arch_init_net();
        stack_up = true;
    }
    netif_t *n = net_default_dev;
    if(n && strcmp(n->name, "ppp")) {
        if(!has_address(n->ip_addr)) {
            failure = "NO IP ADDRESS (CHECK DHCP)";
            goto failed;
        }
        modem_link = false;
        snprintf(status_buf, sizeof(status_buf), "BROADBAND %d.%d.%d.%d", n->ip_addr[0], n->ip_addr[1],
                 n->ip_addr[2], n->ip_addr[3]);
    }
    else {
        modem_link = true;
        hang_up();
        if(!dial()) goto failed;
    }

    /* Find the server: DNS, or the built-in address if lookup fails. */
    stage = LINK_RESOLVING;
    server_ip = 0;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if(getaddrinfo(DK_NET_HOST, NULL, &hints, &res) == 0 && res) {
        server_ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        freeaddrinfo(res);
    }
    if(!server_ip) server_ip = inet_addr(DK_NET_FALLBACK_IP);
    stage = LINK_UP;
    worker_running = 0;
    return NULL;

failed:
    if(modem_link) hang_up();
    stage = LINK_FAILED;
    worker_running = 0;
    return NULL;
}

void link_start(void) {
    if(worker_running || stage == LINK_UP) return;
    if(worker) {
        thd_join(worker, NULL);
        worker = NULL;
    }
    stage = LINK_STARTING;
    worker_running = 1;
    worker = thd_create(0, link_worker, NULL);
    if(!worker) {
        failure = "NO WORKER THREAD";
        stage = LINK_FAILED;
        worker_running = 0;
    }
}

int link_stage(void) {
    if(stage == LINK_UP && modem_link && (link_lost || !modem_is_connected())) {
        failure = "DIAL-UP CONNECTION DROPPED";
        stage = LINK_FAILED;
    }
    return stage;
}

const char *link_status(void) {
    switch(stage) {
        case LINK_STARTING: return "LOOKING FOR A NETWORK ADAPTER";
        case LINK_DIALING: return "DIALING DREAMPI";
        case LINK_NEGOTIATING: return "CONNECTING TO DREAMPI";
        case LINK_RESOLVING: return "FINDING THE SERVER";
        case LINK_UP: return status_buf;
        case LINK_FAILED: return failure;
        default: return "";
    }
}

bool link_is_modem(void) {
    return modem_link;
}

uint32_t link_server_ip(void) {
    return server_ip;
}

void link_shutdown(void) {
    if(worker) {
        thd_join(worker, NULL);
        worker = NULL;
    }
    if(modem_link) hang_up();
    stage = LINK_OFF;
}
