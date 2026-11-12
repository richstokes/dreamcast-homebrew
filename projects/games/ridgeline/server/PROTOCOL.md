# Ridgeline online protocol

One UDP socket per console talks to one server (default `dcvmu.com`, port
7536). Sized for a 56k modem: a console sends ~0.6 KB/s and receives under
1.5 KB/s with four trucks on the mountain.

## Principles

- **State, not events.** The server resends each lobby's state four times
  a second; a console resends its JOIN until the state shows it. A lost
  packet costs a quarter of a second, never a desync, so there is no ack
  or retransmit layer.
- **Each console drives its own truck.** It sends its truck 12 times a
  second; the server relays everyone's newest to everyone else on its own
  83 ms beat, with each truck's age. Consoles draw the others ~110-200 ms
  in the past, interpolated between samples spaced by the sender's own
  clock (in the packet), and set the wheels down on their own copy of the
  mountain.
- **No race, no leader.** Joining a lobby puts the truck straight on the
  mountain; leaving takes it off. Slots are handed out lowest first.
- **One day per lobby.** The server keeps each lobby's time of day,
  running an hour a minute like the console. An empty lobby's day starts
  again at 09:00 when someone arrives.
- **Bytes, not structs.** Little-endian, packed by hand (the SH-4 faults
  on unaligned loads). Names are 11 bytes, ASCII `A-Z 0-9 space - .`, NUL
  padded.
- **Versions.** `src/net_version.h` holds the protocol, the latest build
  and the oldest build the server accepts; `lobby/proto.go` repeats them
  and `go test` checks they match.

## Header

Console to server, 18 bytes:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8[2] | magic `'R' 'L'` |
| 2 | u8 | protocol (`RL_NET_PROTO`, currently 1) |
| 3 | u8 | packet type |
| 4 | u32 | session token (random, chosen by the console) |
| 8 | u32 | echo: the newest `server_ms` received |
| 12 | u16 | hold: ms since that packet arrived (for ping) |
| 14 | u16 | console build (`RL_NET_BUILD`) |
| 16 | u16 | 0 |

Server to console, 8 bytes: magic, protocol, type, then u32 `server_ms`.

Players are known by token, not address, so a modem that redials keeps its
place. A HELLO on another protocol gets a REJECT 1 stamped with the
server's protocol byte; the header and the REJECT body never move, so any
build can read it.

## Console to server

| Type | Name | Body | Sent |
| --- | --- | --- | --- |
| 1 | `HELLO` | name `char[11]` | every 500 ms until `WELCOME` |
| 2 | `LIST` | nothing | 2 Hz on the lobby browser |
| 3 | `JOIN` | u8 lobby (0-19), u8 truck model (0-6), u8 livery (0-3) | every 500 ms until the lobby state names this console |
| 4 | `LEAVE` | nothing | 3 times |
| 5 | `TRUCK` | 30 bytes, below | 12 Hz on the mountain |
| 7 | `PING` | nothing | 1 Hz when nothing else goes (keeps NAT open); answered with `LOBBIES` while in no lobby (the truck select), as the console gives up after 6 s of silence |

`TRUCK`:

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u8 | flags: bit0 reached the summit this session, bits 1-2 lights (off, dipped, full), bit3 horn, bit4 braking, bit5 stalled |
| 1 | i16 x3 | centre of gravity x, y, z * 64 (m) |
| 7 | i16 x4 | orientation quaternion x, y, z, w * 32767 (body axes right, up, forward) |
| 15 | i16 x3 | velocity x, y, z * 256 (m/s) |
| 21 | i8 | front wheel steer * 160 (radians) |
| 22 | u8 | rpm / 32 |
| 23 | u8 | engine load * 255 |
| 24 | u8 | the slipperiest tyre's slip * 16 (m/s) |
| 25 | i8 | gear (-1 reverse) |
| 26 | u32 | sender's clock, ms; receivers space samples by it |

The summit bit earns the trophy: the server sets the player's summit flag
the first time it sees it, and keeps it for the session (moving lobby
included).

## Server to console

| Type | Name | Body (from offset 8) |
| --- | --- | --- |
| 64 | `WELCOME` | u8 status (0 up to date, 1 a newer build exists), u16 latest build, u16 min build, u8 lobbies, u8 max players |
| 65 | `REJECT` | u8 reason, u16 latest build, u16 min build |
| 66 | `LOBBIES` | u8 lobby count, then per lobby: u8 players n, u8 hour * 10, then n x (u8 flags, `char[11]` name) |
| 67 | `LOBBY` | u8 lobby, u8 your slot, u16 hour * 2000, u8 players n, then n x 17-byte player |
| 68 | `TRUCKS` | u8 count, then per truck: u8 slot, u16 age ms, the 30-byte `TRUCK` body as received |

REJECT reasons: 1 console too old (or another protocol), 2 server full,
3 lobby full, 4 bad name (the bad word filter, as DreamKarts), 5 unknown
session (say `HELLO` again), 6 idle (build 7 on): the truck has not moved
2 m, honked or switched its lights in 10 minutes, so the server took it out
of its lobby. Older builds get a 5 instead and land on the lobby list.

Player entry (17 bytes): u8 slot, `char[11]` name, u8 truck model, u8
livery, u8 flags (bit0 reached the summit), u16 ping ms.

`LOBBY` goes to every member four times a second and at once after any
change. `TRUCKS` goes to every member on each beat, others' trucks only
(an empty one still goes). A console silent for 10 s in a lobby, or 30 s
on the browser, is dropped.
