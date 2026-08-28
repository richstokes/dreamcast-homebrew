#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"

if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    exit 1
fi

if [[ "${1:-}" != "--skip-build" ]]; then
    make -C "${PROJECT_DIR}"
fi

# Flycast 2.7 stores ta.skip as an integer, but "no" safely falls back to its
# zero default and keeps this override transient. On macOS, Flycast can also
# intermittently fail its dynarec VM reservation before parsing any options;
# exit status 6 identifies that host-side assertion, so retry a fresh ASLR
# layout instead of mistaking it for a Dreamcast-side crash.
FLYCAST_CONFIG="config:Debug.SerialConsoleEnabled=yes,config:rend.vsync=yes,config:rend.DupeFrames=no,config:ta.skip=no,config:pvr.AutoSkipFrame=0"
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
