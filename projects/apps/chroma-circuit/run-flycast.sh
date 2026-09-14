#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"
INPUT_MODE="${CHROMA_CIRCUIT_INPUT:-auto}"
SKIP_BUILD=false

usage() {
    cat <<'EOF'
Usage: ./run-flycast.sh [--skip-build] [--input auto|gamepad|keyboard]

Input defaults to auto. A connected macOS USB HID joystick/gamepad exclusively
controls Dreamcast port A; otherwise Flycast's keyboard mapping exclusively
controls port A. CHROMA_CIRCUIT_INPUT may also select an input mode.
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

if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    exit 1
fi

if [[ "${SKIP_BUILD}" != true ]]; then
    make -C "${PROJECT_DIR}"
fi

macos_usb_gamepad_connected() {
    local devices

    [[ "$(uname -s)" == "Darwin" ]] || return 1
    [[ -x /usr/bin/hidutil ]] || return 1

    # Generic Desktop usages 4 and 5 are joysticks and gamepads. Requiring
    # Transport=USB avoids assigning stale joystick zero for an absent pad and
    # deliberately leaves Bluetooth-only controllers on the keyboard path.
    devices="$(/usr/bin/hidutil list --ndjson --matching \
        '[{"PrimaryUsagePage":1,"PrimaryUsage":4,"Transport":"USB"},{"PrimaryUsagePage":1,"PrimaryUsage":5,"Transport":"USB"}]' \
        2>/dev/null || true)"
    [[ -n "${devices}" ]]
}

if [[ "${INPUT_MODE}" == "auto" ]]; then
    if macos_usb_gamepad_connected; then
        INPUT_MODE="gamepad"
    else
        INPUT_MODE="keyboard"
    fi
fi

# Keep one emulated Dreamcast controller on port A while assigning exactly one
# host source. These transient overrides do not change saved Flycast mappings.
if [[ "${INPUT_MODE}" == "gamepad" ]]; then
    INPUT_CONFIG="input:maple_sdl_keyboard=-1,input:maple_sdl_mouse=-1,input:maple_sdl_joystick_0=0,input:maple_sdl_joystick_1=-1,input:maple_sdl_joystick_2=-1,input:maple_sdl_joystick_3=-1"
    echo "Chroma Circuit input: using USB gamepad; keyboard controls disabled."
else
    INPUT_CONFIG="input:maple_sdl_keyboard=0,input:maple_sdl_mouse=-1,input:maple_sdl_joystick_0=-1,input:maple_sdl_joystick_1=-1,input:maple_sdl_joystick_2=-1,input:maple_sdl_joystick_3=-1"
    echo "Chroma Circuit input: no USB gamepad selected; using keyboard controls."
fi

# Flycast 2.7 stores ta.skip as an integer, but "no" safely falls back to its
# zero default and keeps this override transient. On macOS, Flycast can also
# intermittently fail its dynarec VM reservation before parsing any options;
# exit status 6 identifies that host-side assertion, so retry a fresh ASLR
# layout instead of mistaking it for a Dreamcast-side crash.
FLYCAST_CONFIG="config:Debug.SerialConsoleEnabled=yes,config:rend.vsync=yes,config:rend.DupeFrames=no,config:ta.skip=no,config:pvr.AutoSkipFrame=0,input:device1=0,input:device2=10,input:device3=10,input:device4=10,${INPUT_CONFIG}"
MAX_START_ATTEMPTS=8

for ((attempt = 1; attempt <= MAX_START_ATTEMPTS; attempt++)); do
    attempt_dir="$(mktemp -d "${TMPDIR:-/tmp}/chroma-circuit.XXXXXX")"
    attempt_log="${attempt_dir}/flycast.log"

    # Keep serial output visible while retaining enough startup text to detect
    # Flycast's assertion. A failed process can otherwise remain in its crash
    # handler indefinitely instead of returning status 6 to this wrapper.
    "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" \
        "${PROJECT_DIR}/chroma-circuit.elf" > >(tee "${attempt_log}") 2>&1 &
    flycast_pid=$!
    vm_failure=0
    process_exited=0

    for ((probe = 0; probe < 30; probe++)); do
        if [[ -s "${attempt_log}" ]] && grep -q "driver.cpp : 349" "${attempt_log}"; then
            vm_failure=1
            break
        fi
        if ! kill -0 "${flycast_pid}" 2>/dev/null; then
            process_exited=1
            break
        fi
        sleep 0.1
    done

    if (( vm_failure )); then
        kill -KILL "${flycast_pid}" 2>/dev/null || true
        wait "${flycast_pid}" 2>/dev/null || true
        rm -f "${attempt_log}"
        rmdir "${attempt_dir}" 2>/dev/null || true
        if (( attempt == MAX_START_ATTEMPTS )); then
            echo "Flycast dynarec VM reservation failed after ${attempt} attempts." >&2
            exit 6
        fi
        echo "Flycast dynarec VM reservation failed; retrying (${attempt}/${MAX_START_ATTEMPTS})..." >&2
        continue
    fi

    # Once startup survives the probe, behave like a normal foreground
    # launcher: show live serial output and forward terminal interrupts.
    trap 'kill -INT "${flycast_pid}" 2>/dev/null || true' INT TERM HUP
    set +e
    wait "${flycast_pid}"
    status=$?
    set -e
    trap - INT TERM HUP
    rm -f "${attempt_log}"
    rmdir "${attempt_dir}" 2>/dev/null || true

    if (( process_exited && status == 6 && attempt < MAX_START_ATTEMPTS )); then
        echo "Flycast exited during startup; retrying (${attempt}/${MAX_START_ATTEMPTS})..." >&2
        continue
    fi
    exit "${status}"
done
