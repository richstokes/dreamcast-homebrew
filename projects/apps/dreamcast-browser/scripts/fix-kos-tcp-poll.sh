#!/usr/bin/env bash
# Remove the TCP receive/poll/socket-cleanup lock cycle without rebuilding
# unrelated SDK components or replacing existing network fixes.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PATCH_FILE="${SCRIPT_DIR}/../patches/kos-tcp-poll-lock.patch"
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
NET_DIR="${KOS_BASE}/kernel/net"
SDK_ARCHIVE="${KOS_BASE}/lib/${KOS_ARCH}/libkallisti.a"
if [[ ! -f "$SDK_ARCHIVE" || ! -f "${KOS_BASE}/kernel/build/net_tcp.o" ]]; then
    echo "Build this KOS SDK once before applying the targeted TCP poll fix." >&2
    exit 1
fi

if git -C "$KOS_BASE" apply --check "$PATCH_FILE" 2>/dev/null; then
    git -C "$KOS_BASE" apply "$PATCH_FILE"
    echo "Applied KOS TCP poll lock-order fix."
elif git -C "$KOS_BASE" apply --reverse --check "$PATCH_FILE" 2>/dev/null; then
    echo "KOS TCP poll lock-order fix is already applied."
else
    echo "This TCP source does not match the patch; no files were changed." >&2
    echo "Check whether the installed KOS version already includes the fix." >&2
    exit 1
fi

# Compile and stage just this object. Keep all other SDK objects, including
# the user's local BBA, TCP, keyboard, and PPP modifications.
make -C "$NET_DIR" defaultall OBJS=net_tcp.o SUBDIRS=
make -C "${KOS_BASE}/kernel" all SUBDIRS=
echo "Rebuilt $SDK_ARCHIVE with corrected TCP poll notifications."
echo "Relink Dreamcast applications to use the fix."
