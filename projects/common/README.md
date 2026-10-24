# Shared code

Code more than one project uses. Sources include the headers by relative
path (`#include "../../common/softreset.h"`). `.c` modules build into each
project with that project's own flags through [common.mk](common.mk).

| File | What | Used by |
| --- | --- | --- |
| `softreset.h` | A+B+X+Y+Start back to the title screen, and from there out to the BIOS menu | The games, Ping Cube, IRC, Browser, LemonPlay, PICO-8 Player |
| `dialup.h`, `dialup.c` | Modem dial-up and PPP for when there's no Ethernet adapter: the saved ISP profile or DreamPi defaults, on a worker thread, with cancel and drop detection | Ping Cube, IRC, Browser, DCVMU, PICO-8 Player, DreamKarts |
| `https.h`, `https.c` | curl's TLS options for HTTPS, and the certificate date check the kos-ports Mbed TLS leaves out | Browser, DCVMU, PICO-8 Player |
| `cacert.pem` | Mozilla's CA roots as curl exports them; `update-cacert.sh` refreshes it, and builds copy it into the romdisk (`CACERT` in `common.mk`) | Browser, DCVMU, PICO-8 Player |
| `vmusave.h` | One small packaged save on a VMU: picking the card, header text, an ASCII-art icon, writing and reading it back | IRC, Browser, BASIC, LemonPlay, DCVMU's login |
| `common.mk` | Finds the `.c` modules a project lists in `OBJS`, and copies the CA bundle in | Projects on KOS's default rules, HTTPS projects |

Headers stay valid C++ (PICO-8 Player is C++), and every name starts with its
module's name so it can't clash with a game's own helpers.

## Checks

| Command | Checks |
| --- | --- |
| `python3 projects/common/tests/run_dialup_checks.py --log-dir /tmp/dialup-checks` | The dialer on SH-4 with the modem and PPP faked: ISP profiles, every failure, cancel before and during a dial, the worker thread, drops and redial |
| `uv run projects/common/tests/run_https_checks.py --log-dir /tmp/https-checks` | Real curl and Mbed TLS on SH-4 against local servers: expired, not-yet-valid, untrusted, wrong-host and expired-intermediate certificates, the time zone leeway, an unset clock, connection reuse |
| `python3 projects/common/tests/run_vmusave_checks.py --log-dir /tmp/vmusave-checks` | Saves on blank Flycast cards: card choice, header text limits, icon, exact read-back, saves made the old fs_vmu way, overwrite, a full card; then the card images through DCVMU's VMU parser |

## Gotchas

- `dialup.c` needs `tools/kos/patches/kos/kos-ppp-lifecycle.patch`
- The Dreamcast clock holds local time and KOS reads it as UTC (Flycast's clock is the Mac's local time too), so certificate dates get 14 hours' leeway. Exact checks would refuse a freshly renewed certificate for hours on US consoles
- curl's mbedTLS backend has its own verify callback (the error text, VERIFYPEER/VERIFYHOST); `https.c` runs the date check first and then calls it, rather than replacing it
- KOS 2.3's `vmufs_write` rounds a file up to whole blocks by reading past the end of its buffer, so a package handed straight to it picks up stray heap. Build packages with `vmusave_build`. Saves through fs_vmu (`fs_open` on `/vmu/a1/...`) always carried 384 such bytes, and fs_vmu reads also run past the end of the file
- `vmu_pkg_build` copies a description longer than the file's 16 or 32 characters into the next field; `vmusave_header` cuts them to fit
- Two modem-mode Flycast instances started at the same moment both crashed at boot (`Verify Failed` in the dynarec). Run modem tests one at a time
