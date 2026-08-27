#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_KOS_ENV="${HOME}/.local/share/dreamcast/kos/environ.sh"
KOS_ENV="${KOS_ENV:-${DEFAULT_KOS_ENV}}"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"

if [[ ! -f "${KOS_ENV}" ]]; then
    echo "KallistiOS environment not found: ${KOS_ENV}" >&2
    exit 1
fi
if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    exit 1
fi

set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u

if [[ "${1:-}" != "--skip-build" ]]; then
    make -C "${PROJECT_DIR}"
fi

exec "${FLYCAST_BIN}" \
    -config "network:EmulateBBA=yes,network:DCNet=no,config:Debug.SerialConsoleEnabled=yes,input:device1=5,input:device1.1=10,input:device1.2=10,input:device2=6,input:device2.1=10,input:device2.2=10,input:device3=0,input:device3.1=1,input:device3.2=1,input:maple_sdl_keyboard=0,input:maple_sdl_mouse=1" \
    "${PROJECT_DIR}/dreamcast-browser.elf"
