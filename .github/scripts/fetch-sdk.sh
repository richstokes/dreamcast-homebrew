#!/bin/sh

set -eu

# Checks out the pinned KallistiOS and kos-ports into .ci/ and applies the
# patches in tools/kos/patches. Run from the repository root with KOS_COMMIT
# and KOS_PORTS_COMMIT set, as the release workflow does. CI caches the built
# result keyed on this file, so a change here rebuilds the SDK.

mkdir -p .ci

git init .ci/kos
git -C .ci/kos remote add origin https://github.com/KallistiOS/KallistiOS.git
git -C .ci/kos fetch --depth 1 origin "$KOS_COMMIT"
git -C .ci/kos checkout --detach FETCH_HEAD

git init .ci/kos-ports
git -C .ci/kos-ports remote add origin https://github.com/KallistiOS/kos-ports.git
git -C .ci/kos-ports fetch --depth 1 origin "$KOS_PORTS_COMMIT"
git -C .ci/kos-ports checkout --detach FETCH_HEAD

tools/kos/apply-patches.sh .ci/kos .ci/kos-ports
