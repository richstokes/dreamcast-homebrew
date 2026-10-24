#!/bin/sh

set -eu

# Checks out the pinned KallistiOS and kos-ports into .ci/ and applies the
# patches the projects need. Run from the repository root with KOS_COMMIT and
# KOS_PORTS_COMMIT set, as the release workflow does. CI caches the built
# result keyed on this file, so a change here rebuilds the SDK.

workspace=$(pwd)

mkdir -p .ci

git init .ci/kos
git -C .ci/kos remote add origin https://github.com/KallistiOS/KallistiOS.git
git -C .ci/kos fetch --depth 1 origin "$KOS_COMMIT"
git -C .ci/kos checkout --detach FETCH_HEAD

for patch in projects/dcvmu.com/client/patches/kos-*.patch; do
    git -C .ci/kos apply --check "$workspace/$patch"
    git -C .ci/kos apply "$workspace/$patch"
done

for patch in \
    projects/apps/dreamcast-browser/patches/kos-keyboard-attach.patch \
    projects/apps/dreamcast-browser/patches/kos-tcp-poll-lock.patch \
    projects/apps/dreamcast-browser/patches/kos-tcp-orphan-reset.patch \
    projects/apps/pico8-dreamcast/patches/kos-maple-rx-cache.patch \
    projects/games/dreamkarts/patches/kos-dhcp-fast-retry.patch; do
    git -C .ci/kos apply --check "$workspace/$patch"
    git -C .ci/kos apply "$workspace/$patch"
done

git init .ci/kos-ports
git -C .ci/kos-ports remote add origin https://github.com/KallistiOS/kos-ports.git
git -C .ci/kos-ports fetch --depth 1 origin "$KOS_PORTS_COMMIT"
git -C .ci/kos-ports checkout --detach FETCH_HEAD
git -C .ci/kos-ports apply --check \
    "$workspace/projects/apps/dreamcast-browser/patches/kos-ports-kos23.patch"
git -C .ci/kos-ports apply \
    "$workspace/projects/apps/dreamcast-browser/patches/kos-ports-kos23.patch"
