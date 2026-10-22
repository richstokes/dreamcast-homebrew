#!/usr/bin/env bash
# Boot LemonPlay in Flycast with the Broadband Adapter on Flycast's local
# TCP/UDP proxy (DCNet=no). The proxy reaches a DLNA server on this Mac's LAN
# address, but SSDP broadcasts don't get through it, so build with
# -DLP_SERVER (see README) or enter the address by hand.
#
#   LP_ELF=lemonplay-qa.elf     which ELF to boot
#   LP_EXIT_ON_LOG / LP_FAIL_ON_LOG / LP_TEST_SECONDS
#                               scripted runs: stop Flycast on a serial line
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
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

ELF="${PROJECT_DIR}/${LP_ELF:-lemonplay.elf}"
CONFIG="network:EmulateBBA=yes,network:DCNet=no,config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no"

EXIT_ON_LOG="${LP_EXIT_ON_LOG:-}"
FAIL_ON_LOG="${LP_FAIL_ON_LOG:-}"
if [[ -z "${EXIT_ON_LOG}" && -z "${FAIL_ON_LOG}" ]]; then
    exec "${FLYCAST_BIN}" -config "${CONFIG}" "${ELF}"
fi

log="$(mktemp -t lemonplay-flycast.XXXXXX)"
pid=""
cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -TERM "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
    rm -f -- "${log}"
}
trap cleanup EXIT

"${FLYCAST_BIN}" -config "${CONFIG}" "${ELF}" > >(tee "${log}") 2>&1 &
pid="$!"
deadline=$((SECONDS + ${LP_TEST_SECONDS:-300}))
result="ended"
while kill -0 "${pid}" 2>/dev/null; do
    if [[ -n "${FAIL_ON_LOG}" ]] && /usr/bin/grep -a -Fq -- "${FAIL_ON_LOG}" "${log}"; then
        result="fail"
        break
    fi
    if [[ -n "${EXIT_ON_LOG}" ]] && /usr/bin/grep -a -Fq -- "${EXIT_ON_LOG}" "${log}"; then
        result="pass"
        break
    fi
    if (( SECONDS >= deadline )); then
        result="timeout"
        break
    fi
    sleep .2
done
kill -TERM "${pid}" 2>/dev/null || true
wait "${pid}" 2>/dev/null || true
pid=""
echo "run-flycast: ${result}" >&2
[[ "${result}" == "pass" ]]
