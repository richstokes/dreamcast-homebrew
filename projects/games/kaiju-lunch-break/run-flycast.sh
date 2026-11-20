#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_PREFIX=KL
ELF_DEFAULT=kaiju-lunch-break.elf

usage() {
    cat <<'USAGE'
Usage: ./run-flycast.sh [--skip-build]

Builds Kaiju Lunch Break (unless --skip-build), wraps the ELF and the
music's CD-DA track (cdda/, made by make music) in a throwaway CDI
(build/cdi/, with mkdcdisc: tools/mkdcdisc/build-macos.sh) and boots it in
Flycast with the serial console on this terminal. The music is an audio
track on the disc, so a bare ELF would run without it. Only transient -config flags are
used.

QA hooks: KL_ELF picks another ELF in this folder; KL_EXIT_ON_LOG and
KL_FAIL_ON_LOG stop Flycast when that text appears on the serial console.
KL_FLYCAST_CONFIG adds more -config settings.
USAGE
}

source "$PROJECT_DIR/../../common/engine/tools/flycast-run.sh"
launcher_build
make -C "${PROJECT_DIR}" music

# The disc: the music's audio track, then the ELF. Rebuilt when the ELF or
# the track is newer than the last image.
CDI_PATH="${PROJECT_DIR}/build/cdi/${ELF_NAME%.elf}.cdi"
stale=false
if [[ ! -f "${CDI_PATH}" || "${ELF_PATH}" -nt "${CDI_PATH}" ]]; then stale=true; fi
for track in "${PROJECT_DIR}"/cdda/*.wav; do
    [[ "${track}" -nt "${CDI_PATH}" ]] && stale=true
done
if [[ "${stale}" == true ]]; then
    mkdir -p "$(dirname "${CDI_PATH}")"
    "${PROJECT_DIR}/../../../tools/flycast/elf-to-cdi.sh" --cdda "${PROJECT_DIR}/cdda" "${ELF_PATH}" "${CDI_PATH}"
fi

launcher_run "${CDI_PATH}"
