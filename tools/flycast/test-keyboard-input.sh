#!/usr/bin/env bash
set -euo pipefail
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/flycast-keyboard-test.XXXXXX")"
trap 'rm -rf "$TEST_DIR"' EXIT

# Exercise the exact helper shipped in the patch without cloning Flycast.
cd "$TEST_DIR"
git apply --include=core/input/keyboard_transition_queue.h "$TOOLS_DIR/keyboard-input.patch"
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    -I "$TEST_DIR/core/input" "$TOOLS_DIR/tests/keyboard-input.cpp" \
    -o "$TEST_DIR/keyboard-input-test"
"$TEST_DIR/keyboard-input-test"
