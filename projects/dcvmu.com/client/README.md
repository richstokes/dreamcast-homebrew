# DCVMU Dreamcast client

A KallistiOS application that uploads and downloads VMU data saves with [dcvmu.com](https://dcvmu.com).
Register on the website, then log in here with the same username and password.
Requires a broadband adapter **or a Dreamcast modem with a working DreamPi setup**,
and a controller or Dreamcast keyboard. Uploading reads the source save without
changing it. Downloads write to a chosen VMU only after confirmation.

<img src="../service/static/dcvmu-client.png" alt="DCVMU browsing VMU saves with game icons and Power Stone selected" width="456">

## Download

Download the latest **[self-booting CDI](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.cdi)**
for Flycast or CD-R. A direct-load [ELF](https://github.com/richstokes/dreamcast-homebrew/releases/latest/download/dcvmu-client.elf)
is also available. Real hardware has not been verified yet.

## Using the client

After login, the main menu offers **Upload a save**, **Download a save**,
**Browse Public Saves**, and **Sign out**.

- **Upload**: pick a save from any attached VMU, optionally edit its title,
  notes, and visibility, then upload. Visibility starts **public**; toggle it to
  **private** if only your account should see the save. If a backup of the same
  VMU file already exists, you can replace it or keep both. VMU mini-games are
  not listed.
- **Download**: opens **My saves**, including private ones. Selecting a save
  downloads and verifies it, then asks which VMU to install it on. Existing
  saves require explicit replacement, and nothing is written before you
  confirm.
- **Browse Public Saves**: enter another user's exact username (find it on
  dcvmu.com first) and optionally part of a game title, then select
  **Find saves**. Only that account's public saves appear.

**Custom icons:** choose **Customise my VMU** in your website account to draw or
import artwork, then find the saved icon set under **Download a save** here.
Select a VMU and confirm installation of `ICONDATA_VMS`. Replacing an icon set
asks first and keeps game saves. Restart the Dreamcast/VMU to see the menu icons;
the optional hidden 3D BIOS animation is included in the file. Custom icon files
are excluded from the upload list; archive an existing set using the card importer.

**Imported saves:** the website's **Import a memory card** accepts `.bin`, `.vmu`,
`.dcm` and `.dci`. Selected files appear in **My saves**, ready to install. Their
bytes and header offsets are preserved; original directory timestamps and
copy-protection flags are not restored.

Do not remove the VMU or power off while a save is being written.

## Controls

- D-pad / keyboard arrows: choose a field or save. Tab advances fields.
- Left/right on the save list: cycle between all cards and each attached VMU.
  On cloud lists, left/right changes page.
- A / Enter: edit, select, or confirm.
- B / Escape / Backspace: go back, or cancel an active network transfer.
- Y / R: rescan VMUs, or refresh a cloud list.
- X / N on **My saves**: rename the selected cloud save. Public saves cannot be
  renamed.
- X / L at the main menu: sign out.
- Start: exit, keeping a remembered login.
- Text editing: type with a keyboard, or use the on-screen keyboard with the
  D-pad and A. X erases, Y finishes, B cancels.

A canceled or failed request may already have reached the server; check your
account before retrying.

## Login and security

Every request uses HTTPS with certificate and hostname verification; there is
no insecure fallback. Keep the Dreamcast clock correct.

Your password is never written anywhere. After login, a revocable token is
stored in a one-block `DCVMU_AUTH` save on an available VMU so the next launch
can sign in automatically. Keep the same VMU attached. Remembered sessions
expire after 90 days, and signing out revokes the token and deletes the save.

Anyone with that VMU login save can access your account until the token expires
or is revoked, so sign out before lending the card to someone.

## Modem / DreamPi

A Broadband Adapter is always preferred; the modem is used only when no
Ethernet adapter is found.

Set up and test DreamPi with another application first. DCVMU dials on every
boot using the phone number and PPP login from the console's saved
**PlanetWeb** profile, falling back to **DreamPassport**, and then to the
DreamPi defaults (`555`, `dream` / `cast`). Only tone dialing is supported, and
dialing prefixes are not used; configure the direct DreamPi number.

Dialing can take up to 65 seconds. Failures show a connection error; restart to
dial again. A dropped connection is not redialed. Real modem and DreamPi
hardware validation is still outstanding.

## Build

Uses the KOS 2.3 toolchain and the kos-ports curl, mbedTLS and zlib. The
[browser project's README](../../apps/dreamcast-browser/README.md) documents
the ports compatibility patch.

Apply this project's KOS networking fixes once, then rebuild KOS (CI does this
automatically):

```sh
source "$HOME/.local/share/dreamcast/kos/environ.sh"
for p in patches/kos-*.patch; do git -C "$KOS_BASE" apply "$PWD/$p"; done
make -C "$KOS_BASE/kernel" -j4
make -C "$KOS_BASE/addons/libppp" -j4
```

Then build the ELF from this directory:

```sh
make
```

## Run in Flycast

```sh
./run-flycast.sh
```

The launcher enables the emulated BBA, attaches controllers, VMUs, and a
keyboard, and changes no saved Flycast settings. Click the Flycast window to
focus it, then use arrows/Tab to navigate and Enter to edit a field before
typing. If uploads time out intermittently, install the
[native Flycast build](../../../tools/flycast/README.md#tcp-upload-fix).

The launcher mounts **persistent test copies** of your Flycast VMU images, kept
under `~/Library/Application Support/DCVMU/VMUs`, so a test install can never
overwrite a game's real save. A1 is reserved for the DCVMU login, and the other
images are spread across numbered banks of five:

```sh
./run-flycast.sh --list-vmus
./run-flycast.sh --bank 2
```

`--vmu-image /path/to/card.bin` (repeatable) and `--vmus-dir /path` choose
images explicitly, and `--dry-run` reports the mapping without launching.

## Tests

The integration tests run in Flycast against a temporary local HTTPS service
with synthetic accounts and isolated VMUs. Supply the Mac's LAN IPv4 address,
and add `--modem` to use Flycast's modem emulation:

```sh
python3 projects/dcvmu.com/client/tests/run_modem_checks.py
projects/dcvmu.com/service/.venv/bin/python projects/dcvmu.com/client/tests/run_public_browse.py --host <Mac-LAN-IP> --log-dir /tmp/dcvmu-public-test
projects/dcvmu.com/service/.venv/bin/python projects/dcvmu.com/client/tests/run_public_browse.py --tools --host <Mac-LAN-IP> --log-dir /tmp/dcvmu-tools-test
python3 projects/dcvmu.com/client/tests/run_upload_stress.py --host <Mac-LAN-IP>
python3 -m unittest discover -s projects/dcvmu.com/client/tests
```

The `--tools` test creates icons through the studio and saves through the card
importer, then checks HTTPS downloads, cancellation, installation, replacement,
header offsets and preservation of existing saves in Flycast. It also verifies
that the upload list excludes icons/login files. Add `--modem` for modem testing.
Real-hardware icon display and the BIOS animation still need verification on a
physical Dreamcast/VMU.

Add `--tools --log-dir /tmp/dcvmu-tools-test` to `run_public_browse.py` to verify
custom icon downloads and card-imported saves on isolated VMUs.

Self-test builds (`CPPFLAGS=-DDCVMU_SELF_TEST` and similar) read disposable
credentials from git-ignored `romdisk/test-*` files. Never distribute one, and
never run the test fixtures against personal VMUs; remove those files and run
`make clean && make` before a release.

## Credits

Powered by [KallistiOS](https://kos-docs.dreamcast.wiki/), the independent
Dreamcast SDK. KallistiOS is distributed under its BSD-like KOS license and
requires attribution.
