#!/bin/sh
# Clone or fast-forward SH4ZAM checkouts to the latest upstream branch head.
#
# Usage: tools/update-sh4zam.sh <checkout-dir>...
#
# SH4ZAM is deliberately not vendored or pinned in this repository. Each
# checkout directory is ignored by Git and always tracks upstream master; a
# failed clone or fetch exits non-zero so builds stop instead of using stale
# or missing headers.
set -eu

SH4ZAM_URL="${SH4ZAM_URL:-https://github.com/gyrovorbis/sh4zam.git}"
SH4ZAM_BRANCH="${SH4ZAM_BRANCH:-master}"

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <checkout-dir>..." >&2
    exit 2
fi

for dir in "$@"; do
    if [ -e "$dir/.git" ]; then
        git -C "$dir" fetch -q origin "$SH4ZAM_BRANCH"
        git -C "$dir" merge -q --ff-only FETCH_HEAD
    else
        rm -rf "$dir"
        git clone -q --branch "$SH4ZAM_BRANCH" "$SH4ZAM_URL" "$dir"
    fi
    printf 'SH4ZAM %s: %s\n' "$dir" "$(git -C "$dir" rev-parse HEAD)"
done
