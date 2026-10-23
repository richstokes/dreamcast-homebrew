# DreamKarts lobby server

The online server for [DreamKarts](../README.md): Go, ten lobbies of up to
six Dreamcasts over UDP port 7535 at `dcvmu.com`, reachable by broadband
adapter or 56k modem. The wire format is in [PROTOCOL.md](PROTOCOL.md). It
keeps lobby state, runs each race's countdown, results and points (a race
can be started alone, as practice while waiting, and Start mid-race drops a
player out of it back to the lobby room), relays everyone's kart 12 times
a second and passes lobby chat round with bad words starred out (a name
that trips the filter is refused). The word list is not in this
repository: the server downloads the English list from
[LDNOOBW](https://github.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words)
when it starts, keeps a copy in its state directory for restarts while
GitHub is down, and does not start without one or the other.
`DREAMKARTS_BAD_WORDS_URL` points it at another list (one word per line)
and `DREAMKARTS_BAD_WORDS_EXTRA` adds words of your own, comma-separated;
`lobby/filter.go` says how a word matches. Nothing else is stored on disk: a
restart empties every lobby. Lobby 10 is a [demo lobby](#demo-lobby) of
bots that anyone can watch.

## Online change checklist

[`../src/net_version.h`](../src/net_version.h) is the single source of
truth; `lobby/proto.go` repeats its numbers and `go test` fails if they
differ.

1. For any change that touches online play (`src/net_version.h`,
   `src/net.c`, `src/online.c`, kart state sent online, the track list,
   anything under `server/`), bump `DK_NET_BUILD`. Raise `DK_NET_MIN_BUILD`
   to match if older consoles would desync (they are then told to download
   the latest). Bump `DK_NET_PROTO` for any layout change in PROTOCOL.md
2. Update `lobby/proto.go` to match and run `go test ./...`
3. Redeploy with `./deploy/deploy.sh`. It tests, cross-compiles for
   linux/amd64, installs to `/opt/dreamkarts/bin` (keeping `.previous`),
   restarts `dreamkarts-lobby.service` and fails unless a UDP smoke test
   sees the new build live. A restart drops everyone online back to the
   lobby browser, so prefer quiet times. `DREAMKARTS_DEPLOY_HOST` overrides
   `root@dcvmu.com`

The host, its port registry and runbook are in
[richstokes/infra](https://github.com/richstokes/infra/tree/main/servers/netsplit.vip)
(app id `dreamkarts`); keep the port there in step with `DK_NET_PORT` in
`../src/netlink.h`. UDP 7535 is open to the internet with no cloud firewall
rule needed (checked 2026-10-17).

Check the live server at any time:

```bash
go run ./cmd/dk-bot -smoke
ssh root@dcvmu.com journalctl -u dreamkarts-lobby -n 50
```

## Run locally

```bash
go run ./cmd/dreamkarts-lobby -listen :7535     # -lag 40ms -jitter 8ms fakes a WAN
go run ./cmd/dk-bot -addr 127.0.0.1:7535 -smoke
```

To race a Flycast console against it, start a bot that leads a lobby and
starts races (`dk-bot` lobby numbers are 0-based, so `-lobby 8` is lobby 9;
lobby 10 is the demo lobby), then build the game pointing at this Mac's LAN
address (Flycast's proxy does not reach 127.0.0.1) and run the online
autotest:

```bash
go run ./cmd/dk-bot -addr 127.0.0.1:7535 -name SONIC -lobby 8 -start -start-delay 28s -race 69 -path cmd/dk-bot/emerald-beach.path -chat "HELLO FROM BOT;YOU ARE A SHITHEAD"
make -C .. VARIANT=autotest dreamkarts-autotest.elf EXTRA_CFLAGS="-DDK_AT_ONLINE -DDK_NET_TEST_HOST=192.168.1.184"
cd .. && DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DK AUTOTEST: PASS' DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' ./run-flycast.sh --skip-build
```

`-chat` makes the bot say each line in turn once two are in the lobby (said
again whenever company arrives) and log every line it hears; the autotest
expects those two lines, the second starred by the filter. `-start-delay`
gives the chat time before the bot starts the race.

The practice flow (start alone, the join bell, dropping out back to the
room, a rival's kart leaving when it drops out) has its own autotest in an
empty lobby, with a bot that joins ~25 s in and drops out 6 s into each
race:

```bash
make -C .. VARIANT=autotest dreamkarts-autotest.elf EXTRA_CFLAGS="-DDK_AT_ONLINE -DDK_AT_PRACTICE -DDK_AT_LOBBY=6 -DDK_NET_TEST_HOST=192.168.1.184"
(sleep 25; go run ./cmd/dk-bot -addr 127.0.0.1:7535 -name SONIC -lobby 6 -retire 6s -for 150s) &
cd .. && DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DK AUTOTEST: PASS' DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' ./run-flycast.sh --skip-build
```

`DK_NET=modem` in front of the launcher uses Flycast's emulated modem, which
dials and runs PPP like a real dial-up server. Two Flycast windows can race
each other the same way. A `-DDK_PATH_LOG` autotest prints the lines `-path`
replays. To watch the demo lobby instead, build with `-DDK_AT_SPECTATE` (no
bot needed), or from a terminal
`go run ./cmd/dk-bot -addr 127.0.0.1:7535 -lobby 9 -spectate`.

The online autotest ends with a `DK ONLINE: remote motion` line (steps, pops
over 25 cm, worst and mean jump, stalls, heading jumps, interpolation delay)
that says how smoothly the rival karts moved. Result at `-lag 40ms -jitter
8ms`, a 3-lap race against the `-path` bot: 263 pops (mean error 6 cm) with
build 5's netcode, 4-10 pops (mean 2 cm, ~150 ms delay) with build 6's
timed sends and smoothing.

### Real hardware

With a broadband adapter (loaded through dcload-ip, which hands the adapter
to the game), start a bot leading lobby 9 on the live server, then:

```bash
make -C .. hwbench EXTRA_CFLAGS="-DDK_AT_ONLINE" HW_LOG=captures/hw/online.log
```

First checked 2026-10-17: connected, raced and passed at 27 ms ping with no
dropped frames in the race.

## Demo lobby

`DREAMKARTS_DEMO_LOBBY` (or `-demo-lobby`; default 10, 0 for none) picks the
lobby where six in-process bots replay recorded all-CPU races from
`demo/races/` (embedded in the binary), on a random track after every race.
It shows as DEMO in the browser; joining is refused, watching is not.

Re-record the races after a physics, AI or track change:

```bash
make -C .. VARIANT=record dreamkarts-record.elf EXTRA_CFLAGS=-DDK_RECORD
cd .. && DK_ELF=dreamkarts-record.elf DK_EXIT_ON_LOG='DKR DONE' ./run-flycast.sh --skip-build > captures/record/record.log
cd .. && uv run tools/record_demo.py captures/record/record.log
```

Add `-DDK_RECORD_ONLY=<track id>` to `EXTRA_CFLAGS` to record one track;
`record_demo.py` then replaces only that track's races and prints how many
emeralds, spears, mines and spin-outs each has. `go test ./demo` checks every
recording loads, every kart finishes and the bots use power-ups. The
recordings seed their emeralds for lobby 10 (`-DDK_RECORD_LOBBY=<id - 1>`
for another demo lobby); replayed elsewhere, the bots still race but the
gems watchers see are not the ones they take.
