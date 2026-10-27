#!/usr/bin/env bash
# Power-cycle the Dreamcast (through tools/dc-hw.sh's Shelly plug, if one is
# set up with --use-shelly), wait for dcload-ip, then upload an ELF and
# capture the dcload console.
# Usage: ./hwdebug-run.sh [elf] [seconds-to-capture]
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELF="${1:-$PROJECT_DIR/chroma-circuit-hwdebug.elf}"
CAPTURE="${2:-40}"
DC_HW="$PROJECT_DIR/../../../tools/dc-hw.sh"
TOOL="${DC_TOOL:-$(command -v dc-tool-ip || echo "$HOME/Dropbox/Games/ROMs/DREAMCAST/dcload-ip/dc-tool-ip")}"
LOG="${DC_LOG:-$PROJECT_DIR/dcload-console.log}"

if [[ "$("$DC_HW" status)" == "plug:      none"* ]]; then
    echo "No Shelly plug set up: boot the console into dcload-ip yourself"
else
    "$DC_HW" power cycle
fi
"$DC_HW" wait
CONSOLE="$("$DC_HW" status | sed -n 's/.*answering at \([0-9.]*\).*/\1/p')"
[[ -n "$CONSOLE" ]] || { echo "dcload-ip never came online" >&2; exit 1; }

rm -f "$LOG"
pkill -x dc-tool-ip || true
# `script` supplies a pty so dc-tool-ip line-buffers console output. It is left
# running after the capture: killing it would block the demo's next console write.
nohup script -q "$LOG" "$TOOL" -t "$CONSOLE" -x "$ELF" >/dev/null 2>&1 &
sleep "$CAPTURE"
cat -v "$LOG" | sed 's/\^M//g' | tail -n 40
echo "(dc-tool-ip still attached; log: $LOG)"
