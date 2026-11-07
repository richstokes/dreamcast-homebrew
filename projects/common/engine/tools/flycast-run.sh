# Sourced by a game's run-flycast.sh (bash, set -euo pipefail) once it has set:
#
#   PROJECT_DIR   the game's folder
#   GAME_PREFIX   DK, RL: the QA variables are then DK_ELF (another ELF in
#                 the folder), DK_EXIT_ON_LOG and DK_FAIL_ON_LOG (stop
#                 Flycast when that text appears on the serial console) and
#                 DK_FLYCAST_CONFIG (more -config settings, e.g.
#                 window:left=700,window:top=40 to place the window clear
#                 of another instance's, which would otherwise stop it
#                 presenting frames to capture)
#   ELF_DEFAULT   the ELF when ${GAME_PREFIX}_ELF is unset
#   usage()       the launcher's --help text
#
# Sourcing parses the arguments (--skip-build, -h) and sources KOS, leaving
# ELF_NAME, ELF_PATH, SKIP_BUILD and FLYCAST_CONFIG (the serial console on
# this terminal; append the game's own settings). Then:
#
#   launcher_build        make ELF_NAME unless --skip-build
#   launcher_run IMAGE    boot IMAGE (the ELF, or a disc made from it) in
#                         Flycast and exit with its status: 0 when the
#                         exit-on text appeared, 1 on the fail text, a KOS
#                         panic or an unhandled exception
#
# Only transient -config flags are used; Flycast's saved settings are never
# written.
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
# shellcheck disable=SC1091
source "$(dirname "${BASH_SOURCE[0]}")/../../../../tools/flycast/resolve.sh"
_v="${GAME_PREFIX}_ELF"; ELF_NAME="${!_v:-${ELF_DEFAULT}}"
_v="${GAME_PREFIX}_EXIT_ON_LOG"; EXIT_ON_LOG="${!_v:-}"
_v="${GAME_PREFIX}_FAIL_ON_LOG"; FAIL_ON_LOG="${!_v:-}"
_v="${GAME_PREFIX}_FLYCAST_CONFIG"; EXTRA_FLYCAST_CONFIG="${!_v:-}"
unset _v
ELF_PATH="${PROJECT_DIR}/${ELF_NAME}"
SKIP_BUILD=false

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

FLYCAST_CONFIG="config:Debug.SerialConsoleEnabled=yes"

launcher_build() {
    if [[ "${SKIP_BUILD}" != true ]]; then
        make -C "${PROJECT_DIR}" "${ELF_NAME}"
    fi
}

_flycast_log=""
_flycast_pid=""
_launcher_cleanup() {
    if [[ -n "${_flycast_pid}" ]] && kill -0 "${_flycast_pid}" 2>/dev/null; then
        kill -TERM "${_flycast_pid}" 2>/dev/null || true
        wait "${_flycast_pid}" 2>/dev/null || true
    fi
    [[ -n "${_flycast_log}" && -f "${_flycast_log}" ]] && rm -f -- "${_flycast_log}"
    return 0
}

launcher_run() {
    local image="$1"
    if [[ -n "${EXTRA_FLYCAST_CONFIG}" ]]; then
        FLYCAST_CONFIG+=",${EXTRA_FLYCAST_CONFIG}"
    fi
    trap _launcher_cleanup EXIT
    _flycast_log="$(mktemp -t "${ELF_NAME%.elf}-flycast.XXXXXX")"
    local status
    set +e
    if [[ -n "${EXIT_ON_LOG}" || -n "${FAIL_ON_LOG}" ]]; then
        local result=""
        "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${image}" \
            > >(tee "${_flycast_log}") 2>&1 &
        _flycast_pid="$!"
        while kill -0 "${_flycast_pid}" 2>/dev/null; do
            if [[ -n "${FAIL_ON_LOG}" ]] && /usr/bin/grep -aFq -- "${FAIL_ON_LOG}" "${_flycast_log}"; then
                result="fail"; kill -TERM "${_flycast_pid}" 2>/dev/null || true; break
            fi
            if /usr/bin/grep -aFq -- "KOS panic" "${_flycast_log}" || /usr/bin/grep -aFq -- "Unhandled exception" "${_flycast_log}"; then
                result="fail"; kill -TERM "${_flycast_pid}" 2>/dev/null || true; break
            fi
            if [[ -n "${EXIT_ON_LOG}" ]] && /usr/bin/grep -aFq -- "${EXIT_ON_LOG}" "${_flycast_log}"; then
                result="pass"; kill -TERM "${_flycast_pid}" 2>/dev/null || true; break
            fi
            sleep .2
        done
        wait "${_flycast_pid}"
        status="$?"
        _flycast_pid=""
        [[ "${result}" == "pass" ]] && status=0
        [[ "${result}" == "fail" ]] && status=1
    else
        "${FLYCAST_BIN}" -config "${FLYCAST_CONFIG}" "${image}" 2>&1 | tee "${_flycast_log}"
        status="${PIPESTATUS[0]}"
    fi
    set -e
    exit "${status}"
}
