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
| `kos/kos-bba-rx-consumer.patch` | BBA receive polling and the RX worker could both claim a packet, delivering it twice | Not filed: this fix can leave packets waiting for the next interrupt, so it needs rework first |
| `kos/kos-dhcp-fast-retry.patch` | A lost first DHCP DISCOVER was resent after 4 s, so bringing up the network took 7-12 s on the console; backs off from 1 s. Also keeps the REQUEST when a second OFFER arrives | Second OFFER: issue [#1573](https://github.com/KallistiOS/KallistiOS/issues/1573). The 1 s retry stays local (RFC 2131 says about 4 s). PR [#1564](https://github.com/KallistiOS/KallistiOS/pull/1564) fixes other DHCP bugs |
| `kos/kos-maple-rx-cache.patch` | A stale cache line over a DMA-filled maple receive buffer broke boot (`cont_cond_t` assertion) on some link layouts | Issue [#1572](https://github.com/KallistiOS/KallistiOS/issues/1572), PR [#1576](https://github.com/KallistiOS/KallistiOS/pull/1576); maintainers point at the earlier [#1526](https://github.com/KallistiOS/KallistiOS/pull/1526) and want a lighter fix |
| `kos/kos-maple-timeout-dequeue.patch` | A VMU (or SIP, Dreameye, reset) command that timed out left its frame on the maple queue, so the next use of that device asserted `!frame->queued` and the console aborted; a write timing out before its frame was sent counted as done | Not filed yet |
| `kos/kos-ppp-lifecycle.patch` | `ppp_shutdown` left its receive thread running into the freed modem buffers, and `net_init` set up no sockets without an Ethernet adapter; a modem send could also spin forever. Every dial-up project needs it | Issues [#1570](https://github.com/KallistiOS/KallistiOS/issues/1570) (shutdown) and [#1571](https://github.com/KallistiOS/KallistiOS/issues/1571) (`net_init`); no PR yet |
| `kos/kos-tcp-upload-window.patch` | Send-window bugs: a shrinking peer window wrapped unsigned maths, retransmits moved SND.NXT backwards, and pure window updates were dropped | Not filed: needs a standalone repro first |
| `kos-ports/kos-ports-kos23.patch` | mbedTLS had its platform entropy source disabled though KOS 2.3 has `/dev/urandom`; stb_image only built with G++ | Not filed |

Two more upstream fixes have no patch here, because `projects/common/vmusave.h`
works around them: `vmufs_write` padding from past the buffer (issue
[#1568](https://github.com/KallistiOS/KallistiOS/issues/1568), PR [#1574](https://github.com/KallistiOS/KallistiOS/pull/1574)) and fs_vmu's packaged-file sizes (issue [#1569](https://github.com/KallistiOS/KallistiOS/issues/1569), PR [#1575](https://github.com/KallistiOS/KallistiOS/pull/1575)).

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
