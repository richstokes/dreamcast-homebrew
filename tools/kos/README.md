# KOS patches

Fixes the projects need in KallistiOS 2.3 and kos-ports. Every project builds
against all of them, and release CI applies the lot to its pinned SDK
(`.github/scripts/fetch-sdk.sh`).

```sh
tools/kos/apply-patches.sh --check   # report what the installed SDK is missing
tools/kos/apply-patches.sh           # apply it
make -C "$KOS_BASE"                  # rebuild KOS, then relink projects
```

The script reads `$KOS_BASE` and `$KOS_PORTS` (sourcing the installed
`environ.sh` when they are unset), or takes both trees as arguments. It skips
patches that are already in, and changes nothing if one matches neither way.
After the kos-ports patch, reinstall the ports it touches:

```sh
make -C "$KOS_PORTS/mbedtls" force-install
make -C "$KOS_PORTS/curl" install
make -C "$KOS_PORTS/stb_image" install
```

| Patch | Fixes | Upstream |
| --- | --- | --- |
| `kos/kos-bba-rx-consumer.patch` | BBA receive polling and the RX worker could both claim a packet, delivering it twice | |
| `kos/kos-dhcp-fast-retry.patch` | A lost first DHCP DISCOVER was resent after 4 s, so bringing up the network took 7-12 s on the console; backs off from 1 s | |
| `kos/kos-keyboard-attach.patch` | Keyboard attach cleared memory past its own state | [KallistiOS#1537](https://github.com/KallistiOS/KallistiOS/pull/1537) |
| `kos/kos-maple-rx-cache.patch` | A stale cache line over a DMA-filled maple receive buffer broke boot (`cont_cond_t` assertion) on some link layouts | |
| `kos/kos-ppp-lifecycle.patch` | `ppp_shutdown` left its receive thread running into the freed modem buffers. Every dial-up project needs it | |
| `kos/kos-tcp-orphan-reset.patch` | Sockets closed mid-transfer were never reset, each leaking 128 KiB until transfers stopped | [KallistiOS#1551](https://github.com/KallistiOS/KallistiOS/pull/1551) |
| `kos/kos-tcp-poll-lock.patch` | The TCP receive, poll and close locks deadlocked after a few page loads | [KallistiOS#1550](https://github.com/KallistiOS/KallistiOS/pull/1550) |
| `kos/kos-tcp-upload-window.patch` | Send-window bugs: a shrinking peer window wrapped unsigned maths, retransmits moved SND.NXT backwards, and pure window updates were dropped | |
| `kos-ports/kos-ports-kos23.patch` | mbedTLS had its platform entropy source disabled though KOS 2.3 has `/dev/urandom`; stb_image only built with G++ | |

Patches apply in name order (three touch `net_tcp.c`). Drop one once the
pinned SDK has its fix.
