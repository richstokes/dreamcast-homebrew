#!/usr/bin/env bash
# stock-sdk.sh - an unpatched KallistiOS tree beside the installed one, for
# A/B runs that show what tools/kos/patches fix (KOS_Test's make auto-stock).
#
#   tools/kos/stock-sdk.sh             upstream master, fetched now
#   tools/kos/stock-sdk.sh e7ab1a2     any commit, e.g. the pinned one
#
# The tree is a shared clone of the installed KOS repo (STOCK_KOS, default
# ~/.local/share/dreamcast/kos-stock), so the installed tree and its patches
# are never touched. It gets an environ.sh that is the installed one pointed
# at it, and its kernel, addons, bin2c and genromfs are built. Rerunning on
# the same commit only rebuilds what changed.
set -euo pipefail

KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
STOCK="${STOCK_KOS:-${HOME}/.local/share/dreamcast/kos-stock}"
UPSTREAM=https://github.com/KallistiOS/KallistiOS.git
REF="${1:-master}"

set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u
SRC="${KOS_BASE}"

if [[ ! -d "${STOCK}/.git" ]]; then
    echo "stock-sdk: cloning ${SRC} into ${STOCK}"
    git clone --quiet --shared --no-checkout "${SRC}" "${STOCK}"
fi
if [[ "${REF}" == master ]]; then
    git -C "${STOCK}" fetch --quiet "${UPSTREAM}" master
    commit="$(git -C "${STOCK}" rev-parse FETCH_HEAD)"
else
    commit="$(git -C "${STOCK}" rev-parse --verify "${REF}^{commit}" 2>/dev/null)" || {
        git -C "${STOCK}" fetch --quiet "${UPSTREAM}"
        commit="$(git -C "${STOCK}" rev-parse --verify "${REF}^{commit}")"
    }
fi

if [[ "$(git -C "${STOCK}" rev-parse HEAD 2>/dev/null || true)" != "${commit}" ]]; then
    echo "stock-sdk: checking out ${commit:0:7} (clean build)"
    git -C "${STOCK}" checkout --quiet --force --detach "${commit}"
    git -C "${STOCK}" clean -fdxq
fi
if [[ -n "$(git -C "${STOCK}" status --porcelain --untracked-files=no)" ]]; then
    echo "stock-sdk: ${STOCK} has local changes; it should stay unpatched" >&2
    exit 1
fi

sed "s|^export KOS_BASE=.*|export KOS_BASE=\"${STOCK}\"|" "${KOS_ENV}" > "${STOCK}/environ.sh"

# The kernel, addons and the two utils project builds use; the other host
# utils want libpng and libjpeg on the Mac and nothing here needs them.
echo "stock-sdk: building KOS ${commit:0:7} (log in ${STOCK}/build.log)"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
if ! ( set +u; source "${STOCK}/environ.sh"; set -u
       for dir in kernel addons utils/bin2c utils/genromfs; do
           make -C "${STOCK}/${dir}" -j"${jobs}" || exit 1
       done ) > "${STOCK}/build.log" 2>&1; then
    tail -20 "${STOCK}/build.log" >&2
    echo "stock-sdk: build failed" >&2
    exit 1
fi
echo "stock-sdk: ready, KOS ${commit:0:7} unpatched at ${STOCK}"
