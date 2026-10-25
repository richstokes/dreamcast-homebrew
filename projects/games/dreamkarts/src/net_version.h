/* DreamKarts online versions: the single source of truth shared by the
   console and the lobby server (server/version_test.go checks the server
   matches).

   DK_NET_PROTO      wire format (server/PROTOCOL.md). Bump on any layout
                     change; consoles on another protocol are rejected.
   DK_NET_BUILD      bump for every release that changes online behaviour
                     (physics, tracks, menus that affect races). The server
                     tells older consoles a newer version exists.
   DK_NET_MIN_BUILD  the oldest build the server still lets play. Raise it
                     to DK_NET_BUILD when old consoles would desync.

   After changing any of these, redeploy the server
   (server/deploy/deploy.sh; see server/README.md). */
#ifndef DK_NET_VERSION_H
#define DK_NET_VERSION_H

#define DK_NET_PROTO      4
#define DK_NET_BUILD      25
#define DK_NET_MIN_BUILD  24

#endif
