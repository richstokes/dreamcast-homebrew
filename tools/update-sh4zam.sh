#!/bin/sh
# Check out the pinned SH4ZAM commit in each SH4ZAM checkout.
#
# Usage: tools/update-sh4zam.sh <checkout-dir>...
#
# SH4ZAM is not vendored: each checkout directory is an ignored clone of
# upstream, held at SH4ZAM_COMMIT below so local and CI builds compile the
# same headers. The weekly SH4ZAM canary (.github/workflows/sh4zam-canary.yml)
# fails when upstream moves past the pin; AGENTS.md says how to move it.
# SH4ZAM_COMMIT=<commit or branch> in the environment builds against another
# revision, for testing a bump. A failed clone or fetch exits non-zero so
# builds stop instead of using stale or missing headers.
set -eu

SH4ZAM_URL="${SH4ZAM_URL:-https://github.com/gyrovorbis/sh4zam.git}"
SH4ZAM_COMMIT="${SH4ZAM_COMMIT:-70dbb177b20c1734fbe4f97df0f8c6ec87a8e8d3}"   # v0.10.0

if [ "$#" -eq 0 ]; then
    echo "usage: $0 <checkout-dir>..." >&2
    exit 2
fi

for dir in "$@"; do
    # A detached checkout's HEAD file holds its commit, so a checkout already
    # at the pin needs neither git nor the network (CI builds inside Docker).
    if [ "$(cat "$dir/.git/HEAD" 2>/dev/null)" != "$SH4ZAM_COMMIT" ]; then
        if [ ! -e "$dir/.git" ]; then
            rm -rf "$dir"
            git clone -q --no-checkout "$SH4ZAM_URL" "$dir"
        fi
        git -C "$dir" fetch -q origin "$SH4ZAM_COMMIT"
        git -C "$dir" checkout -q --detach FETCH_HEAD
    fi
    printf 'SH4ZAM %s: %s\n' "$dir" "$(cat "$dir/.git/HEAD")"
done
