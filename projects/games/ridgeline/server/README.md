# Ridgeline online server

The online server for [Ridgeline](../README.md): Go, twelve lobbies of up to
four Dreamcasts over UDP port 7536 at `dcvmu.com`, reachable by broadband
adapter, W5500 or 56k modem. Lobbies 1-3 are Cascade Ridge's, 4-6 Mesa
Grande's, 7-9 Moses Lake's and 10-12 Loch Lowland's: the consoles work
that out from the lobby's number, the server never knows. Changing how
many an area has moves every later lobby to another area, so the minimum
build goes up with it. Players drop in and out of a lobby whenever
they like; there is no race and no leader. The server hands out slots,
keeps each lobby's time of day, relays everyone's truck 12 times a second
and remembers who has reached the summit (a trophy by their name). A
truck left idle for 10 minutes is taken out of its lobby. Names
go through the bad word filter DreamKarts uses
([`projects/common/gameserver`](../../../common/gameserver)). Nothing is
stored: a restart empties every lobby. The wire format is in
[PROTOCOL.md](PROTOCOL.md).

## Status page

The server also publishes its lobbies as JSON on `127.0.0.1:8094/lobbies.json`
(`-status`, `RIDGELINE_STATUS`; test names left out), which
[dcvmu.com/lobbies](https://dcvmu.com/lobbies) shows beside DreamKarts'.
Loopback only: DCVMU reads it on the same host.

## Alerts

Someone joining a lobby sends a Bark push (or a Discord post) to each
target in `/etc/ridgeline/alerts.json`, at most once per player every 5
minutes; test clients (`AT` and a digit or dash, like `AT-BOT`) never do.
The targets are secret, so they live in the private
[infra repo](https://github.com/richstokes/infra/tree/main/servers/netsplit.vip)
(`config/ridgeline/alerts.json`); change them there with `manage.py check`
and `apply --restart-apps`. The format is DreamKarts' (its
[server README](../../dreamkarts/server/README.md#alerts)); only
`lobby_join` means anything here. `-alerts file.json` points a local
server at a file. Bark shows `assets/icon.png` from `main`.

## Online change checklist

[`../src/net_version.h`](../src/net_version.h) is the single source of
truth; `lobby/proto.go` repeats it and `go test` fails if they differ (and
if the console's packet sizes in `src/net.h` drift).

1. For any change that touches online play (`src/net_version.h`,
   `src/net.c`, `src/online.c`, what a `TRUCK` carries, the truck physics
   or the level, anything under `server/`), bump `RL_NET_BUILD`. Raise
   `RL_NET_MIN_BUILD` to match if old consoles would disagree with new ones.
   Bump `RL_NET_PROTO` for any layout change in PROTOCOL.md
2. Update `lobby/proto.go` to match and run `go test ./...`
3. Redeploy with `./deploy/deploy.sh`: it tests, cross-compiles for
   linux/amd64, installs to `/opt/ridgeline/bin` (keeping `.previous`),
   restarts `ridgeline-lobby.service` and fails unless a UDP smoke test sees
   the new build live. A restart sends everyone online back to the lobby
   browser. `RIDGELINE_DEPLOY_HOST` overrides `root@dcvmu.com`

The host, its port registry and runbook are in
[richstokes/infra](https://github.com/richstokes/infra/tree/main/servers/netsplit.vip)
(app id `ridgeline`); keep the port there in step with `RL_NET_PORT` in
`../src/net.h`.

```bash
go run ./cmd/rl-bot -smoke                              # the live server answers
ssh root@dcvmu.com journalctl -u ridgeline-lobby -n 50  # who came, went and summited
```

## Testing

`make online-test` in the game directory runs the whole thing in Flycast
(`../tools/online-test.sh`): this server and a bot on the Mac, the online
autotest pointed at them. `RL_NET=modem` dials in through Flycast's
emulated modem; `RL_QA_LIVE=1` uses dcvmu.com. To do it by hand:

```bash
go run ./cmd/ridgeline-lobby -listen :7594 -words-extra BLORP     # -lag 40ms -jitter 8ms fakes a WAN
go run ./cmd/rl-bot -addr 127.0.0.1:7594 -lobby 3 -summit-after 5s -level ../assets/generated/level_cascade.lvl
make -C .. VARIANT=autotest ridgeline-autotest.elf EXTRA_CFLAGS="-DRL_AT_ONLINE -DRL_NET_TEST_HOST=$(ipconfig getifaddr en0) -DRL_NET_PORT=7594"
cd .. && RL_ELF=ridgeline-autotest.elf RL_EXIT_ON_LOG='RL AUTOTEST: PASS' RL_FAIL_ON_LOG='RL AUTOTEST: FAIL' ./run-flycast.sh --skip-build
```

`rl-bot` drives a loop in the meadow (`-x -z -radius -speed`), heights read
from the area's packed level (`level_moab.lvl` for lobbies 4-6, `level_dunes.lvl` for 7-9,
`level_lowland.lvl` for 10-12), and logs
the lobby and everyone else's truck.
`-summit-after` claims the summit, `-leave-after` and `-away` drop out and
back in. Two consoles: build the autotest twice with
`-DRL_AT_NAME=AT-RL1` / `AT-RL2`, copy each to its own ELF name and launch
both (`RL_FLYCAST_CONFIG=window:left=760` keeps the windows apart).
`-DRL_AT_LOBBY`, `-DRL_AT_SUMMIT_AFTER`, `-DRL_AT_ONLINE_SECONDS` and
`-DRL_AT_EXPECT_TROPHIES` tune the autotest; `-DRL_AT_ONLINE_ALONE` passes
without anyone else there.

Results (2026-11-08): against the bot and console to console, BBA and
modem, local and live: no pops in the others' motion at 140-190 ms delay
(only the CPU driver's winch jumps), both trophies through, joins and
leaves seen.

On the console, test from the CDI rather than through dcload-ip, whose
hand-over of the broadband adapter to the game is unreliable.
