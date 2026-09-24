#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
source "$PROJECT_DIR/../../../tools/flycast/resolve.sh"
SKIP_BUILD=no
TARGET=pico8-dreamcast.elf
NETWORK="network:EmulateBBA=yes,network:DCNet=no"
for argument in "$@"; do
    case "$argument" in
        --skip-build) SKIP_BUILD=yes ;;
        --smoke-test) TARGET=pico8-smoke.elf ;;
        --network-test) TARGET=pico8-network-test.elf ;;
        --modem) NETWORK="network:EmulateBBA=no,network:DCNet=no" ;;
        *) echo "Usage: ${0##*/} [--skip-build] [--smoke-test|--network-test] [--modem]" >&2; exit 1 ;;
    esac
done
[[ -f "$KOS_ENV" ]] || { echo "KOS environment missing: $KOS_ENV" >&2; exit 1; }
[[ -x "$FLYCAST_BIN" ]] || { echo "Flycast missing: $FLYCAST_BIN" >&2; exit 1; }
set +u
source "$KOS_ENV"
set -u
if [[ "$SKIP_BUILD" != yes ]]; then make -C "$PROJECT_DIR" "$TARGET"; fi
exec "$FLYCAST_BIN" -config "$NETWORK,config:Debug.SerialConsoleEnabled=yes" "$PROJECT_DIR/$TARGET"
