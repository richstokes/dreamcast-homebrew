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
# Flycast emulates either the Broadband Adapter or the modem, never both, so
# --modem turns the BBA off and lets the demo dial Flycast's PPP peer instead.
NETWORK="network:EmulateBBA=yes,network:DCNet=yes"

for argument in "$@"; do
    case "${argument}" in
        --skip-build)
            SKIP_BUILD=yes
            ;;
        --modem)
            NETWORK="network:EmulateBBA=no,network:DCNet=no"
            ;;
        *)
            echo "Usage: ${0##*/} [--skip-build] [--modem]" >&2
            exit 1
            ;;
    esac
done

if [[ "${SKIP_BUILD}" != yes ]]; then
    make -C "${PROJECT_DIR}"
fi

exec "${FLYCAST_BIN}" \
    -config "${NETWORK},config:Debug.SerialConsoleEnabled=yes" \
    "${PROJECT_DIR}/ping-cube.elf"
