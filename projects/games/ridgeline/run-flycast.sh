#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_PREFIX=RL
ELF_DEFAULT=ridgeline.elf

usage() {
    cat <<'USAGE'
Usage: ./run-flycast.sh [--skip-build]

Builds Ridgeline (unless --skip-build) and boots the ELF in Flycast with
the serial console on this terminal. Only transient -config flags are used.

QA hooks: RL_ELF picks another ELF in this folder; RL_EXIT_ON_LOG and
RL_FAIL_ON_LOG stop Flycast when that text appears on the serial console.
Online play goes through Flycast's emulated broadband adapter by default,
whatever Flycast's saved network settings say; RL_NET=modem uses the
emulated modem instead (both through Flycast's local proxy, not DCNet).
RL_FLYCAST_CONFIG adds more -config settings (window:left=700,... to place
the window clear of another instance's).
USAGE
}

source "$PROJECT_DIR/../../common/engine/tools/flycast-run.sh"
launcher_build
case "${RL_NET:-bba}" in
    bba) FLYCAST_CONFIG+=",network:EmulateBBA=yes,network:DCNet=no" ;;
    modem) FLYCAST_CONFIG+=",network:EmulateBBA=no,network:DCNet=no" ;;
    *) echo "RL_NET must be bba or modem" >&2; exit 2 ;;
esac
launcher_run "${ELF_PATH}"
