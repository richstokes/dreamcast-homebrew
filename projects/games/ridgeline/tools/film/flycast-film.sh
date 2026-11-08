#!/bin/bash
# flycast-film.sh <elf> <serial log>: Flycast for filming (make film). The
# same 744x540 pt window DreamKarts' record.py crops, with 2160-line
# rendering that the Retina window downsamples 2:1. Transient -config only.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${HERE}/../../../../../tools/flycast/resolve.sh"
exec "$FLYCAST_BIN" -config "config:Debug.SerialConsoleEnabled=yes,window:width=744,window:height=540,window:left=40,window:top=60,window:maximized=no,window:fullscreen=no,config:rend.Resolution=2160,config:rend.ShowFPS=no" "$1" > "$2" 2>&1
