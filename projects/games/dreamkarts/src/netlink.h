/* DreamKarts network link: Broadband/LAN adapter or the modem to a DreamPi,
   brought up on a worker thread when the player goes online. */
#ifndef DK_NETLINK_H
#define DK_NETLINK_H

#include "dk.h"

/* The lobby server. Test builds can point at another one with
   EXTRA_CFLAGS=-DDK_NET_TEST_HOST=192.168.1.20 (an address or host name). */
#define DK_NET_STR2(x) #x
#define DK_NET_STR(x) DK_NET_STR2(x)
#ifdef DK_NET_TEST_HOST
#define DK_NET_HOST DK_NET_STR(DK_NET_TEST_HOST)
#endif
#ifndef DK_NET_HOST
#define DK_NET_HOST "dcvmu.com"
#endif
#ifndef DK_NET_FALLBACK_IP
#define DK_NET_FALLBACK_IP "143.110.224.215"   /* dcvmu.com, if DNS fails */
#endif
#ifndef DK_NET_PORT
#define DK_NET_PORT 7535
#endif

enum { LINK_OFF, LINK_STARTING, LINK_DIALING, LINK_NEGOTIATING, LINK_RESOLVING, LINK_UP, LINK_FAILED };

/* Start (or retry) bringing the link up. Safe to call every frame. */
void link_start(void);
int link_stage(void);
/* What is happening, or why it failed, in upper case for the HUD font. */
const char *link_status(void);
bool link_is_modem(void);
/* The lobby server's IPv4 address in network byte order (once LINK_UP). */
uint32_t link_server_ip(void);
void link_shutdown(void);

#endif
