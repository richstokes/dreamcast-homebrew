/* Bringing up the Broadband Adapter (or the 10 Mbit LAN adapter) at runtime. */
#ifndef LEMONPLAY_NETLINK_H
#define LEMONPLAY_NETLINK_H

#include <stdint.h>

enum { LINK_STARTING, LINK_UP, LINK_FAILED };

void link_start(void);
int link_stage(void);
/* "BROADBAND 192.168.1.20", or why it failed. */
const char *link_status(void);
/* Our address in network byte order, 0 if none. */
uint32_t link_address(void);
/* The subnet's broadcast address in network byte order. */
uint32_t link_broadcast(void);

#endif
