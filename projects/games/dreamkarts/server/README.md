# DreamKarts lobby server

The online multiplayer server for [DreamKarts](../README.md): ten lobbies of
up to six Dreamcasts, over UDP port 7535 at `dcvmu.com`. Consoles reach it with
a broadband adapter or a 56k dial-up modem (a DreamPi or similar). The wire
format is in [PROTOCOL.md](PROTOCOL.md).

The server keeps lobby state (players, leader, track, laps, speed, colours,
pings and points) and runs each race's countdown, results and points. Each
console drives its own kart and the server relays everyone's kart 12 times a
second. Nothing is stored on disk: a restart empties every lobby.

## Layout

| Path | What |
| --- | --- |
| `lobby/` | Protocol packing and the lobby/race state machine, with tests |
| `cmd/dreamkarts-lobby/` | The server: one goroutine reads UDP, one runs the lobbies |
| `cmd/dk-bot/` | A pretend console: `-smoke` checks a server, otherwise it joins a lobby and races (`-path` replays a recorded drive) |
| `deploy/` | `deploy.sh`, the systemd unit and default settings |

## Run locally

```bash
go run ./cmd/dreamkarts-lobby -listen :7535
```

```bash
go run ./cmd/dk-bot -addr 127.0.0.1:7535 -smoke
```

To race a Flycast console against it, build the game pointing at this Mac
(Flycast's network proxy reaches the LAN address, not 127.0.0.1), then start a
bot that leads lobby 10 and starts races:

```bash
make -C .. VARIANT=autotest dreamkarts-autotest.elf EXTRA_CFLAGS="-DDK_AT_ONLINE -DDK_NET_TEST_HOST=192.168.1.184"
```

```bash
go run ./cmd/dk-bot -addr 127.0.0.1:7535 -name SONIC -lobby 9 -start -race 76 -path cmd/dk-bot/emerald-beach.path
```

```bash
cd .. && DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DK AUTOTEST: PASS' DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' ./run-flycast.sh --skip-build
```

The launcher uses Flycast's emulated broadband adapter by default;
`DK_NET=modem` uses its emulated modem instead, which dials and runs PPP like
a real dial-up server. Two Flycast windows can race each other the same way. A
`-DDK_PATH_LOG` autotest prints the lines `-path` replays.

## Versions and redeploying

[`../src/net_version.h`](../src/net_version.h) is the single source of truth:

- `DK_NET_PROTO` is the wire format. Change it with any layout change in
  PROTOCOL.md; consoles on another protocol are refused.
- `DK_NET_BUILD` numbers each console release that changes online behaviour
  (physics, tracks, kart state, menus that affect a race). The server tells
  consoles older than its `Build` that a newer version is out; they can still
  play.
- `DK_NET_MIN_BUILD` is the oldest console the server lets in. Raise it to
  `DK_NET_BUILD` when older consoles would desync or misbehave; they are then
  told to download the latest version.

`lobby/proto.go` repeats these numbers and `go test` fails if they drift
from the header, so the server and console in a commit always agree.

**Redeploy the server whenever a change touches any of these:**
`src/net_version.h`, `src/net.c`, anything under `server/`, the track list
(lobbies validate track numbers loosely, but new tracks should still go out
with a version bump), or kart state sent online. Then:

```bash
./deploy/deploy.sh
```

It runs the tests, cross-compiles for the amd64 host, installs to
`/opt/dreamkarts/bin` (keeping `.previous`), restarts
`dreamkarts-lobby.service`, and finishes with a UDP smoke test that fails
unless the live server reports the `DK_NET_BUILD` in the header. A restart
drops anyone online back to the lobby browser, so prefer quiet times.
`DREAMKARTS_DEPLOY_HOST` overrides the SSH target (default `root@netsplit.vip`,
the same host as `dcvmu.com`).

The host, its port registry and runbook live in
[richstokes/infra](https://github.com/richstokes/infra/tree/main/servers/netsplit.vip)
(app id `dreamkarts`); keep the port there in step with `DK_NET_PORT` in
`../src/netlink.h`. UDP 7535 is open to the internet with no cloud firewall
rule needed (checked 2026-10-17).

Check the live server at any time:

```bash
go run ./cmd/dk-bot -smoke
```

```bash
ssh root@netsplit.vip journalctl -u dreamkarts-lobby -n 50
```
