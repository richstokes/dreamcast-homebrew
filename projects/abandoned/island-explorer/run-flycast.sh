#!/usr/bin/env bash
set -euo pipefail
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
source "$PROJECT_DIR/../../../tools/flycast/resolve.sh"
SKIP_BUILD=no
for arg in "$@"; do
    case "$arg" in
        --skip-build) SKIP_BUILD=yes ;;
        *) echo "Usage: ${0##*/} [--skip-build]" >&2; exit 1 ;;
    esac
done
[[ -f "$KOS_ENV" && -x "$FLYCAST_BIN" ]] || { echo 'KOS or Flycast is missing.' >&2; exit 1; }
set +u
source "$KOS_ENV"
set -u
if [[ "$SKIP_BUILD" != yes ]]; then make -C "$PROJECT_DIR"; fi
exec "$FLYCAST_BIN" -config 'config:Debug.SerialConsoleEnabled=yes' "$PROJECT_DIR/island-explorer.elf"
