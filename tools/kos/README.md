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
| `kos/kos-maple-rx-cache.patch` | A stale cache line over a DMA-filled maple receive buffer broke boot (`cont_cond_t` assertion) on some link layouts | |
| `kos/kos-maple-timeout-dequeue.patch` | A VMU (or SIP, Dreameye, reset) command that timed out left its frame on the maple queue, so the next use of that device asserted `!frame->queued` and the console aborted; a write timing out before its frame was sent counted as done | |
| `kos/kos-ppp-lifecycle.patch` | `ppp_shutdown` left its receive thread running into the freed modem buffers. Every dial-up project needs it | |
| `kos/kos-tcp-upload-window.patch` | Send-window bugs: a shrinking peer window wrapped unsigned maths, retransmits moved SND.NXT backwards, and pure window updates were dropped | |
| `kos-ports/kos-ports-kos23.patch` | mbedTLS had its platform entropy source disabled though KOS 2.3 has `/dev/urandom`; stb_image only built with G++ | |

Patches apply in name order. Drop one once the pinned SDK has its fix; the
keyboard attach, TCP orphan reset and TCP poll-lock fixes went upstream
(KallistiOS#1537, #1551, #1550) and were dropped with the move to master
`e7ab1a2`.

## Moving the pin

CI builds against the KallistiOS commit in `.github/workflows/release.yml`
(`KOS_COMMIT`), currently master `e7ab1a2`. The weekly
[KOS canary](../../.github/workflows/kos-canary.yml) builds every project
against the newest master with these patches and fails when one has gone
upstream (drop it), no longer applies, or a build breaks; its summary says
how far master has moved. To move:

```sh
git -C "$KOS_BASE" checkout -- . && git -C "$KOS_BASE" fetch origin
git -C "$KOS_BASE" checkout --detach <commit>
tools/kos/apply-patches.sh                     # after deleting patches now upstream
make -C "$KOS_BASE/kernel" clean all && make -C "$KOS_BASE/addons" clean all
make -C "$KOS_PORTS/zlib" force-install        # and mbedtls, curl, stb_image
```

Compare `environ.sh` with the new `doc/environ.sh.sample` first: upstream
moved `environ_base.sh` to `env/` and added `KOS_TOOLCHAIN` for `e7ab1a2`.
Then set `KOS_COMMIT`, rebuild every project, and run the Flycast suites.
Before it ships, A/B the console benchmarks on the old and new SDK (`make
hwbench` in DreamKarts, Totally Bazaar and PICO-8 Player): Flycast timings
don't reflect the hardware.
