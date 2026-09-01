#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_KOS_ENV="${HOME}/.local/share/dreamcast/kos/environ.sh"
KOS_ENV="${KOS_ENV:-${DEFAULT_KOS_ENV}}"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"
FLYCAST_STARTUP_RETRIES="${FLYCAST_STARTUP_RETRIES:-8}"
INPUT_MODE="${DRIFT_LA_INPUT:-auto}"
SKIP_BUILD=false

usage() {
    cat <<'EOF'
Usage: ./run-flycast.sh [--skip-build] [--input auto|gamepad|keyboard]

Input defaults to auto. A connected macOS HID joystick/gamepad exclusively
controls Dreamcast port A; when no pad is connected, Flycast's keyboard mapping
exclusively controls port A instead. DRIFT_LA_INPUT may also be set to auto,
gamepad, or keyboard.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build)
            SKIP_BUILD=true
            ;;
        --input)
            if [[ $# -lt 2 ]]; then
                echo "--input requires auto, gamepad, or keyboard." >&2
                exit 2
            fi
            INPUT_MODE="$2"
            shift
            ;;
        --input=*)
            INPUT_MODE="${1#--input=}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

case "${INPUT_MODE}" in
    auto|gamepad|keyboard)
        ;;
    *)
        echo "Invalid input mode '${INPUT_MODE}'; use auto, gamepad, or keyboard." >&2
        exit 2
        ;;
esac

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

if [[ "${SKIP_BUILD}" != true ]]; then
    make -C "${PROJECT_DIR}"
fi

macos_gamepad_connected() {
    local devices

    [[ "$(uname -s)" == "Darwin" ]] || return 1
    [[ -x /usr/bin/hidutil ]] || return 1

    # Generic Desktop HID usages 4 and 5 are joysticks and gamepads. Exact
    # matching avoids treating the built-in keyboard or trackpad as a pad.
    devices="$(/usr/bin/hidutil list --ndjson --matching \
        '[{"PrimaryUsagePage":1,"PrimaryUsage":4},{"PrimaryUsagePage":1,"PrimaryUsage":5}]' \
        2>/dev/null || true)"
    [[ -n "${devices}" ]]
}

if [[ "${INPUT_MODE}" == "auto" ]]; then
    if macos_gamepad_connected; then
        INPUT_MODE="gamepad"
    else
        INPUT_MODE="keyboard"
    fi
fi

# Keep one emulated Dreamcast controller on port A while assigning exactly one
# host input source. These are transient overrides: saved Flycast mappings are
# not edited, and keyboard/gamepad events can never be merged accidentally.
if [[ "${INPUT_MODE}" == "gamepad" ]]; then
    INPUT_CONFIG="input:maple_sdl_keyboard=-1,input:maple_sdl_mouse=-1,input:maple_sdl_joystick_0=0,input:maple_sdl_joystick_1=-1,input:maple_sdl_joystick_2=-1,input:maple_sdl_joystick_3=-1"
    echo "Drift Los Angeles input: using gamepad controls; keyboard controls disabled."
else
    INPUT_CONFIG="input:maple_sdl_keyboard=0,input:maple_sdl_mouse=-1,input:maple_sdl_joystick_0=-1,input:maple_sdl_joystick_1=-1,input:maple_sdl_joystick_2=-1,input:maple_sdl_joystick_3=-1"
    echo "Drift Los Angeles input: no gamepad detected; using keyboard controls."
fi

FLYCAST_CONFIG="config:Debug.SerialConsoleEnabled=yes,input:device1=0,input:device2=10,input:device3=10,input:device4=10,${INPUT_CONFIG}"

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
        -config "${FLYCAST_CONFIG}" \
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
