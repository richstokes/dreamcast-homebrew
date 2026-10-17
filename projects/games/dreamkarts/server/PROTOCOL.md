# DreamKarts online protocol

One UDP socket per console talks to one lobby server (default `dcvmu.com`,
port 7535). Everything fits a 56k dial-up modem: the console sends about
1 KB/s while racing and receives under 2.5 KB/s with six karts.

## Principles

- **State, not events.** The server resends the whole lobby state four
  times a second, and the console resends its wishes (colour, track, laps,
  speed, start) four times a second until the state reflects them. A lost
  packet costs a quarter of a second, never a desync, so there is no ack
  or retransmit layer.
- **Each console drives its own kart.** Consoles send their kart state at
  12 Hz; the server relays everyone's latest state at 12 Hz; consoles
  interpolate remote karts about 100 ms in the past. Finish times are
  measured by each console from its own green light and repeated in every
  kart packet until the results arrive.
- **Bytes, not structs.** All fields are packed explicitly, little-endian,
  with no padding (the SH-4 faults on unaligned loads). Strings are fixed
  width, ASCII `A-Z 0-9 space - .`, NUL padded.
- **Versions.** `DK_NET_BUILD` in `src/net_version.h` numbers every client
  release that changes online behaviour; `DK_NET_MIN_BUILD` is the oldest
  build the server still accepts. The server is built from the same header,
  so its "latest" build always matches the client in the repository.

## Header (all packets, 4 bytes)

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8[2] | magic `'D' 'K'` |
| 2 | u8 | protocol (`DK_NET_PROTO`, currently 1) |
| 3 | u8 | packet type |

A packet with a bad magic or a different protocol number gets a `REJECT`
(if it is a `HELLO`) or is dropped.

## Client to server

Every client packet continues with this 14-byte block after the header:

| Offset | Type | Field |
| --- | --- | --- |
| 4 | u32 | session token (random, chosen by the console at connect) |
| 8 | u32 | echo: the newest `server_ms` the console has received |
| 12 | u16 | hold: ms since that server packet arrived (for ping) |
| 14 | u16 | client build (`DK_NET_BUILD`) |

The server identifies players by token, not address, so a modem that
redials keeps its seat if it comes back within the timeout.

| Type | Name | Body (from offset 18) | Sent |
| --- | --- | --- | --- |
| 1 | `HELLO` | name `char[11]` | until `WELCOME`, every 500 ms |
| 2 | `LIST` | nothing | 2 Hz on the lobby browser |
| 3 | `JOIN` | u8 lobby (0-9) | until the lobby state names this player |
| 4 | `LEAVE` | nothing | 3 times |
| 5 | `WISH` | u8 colour, u8 track, u8 laps, u8 cc, u8 start nonce | 4 Hz in a lobby |
| 6 | `KART` | see below | 12 Hz in a race |
| 7 | `PING` | nothing | 1 Hz otherwise idle (keeps NAT open) |

`WISH`: everyone's colour applies (if free); track, laps (3, 5 or 10) and
cc apply only from the leader. When the leader presses Start the console
increments the start nonce; the server starts a race when it sees a new
nonce from the leader and at least two players are in the lobby.

`KART` (body 26 bytes, 44 in all):

| Offset | Type | Field |
| --- | --- | --- |
| 18 | u8 | race id (ignored unless it matches the current race) |
| 19 | u8 | flags: bit0 finished, bit1 grounded, bit2 boosting, bit3 drifting, bit4 drift right |
| 20 | i16 | x * 16 (m) |
| 22 | i16 | y * 64 |
| 24 | i16 | z * 16 |
| 26 | u16 | heading * 65536 / 2pi |
| 28 | i16 | speed * 64 (m/s) |
| 30 | i8 | steer * 127 |
| 31 | u8 | spark stage 0-3 |
| 32 | u8 | lap (0 = before the line) |
| 33 | u8 | reserved |
| 34 | u16 | lap distance * 16 (m) |
| 36 | u32 | race time in ms (finish time once finished) |
| 40 | u32 | client clock ms (for interpolation) |

## Server to client

Every server packet continues with:

| Offset | Type | Field |
| --- | --- | --- |
| 4 | u32 | server_ms (monotonic, echoed by the console) |

| Type | Name | Body (from offset 8) |
| --- | --- | --- |
| 64 | `WELCOME` | u8 status, u16 latest build, u16 min build, u8 lobby count, u8 max players |
| 65 | `REJECT` | u8 reason, u16 latest build, u16 min build |
| 66 | `LOBBIES` | lobby count x (u8 players, u8 state, u8 track, char[11] leader) |
| 67 | `LOBBY` | see below |
| 68 | `RACE` | see below |

`WELCOME` status: 0 up to date, 1 a newer build exists (playable). `REJECT`
reasons: 1 client too old (below min build), 2 server full, 3 lobby full,
4 bad name, 5 unknown session (send `HELLO` again). A console below the min build shows "please download the
latest version" and never gets further.

`LOBBY` (sent 4 Hz to members, at once after any change):

| Offset | Type | Field |
| --- | --- | --- |
| 8 | u8 | lobby id |
| 9 | u8 | state: 0 waiting, 1 countdown, 2 racing, 3 results |
| 10 | u8 | your slot |
| 11 | u8 | leader slot |
| 12 | u8 | track |
| 13 | u8 | laps |
| 14 | u8 | cc |
| 15 | u8 | race id |
| 16 | i32 | ms until the green light (countdown) or since it (racing) |
| 20 | u8 | start nonce the server last accepted from the leader |
| 21 | u8 | player count n |
| 22 | n x 20 bytes | players |

Player entry (20 bytes): u8 slot, char[11] name, u8 colour, u8 flags (bit0
leader, bit1 racing, bit2 finished), u16 ping ms, u16 points, u8 last place
(0 none), u8 grid position.

`RACE` (12 Hz during countdown, race and results): u8 race id, u8 count,
then per racer: u8 slot, u16 age ms (how old the state is at send time),
then the 21 bytes of the `KART` body at offsets 19-39 (flags through race
time), 24 bytes per racer.

## Race lifecycle

1. **Waiting:** up to six players. The leader is the earliest joiner still
   present; when the leader leaves, the next earliest takes over.
2. **Countdown:** on a valid start the server fixes the racers (everyone in
   the lobby), their grid order (points, then join order) and a race id,
   and counts 4 seconds to green.
3. **Racing:** ends when every racer has finished, 30 seconds after the
   first finisher, or when fewer than one racer is still connected.
4. **Results:** places by finish time, then unfinished racers by distance
   covered. Points: 10, 8, 6, 4, 2, 1 for places 1-6. Points accumulate per
   player for as long as they stay in the lobby. Results show 8 seconds,
   then the lobby is waiting again.

Players who join mid-race wait in the lobby until the next race. A player
silent for 10 seconds is dropped.
