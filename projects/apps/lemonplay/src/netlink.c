/* LemonPlay boots without INIT_NET, so a missing adapter never stalls the
   boot, and starts the network from a worker thread instead (as DreamKarts
   does). arch_init_net also takes the adapter over from dcload-ip when the
   program was loaded that way. The modem is not supported: 56k is far too
   slow for video. */
#include "netlink.h"

#include <dc/net/broadband_adapter.h>
#include <dc/net/lan_adapter.h>
#include <kos.h>
#include <stdio.h>
#include <string.h>

extern void arch_init_net(void);

static volatile int stage = LINK_STARTING;
static char status[48] = "STARTING THE NETWORK";

static int has_address(const uint8_t a[4]) {
    return a[0] | a[1] | a[2] | a[3];
}

static void *link_worker(void *unused) {
    (void)unused;
    bba_init();
    la_init();
    arch_init_net();
    netif_t *n = net_default_dev;
    if(!n || !strcmp(n->name, "ppp")) {
        snprintf(status, sizeof(status), "NO BROADBAND ADAPTER FOUND");
        stage = LINK_FAILED;
        return NULL;
    }
    if(!has_address(n->ip_addr)) {
        snprintf(status, sizeof(status), "NO IP ADDRESS (CHECK DHCP)");
        stage = LINK_FAILED;
        return NULL;
    }
    snprintf(status, sizeof(status), "%s %d.%d.%d.%d", strcmp(n->name, "bba") ? "LAN ADAPTER" : "BROADBAND",
             n->ip_addr[0], n->ip_addr[1], n->ip_addr[2], n->ip_addr[3]);
    printf("LemonPlay: %s\n", status);
    stage = LINK_UP;
    return NULL;
}

void link_start(void) {
    thd_create(1, link_worker, NULL);
}

int link_stage(void) {
    return stage;
}

const char *link_status(void) {
    return status;
}

static uint32_t pack(const uint8_t a[4]) {
    uint32_t v;
    memcpy(&v, a, 4);
    return v;
}

uint32_t link_address(void) {
    netif_t *n = net_default_dev;
    return stage == LINK_UP && n ? pack(n->ip_addr) : 0;
}

uint32_t link_broadcast(void) {
    netif_t *n = net_default_dev;
    if(stage != LINK_UP || !n) return 0xffffffffu;
    if(has_address(n->broadcast)) return pack(n->broadcast);
    return pack(n->ip_addr) | ~pack(n->netmask);
}
