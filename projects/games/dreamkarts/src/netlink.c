/* DreamKarts network link: the Broadband/LAN adapter, a WIZnet W5500 on the
   serial port, or the modem dialling a dial-up server (a DreamPi or any
   other PPP setup).

   Nothing touches the network until the player picks Online, so normal
   boots stay fast and never wait on an unplugged adapter. Entering the
   online screens starts a worker thread (link_prepare) that brings up KOS's
   network stack while the player types a name; arch_init_net also hands
   dcload-ip's console over to KOS when the game was loaded that way. If no
   Ethernet adapter came up, the modem dials once the player confirms the
   name (link_start): the number and PPP login come from the console's saved
   PlanetWeb or DreamPassport ISP profile, falling back to 555 with login
   dream/cast (a DreamPi accepts these). Dialling and PPP block for up to a
   minute, so they never run on the render thread.

   Dialing itself is projects/common/dialup.c, run on this file's worker. */
#include "netlink.h"
#include "../../../common/dialup.h"
#include <kos.h>
#include <dc/net/broadband_adapter.h>
#include <dc/net/lan_adapter.h>
#include <dc/net/w5500_adapter.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>

extern void arch_init_net(void);

static kthread_t *worker;
static volatile int stage = LINK_OFF;
static volatile int worker_running;
static volatile int want_dial;   /* the player asked to connect, so the modem may dial */
static volatile bool modem_link;
static const char *volatile failure = "";
static char status_buf[48];
static bool stack_up;
static uint32_t server_ip;   /* network byte order */

static bool has_address(const uint8_t a[4]) {
    return a[0] | a[1] | a[2] | a[3];
}

static const char *dial_failure(dialup_error_t error) {
    switch(error) {
        case DIALUP_ERR_NO_MODEM: return "NO NETWORK ADAPTER OR MODEM";
        case DIALUP_ERR_SETTINGS: return "UNSUPPORTED DIAL SETTINGS";
        case DIALUP_ERR_PPP_SETUP: return "PPP SETUP FAILED";
        case DIALUP_ERR_NO_DIAL_TONE: return "NO DIAL TONE";
        case DIALUP_ERR_DIAL: return "DIAL FAILED";
        case DIALUP_ERR_NO_ANSWER: return "NO ANSWER ON THE MODEM LINE";
        case DIALUP_ERR_PPP: return "PPP CONNECTION FAILED";
        case DIALUP_ERR_NO_ADDRESS: return "NO ADDRESS FROM THE DIAL-UP SERVER";
        default: return "DIAL-UP FAILED";
    }
}

/* Dial out with the modem and bring PPP up. Returns false with failure set.
   Silent: under dcload every printf stalls the console. */
static bool dial(void) {
    dialup_error_t error = dialup_connect(NULL, 0, NULL);
    if(error != DIALUP_OK) {
        failure = dial_failure(error);
        return false;
    }
    snprintf(status_buf, sizeof(status_buf), "DIAL-UP %d BPS", dialup_rate());
    return true;
}

/* While the worker dials, the shared dialer knows which step it is on. */
static int shown_stage(void) {
    if(stage == LINK_STARTING && modem_link) {
        dialup_stage_t step = dialup_stage();
        if(step == DIALUP_DIALING) return LINK_DIALING;
        if(step == DIALUP_NEGOTIATING) return LINK_NEGOTIATING;
    }
    return stage;
}

#ifdef DK_NET_TRACE
/* Time each step of bringing the link up (few lines: printf stalls under dcload). */
static uint64_t trace_t0;
#define NET_TRACE(what) printf("DK NET %5lu ms %s\n", (unsigned long)(timer_ms_gettime64() - trace_t0), what)

/* Watch KOS from outside: when the adapter's link came up, and when DHCP
   gave an address. KOS mutes dcload's console while it starts the network,
   so the times are kept and printed afterwards. */
static volatile uint64_t trace_link_ms, trace_dhcp_ms, trace_player_ms;

static void *trace_watch(void *unused) {
    (void)unused;
    for(int i = 0; i < 6000 && !trace_dhcp_ms; i++) {
        netif_t *n = net_default_dev;
        if(!trace_link_ms && n && (n->flags & NETIF_RUNNING)) trace_link_ms = timer_ms_gettime64() - trace_t0;
        if(trace_link_ms && has_address(n->ip_addr)) trace_dhcp_ms = timer_ms_gettime64() - trace_t0;
        thd_sleep(5);
    }
    return NULL;
}

static void trace_report(void) {
    printf("DK NET link up %lu ms, DHCP address %lu ms, player connects %lu ms\n",
           (unsigned long)trace_link_ms, (unsigned long)trace_dhcp_ms, (unsigned long)trace_player_ms);
}
#else
#define NET_TRACE(what) do { } while(0)
#endif

static void *link_worker(void *unused) {
    (void)unused;
    failure = "";
#ifdef DK_NET_TRACE
    trace_t0 = timer_ms_gettime64();
    if(!stack_up) thd_create(1, trace_watch, NULL);
#endif
    if(!stack_up) {
        /* The adapter drivers are only registered with INIT_NET, which the
           game leaves off so it boots without probing the network. */
        bba_init();
        NET_TRACE("bba_init");
        la_init();
        NET_TRACE("la_init");
        /* Registered last so a BBA wins if both are plugged in. Probing
           briefly borrows the serial port, then hands it back. */
        w5500_adapter_init(NULL, true);
        NET_TRACE("w5500_adapter_init");
        arch_init_net();
        NET_TRACE("arch_init_net (adapter start, DHCP)");
#ifdef DK_NET_TRACE
        trace_report();
#endif
        stack_up = true;
    }
    netif_t *n = net_default_dev;
    if(n && strcmp(n->name, "ppp")) {
        if(!has_address(n->ip_addr)) {
            failure = "NO IP ADDRESS (CHECK DHCP)";
            goto failed;
        }
        modem_link = false;
        snprintf(status_buf, sizeof(status_buf), "%s %d.%d.%d.%d", strcmp(n->name, "w5500") ? "BROADBAND" : "W5500",
                 n->ip_addr[0], n->ip_addr[1], n->ip_addr[2], n->ip_addr[3]);
    }
    else {
        if(!want_dial) {
            /* Started early by link_prepare: dialling waits for link_start. */
            NET_TRACE("no Ethernet adapter, dialling waits for the player");
            stage = LINK_OFF;
            worker_running = 0;
            return NULL;
        }
        modem_link = true;
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
    NET_TRACE(server_ip ? "DNS lookup" : "DNS lookup failed");
    if(!server_ip) server_ip = inet_addr(DK_NET_FALLBACK_IP);
    stage = LINK_UP;
    worker_running = 0;
    return NULL;

failed:
    stage = LINK_FAILED;
    worker_running = 0;
    return NULL;
}

static void start_worker(void) {
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

void link_prepare(void) {
    if(worker_running || stage != LINK_OFF) return;
    start_worker();
}

void link_start(void) {
#ifdef DK_NET_TRACE
    if(!want_dial) trace_player_ms = timer_ms_gettime64() - trace_t0;
#endif
    want_dial = 1;
    if(worker_running || stage == LINK_UP) return;
    start_worker();
}

int link_stage(void) {
    /* An early start found no Ethernet adapter after link_start was called. */
    if(want_dial && stage == LINK_OFF && !worker_running) start_worker();
    if(stage == LINK_UP && modem_link && dialup_dropped()) {
        failure = "DIAL-UP CONNECTION DROPPED";
        stage = LINK_FAILED;
    }
    return stage;
}

const char *link_status(void) {
    switch(shown_stage()) {
        case LINK_STARTING: return "LOOKING FOR A NETWORK ADAPTER";
        case LINK_DIALING: return "DIALING MODEM";
        case LINK_NEGOTIATING: return "CONNECTING (PPP)";
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
    if(modem_link) dialup_hang_up();
    want_dial = 0;
    stage = LINK_OFF;
}
