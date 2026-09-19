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
EMULATE_BBA=yes
for argument in "$@"; do
    case "$argument" in
        --skip-build) SKIP_BUILD=yes ;;
        --modem) EMULATE_BBA=no ;;
        --help) echo "Usage: $0 [--skip-build] [--modem]"; exit 0 ;;
        *) echo "Unknown option: $argument" >&2; exit 1 ;;
    esac
done
if [[ "$SKIP_BUILD" != yes ]]; then
    make -C "${PROJECT_DIR}"
fi

# Port A remains a controller. Port B is transiently configured as a Dreamcast
# keyboard, with the host keyboard forwarded to it. picoTCP provides outbound
# access to Libera.Chat without changing the user's saved Flycast settings.
exec "${FLYCAST_BIN}" \
    -config "network:EmulateBBA=${EMULATE_BBA},network:DCNet=no,input:device2=5,input:maple_sdl_keyboard=1,config:Debug.SerialConsoleEnabled=yes" \
    "${PROJECT_DIR}/dreamcast-irc.elf"
