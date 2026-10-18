#!/bin/bash
# flycast-film.sh <elf> <serial log>: Flycast for filming. Serial console on,
# broadband adapter (online spectating), a 744x540 pt window (the game's
# 720x540 plus thin bars that hold macOS's rounded corners, which
# record.py crops) and 2160-line rendering, which the 1440x1080 Retina
# window downsamples 2:1 for anti-aliasing. Transient -config flags only.
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/../../../../../tools/flycast/resolve.sh"
exec "$FLYCAST_BIN" -config "config:Debug.SerialConsoleEnabled=yes,network:EmulateBBA=yes,network:DCNet=no,window:width=744,window:height=540,window:left=40,window:top=60,window:maximized=no,window:fullscreen=no,config:rend.Resolution=2160,config:rend.ShowFPS=no" "$1" > "$2" 2>&1
