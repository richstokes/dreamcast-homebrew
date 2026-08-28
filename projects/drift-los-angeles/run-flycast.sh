#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_KOS_ENV="${HOME}/.local/share/dreamcast/kos/environ.sh"
KOS_ENV="${KOS_ENV:-${DEFAULT_KOS_ENV}}"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"
FLYCAST_STARTUP_RETRIES="${FLYCAST_STARTUP_RETRIES:-8}"

if [[ ! -f "${KOS_ENV}" ]]; then
    echo "KallistiOS environment not found: ${KOS_ENV}" >&2
    exit 1
fi

if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    exit 1
fi

if [[ ! "${FLYCAST_STARTUP_RETRIES}" =~ ^[1-9][0-9]*$ ]]; then
    echo "FLYCAST_STARTUP_RETRIES must be a positive integer." >&2
    exit 1
fi

# KOS probes optional variables while its environment is imported.
# shellcheck disable=SC1090
set +u
source "${KOS_ENV}"
set -u

if [[ "${1:-}" != "--skip-build" ]]; then
    make -C "${PROJECT_DIR}"
fi

attempt=1
flycast_log=""

cleanup() {
    if [[ -n "${flycast_log}" && -f "${flycast_log}" ]]; then
        rm -f -- "${flycast_log}"
    fi
}
trap cleanup EXIT

while (( attempt <= FLYCAST_STARTUP_RETRIES )); do
    flycast_log="$(mktemp -t drift-los-angeles-flycast.XXXXXX)"
    set +e
    # Flycast's native-memory backend needs one large contiguous reservation.
    # Disabling macOS's nano allocator for this child process prevents its tiny
    # regions from fragmenting that range and avoids driver.cpp's sq_buffer
    # assertion without changing any persistent emulator setting.
    MallocNanoZone=0 "${FLYCAST_BIN}" \
        -config "config:Debug.SerialConsoleEnabled=yes" \
        "${PROJECT_DIR}/drift-los-angeles.elf" 2>&1 | tee "${flycast_log}"
    flycast_status="${PIPESTATUS[0]}"
    set -e

    if (( flycast_status == 0 )); then
        exit 0
    fi

    if (( attempt >= FLYCAST_STARTUP_RETRIES )) ||
       ! grep -Eq 'Verify Failed.*sq_buffer|driver\.cpp : 349' "${flycast_log}"; then
        exit "${flycast_status}"
    fi

    echo "Flycast hit its pre-boot dynarec address-space assertion; retrying in 2 seconds (${attempt}/${FLYCAST_STARTUP_RETRIES})." >&2
    rm -f -- "${flycast_log}"
    flycast_log=""
    attempt=$((attempt + 1))
    sleep 2
done
