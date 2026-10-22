/* DLNA/UPnP media server discovery and ContentDirectory browsing. All calls
   block on the network. */
#ifndef LEMONPLAY_UPNP_H
#define LEMONPLAY_UPNP_H

#include <stdint.h>

#define MAX_SERVERS 8
#define MAX_ENTRIES 800

typedef struct {
    char name[64];          /* friendlyName, ISO-8859-1 */
    char location[256];     /* description.xml URL */
    char control[256];      /* ContentDirectory control URL */
} server_t;

enum { ENTRY_FOLDER, ENTRY_VIDEO, ENTRY_AUDIO, ENTRY_IMAGE, ENTRY_OTHER };

typedef struct {
    char id[64];
    char title[120];        /* ISO-8859-1 */
    char url[400];          /* the Dreamcast transcode, or the first <res> */
    int32_t duration;       /* seconds, -1 if unknown */
    uint8_t kind;
    uint8_t playable;       /* url is a ZeroConfigDLNA --dreamcast-mode transcode */
} entry_t;

/* Asks the LAN for media servers for about timeout_ms. Returns how many
   were found (and described) into out. */
int upnp_discover(server_t *out, int max, int timeout_ms);
/* Reads a server's description.xml. Returns 0 on success. */
int upnp_describe(const char *location, server_t *out);
/* Lists a container's children, folders first, then by title. Returns the
   count, or -1 on error. */
int upnp_browse(const server_t *server, const char *object_id, entry_t *out, int max);

#endif
