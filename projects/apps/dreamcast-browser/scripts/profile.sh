#!/usr/bin/env bash
# Build the local performance self-test and run it with serial profiling.
# This leaves a profiling ELF: clean/rebuild before the next normal release.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"

usage() {
    printf 'Usage: %s [LOG]\n' "$0"
    printf 'Build BROWSER_PROFILE + BROWSER_PERF_SELF_TEST and launch Flycast.\n'
    printf 'Optionally tee serial output to LOG. No remote test server is needed.\n'
    printf 'After profiling, source KOS and run make clean && make in the project.\n'
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    usage
    exit 0
fi
if [[ "$#" -gt 1 || "${1:-}" == -* ]]; then
    usage >&2
    exit 2
fi
if [[ ! -f "$KOS_ENV" ]]; then
    printf 'KallistiOS environment not found: %s\n' "$KOS_ENV" >&2
    exit 1
fi

set +u
# shellcheck disable=SC1090
source "$KOS_ENV"
set -u

make -C "$PROJECT_DIR" clean
make -C "$PROJECT_DIR" CPPFLAGS='-DBROWSER_PROFILE -DBROWSER_PERF_SELF_TEST'

# Keep this reminder visible even when Flycast exits with an error.
trap 'printf "Profiling build remains. Source KOS, then run: make -C %q clean && make -C %q\n" "$PROJECT_DIR" "$PROJECT_DIR" >&2' EXIT
if [[ "$#" -eq 1 ]]; then
    "$PROJECT_DIR/run-flycast.sh" --skip-build 2>&1 | tee -- "$1"
else
    "$PROJECT_DIR/run-flycast.sh" --skip-build
fi
