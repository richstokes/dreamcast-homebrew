#!/usr/bin/env bash
# Fix the KOS keyboard attach overrun without rebuilding unrelated drivers.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_FILE="${SCRIPT_DIR}/../patches/kos-keyboard-attach.patch"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"

if [[ ! -f "$KOS_ENV" ]]; then
    echo "KallistiOS environment not found: $KOS_ENV" >&2
    exit 1
fi
set +u
# shellcheck disable=SC1090
source "$KOS_ENV"
set -u

if [[ "$KOS_ARCH" != dreamcast ]]; then
    echo "This fix requires a Dreamcast KOS environment." >&2
    exit 1
fi
MAPLE_DIR="${KOS_BASE}/kernel/arch/dreamcast/hardware/maple"
SDK_ARCHIVE="${KOS_BASE}/lib/${KOS_ARCH}/libkallisti.a"
if [[ ! -f "$SDK_ARCHIVE" || ! -f "${KOS_BASE}/kernel/build/keyboard.o" ]]; then
    echo "Build this KOS SDK once before applying the targeted keyboard fix." >&2
    exit 1
fi

if git -C "$KOS_BASE" apply --check "$PATCH_FILE" 2>/dev/null; then
    git -C "$KOS_BASE" apply "$PATCH_FILE"
    echo "Applied KOS keyboard attach fix."
elif git -C "$KOS_BASE" apply --reverse --check "$PATCH_FILE" 2>/dev/null; then
    echo "KOS keyboard attach fix is already applied."
else
    echo "This keyboard source does not match the patch; no files were changed." >&2
    echo "Check whether the installed KOS version already includes the fix." >&2
    exit 1
fi

# KOS's normal prefab target compiles keyboard.o and stages it in kernel/build.
# Suppress recursion so this repair never rebuilds other Maple drivers.
make -C "$MAPLE_DIR" defaultall OBJS=keyboard.o SUBDIRS=

# Repack the SDK libraries with KOS's own target and existing objects. Skipping
# subdirectories preserves unrelated SDK changes, including local networking.
make -C "${KOS_BASE}/kernel" all SUBDIRS=
echo "Rebuilt $SDK_ARCHIVE with the corrected keyboard driver."
echo "Relink Dreamcast applications to use the fix."
