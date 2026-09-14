#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_KOS_ENV="${HOME}/.local/share/dreamcast/kos/environ.sh"
KOS_ENV="${KOS_ENV:-${DEFAULT_KOS_ENV}}"
source "$PROJECT_DIR/../../../tools/flycast/resolve.sh"

if [[ ! -f "${KOS_ENV}" ]]; then
    echo "KallistiOS environment not found: ${KOS_ENV}" >&2
    echo "Set KOS_ENV to the path of your KallistiOS environ.sh." >&2
    exit 1
fi

if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    echo "Set FLYCAST_BIN to the Flycast executable." >&2
    exit 1
fi

# KOS's environment scripts probe optional variables that may be unset. Keep
# strict mode for this launcher, but relax nounset while importing the SDK.
# shellcheck disable=SC1090
set +u
source "${KOS_ENV}"
set -u

SKIP_BUILD=no
VMU_ARGS=()
for argument in "$@"; do
    if [[ "$argument" == "--skip-build" ]]; then
        SKIP_BUILD=yes
    else
        VMU_ARGS+=("$argument")
    fi
    if [[ "$argument" == "--list-vmus" || "$argument" == "--help" ]]; then SKIP_BUILD=yes; fi
done
if [[ "$SKIP_BUILD" != yes ]]; then make -C "$PROJECT_DIR"; fi
# Bash 3.2 treats empty arrays as unset under nounset; expand only when populated.
exec python3 "$PROJECT_DIR/flycast-vmus.py" \
    --elf "$PROJECT_DIR/dcvmu-client.elf" --flycast "$FLYCAST_BIN" \
    --arch "${FLYCAST_ARCH:-}" ${VMU_ARGS[@]+"${VMU_ARGS[@]}"}
