#!/usr/bin/env bash
# Power-cycle the Dreamcast through its Shelly plug, wait for GDEMU -> OpenMenu ->
# dcload-ip to come online, then upload an ELF and capture the dcload console.
# Usage: ./hwdebug-run.sh [elf] [seconds-to-capture]
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELF="${1:-$PROJECT_DIR/chroma-circuit-hwdebug.elf}"
CAPTURE="${2:-40}"
PLUG="${DC_PLUG_IP:-192.168.1.173}"
CONSOLE="${DC_IP:-192.168.1.171}"
TOOL="${DC_TOOL_IP:-$HOME/Dropbox/Games/ROMs/DREAMCAST/dcload-ip/dc-tool-ip}"
LOG="${DC_LOG:-$PROJECT_DIR/dcload-console.log}"

plug() { curl -fsS -m 5 "http://$PLUG/rpc/Switch.Set?id=0&on=$1" >/dev/null; }

echo "Power cycling Dreamcast via $PLUG"
plug false
sleep 2
plug true

echo "Waiting for dcload-ip at $CONSOLE (up to 120 s)"
# Give GDEMU/OpenMenu time to start before pinging; the BBA answers ICMP once dcload is up.
sleep 30
for _ in $(seq 1 90); do
    if ping -c1 -W1000 "$CONSOLE" >/dev/null 2>&1; then
        echo "dcload-ip is answering"
        break
    fi
    sleep 1
done
ping -c1 -W1000 "$CONSOLE" >/dev/null 2>&1 || { echo "dcload-ip never came online" >&2; exit 1; }
sleep 3

rm -f "$LOG"
pkill -x dc-tool-ip || true
# `script` supplies a pty so dc-tool-ip line-buffers console output. It is left
# running after the capture: killing it would block the demo's next console write.
nohup script -q "$LOG" "$TOOL" -t "$CONSOLE" -x "$ELF" >/dev/null 2>&1 &
sleep "$CAPTURE"
cat -v "$LOG" | sed 's/\^M//g' | tail -n 40
echo "(dc-tool-ip still attached; log: $LOG)"
