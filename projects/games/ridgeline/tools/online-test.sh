#!/bin/bash
# make online-test: Ridgeline's online mode in Flycast. Builds the server
# and its bot from server/, runs them on this Mac, builds the online
# autotest pointed at this Mac's LAN address (Flycast's network proxy can't
# reach 127.0.0.1) and boots it. The console joins lobby 9, where the bot
# drives a loop in the meadow, claims the summit, leaves and comes back;
# the CPU drives the console's truck, which jumps to the summit 25 s in.
# Passes when each has seen the other's truck and both trophies show.
#
#   RL_NET=modem      dial in through Flycast's emulated modem instead
#   RL_QA_LIVE=1      against the live server at dcvmu.com:7536
#   RL_QA_PORT=7594   the local server's port
#   RL_BOT_TRUCK=2    the bot's truck model (default 0, the Cascade 84)
set -euo pipefail

GAME="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${RL_QA_PORT:-7594}"
OUT="${GAME}/build/online-test"
mkdir -p "${OUT}"
pids=()
cleanup() {
    for p in ${pids[@]+"${pids[@]}"}; do kill "${p}" 2>/dev/null || true; done
}
trap cleanup EXIT

(cd "${GAME}/server" && go build -o "${OUT}/ridgeline-lobby" ./cmd/ridgeline-lobby && go build -o "${OUT}/rl-bot" ./cmd/rl-bot)

if [[ -n "${RL_QA_LIVE:-}" ]]; then
    ADDR="dcvmu.com:7536"
    FLAGS=""
else
    LAN_IP="$(ipconfig getifaddr en0 || ipconfig getifaddr en1)"
    ADDR="127.0.0.1:${PORT}"
    FLAGS="-DRL_NET_TEST_HOST=${LAN_IP} -DRL_NET_PORT=${PORT}"
    "${OUT}/ridgeline-lobby" -listen ":${PORT}" -words-extra BLORP > "${OUT}/server.log" 2>&1 &
    pids+=($!)
    # It answers once its bad word list is in.
    for i in $(seq 1 30); do
        "${OUT}/rl-bot" -addr "${ADDR}" -smoke > /dev/null 2>&1 && break
        sleep 1
    done
fi
echo "server ${ADDR}, logs in ${OUT}"

make -C "${GAME}" VARIANT=autotest ridgeline-autotest.elf EXTRA_CFLAGS="-DRL_AT_ONLINE -DRL_AT_EXPECT_TROPHIES=2 ${FLAGS}"
# Its own name, so captures and other sessions' Flycasts never mix it up.
cp "${GAME}/ridgeline-autotest.elf" "${GAME}/ridgeline-online.elf"

"${OUT}/rl-bot" -addr "${ADDR}" -name AT-BOT -lobby 9 -model "${RL_BOT_TRUCK:-0}" -summit-after 5s -leave-after 40s -away 6s -for 180s \
    -level "${GAME}/assets/generated/level.bin" > "${OUT}/bot.log" 2>&1 &
pids+=($!)

status=0
RL_ELF=ridgeline-online.elf RL_EXIT_ON_LOG='RL AUTOTEST: PASS' RL_FAIL_ON_LOG='RL AUTOTEST: FAIL' \
    "${GAME}/run-flycast.sh" --skip-build > "${OUT}/console.log" 2>&1 || status=$?
/usr/bin/grep -a 'RL ONLINE\|RL AUTOTEST' "${OUT}/console.log" || true
echo "bot:"
tail -5 "${OUT}/bot.log"
exit "${status}"
