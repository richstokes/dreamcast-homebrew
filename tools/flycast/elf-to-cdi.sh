#!/usr/bin/env bash
# elf-to-cdi.sh [--cdda DIR] prog.elf [disc-dir] out.cdi
#
# Wrap a KOS ELF (and the files of disc-dir, if given) in a self-booting CDI
# with mkdcdisc. --cdda DIR adds every DIR/*.wav, in name order, as a CD-DA
# audio track ahead of the data track (DreamKarts' music). The Flycast
# launchers boot these instead of bare ELFs, so a game that reads its assets
# from the disc runs the same way in QA as it does from the published image.
# The disc's title is the ELF's name, which Flycast puts in the window title. Every disc
# gets the same MR logo on the BIOS license screen (tools/mkdcdisc/logos).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck disable=SC1091
source "${HERE}/../mkdcdisc/resolve.sh"

cdda_dir=""
if [[ "${1:-}" == "--cdda" ]]; then cdda_dir="$2"; shift 2; fi
elf="$1"
if [[ $# -ge 3 ]]; then disc_dir="$2"; out="$3"; else disc_dir=""; out="$2"; fi
[[ -f "${elf}" ]] || { echo "no such ELF: ${elf}" >&2; exit 1; }
stem="$(basename "${elf}" .elf)"
args=(--quiet --allow-overwrite --disable-data-track-padding --main-elf "${elf}" --title "${stem}" --author richstokes
      --bootstrap-mr-image "${HERE}/../mkdcdisc/logos/gotta-go-fast.png" --output "${out}")
if [[ -n "${disc_dir}" && -d "${disc_dir}" ]]; then
    args+=(--directory-contents "${disc_dir}")
fi
if [[ -n "${cdda_dir}" ]]; then
    tracks=("${cdda_dir}"/*.wav)
    [[ -f "${tracks[0]}" ]] || { echo "no CD-DA tracks in ${cdda_dir}" >&2; exit 1; }
    for track in "${tracks[@]}"; do args+=(--cdda-track "${track}"); done
fi
mkdir -p "$(dirname "${out}")"
"${MKDCDISC}" "${args[@]}"
