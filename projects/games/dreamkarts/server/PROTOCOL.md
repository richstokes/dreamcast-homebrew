# DreamKarts online protocol

One UDP socket per console talks to one lobby server (default `dcvmu.com`,
port 7535). Everything fits a 56k dial-up modem: the console sends about
1 KB/s while racing and receives under 3 KB/s with six karts.

## Principles

- **State, not events.** The server resends the whole lobby state four
  times a second, and the console resends its wishes (colour, track, laps,
  speed, start) four times a second until the state reflects them. A lost
  packet costs a quarter of a second, never a desync, so there is no ack
  or retransmit layer.
- **Each console drives its own kart.** The server relays everyone's
  latest kart state 12 times a second on a steady beat (every 83 ms, from
  its own timer); each console sends its kart once per beat, timed from
  the `RACE` packets it receives so the packet reaches the server shortly
  before the next relay. Consoles draw remote karts a little in the past
  (adaptive, about 110-150 ms on broadband) interpolated between the
  samples either side, spacing the samples by the sender's race clock in
  the packet rather than by arrival time. Finish times are measured by
  each console from its own green light and repeated in every kart packet
  until the results arrive.
- **Power-ups are state too.** A kart packet carries what its kart holds,
  whether its barrier is up or it is spun out, its spear in flight
  (sequence number, position, target) and its mine on the road (sequence
  number, position). Every console runs its own copy of each spear and
  mine from those packets and judges hits on its own kart only: a hit
  shows up as that kart's spun-out (or barrier-popped) state in its next
  packet, and the owner's console drops the spear or mine when its own
  copy arrives or that state shows beside it; a mine whose number leaves
  the owner's packets leaves every console. Which emerald slot is lit
  comes from a seed every console derives from the lobby, race id and
  track, so nothing about the emeralds themselves is sent.
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
| 2 | u8 | protocol (`DK_NET_PROTO`, currently 4) |
| 3 | u8 | packet type |

A packet with a bad magic or a different protocol number gets a `REJECT`
(if it is a `HELLO`) or is dropped. The `REJECT` carries the server's
protocol byte; consoles from build 18 read a `REJECT` whatever its
protocol byte (earlier builds dropped it and reported a lost connection
after a protocol bump). The server logs every rejected `HELLO`, at most
once a minute per address and reason.

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
| 3 | `JOIN` | u8 lobby (0-9), u8 flags (bit0 spectate; build 4) | until the lobby state names this player (or shows it watching) |
| 4 | `LEAVE` | nothing | 3 times |
| 5 | `WISH` | u8 colour, u8 track, u8 laps, u8 cc, u8 start nonce, u8 flags (bit0 power-ups on; build 11), u8[3] custom kart picks (body, trim, Chao palette indices; build 17) | 4 Hz in a lobby |
| 6 | `KART` | see below | once per `RACE` relay in a race (12 Hz) |
| 7 | `PING` | nothing | 1 Hz otherwise idle (keeps NAT open) |
| 8 | `CHAT` | u8 message id, text `char[33]` (build 14) | every 500 ms until the `CHAT` echo names this id, for up to 3 s |
| 9 | `RETIRE` | u8 race id (build 16) | every 250 ms until the lobby state shows this player not racing |

A `JOIN` with the spectate flag makes the console a watcher: it gets the
lobby's `LOBBY` and `RACE` packets but has no slot, kart or say. Any number
can watch a lobby, full or not (the demo lobby included); a watcher sends
`PING` once a second and `LEAVE` when it is done. A one-byte `JOIN` (build 3)
always means play.

`WISH`: everyone's colour applies (if free); track, laps (3, 5 or 10), cc
and the power-ups switch apply only from the leader (a five-byte wish
leaves the switch alone). Colours 0-7 are the presets, one player each;
colour 8 is the player's own kart, which any number may pick, painted from
the three palette picks that follow the flags (a six-byte wish leaves the
picks alone). The picks are kept whatever the colour, so a player switching
back to custom gets their kart back. When the leader presses Start the console
increments the start nonce; the server starts a race when it sees a new
nonce from the leader, even with nobody else in the lobby (practice while
waiting; anyone who joins meanwhile sits out until the next race).

`RETIRE` (build 16) drops the player out of the race named (the current
one; any other is ignored) without leaving the lobby: no place or points
that race, and its kart leaves the `RACE` packets, so other consoles take
it off the track. A race whose every racer has retired or left goes
straight back to waiting with no results.

`KART` (body 34 bytes, 52 in all):

| Offset | Type | Field |
| --- | --- | --- |
| 18 | u8 | race id (ignored unless it matches the current race) |
| 19 | u8 | flags: bit0 finished, bit1 grounded, bit2 boosting, bit3 drifting, bit4 drift right, bit5 waving, bit6 waving the left arm (build 20) |
| 20 | i16 | x * 16 (m) |
| 22 | i16 | y * 64 |
| 24 | i16 | z * 16 |
| 26 | u16 | heading * 65536 / 2pi |
| 28 | i16 | speed * 64 (m/s) |
| 30 | i8 | steer * 127 |
| 31 | u8 | spark stage 0-3 |
| 32 | u8 | lap (0 = before the line) |
| 33 | u8 | items: bits 0-2 held (0 none, 1 Power Sneakers, 2 Chaos Spear, 3 Barrier, 4 Mine), bit 3 barrier up, bit 4 spun out, bit 5 spear thrown backwards |
| 34 | u16 | lap distance * 16 (m) |
| 36 | u32 | race time in ms (finish time once finished); receivers space samples by it |
| 40 | u8 | bits 0-3 spear sequence (0 none, else 1-15, a new number per throw), bits 4-7 mine sequence (0 none, else 1-15, a new number per drop; 0 again once the mine has gone) |
| 41 | u16 | spear lap distance * 16 (m) |
| 43 | i8 | spear lateral offset * 4 (m, + = right) |
| 44 | u8 | spear target slot (255 none) |
| 45 | u16 | mine lap distance * 16 (m) |
| 47 | i8 | mine lateral offset * 4 (m, + = right) |
| 48 | u32 | client clock ms (unused; not relayed) |

## Server to client

Every server packet continues with:

| Offset | Type | Field |
| --- | --- | --- |
| 4 | u32 | server_ms (monotonic, echoed by the console) |

| Type | Name | Body (from offset 8) |
| --- | --- | --- |
| 64 | `WELCOME` | u8 status, u16 latest build, u16 min build, u8 lobby count, u8 max players |
| 65 | `REJECT` | u8 reason, u16 latest build, u16 min build |
| 66 | `LOBBIES` | lobby count x (u8 players, u8 state, u8 track, char[11] leader), then lobby count x (u8 watchers, u8 flags) |
| 67 | `LOBBY` | see below |
| 68 | `RACE` | see below |
| 69 | `CHAT` | u8 slot (255 none), name `char[11]`, u8 colour, u8 message id, text `char[33]` (build 14) |

`WELCOME` status: 0 up to date, 1 a newer build exists (playable). `REJECT`
reasons: 1 client too old (below min build), 2 server full, 3 lobby full,
4 bad name, 5 unknown session (send `HELLO` again). A console below the min build shows "please download the
latest version" and never gets further.

`LOBBY` (sent 4 Hz to members, at once after any change):

| Offset | Type | Field |
| --- | --- | --- |
| 8 | u8 | lobby id |
| 9 | u8 | state: 0 waiting, 1 countdown, 2 racing, 3 results |
| 10 | u8 | your slot (255 while watching) |
| 11 | u8 | leader slot |
| 12 | u8 | track |
| 13 | u8 | laps |
| 14 | u8 | cc |
| 15 | u8 | race id |
| 16 | i32 | ms until the green light (countdown) or since it (racing) |
| 20 | u8 | start nonce the server last accepted from the leader |
| 21 | u8 | player count n |
| 22 | n x 23 bytes | players |
| 22 + 23n | u8 | watchers (capped at 255) |
| 23 + 23n | u8 | lobby flags: bit0 the server's demo lobby (bots race, watch only) |
| 24 + 23n | i32 | ms until the race ends after the first finisher (racing only; 0 until someone finishes) |
| 28 + 23n | u8 | power-ups on (the leader's switch; on by default) |

The watcher count and flags (also after the `LOBBIES` entries) arrived in
build 4, the race end in build 7 and the power-ups switch in build 11;
older consoles ignore the extra bytes. Protocol 4 (build 17) grew the
player entry from 20 to 23 bytes for the custom kart.

Player entry (23 bytes): u8 slot, char[11] name, u8 colour (0-7 a preset,
8 the player's own kart), u8 flags (bit0 leader, bit1 racing, bit2
finished), u16 ping ms, u16 points, u8 last place (0 none), u8 grid
position, u8[3] custom kart picks (body, trim, Chao; meaningful with
colour 8). A `CHAT` from a custom-coloured player carries colour 8; the
console colours the name from that player's lobby entry.

`RACE` (every 83 ms during countdown, race and results, on the server's
own beat): u8 race id, u8 count, then per racer: u8 slot, u16 age ms (how
old the state is at send time), then the 29 bytes of the `KART` body at
offsets 19-47 (flags through the mine), 32 bytes per racer. A racer whose
kart has not changed since the last relay is sent again as is; consoles
drop the repeat.

## Lobby chat

A player (not a watcher) in a lobby sends a `CHAT` with a fresh message id
and the line it typed; the server cleans it (printable ASCII in capitals,
32 characters), stars out bad words (`lobby/filter.go`; a name that trips
the filter gets `REJECT` 4) and sends it as a server `CHAT` to every player
and watcher of that lobby, the sender included. That echo is the only
acknowledgement: the console resends the same id every 500 ms until it
sees it, and the server answers a repeat of the newest id with the echo
again instead of a second copy. Each player may send a burst of three
messages, then one a second; anything faster is dropped, and the
console's resends carry it in once the bucket refills. Chat is kept
nowhere: a console sees only what arrives while it is in the lobby.

## Race lifecycle

1. **Waiting:** up to six players. The leader is the earliest joiner still
   present; when the leader leaves, the next earliest takes over.
2. **Countdown:** on a valid start the server fixes the racers (everyone in
   the lobby), their grid order (points, then join order) and a race id,
   and counts 4 seconds to green.
3. **Racing:** ends when every racer has finished, 30 seconds after the
   first finisher, or (with no results) when no racer is left: everyone
   retired or disconnected.
4. **Results:** places by finish time, then unfinished racers by distance
   covered. Points: 10, 8, 6, 4, 2, 1 for places 1-6. Points accumulate per
   player for as long as they stay in the lobby. Results show 8 seconds,
   then the lobby is waiting again.

Players who join mid-race wait in the lobby until the next race; watchers
who arrive mid-race go straight to it. A player or watcher silent for 10
seconds is dropped.

## Demo lobby

The server can fill one lobby (lobby 10 by default) with six bots that are
never dropped and never send packets. They replay races recorded from the
console's own CPUs (`demo/races/`): 14 seconds in the lobby, then the leader
picks another track at random and the server starts one of that track's
recordings, dealing the bots onto its karts at random, so each bot drives
differently every race. Their kart states, power-ups included, are relayed
exactly like a console's; the race ends, places and points work as usual.
Points start again every 8 races. People joining it are told it is full;
anyone can watch it.

A recording keeps the full 29-byte kart, so the bots hold emeralds, throw
spears, drop mines, raise barriers and spin out as the CPUs did. Each was made with its
emeralds seeded as a watcher of the demo lobby will seed them (lobby 10, a
race id of its own, its track; `DK_RECORD_LOBBY` in `src/main.c`), and the
server starts it under that race id, so the gems watchers see are the ones
the bots take. A recorded spear's target is a kart index; the server sends
the slot of the bot driving that kart.
