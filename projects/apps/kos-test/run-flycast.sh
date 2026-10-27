#!/usr/bin/env bash
# Boot KOS_Test in Flycast with the Broadband Adapter.
#
#   ./run-flycast.sh [--skip-build] [--modem] [--dcnet]
#     --modem     emulate the modem instead of the BBA (Flycast's PPP peer)
#     --dcnet     BBA over the DCNet VPN instead of the local TCP/UDP proxy
#
# Scripted runs:
#   KT_ELF=kos-test-autotest.elf          which ELF to boot
#   KT_EXIT_ON_LOG / KT_FAIL_ON_LOG       stop Flycast when this serial line appears
#   KT_TEST_SECONDS=600                   give up after this long
#   KT_BLANK_VMUS=1                       run on blank scratch VMUs, never the real cards
#   KT_KEYBOARD=1                         feed the Mac keyboard to a Dreamcast keyboard on port B
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
source "$PROJECT_DIR/../../../tools/flycast/resolve.sh"

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

# KOS's environment scripts probe optional variables that may be unset.
set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u

SKIP_BUILD=no
NETWORK="network:EmulateBBA=yes,network:DCNet=no"
for argument in "$@"; do
    case "${argument}" in
        --skip-build) SKIP_BUILD=yes ;;
        --modem) NETWORK="network:EmulateBBA=no,network:DCNet=no" ;;
        --dcnet) NETWORK="network:EmulateBBA=yes,network:DCNet=yes" ;;
        *)
            echo "Usage: ${0##*/} [--skip-build] [--modem] [--dcnet]" >&2
            exit 1
            ;;
    esac
done

if [[ "${SKIP_BUILD}" != yes ]]; then
    make -C "${PROJECT_DIR}"
fi

ELF="${PROJECT_DIR}/${KT_ELF:-kos-test.elf}"
CONFIG="${NETWORK},config:Debug.SerialConsoleEnabled=yes,config:UploadCrashLogs=no"

if [[ "${KT_KEYBOARD:-}" == 1 ]]; then
    CONFIG="${CONFIG},input:device2=5,input:maple_sdl_keyboard=1"
fi

vmus=""
if [[ "${KT_BLANK_VMUS:-}" == 1 ]]; then
    # Flycast moves the real card into an empty VMUPath folder and deletes
    # the original, so every slot gets its own blank card first.
    vmus="$(mktemp -d -t kos-test-vmus.XXXXXX)"
    for slot in A1 A2 B1 B2 C1 C2 D1 D2; do
        head -c 131072 /dev/zero > "${vmus}/vmu_save_${slot}.bin"
    done
    CONFIG="${CONFIG},config:Dreamcast.VMUPath=${vmus},config:PerGameVmu=no"
fi

EXIT_ON_LOG="${KT_EXIT_ON_LOG:-}"
FAIL_ON_LOG="${KT_FAIL_ON_LOG:-}"
if [[ -z "${EXIT_ON_LOG}" && -z "${FAIL_ON_LOG}" && -z "${vmus}" ]]; then
    exec "${FLYCAST_BIN}" -config "${CONFIG}" "${ELF}"
fi

log="$(mktemp -t kos-test-flycast.XXXXXX)"
pid=""
cleanup() {
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -TERM "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    fi
    rm -f -- "${log}"
    if [[ -n "${vmus}" ]]; then
        rm -f -- "${vmus}"/vmu_save_*.bin
        rmdir -- "${vmus}" 2>/dev/null || true
    fi
}
trap cleanup EXIT

"${FLYCAST_BIN}" -config "${CONFIG}" "${ELF}" > >(tee "${log}") 2>&1 &
pid="$!"
deadline=$((SECONDS + ${KT_TEST_SECONDS:-600}))
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
[[ "${result}" == "pass" || -z "${EXIT_ON_LOG}${FAIL_ON_LOG}" ]]
