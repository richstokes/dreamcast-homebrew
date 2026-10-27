#!/usr/bin/env bash
# kos-at.sh - KallistiOS at any upstream commit, built beside the installed
# SDK, to build and run projects against another KOS version.
#
#   tools/kos/kos-at.sh 368a2eb                build it unpatched, print its path
#   tools/kos/kos-at.sh --patched master       upstream master plus tools/kos/patches
#   tools/kos/kos-at.sh pr/1576                a KallistiOS pull request's head
#   tools/kos/kos-at.sh 368a2eb -- make -C projects/apps/kos-test auto
#                                              run a command with that SDK's environment
#
# REF is a commit, branch, tag or pr/<number>; master is fetched fresh every
# time. Trees are worktrees of one partial clone of upstream under KOS_AT_DIR
# (default ~/.local/share/dreamcast/kos-at), named <sha7> or <sha7>-patched,
# so a second run on the same commit only rebuilds what changed. Each gets an
# environ.sh from its own sample with the installed toolchain and kos-ports.
# The command mode also exports KOS_AT_NAME, the tree's name,
# which KOS_Test uses to keep that build's objects and ELF apart.
#
# Only the kernel, addons, bin2c and genromfs are built: the other host utils
# want libpng and libjpeg on the Mac and nothing here needs them. To delete a
# tree: rm -rf it, then git -C "$KOS_AT_DIR/kos.git" worktree prune.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
ROOT="${KOS_AT_DIR:-${HOME}/.local/share/dreamcast/kos-at}"
UPSTREAM=https://github.com/KallistiOS/KallistiOS.git

usage() {
    echo "Usage: ${0##*/} [--patched] REF [-- COMMAND...]" >&2
    exit 2
}

patched=no
ref=
while [[ $# -gt 0 ]]; do
    case "$1" in
        --patched) patched=yes ;;
        --) shift; [[ $# -gt 0 ]] || usage; break ;;
        -*) usage ;;
        *) [[ -z "${ref}" ]] || usage; ref="$1" ;;
    esac
    shift
done
[[ -n "${ref}" ]] || usage

log() { echo "kos-at: $*" >&2; }

set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u
ports="$(cd "${KOS_PORTS}" && pwd)"

repo=(git -C "${ROOT}/kos.git")
if [[ ! -d "${ROOT}/kos.git" ]]; then
    log "cloning KallistiOS history into ${ROOT}/kos.git (once)"
    mkdir -p "${ROOT}"
    git clone --quiet --bare --filter=blob:none "${UPSTREAM}" "${ROOT}/kos.git"
fi
fetch() {
    "${repo[@]}" fetch --quiet --update-head-ok origin \
        '+refs/heads/*:refs/heads/*' '+refs/tags/*:refs/tags/*' "$@"
}

spec="${ref}"
case "${ref}" in
    pr/*) fetch "+refs/pull/${ref#pr/}/head:refs/pr/${ref#pr/}"; spec="refs/pr/${ref#pr/}" ;;
    master) fetch ;;
esac
resolve() { "${repo[@]}" rev-parse --verify --quiet "${spec}^{commit}"; }
commit="$(resolve)" || { fetch; commit="$(resolve)"; } || {
    log "no commit '${ref}' in ${UPSTREAM}"
    exit 1
}

short="${commit:0:7}"
name="${short}"
[[ "${patched}" == yes ]] && name="${short}-patched"
tree="${ROOT}/${name}"
if [[ ! -e "${tree}/.git" ]]; then
    log "checking out ${short} into ${tree}"
    "${repo[@]}" worktree prune
    "${repo[@]}" worktree add --quiet --detach "${tree}" "${commit}"
fi

if [[ "${patched}" == yes ]]; then
    "${HERE}/apply-patches.sh" --kos-only "${tree}" >&2
elif [[ -n "$(git -C "${tree}" status --porcelain --untracked-files=no)" ]]; then
    log "${tree} has local changes; an unpatched tree should stay clean"
    exit 1
fi

# That commit's own sample (the env layout moves between versions), with the
# installed toolchain and kos-ports.
sed -e "s|^export KOS_BASE=.*|export KOS_BASE=\"${tree}\"|" \
    -e "s|^export KOS_PORTS=.*|export KOS_PORTS=\"${ports}\"|" \
    -e "s|^export KOS_CC_BASE=.*|export KOS_CC_BASE=\"${KOS_CC_BASE}\"|" \
    -e "s|^export DC_ARM_BASE=.*|export DC_ARM_BASE=\"${DC_ARM_BASE}\"|" \
    -e "s|^export DC_TOOLS_BASE=.*|export DC_TOOLS_BASE=\"${DC_TOOLS_BASE}\"|" \
    "${tree}/doc/environ.sh.sample" > "${tree}/environ.sh"

# Rebuilt when the commit or its patches change; make only redoes what did.
state="${commit} $(git -C "${tree}" diff HEAD | git hash-object --stdin)"
if [[ "$(cat "${tree}/.kos-at-built" 2>/dev/null)" != "${state}" ]]; then
    log "building KOS ${short}$([[ "${patched}" == yes ]] && echo ' with patches') (log in ${tree}/build.log)"
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    if ! ( set +u; source "${tree}/environ.sh"; set -u
           for dir in kernel addons utils/bin2c utils/genromfs; do
               make -C "${tree}/${dir}" -j"${jobs}" || exit 1
           done ) > "${tree}/build.log" 2>&1; then
        tail -20 "${tree}/build.log" >&2
        log "build failed"
        exit 1
    fi
    echo "${state}" > "${tree}/.kos-at-built"
fi

if [[ $# -eq 0 ]]; then
    log "ready: KOS ${short}$([[ "${patched}" == yes ]] && echo ' with patches') at ${tree}"
    echo "${tree}"
    exit 0
fi
set +u
# shellcheck disable=SC1091
source "${tree}/environ.sh"
set -u
export KOS_AT_NAME="${name}"
exec "$@"
