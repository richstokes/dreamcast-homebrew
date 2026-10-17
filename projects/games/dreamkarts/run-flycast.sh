#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
source "$PROJECT_DIR/../../../tools/flycast/resolve.sh"
ELF_NAME="${DK_ELF:-dreamkarts.elf}"
EXIT_ON_LOG="${DK_EXIT_ON_LOG:-}"
FAIL_ON_LOG="${DK_FAIL_ON_LOG:-}"
SKIP_BUILD=false

usage() {
    cat <<'USAGE'
Usage: ./run-flycast.sh [--skip-build]

Builds DreamKarts (unless --skip-build) and boots the ELF in Flycast with the
serial console on this terminal. Only transient -config flags are used.

QA hooks: DK_ELF picks another ELF in this folder; DK_EXIT_ON_LOG and
DK_FAIL_ON_LOG stop Flycast when that text appears on the serial console.
DK_NET=bba or DK_NET=modem picks the emulated broadband adapter or the
emulated modem (Flycast's local proxy either way) for online play.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

[[ -f "${KOS_ENV}" ]] || { echo "KallistiOS environment not found: ${KOS_ENV}" >&2; exit 1; }
[[ -x "${FLYCAST_BIN}" ]] || { echo "Flycast executable not found: ${FLYCAST_BIN}" >&2; exit 1; }

# KOS's environment scripts probe optional variables that may be unset.
set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u

if [[ "${SKIP_BUILD}" != true ]]; then
    make -C "${PROJECT_DIR}" "${ELF_NAME}"
fi

FLYCAST_CONFIG="config:Debug.SerialConsoleEnabled=yes"
case "${DK_NET:-}" in
    bba) FLYCAST_CONFIG+=",network:EmulateBBA=yes,network:DCNet=no" ;;
    modem) FLYCAST_CONFIG+=",network:EmulateBBA=no,network:DCNet=no" ;;
    "") ;;
    *) echo "DK_NET must be bba or modem" >&2; exit 2 ;;
esac
ELF_PATH="${PROJECT_DIR}/${ELF_NAME}"

flycast_log=""
flycast_pid=""
cleanup() {
    if [[ -n "${flycast_pid}" ]] && kill -0 "${flycast_pid}" 2>/dev/null; then
        kill -TERM "${flycast_pid}" 2>/dev/null || true
        wait "${flycast_pid}" 2>/dev/null || true
    fi
    [[ -n "${flycast_log}" && -f "${flycast_log}" ]] && rm -f -- "${flycast_log}"
    return 0
}
trap cleanup EXIT

flycast_log="$(mktemp -t dreamkarts-flycast.XXXXXX)"
set +e
if [[ -n "${EXIT_ON_LOG}" || -n "${FAIL_ON_LOG}" ]]; then
    qa_result=""
    "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${ELF_PATH}" \
        > >(tee "${flycast_log}") 2>&1 &
    flycast_pid="$!"
    while kill -0 "${flycast_pid}" 2>/dev/null; do
        if [[ -n "${FAIL_ON_LOG}" ]] && /usr/bin/grep -aFq -- "${FAIL_ON_LOG}" "${flycast_log}"; then
            qa_result="fail"; kill -TERM "${flycast_pid}" 2>/dev/null || true; break
        fi
        if /usr/bin/grep -aFq -- "KOS panic" "${flycast_log}" || /usr/bin/grep -aFq -- "Unhandled exception" "${flycast_log}"; then
            qa_result="fail"; kill -TERM "${flycast_pid}" 2>/dev/null || true; break
        fi
        if [[ -n "${EXIT_ON_LOG}" ]] && /usr/bin/grep -aFq -- "${EXIT_ON_LOG}" "${flycast_log}"; then
            qa_result="pass"; kill -TERM "${flycast_pid}" 2>/dev/null || true; break
        fi
        sleep .2
    done
    wait "${flycast_pid}"
    flycast_status="$?"
    flycast_pid=""
    [[ "${qa_result}" == "pass" ]] && flycast_status=0
    [[ "${qa_result}" == "fail" ]] && flycast_status=1
else
    "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${ELF_PATH}" 2>&1 | tee "${flycast_log}"
    flycast_status="${PIPESTATUS[0]}"
fi
set -e
exit "${flycast_status}"
