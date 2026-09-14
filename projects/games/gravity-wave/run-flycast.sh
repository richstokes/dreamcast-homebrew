#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_KOS_ENV="${HOME}/.local/share/dreamcast/kos/environ.sh"
KOS_ENV="${KOS_ENV:-${DEFAULT_KOS_ENV}}"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"
INPUT_MODE="${GRAVITY_WAVE_INPUT:-auto}"
SKIP_BUILD=false

usage() {
    cat <<'EOF'
Usage: ./run-flycast.sh [--skip-build] [--input auto|gamepad|keyboard]

Input defaults to auto: a connected macOS HID gamepad gets Dreamcast port A;
when none is detected, Flycast's keyboard mapping gets port A instead.
GRAVITY_WAVE_INPUT may also be set to auto, gamepad, or keyboard.
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

if [[ "${SKIP_BUILD}" != true ]]; then
    make -C "${PROJECT_DIR}"
fi

macos_gamepad_connected() {
    local devices

    [[ "$(uname -s)" == "Darwin" ]] || return 1
    [[ -x /usr/bin/hidutil ]] || return 1

    # Generic Desktop HID usages 4 and 5 are joysticks and gamepads. Exact
    # usage matching avoids mistaking the Mac's keyboard/trackpad for a pad.
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

# These are transient Flycast settings. A single emulated Dreamcast controller
# remains on port A while every unselected host input is explicitly unassigned.
# This keeps saved global mappings untouched and prevents merged keyboard/pad
# input. SDL gives the first connected joystick the stable launch-time id 0.
if [[ "${INPUT_MODE}" == "gamepad" ]]; then
    INPUT_CONFIG="input:maple_sdl_keyboard=-1,input:maple_sdl_mouse=-1,input:maple_sdl_joystick_0=0,input:maple_sdl_joystick_1=-1,input:maple_sdl_joystick_2=-1,input:maple_sdl_joystick_3=-1"
    echo "Gravity Wave input: using gamepad controls; keyboard controls disabled."
else
    INPUT_CONFIG="input:maple_sdl_keyboard=0,input:maple_sdl_mouse=-1,input:maple_sdl_joystick_0=-1,input:maple_sdl_joystick_1=-1,input:maple_sdl_joystick_2=-1,input:maple_sdl_joystick_3=-1"
    echo "Gravity Wave input: using keyboard controls; gamepad controls disabled."
fi

exec "${FLYCAST_BIN}" \
    -config "config:Debug.SerialConsoleEnabled=yes,input:device1=0,input:device2=10,input:device3=10,input:device4=10,${INPUT_CONFIG}" \
    "${PROJECT_DIR}/gravity-wave.elf"
