#!/bin/bash
# Online QA: every online autotest scenario, one after another, each in its
# own Flycast against a lobby server, with the bots each needs.
#
#   tools/online-qa.sh [scenario ...]        (make online-qa)
#
# Scenarios (default: all that fit the mode):
#   race      join a lobby, chat with the bot (the filter stars its second
#             line), race it, get a place and see its skid marks
#   custom    the same with a custom kart on both sides
#   practice  start alone, drop out, leave and rejoin, race alone until the
#             bot joins, drop out, race it, see it drop out, drop out
#   spectate  watch the demo lobby through a race
#   watch     watch two bots race in a lobby of our own, braking every few
#             seconds: their skid marks must show on the spectator cameras
#   reject    a server that moved on to another protocol: the console must
#             show THIS VERSION IS TOO OLD, not a lost connection
#   restart   the server restarts mid-race and repeats its race ids: the
#             console must come back through the browser and get into the
#             next race
#
# Local (default): builds and runs the server from server/ on UDP
# DK_QA_PORT (7535; another session's server there stops the run) and the
# consoles reach it at DK_LAN_IP (en0's address; Flycast's proxy cannot
# reach 127.0.0.1). DK_QA_LIVE=1 runs race, custom, practice and spectate
# against dcvmu.com instead (reject and restart need a server of our own).
# Logs land in captures/online-qa/. Exit 1 if any scenario fails.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="${PROJECT_DIR}/server"
KOS_ENV="${KOS_ENV:-$HOME/.local/share/dreamcast/kos/environ.sh}"
LIVE="${DK_QA_LIVE:-0}"
PORT="${DK_QA_PORT:-7535}"
TIMEOUT="${DK_QA_TIMEOUT:-360}"   # seconds a scenario may take (spectate gets 600: it may catch a race mid-way)
OUT="${PROJECT_DIR}/captures/online-qa"
mkdir -p "${OUT}"

if [[ "${LIVE}" == 1 ]]; then
    ADDR="dcvmu.com:7535"
    HOST_FLAGS="-DDK_AT_LIVE"
    scenarios=(race custom practice spectate)
else
    LAN_IP="${DK_LAN_IP:-$(ipconfig getifaddr en0 2>/dev/null || true)}"
    [[ -n "${LAN_IP}" ]] || { echo "DK_LAN_IP: no en0 address; set it to this Mac's LAN address" >&2; exit 2; }
    ADDR="127.0.0.1:${PORT}"
    HOST_FLAGS="-DDK_NET_TEST_HOST=${LAN_IP} -DDK_NET_PORT=${PORT}"
    scenarios=(race custom practice spectate watch reject restart)
fi
[[ $# -gt 0 ]] && scenarios=("$@")

set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u

tmp="$(mktemp -d)"
pids=()
server_pid=""
cleanup() {
    stop_server
    stop_bots
    rm -rf "${tmp}"
}
trap cleanup EXIT

echo "== build server and bot"
go build -C "${SERVER_DIR}" -o "${tmp}/lobby" ./cmd/dreamkarts-lobby
go build -C "${SERVER_DIR}" -o "${tmp}/bot" ./cmd/dk-bot

start_server() {   # start_server <log> [server flags]
    local log="$1"; shift
    if [[ "${LIVE}" == 1 ]]; then return 0; fi
    if lsof -nP -iUDP:"${PORT}" >/dev/null 2>&1; then
        echo "UDP ${PORT} is in use (another session's server?); set DK_QA_PORT" >&2
        exit 2
    fi
    "${tmp}/lobby" -listen ":${PORT}" -words-extra BLORP "$@" >> "${log}" 2>&1 &
    server_pid="$!"
    for _ in $(seq 1 50); do
        /usr/bin/grep -q "on \[::\]:${PORT}" "${log}" 2>/dev/null && return 0
        kill -0 "${server_pid}" 2>/dev/null || { echo "server died:"; cat "${log}"; exit 1; } >&2
        sleep 0.2
    done
    echo "server did not start:" >&2; cat "${log}" >&2; exit 1
}
stop_server() {
    if [[ -n "${server_pid}" ]] && kill -0 "${server_pid}" 2>/dev/null; then
        kill "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
    fi
    server_pid=""
}
stop_bots() {
    for p in ${pids[@]+"${pids[@]}"}; do
        kill "${p}" 2>/dev/null || true
        wait "${p}" 2>/dev/null || true   # also swallows bash's Terminated line
    done
    pids=()
}
bot() {   # bot <delay-seconds> [bot flags]: runs in the background
    local delay="$1"; shift
    ( sleep "${delay}"; exec "${tmp}/bot" -addr "${ADDR}" "$@" ) >> "${OUT}/${scenario}-bot.log" 2>&1 &
    pids+=("$!")
}
build_client() {   # build_client <cflags>
    make -C "${PROJECT_DIR}" -j8 VARIANT=autotest dreamkarts-autotest.elf EXTRA_CFLAGS="$1 ${HOST_FLAGS}" > "${OUT}/${scenario}-build.log" 2>&1 ||
        { echo "build failed, see ${OUT}/${scenario}-build.log" >&2; return 1; }
}
start_bots() {
    [[ ${#bot_args[@]} -gt 0 ]] && bot "${bot_delay}" ${bot_args[@]+"${bot_args[@]}"}
    [[ ${#bot2_args[@]} -gt 0 ]] && bot "${bot_delay}" "${bot2_args[@]}"
    return 0
}
run_client() {   # run_client [cue]: Flycast until PASS/FAIL; on the cue line, restart the server
    local cue="${1:-}" log="${OUT}/${scenario}.log" start cued=0 bots_cued=0 limit="${TIMEOUT}"
    [[ "${scenario}" == spectate && "${limit}" -lt 600 ]] && limit=600
    : > "${log}"
    DK_ELF=dreamkarts-autotest.elf DK_EXIT_ON_LOG='DK AUTOTEST: PASS' DK_FAIL_ON_LOG='DK AUTOTEST: FAIL' \
        "${PROJECT_DIR}/run-flycast.sh" --skip-build > "${log}" 2>&1 &
    local lp="$!"
    start="$(date +%s)"
    while kill -0 "${lp}" 2>/dev/null; do
        if [[ -n "${cue}" && "${cued}" == 0 ]] && /usr/bin/grep -aFq -- "${cue}" "${log}"; then
            cued=1
            echo "   cue seen: restarting the server"
            stop_server
            start_server "${OUT}/${scenario}-server.log" ${server_flags[@]+"${server_flags[@]}"}
        fi
        if [[ -n "${bot_cue}" && "${bots_cued}" == 0 ]] && /usr/bin/grep -aFq -- "${bot_cue}" "${log}"; then
            bots_cued=1
            start_bots
        fi
        if (( $(date +%s) - start > limit )); then
            echo "   timed out after ${limit} s"
            kill "${lp}" 2>/dev/null || true
            wait "${lp}" 2>/dev/null || true
            return 1
        fi
        sleep 0.5
    done
    wait "${lp}"
}

results=()
failed=0
for scenario in "${scenarios[@]}"; do
    echo "== ${scenario}"
    server_flags=()
    cue=""
    bot_cue=""   # a console line to start the bots on, instead of at launch
    want=""   # a line the console log must also have to pass
    bot2_args=()
    case "${scenario}" in
        race)
            cflags="-DDK_AT_ONLINE"
            bot_args=(-name AT-SONIC -lobby 8 -start -start-delay 28s -race 69 -skid -path "${SERVER_DIR}/cmd/dk-bot/emerald-beach.path"
                      -chat "HELLO FROM BOT;YOU ARE A BLORPHEAD" -for 5m)
            want="[1-9][0-9]* remote skid marks"
            bot_delay=0 ;;
        custom)
            cflags="-DDK_AT_ONLINE -DDK_AT_CUSTOM"
            bot_args=(-name AT-SONIC -lobby 8 -start -start-delay 28s -race 69 -skid -path "${SERVER_DIR}/cmd/dk-bot/emerald-beach.path"
                      -chat "HELLO FROM BOT;YOU ARE A BLORPHEAD" -colour 8 -custom 2,3,4 -for 5m)
            want="[1-9][0-9]* remote skid marks"
            bot_delay=0 ;;
        practice)
            cflags="-DDK_AT_ONLINE -DDK_AT_PRACTICE -DDK_AT_LOBBY=6"
            # The bot must arrive during the race after the rejoin: earlier,
            # it takes the lead when the console leaves.
            bot_args=(-name AT-SONIC -lobby 6 -retire 6s -for 150s)
            bot_cue="(the first was race"
            bot_delay=2 ;;
        spectate)
            cflags="-DDK_AT_ONLINE -DDK_AT_SPECTATE"
            bot_args=()
            bot_delay=0 ;;
        watch)
            [[ "${LIVE}" == 1 ]] && { echo "   needs a local server, skipped"; continue; }
            cflags="-DDK_AT_ONLINE -DDK_AT_SPECTATE -DDK_AT_LOBBY=7"
            bot_args=(-name AT-SONIC -lobby 7 -start -start-delay 28s -race 69 -skid -path "${SERVER_DIR}/cmd/dk-bot/emerald-beach.path" -for 5m)
            bot2_args=(-name AT-TAILS -lobby 7 -start -start-delay 28s -race 72 -skid -path "${SERVER_DIR}/cmd/dk-bot/emerald-beach.path" -for 5m)
            want="[1-9][0-9]* remote skid marks"
            bot_delay=0 ;;
        reject)
            [[ "${LIVE}" == 1 ]] && { echo "   needs a local server, skipped"; continue; }
            cflags="-DDK_AT_ONLINE -DDK_AT_REJECT"
            bot_args=()
            server_flags=(-reject-proto 9) ;;
        restart)
            [[ "${LIVE}" == 1 ]] && { echo "   needs a local server, skipped"; continue; }
            cflags="-DDK_AT_ONLINE -DDK_AT_RESTART -DDK_AT_LOBBY=5"
            bot_args=()
            server_flags=(-race-seed 40)
            cue="restart the server now" ;;
        *) echo "unknown scenario ${scenario}" >&2; exit 2 ;;
    esac
    status=1
    if [[ "${LIVE}" == 1 && ${#results[@]} -gt 0 ]]; then
        # A killed Flycast sends no LEAVE: let the live server time its
        # console out (10 s) so the lobby is empty for the next scenario.
        sleep 12
    fi
    if build_client "${cflags}"; then
        start_server "${OUT}/${scenario}-server.log" ${server_flags[@]+"${server_flags[@]}"}
        [[ -n "${bot_cue}" ]] || start_bots
        run_client "${cue}" && status=0 || status=$?
        if [[ "${status}" == 0 && -n "${want}" ]] && ! /usr/bin/grep -aq -- "${want}" "${OUT}/${scenario}.log"; then
            echo "   log has no \"${want}\""
            status=1
        fi
        stop_server
        stop_bots
    fi
    verdict="$(/usr/bin/grep -a "DK AUTOTEST: \|DK ONLINE: error\|DK PRACTICE: .*[0-9], [0-9]* in the lobby\|DK RESTART: in race\|DK ONLINE: race over" "${OUT}/${scenario}.log" 2>/dev/null | tail -2 | tr '\n' ' ' || true)"
    [[ -n "${verdict}" ]] || verdict="no verdict line; see ${OUT}/${scenario}.log"
    if [[ "${status}" == 0 ]]; then
        results+=("PASS ${scenario}: ${verdict}")
    else
        results+=("FAIL ${scenario}: ${verdict}")
        failed=1
    fi
    echo "   ${results[${#results[@]}-1]}"
done

echo "== online QA ($( [[ "${LIVE}" == 1 ]] && echo "live ${ADDR}" || echo "local ${ADDR}" ))"
printf '%s\n' "${results[@]}"
exit "${failed}"
