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

Builds DreamKarts (unless --skip-build), wraps the ELF and the music's
CD-DA tracks (cdda/, fetched by make music) in a throwaway CDI (build/cdi/,
with mkdcdisc: tools/mkdcdisc/build-macos.sh) and boots it in Flycast with
the serial console on this terminal. The music is on the disc as audio
tracks, so a bare ELF would run silent. Only transient -config flags are
used.

QA hooks: DK_ELF picks another ELF in this folder; DK_EXIT_ON_LOG and
DK_FAIL_ON_LOG stop Flycast when that text appears on the serial console.
Online play goes through Flycast's emulated broadband adapter by default,
whatever Flycast's saved network settings say. DK_NET=modem uses the
emulated modem instead (both through Flycast's local proxy, not DCNet).
DK_SHOW_VMU=1 shows the VMU screens over the game. DK_KEYBOARD=1 plugs a
Dreamcast keyboard into port B, fed by this Mac's keyboard (lobby chat).
DK_VMU_DIR=<folder> uses that folder's memory cards instead of the real
ones; it must already hold vmu_save_A1.bin to D2.bin (blank 131072-byte
files do), or Flycast moves the real cards there. DK_FLYCAST_CONFIG adds
more -config settings (e.g. window:left=700,window:top=40 to place the
window clear of another instance's, which would otherwise stop it
presenting frames to capture).
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
make -C "${PROJECT_DIR}" music

FLYCAST_CONFIG="config:Debug.SerialConsoleEnabled=yes"
case "${DK_NET:-bba}" in
    bba) FLYCAST_CONFIG+=",network:EmulateBBA=yes,network:DCNet=no" ;;
    modem) FLYCAST_CONFIG+=",network:EmulateBBA=no,network:DCNet=no" ;;
    *) echo "DK_NET must be bba or modem" >&2; exit 2 ;;
esac
if [[ "${DK_SHOW_VMU:-}" == 1 ]]; then
    FLYCAST_CONFIG+=",config:rend.FloatVMUs=yes"
fi
if [[ "${DK_KEYBOARD:-}" == 1 ]]; then
    FLYCAST_CONFIG+=",input:device2=5,input:maple_sdl_keyboard=1"
fi
if [[ -n "${DK_VMU_DIR:-}" ]]; then
    for slot in A1 A2 B1 B2 C1 C2 D1 D2; do
        [[ -f "${DK_VMU_DIR}/vmu_save_${slot}.bin" ]] || { echo "DK_VMU_DIR lacks vmu_save_${slot}.bin (Flycast would move the real card)" >&2; exit 2; }
    done
    FLYCAST_CONFIG+=",config:Dreamcast.VMUPath=${DK_VMU_DIR}"
fi
if [[ -n "${DK_FLYCAST_CONFIG:-}" ]]; then
    FLYCAST_CONFIG+=",${DK_FLYCAST_CONFIG}"
fi
ELF_PATH="${PROJECT_DIR}/${ELF_NAME}"
# The disc: the music's audio tracks, then the ELF. Rebuilt when the ELF or
# any track is newer than the last image (~210 MB of audio, a few seconds).
CDI_PATH="${PROJECT_DIR}/build/cdi/${ELF_NAME%.elf}.cdi"
stale=false
if [[ ! -f "${CDI_PATH}" || "${ELF_PATH}" -nt "${CDI_PATH}" ]]; then stale=true; fi
for track in "${PROJECT_DIR}"/cdda/*.wav; do
    [[ "${track}" -nt "${CDI_PATH}" ]] && stale=true
done
if [[ "${stale}" == true ]]; then
    "${PROJECT_DIR}/../../../tools/flycast/elf-to-cdi.sh" --cdda "${PROJECT_DIR}/cdda" "${ELF_PATH}" "${CDI_PATH}"
fi

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
    "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${CDI_PATH}" \
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
    "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${CDI_PATH}" 2>&1 | tee "${flycast_log}"
    flycast_status="${PIPESTATUS[0]}"
fi
set -e
exit "${flycast_status}"
