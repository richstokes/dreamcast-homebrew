/* Network test. Entering the screen registers KOS's Ethernet drivers, which
   probe for the Broadband Adapter, LAN Adapter and a WIZnet W5500 on the
   serial port; with none of those it looks for the modem. A then brings
   the link up (DHCP, or dial-up through projects/common/dialup.c) and runs
   the connectivity tests: pings, DNS, NTP over UDP and HTTP over TCP.

   The boot leaves INIT_NET off, so nothing touches the network unless this
   screen is opened. Everything that blocks runs on a worker thread. */
#include "gfx.h"
#include "kt.h"
#include "../../../common/dialup.h"

#include <arpa/inet.h>
#include <dc/fs_dcload.h>
#include <dc/modem/modem.h>
#include <dc/net/broadband_adapter.h>
#include <dc/net/lan_adapter.h>
#include <dc/net/w5500_adapter.h>
#include <errno.h>
#include <fcntl.h>
#include <kos/mutex.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

extern void arch_init_net(void);

enum { ST_IDLE, ST_DETECTING, ST_READY, ST_CONNECTING, ST_TESTING, ST_DONE, ST_FAILED };

static volatile int state = ST_IDLE;
static volatile bool worker_busy;
static kthread_t *worker;
static mutex_t lock = MUTEX_INITIALIZER;
static kt_report_t net_report;
static char message[80];

/* Detection results */
static bool drivers_registered, stack_up;
static int found_bba = -1, found_la = -1, found_w5500 = -1, found_modem = -1;
static bool modem_link;

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;

    mutex_lock(&lock);
    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);
    mutex_unlock(&lock);
}

#define ADD(st, name, ...) do { \
        mutex_lock(&lock); \
        report_add(&net_report, st, name, __VA_ARGS__); \
        mutex_unlock(&lock); \
    } while(0)

static bool has_address(const uint8_t a[4]) {
    return a[0] | a[1] | a[2] | a[3];
}

/* ---- Detection ---- */

static void *detect_worker(void *unused) {
    if(!drivers_registered) {
        say("Probing for network adapters...");
        /* Each init registers its adapter only when it finds one. */
        found_bba = bba_init() == 0;
        found_la = la_init() == 0;
        found_w5500 = w5500_adapter_init(NULL, true) == 0;
        drivers_registered = true;
    }
    /* The modem shares the expansion port with the BBA: never probe it
       while an Ethernet adapter is there. */
    if(found_bba || found_la || found_w5500)
        found_modem = -1;
    else if(found_modem < 0) {
        say("No Ethernet adapter. Looking for the modem...");
        found_modem = modem_init() ? 1 : 0;
        if(found_modem)
            modem_shutdown();
    }
    say(found_bba || found_la || found_w5500 ? "Ready: A connects with DHCP and runs the tests." :
        found_modem == 1 ? "Ready: A dials up (DreamPi or a PPP server) and runs the tests." :
        "No network hardware found.");
    state = ST_READY;
    worker_busy = false;
    return NULL;
}

/* ---- Tests ---- */

static volatile int echo_got;
static volatile uint16_t echo_want;
static volatile uint32_t echo_us;
static volatile uint8_t echo_ttl;
static uint16_t echo_seq;

static void on_echo(const uint8_t *ip, uint16_t seq, uint64_t delta_us, uint8_t ttl,
                    const uint8_t *data, size_t len) {
    if(seq == echo_want) {
        echo_us = (uint32_t)delta_us;
        echo_ttl = ttl;
        echo_got = 1;
    }
}

static bool ping_once(const uint8_t ip[4], uint32_t *us) {
    static const uint8_t payload[32] = "KOS_Test ping";

    echo_got = 0;
    echo_want = ++echo_seq;
    if(net_icmp_send_echo(net_default_dev, ip, 0x4b54, echo_seq, payload, sizeof(payload)) < 0)
        return false;
    for(int i = 0; i < 200 && !echo_got; i++)
        thd_sleep(10);
    *us = echo_us;
    return echo_got;
}

static void ping_test(const char *name, const uint8_t ip[4], int count) {
    uint32_t us, min = UINT32_MAX, max = 0, sum = 0;
    int got = 0;

    for(int i = 0; i < count; i++) {
        if(ping_once(ip, &us)) {
            got++;
            sum += us;
            if(us < min) min = us;
            if(us > max) max = us;
        }
        thd_sleep(100);
    }
    if(got)
        ADD(got == count ? KT_PASS : KT_FAIL, name, "%d.%d.%d.%d %d/%d, %.1f/%.1f/%.1f ms, TTL %u",
            ip[0], ip[1], ip[2], ip[3], got, count, min / 1000.0, sum / got / 1000.0,
            max / 1000.0, echo_ttl);
    else
        ADD(KT_FAIL, name, "%d.%d.%d.%d no replies", ip[0], ip[1], ip[2], ip[3]);
}

static bool resolve(const char *host, uint8_t ip[4], uint32_t *ms) {
    struct addrinfo hints, *res = NULL;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    uint64_t t = timer_ms_gettime64();
    int rv = getaddrinfo(host, NULL, &hints, &res);
    *ms = (uint32_t)(timer_ms_gettime64() - t);
    if(rv != 0 || !res)
        return false;
    memcpy(ip, &((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr, 4);
    freeaddrinfo(res);
    return true;
}

static int wait_fd(int fd, short events, int ms) {
    struct pollfd p = { fd, events, 0 };
    return poll(&p, 1, ms);
}

static void http_test(const uint8_t ip[4]) {
    struct sockaddr_in a;
    char buf[256];

    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    memcpy(&a.sin_addr.s_addr, ip, 4);
    uint64_t t = timer_ms_gettime64();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        ADD(KT_FAIL, "TCP connect", "no socket");
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    if((connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0 && errno != EINPROGRESS) ||
       wait_fd(fd, POLLOUT, 8000) <= 0) {
        ADD(KT_FAIL, "TCP connect", "google.com:80 timed out");
        close(fd);
        return;
    }
    ADD(KT_PASS, "TCP connect", "google.com:80 in %lu ms", (unsigned long)(timer_ms_gettime64() - t));
    static const char req[] = "HEAD / HTTP/1.0\r\nHost: google.com\r\nUser-Agent: KOS_Test\r\n\r\n";
    send(fd, req, sizeof(req) - 1, 0);
    int n = 0;
    while(n < (int)sizeof(buf) - 1 && wait_fd(fd, POLLIN, 8000) > 0) {
        int r = recv(fd, buf + n, sizeof(buf) - 1 - n, 0);
        if(r <= 0)
            break;
        n += r;
        if(memchr(buf, '\n', n))
            break;
    }
    close(fd);
    buf[n] = 0;
    char *nl = strpbrk(buf, "\r\n");
    if(nl) *nl = 0;
    if(!strncmp(buf, "HTTP/", 5))
        ADD(KT_PASS, "HTTP request", "%.44s, %lu ms", buf, (unsigned long)(timer_ms_gettime64() - t));
    else
        ADD(KT_FAIL, "HTTP request", n ? "odd reply: %.30s" : "no reply%s", buf);
}

static void ntp_test(void) {
    static const uint8_t fallback[4] = { 216, 239, 35, 0 };     /* time.google.com */
    uint8_t ip[4], pkt[48];
    uint32_t ms;
    struct sockaddr_in a;

    bool named = resolve("time.google.com", ip, &ms);
    if(!named)
        memcpy(ip, fallback, 4);
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0) {
        ADD(KT_FAIL, "NTP (UDP)", "no socket");
        return;
    }
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(123);
    memcpy(&a.sin_addr.s_addr, ip, 4);
    uint64_t t = timer_ms_gettime64();
    int n = -1;
    for(int tries = 0; tries < 3 && n < 48; tries++) {
        memset(pkt, 0, sizeof(pkt));
        pkt[0] = 0x1b;      /* version 3, client */
        t = timer_ms_gettime64();
        sendto(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&a, sizeof(a));
        n = wait_fd(fd, POLLIN, 2000) > 0 ? recv(fd, pkt, sizeof(pkt), 0) : -1;
    }
    close(fd);
    if(n < 48) {
        ADD(KT_FAIL, "NTP (UDP)", "no reply from %d.%d.%d.%d%s", ip[0], ip[1], ip[2], ip[3],
            named ? "" : " (DNS failed)");
        return;
    }
    uint32_t secs = (uint32_t)pkt[40] << 24 | pkt[41] << 16 | pkt[42] << 8 | pkt[43];
    time_t now = (time_t)(secs - 2208988800u);
    struct tm tm;
    gmtime_r(&now, &tm);
    int mode = pkt[0] & 7, stratum = pkt[1];
    if(mode != 4 || stratum < 1 || stratum > 15 || tm.tm_year + 1900 < 2024) {
        ADD(KT_FAIL, "NTP (UDP)", "bad reply: mode %d, stratum %d, year %d", mode, stratum,
            tm.tm_year + 1900);
        return;
    }
    /* The console clock holds local time, so this is its time zone. */
    long off = (long)(rtc_unix_secs() - now);
    ADD(KT_PASS, "NTP (UDP)", "%02d:%02d:%02d UTC, stratum %d, %lu ms, RTC %+ld:%02ld", tm.tm_hour,
        tm.tm_min, tm.tm_sec, stratum, (unsigned long)(timer_ms_gettime64() - t), off / 3600,
        labs(off) / 60 % 60);
}

static void *test_worker(void *unused) {
    netif_t *n;

    mutex_lock(&lock);
    report_begin(&net_report, "NETWORK TEST");
    mutex_unlock(&lock);

    if(!stack_up && (found_bba || found_la || found_w5500)) {
        say("Starting the network stack and asking for a DHCP lease...");
        /* Also hands dcload-ip's console over when that loaded us. */
        uint64_t t = timer_ms_gettime64();
        arch_init_net();
        stack_up = true;
        n = net_default_dev;
        ADD(n && has_address(n->ip_addr) ? KT_PASS : KT_FAIL, "DHCP", "%s in %lu ms",
            n && has_address(n->ip_addr) ? "lease" : "no address", (unsigned long)(timer_ms_gettime64() - t));
    }
    else if(!stack_up && found_modem == 1) {
        say("Dialing... (up to a minute)");
        arch_init_net();    /* sockets and TCP/IP, for PPP to join */
        stack_up = true;
        uint64_t t = timer_ms_gettime64();
        dialup_error_t e = dialup_connect(NULL, DIALUP_REQUIRE_DNS, NULL);
        modem_link = e == DIALUP_OK;
        ADD(modem_link ? KT_PASS : KT_FAIL, "Dial-up", modem_link ? "%d bps in %lu ms" :
            "failed (error %d)", modem_link ? dialup_rate() : (int)e,
            (unsigned long)(timer_ms_gettime64() - t));
    }
    n = net_default_dev;
    if(!n || !has_address(n->ip_addr)) {
        say("No link: check the cable, DHCP server or dial-up settings.");
        state = ST_FAILED;
        worker_busy = false;
        return NULL;
    }
    state = ST_TESTING;
    ADD(KT_INFO, "Interface", "%s: %s", n->name, n->descr ? n->descr : "");
    ADD(KT_INFO, "Address", "%d.%d.%d.%d mask %d.%d.%d.%d", n->ip_addr[0], n->ip_addr[1],
        n->ip_addr[2], n->ip_addr[3], n->netmask[0], n->netmask[1], n->netmask[2], n->netmask[3]);
    ADD(KT_INFO, "Gateway / DNS", "%d.%d.%d.%d / %d.%d.%d.%d", n->gateway[0], n->gateway[1],
        n->gateway[2], n->gateway[3], n->dns[0], n->dns[1], n->dns[2], n->dns[3]);

    net_echo_cb old = net_icmp_echo_cb;
    net_icmp_echo_cb = on_echo;
    say("Pinging...");
    if(has_address(n->gateway))
        ping_test("Ping gateway", n->gateway, 3);
    static const uint8_t google_dns[4] = { 8, 8, 8, 8 };
    ping_test("Ping 8.8.8.8", google_dns, 4);

    say("Looking up google.com...");
    uint8_t gip[4];
    uint32_t ms;
    bool dns = resolve("google.com", gip, &ms);
    if(dns)
        ADD(KT_PASS, "DNS lookup", "google.com = %d.%d.%d.%d in %lu ms", gip[0], gip[1], gip[2],
            gip[3], (unsigned long)ms);
    else
        ADD(KT_FAIL, "DNS lookup", "google.com did not resolve");
    if(dns) {
        ping_test("Ping google.com", gip, 3);
        say("Connecting to google.com:80...");
        http_test(gip);
    }
    say("Asking time.google.com for the time...");
    ntp_test();
    net_icmp_echo_cb = old;

    net_ipv4_stats_t s4 = net_ipv4_get_stats();
    ADD(KT_INFO, "IPv4 packets", "%lu sent, %lu received", (unsigned long)s4.pkt_sent,
        (unsigned long)s4.pkt_recv);
    mutex_lock(&lock);
    report_end(&net_report);
    mutex_unlock(&lock);
    say(net_report.fail ? "Done, with failures. X runs the tests again." :
        "Done: the internet is reachable. X runs the tests again.");
    state = ST_DONE;
    worker_busy = false;
    return NULL;
}

static void start_worker(void *(*fn)(void *)) {
    if(worker_busy)
        return;
    if(worker)
        thd_join(worker, NULL);
    worker_busy = true;
    worker = thd_create(false, fn, NULL);
    if(!worker) {
        worker_busy = false;
        say("Could not start a worker thread.");
    }
}

/* ---- Screen ---- */

static void net_enter(void) {
    if(state == ST_IDLE || state == ST_READY) {
        state = ST_DETECTING;
        start_worker(detect_worker);
    }
}

static void net_update(const kt_input_t *in) {
    if(in->pressed & (CONT_B | CONT_START)) {
        if(worker_busy)
            say("Waiting for the current step to finish...");
        else
            kt_pop();
        return;
    }
    if(worker_busy)
        return;
    bool can = state == ST_READY || state == ST_DONE || state == ST_FAILED;
    bool hw = found_bba == 1 || found_la == 1 || found_w5500 == 1 || found_modem == 1;
    if(can && hw && (in->pressed & (CONT_A | CONT_X))) {
        state = ST_CONNECTING;
        start_worker(test_worker);
    }
}

static const char *found_text(int f) {
    return f == 1 ? "FOUND" : f == 0 ? "NOT FOUND" : "NOT PROBED";
}

static void net_draw(void) {
    netif_t *n = net_default_dev;

    ui_background();
    ui_header("NETWORK TEST", dcload_type == DCLOAD_TYPE_IP ? "LOADED BY DCLOAD-IP" : NULL);

    ui_panel(8, 46, 300, 130, "DEVICES");
    static const char *const names[4] = { "Broadband Adapter", "LAN Adapter", "W5500 (serial)", "Modem" };
    int f[4] = { found_bba, found_la, found_w5500, found_modem };
    for(int i = 0; i < 4; i++) {
        gfx_text(16, 70 + i * 18, C_TEXT, names[i]);
        gfx_text(184, 70 + i * 18, f[i] == 1 ? C_PASS : C_DIM, state == ST_DETECTING && f[i] < 0 && i < 3 ?
                 "..." : found_text(f[i]));
    }
    if(found_bba == 1 && n)
        gfx_textf(16, 148, C_DIM, "MAC %02X:%02X:%02X:%02X:%02X:%02X", n->mac_addr[0], n->mac_addr[1],
                  n->mac_addr[2], n->mac_addr[3], n->mac_addr[4], n->mac_addr[5]);

    ui_panel(316, 46, 316, 130, "LINK");
    if(n && has_address(n->ip_addr)) {
        gfx_textf(324, 70, C_TEXT, "%s  %s", n->name, modem_link ? "DIAL-UP" : "ETHERNET");
        gfx_textf(324, 88, C_TEXT, "IP      %d.%d.%d.%d", n->ip_addr[0], n->ip_addr[1], n->ip_addr[2], n->ip_addr[3]);
        gfx_textf(324, 106, C_TEXT, "GATEWAY %d.%d.%d.%d", n->gateway[0], n->gateway[1], n->gateway[2], n->gateway[3]);
        gfx_textf(324, 124, C_TEXT, "DNS     %d.%d.%d.%d", n->dns[0], n->dns[1], n->dns[2], n->dns[3]);
        gfx_textf(324, 142, C_TEXT, "MTU     %d", n->mtu);
    }
    else
        gfx_text(324, 70, C_DIM, stack_up ? "No address" : "Not connected");

    ui_panel(8, 184, 624, 236, "TESTS");
    mutex_lock(&lock);
    int first = net_report.count > 11 ? net_report.count - 11 : 0;
    for(int i = first; i < net_report.count; i++) {
        const kt_line_t *l = &net_report.line[i];
        float y = 208 + (i - first) * 18;
        gfx_text(16, y, status_color(l->status), status_name(l->status));
        gfx_text_clip(60, y, C_TEXT, l->name, 18);
        gfx_text_clip(212, y, l->status == KT_FAIL ? C_FAIL : C_DIM, l->detail, 52);
    }
    gfx_text_clip(12, 428, worker_busy ? C_HI : C_TEXT, message, 77);
    mutex_unlock(&lock);
    ui_footer(state == ST_DONE || state == ST_FAILED ? "X RUN AGAIN   B BACK" :
              "A CONNECT AND TEST   B BACK");
}

const kt_screen_t net_screen = {
    "Network test", net_enter, net_update, net_draw, NULL
};

#ifdef KT_AUTOTEST
/* Detect, connect, run the tests; finished once they are done or failed. */
int net_autotest(int frame) {
    if(frame == 1)
        kt_push(&net_screen);
    if(state == ST_READY && !worker_busy && frame > 30) {
        if(!(found_bba == 1 || found_la == 1 || found_w5500 == 1 || found_modem == 1)) {
            KT_LOG("Network: no hardware\n");
            return 1;
        }
        state = ST_CONNECTING;
        start_worker(test_worker);
    }
    if((state == ST_DONE || state == ST_FAILED) && !worker_busy) {
        KT_LOG("Network: %s\n", message);
        return frame > 60;
    }
    return frame > 60 * 120;
}
#endif
