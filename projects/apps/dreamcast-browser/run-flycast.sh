#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_KOS_ENV="${HOME}/.local/share/dreamcast/kos/environ.sh"
KOS_ENV="${KOS_ENV:-${DEFAULT_KOS_ENV}}"
source "$PROJECT_DIR/../../../tools/flycast/resolve.sh"

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

# BROWSER_NETWORK=modem removes the emulated Broadband Adapter, so the browser
# dials through Flycast's emulated modem the way it would reach a DreamPi.
case "${BROWSER_NETWORK:-bba}" in
    bba) NETWORK_CONFIG="network:EmulateBBA=yes,network:DCNet=no" ;;
    modem) NETWORK_CONFIG="network:EmulateBBA=no,network:DCNet=no" ;;
    *)
        echo "BROWSER_NETWORK must be bba or modem." >&2
        exit 2
        ;;
esac

FLYCAST_COMMAND=("${FLYCAST_BIN}")
if [[ -n "${FLYCAST_ARCH:-}" ]]; then
    FLYCAST_COMMAND=(/usr/bin/arch "-${FLYCAST_ARCH}" "${FLYCAST_BIN}")
fi
# The browser writes a CPU framebuffer. Refresh it every vblank so idle
# pages still pump host input regularly; this is a transient emulator option.
FLYCAST_CONFIG="${NETWORK_CONFIG},config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no,config:rend.EmulateFramebuffer=yes,input:device1=5,input:device1.1=10,input:device1.2=10,input:device2=6,input:device2.1=10,input:device2.2=10,input:device3=0,input:device3.1=1,input:device3.2=1,input:maple_sdl_keyboard=0,input:maple_sdl_mouse=1"

EXIT_ON_LOG="${BROWSER_EXIT_ON_LOG:-}"
FAIL_ON_LOG="${BROWSER_FAIL_ON_LOG:-}"
if [[ -z "${EXIT_ON_LOG}" && -z "${FAIL_ON_LOG}" ]]; then
    exec "${FLYCAST_COMMAND[@]}" -config "${FLYCAST_CONFIG}" \
        "${PROJECT_DIR}/dreamcast-browser.elf"
fi

# Self-test runs: stop Flycast when the serial console prints the line that
# ends the test, or after BROWSER_TEST_SECONDS (default 300) without one.
flycast_log="$(mktemp -t dreamcast-browser-flycast.XXXXXX)"
flycast_pid=""
cleanup() {
    if [[ -n "${flycast_pid}" ]] && kill -0 "${flycast_pid}" 2>/dev/null; then
        kill -TERM "${flycast_pid}" 2>/dev/null || true
        wait "${flycast_pid}" 2>/dev/null || true
    fi
    rm -f -- "${flycast_log}"
}
trap cleanup EXIT

"${FLYCAST_COMMAND[@]}" -config "${FLYCAST_CONFIG}" \
    "${PROJECT_DIR}/dreamcast-browser.elf" > >(tee "${flycast_log}") 2>&1 &
flycast_pid="$!"
deadline=$((SECONDS + ${BROWSER_TEST_SECONDS:-300}))
result="ended"
while kill -0 "${flycast_pid}" 2>/dev/null; do
    if [[ -n "${FAIL_ON_LOG}" ]] &&
       /usr/bin/grep -a -Fq -- "${FAIL_ON_LOG}" "${flycast_log}"; then
        result="fail"
        break
    fi
    if [[ -n "${EXIT_ON_LOG}" ]] &&
       /usr/bin/grep -a -Fq -- "${EXIT_ON_LOG}" "${flycast_log}"; then
        result="pass"
        break
    fi
    if (( SECONDS >= deadline )); then
        result="timeout"
        break
    fi
    sleep .2
done
kill -TERM "${flycast_pid}" 2>/dev/null || true
wait "${flycast_pid}" 2>/dev/null || true
flycast_pid=""
echo "run-flycast: ${result}" >&2
[[ "${result}" == "pass" ]]
