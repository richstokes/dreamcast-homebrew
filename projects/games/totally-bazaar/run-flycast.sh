#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_KOS_ENV="${HOME}/.local/share/dreamcast/kos/environ.sh"
KOS_ENV="${KOS_ENV:-${DEFAULT_KOS_ENV}}"
source "$PROJECT_DIR/../../../tools/flycast/resolve.sh"
INPUT_MODE="${BAZAAR_INPUT:-auto}"
ELF_NAME="${BAZAAR_ELF:-totally-bazaar.elf}"
EXIT_ON_LOG="${BAZAAR_EXIT_ON_LOG:-}"
FAIL_ON_LOG="${BAZAAR_FAIL_ON_LOG:-}"
SKIP_BUILD=false

usage() {
    cat <<'USAGE'
Usage: ./run-flycast.sh [--skip-build] [--input auto|gamepad|keyboard]

Input defaults to auto. Every connected macOS HID gamepad (up to four) gets
its own Dreamcast port, A to D, so each player can hold a pad. With no
gamepad, Flycast's keyboard mapping drives port A and players pass it round.
BAZAAR_INPUT may also be set to auto, gamepad, or keyboard.

QA hooks: BAZAAR_ELF picks another ELF in this folder; BAZAAR_EXIT_ON_LOG and
BAZAAR_FAIL_ON_LOG stop Flycast when that text appears on the serial console.
USAGE
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=true ;;
        --input)
            if [[ $# -lt 2 ]]; then
                echo "--input requires auto, gamepad, or keyboard." >&2
                exit 2
            fi
            INPUT_MODE="$2"
            shift
            ;;
        --input=*) INPUT_MODE="${1#--input=}" ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

case "${INPUT_MODE}" in
    auto|gamepad|keyboard) ;;
    *) echo "Invalid input mode '${INPUT_MODE}'; use auto, gamepad, or keyboard." >&2; exit 2 ;;
esac

if [[ ! -f "${KOS_ENV}" ]]; then
    echo "KallistiOS environment not found: ${KOS_ENV}" >&2
    exit 1
fi
if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    exit 1
fi

# KOS's environment scripts probe optional variables that may be unset.
# shellcheck disable=SC1090
set +u
source "${KOS_ENV}"
set -u

if [[ "${SKIP_BUILD}" != true ]]; then
    make -C "${PROJECT_DIR}" "${ELF_NAME}"
fi

count_gamepads() {
    local devices
    [[ "$(uname -s)" == "Darwin" && -x /usr/bin/hidutil ]] || { echo 0; return; }
    # Generic Desktop HID usages 4 and 5 are joysticks and gamepads.
    devices="$(/usr/bin/hidutil list --ndjson --matching \
        '[{"PrimaryUsagePage":1,"PrimaryUsage":4},{"PrimaryUsagePage":1,"PrimaryUsage":5}]' \
        2>/dev/null || true)"
    if [[ -z "${devices}" ]]; then
        echo 0
    else
        printf '%s\n' "${devices}" | grep -c . || true
    fi
}

pads="$(count_gamepads)"
if [[ "${INPUT_MODE}" == "auto" ]]; then
    if [[ "${pads}" -gt 0 ]]; then INPUT_MODE="gamepad"; else INPUT_MODE="keyboard"; fi
fi

# Transient Flycast settings only: saved mappings are never edited, and the
# keyboard and gamepads are never merged onto one port.
if [[ "${INPUT_MODE}" == "gamepad" ]]; then
    [[ "${pads}" -ge 1 ]] || pads=1
    [[ "${pads}" -le 4 ]] || pads=4
    INPUT_CONFIG="input:maple_sdl_keyboard=-1,input:maple_sdl_mouse=-1"
    DEVICES=""
    for i in 0 1 2 3; do
        if [[ "${i}" -lt "${pads}" ]]; then
            INPUT_CONFIG+=",input:maple_sdl_joystick_${i}=${i}"
            DEVICES+=",input:device$((i + 1))=0"
        else
            INPUT_CONFIG+=",input:maple_sdl_joystick_${i}=-1"
            DEVICES+=",input:device$((i + 1))=10"
        fi
    done
    echo "Totally Bazaar input: ${pads} gamepad(s) on ports A-$(printf "\\x$(printf %x $((64 + pads)))"); keyboard disabled."
else
    INPUT_CONFIG="input:maple_sdl_keyboard=0,input:maple_sdl_mouse=-1,input:maple_sdl_joystick_0=-1,input:maple_sdl_joystick_1=-1,input:maple_sdl_joystick_2=-1,input:maple_sdl_joystick_3=-1"
    DEVICES=",input:device1=0,input:device2=10,input:device3=10,input:device4=10"
    echo "Totally Bazaar input: keyboard on port A (pass-and-play); gamepads disabled."
fi

FLYCAST_CONFIG="config:Debug.SerialConsoleEnabled=yes${DEVICES},${INPUT_CONFIG}"
ELF_PATH="${PROJECT_DIR}/${ELF_NAME}"

flycast_log=""
flycast_pid=""
cleanup() {
    if [[ -n "${flycast_pid}" ]] && kill -0 "${flycast_pid}" 2>/dev/null; then
        kill -TERM "${flycast_pid}" 2>/dev/null || true
        wait "${flycast_pid}" 2>/dev/null || true
    fi
    if [[ -n "${flycast_log}" && -f "${flycast_log}" ]]; then
        rm -f -- "${flycast_log}"
    fi
}
trap cleanup EXIT

flycast_log="$(mktemp -t totally-bazaar-flycast.XXXXXX)"
set +e
if [[ -n "${EXIT_ON_LOG}" || -n "${FAIL_ON_LOG}" ]]; then
    qa_result=""
    "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${ELF_PATH}" \
        > >(tee "${flycast_log}") 2>&1 &
    flycast_pid="$!"
    while kill -0 "${flycast_pid}" 2>/dev/null; do
        if [[ -n "${FAIL_ON_LOG}" ]] && /usr/bin/grep -aFq -- "${FAIL_ON_LOG}" "${flycast_log}"; then
            qa_result="fail"
            kill -TERM "${flycast_pid}" 2>/dev/null || true
            break
        fi
        if [[ -n "${EXIT_ON_LOG}" ]] && /usr/bin/grep -aFq -- "${EXIT_ON_LOG}" "${flycast_log}"; then
            qa_result="pass"
            kill -TERM "${flycast_pid}" 2>/dev/null || true
            break
        fi
        sleep .2
    done
    wait "${flycast_pid}"
    flycast_status="$?"
    flycast_pid=""
    if [[ "${qa_result}" == "pass" ]]; then
        flycast_status=0
    elif [[ "${qa_result}" == "fail" ]]; then
        flycast_status=1
    fi
else
    "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${ELF_PATH}" 2>&1 | tee "${flycast_log}"
    flycast_status="${PIPESTATUS[0]}"
fi
set -e
exit "${flycast_status}"
