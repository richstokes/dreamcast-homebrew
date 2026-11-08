/* Network link for online games: the Broadband/LAN adapter, a W5500 on
   the serial port, or the modem dialling a dial-up server, brought up on a
   worker thread so the game keeps drawing. Then the game server's address
   is looked up. Used by DreamKarts and Ridgeline.

   The game leaves INIT_NET out of KOS_INIT_FLAGS, so a normal boot never
   probes the network; this starts KOS's network stack the first time a
   player goes online. */
#ifndef COMMON_NETLINK_H
#define COMMON_NETLINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NETLINK_OFF,
    NETLINK_STARTING,
    NETLINK_DIALING,
    NETLINK_NEGOTIATING,
    NETLINK_RESOLVING,
    NETLINK_UP,
    NETLINK_FAILED
};

/* The game server: a host name to look up, and the address to use when
   the lookup fails (dotted IPv4). Call before netlink_prepare.

   NETLINK_TRACE (from the game's engine_config.h) prints how long each
   step of bringing the link up took; never on dcload-ip hardware builds,
   where every printf stalls the console. */
void netlink_set_server(const char *host, const char *fallback_ip);
/* Start bringing up a Broadband or LAN adapter in the background, without
   dialling: called as the player enters the online screens, so the adapter
   and DHCP (several seconds) are usually done by the time a name is typed. */
void netlink_prepare(void);
/* Start (or retry) bringing the link up, dialling with the modem if there is
   no Ethernet adapter. Safe to call every frame. */
void netlink_start(void);
int netlink_stage(void);
/* What is happening, or why it failed, in upper case for a game's font. */
const char *netlink_status(void);
bool netlink_is_modem(void);
/* The server's IPv4 address in network byte order (once NETLINK_UP). */
uint32_t netlink_server_ip(void);
/* Hang up the modem; the next netlink_start brings the link up again. */
void netlink_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
