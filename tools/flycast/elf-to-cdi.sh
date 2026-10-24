#!/usr/bin/env bash
# elf-to-cdi.sh prog.elf [disc-dir] out.cdi
#
# Wrap a KOS ELF (and the files of disc-dir, if given) in a self-booting CDI
# with mkdcdisc. The Flycast launchers boot these instead of bare ELFs, so a
# game that reads its assets from the disc (DreamKarts streams its music)
# runs the same way in QA as it does from the published image. The disc's
# title is the ELF's name, which Flycast puts in the window title.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/../mkdcdisc/resolve.sh"

elf="$1"
if [[ $# -ge 3 ]]; then disc_dir="$2"; out="$3"; else disc_dir=""; out="$2"; fi
[[ -f "${elf}" ]] || { echo "no such ELF: ${elf}" >&2; exit 1; }
stem="$(basename "${elf}" .elf)"
args=(--quiet --allow-overwrite --disable-data-track-padding --main-elf "${elf}" --title "${stem}" --author richstokes --output "${out}")
if [[ -n "${disc_dir}" && -d "${disc_dir}" ]]; then
    args+=(--directory-contents "${disc_dir}")
fi
mkdir -p "$(dirname "${out}")"
"${MKDCDISC}" "${args[@]}"
