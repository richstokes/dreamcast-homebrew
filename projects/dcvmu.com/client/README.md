# DCVMU Dreamcast client

A KallistiOS application that uploads and downloads VMU data saves with [dcvmu.com](https://dcvmu.com).
Register on the website, then log in here with the same username and password.
Requires a broadband adapter and a controller or Dreamcast keyboard. Uploading reads the source save without changing it. Downloads write to a
chosen VMU only after confirmation. The client also writes
its own one-block `DCVMU_AUTH` login save.

<img src="../service/static/dcvmu-client.png" alt="DCVMU browsing VMU saves with game icons and Power Stone selected" width="760">

## Build and run

Uses the installed KOS 2.3 toolchain and kos-ports curl, mbedTLS and zlib. The
browser project's README documents the local ports compatibility patch.

Apply this project's KOS networking fixes before linking, if not already applied:

```sh
source /Users/rich/.local/share/dreamcast/kos/environ.sh
git -C "$KOS_BASE" apply /Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/client/patches/kos-bba-rx-consumer.patch
git -C "$KOS_BASE" apply /Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/client/patches/kos-tcp-upload-window.patch
make -C "$KOS_BASE/kernel" -j4
```

The first patch serializes the BBA polling/worker receive consumers. The second
handles TCP window updates and preserves transmitted sequence numbers during
retransmission; see [RFC 9293 section 3.10.7.4](https://www.rfc-editor.org/rfc/rfc9293.html#section-3.10.7.4). These are portable SDK fixes, not changes to the
service protocol. They are already applied to the SDK used for this build.
Rebuild dependent ELFs after changing the SDK; existing binaries stay unchanged.

```sh
source /Users/rich/.local/share/dreamcast/kos/environ.sh
make -C /Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/client
file /Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/client/dcvmu-client.elf
sh-elf-readelf -h /Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/client/dcvmu-client.elf
/Users/rich/Dropbox/code/dreamcast-dev/projects/dcvmu.com/client/run-flycast.sh --skip-build
```

The launcher transiently enables the BBA using Flycast's `DCNet=no` picoTCP
outbound proxy, attaches controllers/VMUs on ports A-C and a keyboard on
port D, routes the Mac keyboard to that port, and enables serial diagnostics.
Click the Flycast window to focus it, then use arrows/Tab to navigate and Enter
to edit a field before typing. This is not LAN bridging. Set `KOS_ENV`
or `FLYCAST_BIN` to override the installed tools. On Apple Silicon, the installed
Flycast can fail its host memory-layout assertion before booting an ELF. The
Intel build can be selected with `FLYCAST_ARCH=x86_64`; it requires Rosetta and
may need a relaunch if that emulator assertion occurs. No persistent emulator
or network settings are changed. Crash-report uploading is disabled for this
credential-handling application.

CI publishes [CDI disc images](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.cdi)
and [ELFs](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.elf),
linked from the site's account page. It can also boot
with a compatible real-Dreamcast homebrew loader; real hardware has not been
verified. CI packages a self-booting CDI using mkdcdisc; local `make` builds the ELF.

## Controls

- D-pad up/down / keyboard up/down: choose a field or save. Tab advances fields.
- Left/right on the save list: cycle all cards, A1, A2, and other attached VMUs.
  The heading shows the selected card and number of saves, including empty cards.
- A / Enter: edit, select, or confirm. Y / R rescans the VMUs on the save list.
- Text editing: type with a keyboard, or choose characters on the on-screen
  keyboard with the D-pad and A. X erases, Y finishes, B cancels. Keyboard
  Enter/Tab finishes; Escape cancels editing.
- Visibility starts **public**. Toggle it to **private** before uploading if
  only your account should see or download that save.
- Duplicate name: A/Enter replaces the existing archive entry, Y/K keeps both
  with a numbered name, and B/Backspace returns so you can choose another name.
- B / Escape / Backspace returns to the previous page (Escape exits at login/main menu).
- Start exits, retaining a remembered login.
- X / L at the main menu signs out, revokes the token and removes its VMU save.
- B / Escape cancels an active network transfer. A failed or canceled request
  may already have reached the server; check your account before retrying.

The picker and details page show the save's embedded 32x32 colour icon (first
animation frame). Missing or malformed icons use an outline marker. Icons are
read from the VMU and cached only for the current page; rescanning refreshes them.

Name is the archive name; the original 12-byte VMU filename is preserved
separately. The game field starts from the VMU's long description and can be
corrected. Notes are optional. The upload is the complete padded raw VMS file.
VMU mini-games (directory type `0xcc`) and empty entries are not listed.

## Network and credentials

Every request uses HTTPS with hostname and CA verification, TLS 1.2 or newer,
and the bundled Mozilla trust store. No insecure HTTP fallback exists. Keep
the Dreamcast clock correct and update `romdisk/cacert.pem` alongside the
browser's CA bundle when needed. The client reuses its HTTPS connection and
polls BBA receive traffic in a KOS worker to avoid Flycast's IRQ re-entry bug.
Requests are bounded by time and response size; cancellation
remains available during transfers.

Passwords stay in RAM and are cleared after login. A random, revocable token and
username are stored in the one-block `DCVMU_AUTH` save on an available VMU.
The client restores and validates it at startup; remembered sessions expire
90 days after login. If no VMU has space, login still works for the current run
and the client reports that remembering failed. Keep the same VMU attached on
subsequent runs. Offline restore failures preserve the save for a later retry;
expired/revoked tokens require a new login.

Anyone with this VMU login save can access the account until the token expires
or is revoked. It is not encrypted with an embedded/recoverable application key.
Passwords are never written. The reserved filename is hidden from the save list;
client and server also reject its signature if renamed. X/L signs out and deletes
the loaded login save. If disconnected, server revocation cannot be guaranteed,
although local deletion is still attempted. Reinsert a removed card to erase it.

The local launcher reserves a persistent A1 card for login, shared across VMU banks.

## Verification

The integration build (`CPPFLAGS=-DDCVMU_SELF_TEST`) reads a temporary,
git-ignored `romdisk/test-credentials.txt` containing a disposable test username
and password on separate lines. It exercises login, VMU reading, upload,
conflict, revision-checked private replacement, and keep-both against the real
HTTPS service. Never distribute this build. Remove the credential file and
run `make clean && make` before producing a release.

`tests/fixture.c` creates a synthetic 17-block save. Compile it in an isolated
KOS scratch project, and launch it only with a dedicated temporary
`config:Dreamcast.VMUPath` plus `config:PerGameVmu=no`. It intentionally writes
a test VMU; never point it at personal VMUs. Use the same isolated VMU directory
when launching the integration client, and compare the VMU image SHA-256 before
and after. The production client does not compile the fixture code.

Verified in Flycast 2.7 with its picoTCP BBA backend: website registration and
login, then client login/upload/conflict/private replacement/keep-both for both
17-block (8,704-byte) and 193-block (98,816-byte) saves. Authenticated downloads
matched the fixture bytes; anonymous private downloads returned 404. The source
VMU image SHA-256 stayed unchanged. The large fixture uses
`CPPFLAGS=-DDCVMU_FIXTURE_BYTES=98304`. Automated integration drives the same
application functions; real controller and real hardware testing remains open.

## Upload and download navigation

After manual or remembered login, the main menu offers **Upload a save**,
**Download a save**, and **Sign out**. Upload retains the existing VMU picker.
Download starts with **My saves**, including private saves. Y/R toggles public
saves; Left/Right changes pages. A/Enter downloads the selected entry over HTTPS,
checks its expected length and SHA-256, then opens the destination VMU picker.
Y/R rescans destination cards. No card is written while browsing or downloading.

Choose a card to see the install/replace confirmation, which defaults to Cancel.
Existing saves require explicit replacement. The client checks free blocks,
rechecks the target card and existing save before writing, preserves the original
filename and VMS header offset, then reads back the file to verify its bytes.
Do not remove the VMU or power off during writing. VMU writes are not atomic;
a hardware/write failure can still damage the destination save. Games' save
formats and region compatibility remain the games' responsibility.

## Local Flycast VMUs

Running `run-flycast.sh` discovers all 128 KiB `*vmu*.bin` images in Flycast's
data directory and configured VMU directory. Populated cards are listed first.
It mounts **persistent test copies** so installs and remembered logins survive
restarts while original game VMUs remain unchanged. The copies live under
`~/Library/Application Support/DCVMU/VMUs`. A1 is dedicated to DCVMU login;
A2/B1/B2/C1/C2 hold five discovered images per bank, with a keyboard on port D.
All images remain accessible through numbered banks, including empty ones.

```sh
./projects/dcvmu.com/client/run-flycast.sh --list-vmus
FLYCAST_ARCH=x86_64 ./projects/dcvmu.com/client/run-flycast.sh --bank 1
FLYCAST_ARCH=x86_64 ./projects/dcvmu.com/client/run-flycast.sh --bank 2
```

Close the current test session before opening another bank. `--list-vmus` prints
the original image-to-slot mapping and save filenames. Normal launching prints
the active mapping. `--vmu-image /path/to/card.bin` (repeatable) chooses explicit
images instead of discovery; `--vmus-dir /path` overrides discovery. `--dry-run`
prepares/reports the mapping without launching. Omit `FLYCAST_ARCH` when native
Flycast works. All emulator settings are transient; `PerGameVmu=no` lets the
client see the selected cards. This does not merge or expose unlimited physical
VMUs: the Dreamcast still sees only its attached slots.

An original changed by a game gets a new versioned test copy on the next launch;
previous test copies are kept. Installed saves are not copied back to originals
automatically. This prevents a test install from overwriting a game's real save.
On real hardware, insert the destination VMU and choose it in the client.

The read-only `CPPFLAGS=-DDCVMU_VMU_TEST` integration build checks forward and
backward switching among All/A1/A2 with a populated A1 and empty A2. It does
not log in or upload saves. Clean-rebuild without this flag for distribution.

`CPPFLAGS=-DDCVMU_AUTH_TEST` verifies one-block login write/read/filtering on
an isolated VMU; a second boot verifies persistence and deletion. Never run this
fixture against personal VMUs. Clean-rebuild without test flags for release.

`CPPFLAGS=-DDCVMU_DOWNLOAD_TEST` exercises login/menu navigation, private/public
listing and pagination, a large download with SHA-256 validation, installation,
overwrite confirmation/cancellation, nonzero header offsets, and a full card.
It needs a disposable account with eight fixtures (seven public, plus newest
private `DCVMU_TEST` larger than half a VMU), and isolated empty A1/A2 cards.
For a local HTTPS fixture, git-ignored `romdisk/test-service.txt` supplies its
origin and `romdisk/test-ca.pem` its test CA. Certificate verification stays on.
Remove all `romdisk/test-*` files and clean-rebuild before releasing. Launcher
host checks: `python3 -m unittest discover -s projects/dcvmu.com/client/tests`.
