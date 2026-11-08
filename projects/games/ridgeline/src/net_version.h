/* Ridgeline online versions: the single source of truth shared by the
   console and the server (server/lobby/version_test.go checks the server
   matches).

   RL_NET_PROTO      wire format (server/PROTOCOL.md). Bump on any layout
                     change; consoles on another protocol are rejected.
   RL_NET_BUILD      bump for every release that changes online behaviour
                     (truck physics, the level, what is sent). The server
                     tells older consoles a newer version exists.
   RL_NET_MIN_BUILD  the oldest build the server still lets play. Raise it
                     to RL_NET_BUILD when old consoles would disagree with
                     new ones about the mountain.

   After changing any of these, redeploy the server
   (server/deploy/deploy.sh; see server/README.md). */
#ifndef RL_NET_VERSION_H
#define RL_NET_VERSION_H

#define RL_NET_PROTO      1
#define RL_NET_BUILD      7
#define RL_NET_MIN_BUILD  1

#endif
