#!/bin/bash
# flycast-film.sh <elf> <serial log>: Flycast for filming. Serial console on,
# broadband adapter (online spectating), a 744x540 pt window (the game's
# 720x540 plus thin bars that hold macOS's rounded corners, which
# record.py crops) and 2160-line rendering, which the 1440x1080 Retina
# window downsamples 2:1 for anti-aliasing. Transient -config flags only.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../../../../../tools/flycast/resolve.sh"
# The ELF as a throwaway CDI, with the CD-DA music tracks (cdda/, from make
# music) when they are there; DK_FILM_NO_MUSIC builds don't need them.
CDI="${HERE}/../../build/cdi/$(basename "$1" .elf).cdi"
CDDA=()
compgen -G "${HERE}/../../cdda/*.wav" > /dev/null && CDDA=(--cdda "${HERE}/../../cdda")
"${HERE}/../../../../../tools/flycast/elf-to-cdi.sh" "${CDDA[@]}" "$1" "${CDI}" || exit 1
exec "$FLYCAST_BIN" -config "config:Debug.SerialConsoleEnabled=yes,network:EmulateBBA=yes,network:DCNet=no,window:width=744,window:height=540,window:left=40,window:top=60,window:maximized=no,window:fullscreen=no,config:rend.Resolution=2160,config:rend.ShowFPS=no" "${CDI}" > "$2" 2>&1
