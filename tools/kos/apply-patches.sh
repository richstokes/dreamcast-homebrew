#!/usr/bin/env bash
# apply-patches.sh [--check] [--fresh] [KOS_BASE [KOS_PORTS]]
#
# Applies the KOS fixes in patches/kos/ to KallistiOS and those in
# patches/kos-ports/ to kos-ports, in name order. A patch that is already in
# the tree is skipped, so this is safe to rerun; one that matches neither way
# stops the script before anything else changes. --check only reports.
# --fresh is for an untouched upstream checkout (the weekly canary): there a
# patch already in the tree means upstream took the fix, and it is an error
# so the patch gets dropped.
#
# The trees default to $KOS_BASE and $KOS_PORTS, sourcing $KOS_ENV (the
# installed environ.sh) when those are unset. CI passes its own checkouts.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHECK=no
FRESH=no
while [[ "${1:-}" == -* ]]; do
    case "$1" in
        --check) CHECK=yes ;;
        --fresh) FRESH=yes ;;
        *) set -- -bad ; break ;;
    esac
    shift
done
if [[ $# -gt 2 || "${1:-}" == -* ]]; then
    echo "Usage: ${0##*/} [--check] [--fresh] [KOS_BASE [KOS_PORTS]]" >&2
    exit 2
fi

if [[ $# -eq 0 && ( -z "${KOS_BASE:-}" || -z "${KOS_PORTS:-}" ) ]]; then
    KOS_ENV="${KOS_ENV:-${HOME}/.local/share/dreamcast/kos/environ.sh}"
    if [[ ! -f "${KOS_ENV}" ]]; then
        echo "KallistiOS environment not found: ${KOS_ENV}" >&2
        exit 1
    fi
    # KOS's environment probes optional variables that may be unset.
    # shellcheck disable=SC1090
    set +u
    source "${KOS_ENV}"
    set -u
fi
kos_base="${1:-${KOS_BASE}}"
kos_ports="${2:-${KOS_PORTS}}"

# Report every patch's state before changing anything.
pending=()
conflicts=0
for dir in kos kos-ports; do
    tree="${kos_base}"
    [[ "${dir}" == kos-ports ]] && tree="${kos_ports}"
    for patch in "${HERE}/patches/${dir}"/*.patch; do
        name="${dir}/${patch##*/}"
        if git -C "${tree}" apply --check "${patch}" 2>/dev/null; then
            echo "missing  ${name}"
            pending+=("${tree}|${patch}")
        elif git -C "${tree}" apply --reverse --check "${patch}" 2>/dev/null; then
            if [[ "${FRESH}" == yes ]]; then
                echo "UPSTREAM ${name}: already in ${tree}; drop the patch" >&2
                conflicts=$((conflicts + 1))
            else
                echo "applied  ${name}"
            fi
        else
            echo "CONFLICT ${name}: ${tree} matches neither way" >&2
            conflicts=$((conflicts + 1))
        fi
    done
done

if [[ "${conflicts}" -gt 0 ]]; then
    echo "Nothing was changed. Check whether that SDK already has the fix in another form." >&2
    exit 1
fi
if [[ "${#pending[@]}" -eq 0 ]]; then
    echo "All patches are in place."
    exit 0
fi
if [[ "${CHECK}" == yes ]]; then
    echo "${#pending[@]} patch(es) missing; run ${0##*/} without --check to apply them." >&2
    exit 1
fi

for entry in "${pending[@]}"; do
    git -C "${entry%%|*}" apply "${entry#*|}"
    echo "Applied ${entry##*/}"
done
echo "Now rebuild: make -C \"${kos_base}\", and reinstall the kos-ports packages"
echo "a kos-ports patch touched (see tools/kos/README.md). Then relink projects."
