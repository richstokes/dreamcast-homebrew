#!/usr/bin/env bash
# Build mkdcdisc (Simulant's Dreamcast disc image tool) for this Mac, into
# ~/.local/share/dreamcast/mkdcdisc. The Flycast launchers wrap each ELF in a
# throwaway CDI with it, and `make disc` builds the images the releases ship.
#
#   brew install meson ninja libpng
#   tools/mkdcdisc/build-macos.sh
set -euo pipefail

MKDCDISC_COMMIT="${MKDCDISC_COMMIT:-2b98b0d9480e364e3517355648fbc3b49fb19882}"   # the CI pins its own in release.yml
PREFIX="${HOME}/.local/share/dreamcast/mkdcdisc"
SRC="${PREFIX}/src"

mkdir -p "${PREFIX}"
if [[ ! -d "${SRC}/.git" ]]; then
    git init -q "${SRC}"
    git -C "${SRC}" remote add origin https://gitlab.com/simulant/mkdcdisc.git
fi
git -C "${SRC}" fetch -q --depth 1 origin "${MKDCDISC_COMMIT}"
git -C "${SRC}" checkout -q --detach FETCH_HEAD
meson setup "${SRC}/build" "${SRC}" -Dpng=enabled > /dev/null
meson compile -C "${SRC}/build" > /dev/null
install -m 755 "${SRC}/build/mkdcdisc" "${PREFIX}/mkdcdisc"
echo "mkdcdisc installed at ${PREFIX}/mkdcdisc ($("${PREFIX}/mkdcdisc" --version 2>&1 | head -1))"
