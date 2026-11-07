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
RL_FLYCAST_CONFIG adds more -config settings (window:left=700,... to place
the window clear of another instance's).
USAGE
}

source "$PROJECT_DIR/../../common/engine/tools/flycast-run.sh"
launcher_build
launcher_run "${ELF_PATH}"
