#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_PREFIX=DK
ELF_DEFAULT=dreamkarts.elf

usage() {
    cat <<'USAGE'
Usage: ./run-flycast.sh [--skip-build]

Builds DreamKarts (unless --skip-build), wraps the ELF and the music's
CD-DA tracks (cdda/, fetched by make music) in a throwaway CDI (build/cdi/,
with mkdcdisc: tools/mkdcdisc/build-macos.sh) and boots it in Flycast with
the serial console on this terminal. The music is on the disc as audio
tracks, so a bare ELF would run silent. Only transient -config flags are
used.

QA hooks: DK_ELF picks another ELF in this folder; DK_EXIT_ON_LOG and
DK_FAIL_ON_LOG stop Flycast when that text appears on the serial console.
Online play goes through Flycast's emulated broadband adapter by default,
whatever Flycast's saved network settings say. DK_NET=modem uses the
emulated modem instead (both through Flycast's local proxy, not DCNet).
DK_SHOW_VMU=1 shows the VMU screens over the game. DK_KEYBOARD=1 plugs a
Dreamcast keyboard into port B, fed by this Mac's keyboard (lobby chat).
DK_VMU_DIR=<folder> uses that folder's memory cards instead of the real
ones; it must already hold vmu_save_A1.bin to D2.bin (blank 131072-byte
files do), or Flycast moves the real cards there. DK_FLYCAST_CONFIG adds
more -config settings (e.g. window:left=700,window:top=40 to place the
window clear of another instance's, which would otherwise stop it
presenting frames to capture).
USAGE
}

source "$PROJECT_DIR/../../common/engine/tools/flycast-run.sh"
launcher_build
make -C "${PROJECT_DIR}" music

case "${DK_NET:-bba}" in
    bba) FLYCAST_CONFIG+=",network:EmulateBBA=yes,network:DCNet=no" ;;
    modem) FLYCAST_CONFIG+=",network:EmulateBBA=no,network:DCNet=no" ;;
    *) echo "DK_NET must be bba or modem" >&2; exit 2 ;;
esac
if [[ "${DK_SHOW_VMU:-}" == 1 ]]; then
    FLYCAST_CONFIG+=",config:rend.FloatVMUs=yes"
fi
if [[ "${DK_KEYBOARD:-}" == 1 ]]; then
    FLYCAST_CONFIG+=",input:device2=5,input:maple_sdl_keyboard=1"
fi
if [[ -n "${DK_VMU_DIR:-}" ]]; then
    for slot in A1 A2 B1 B2 C1 C2 D1 D2; do
        [[ -f "${DK_VMU_DIR}/vmu_save_${slot}.bin" ]] || { echo "DK_VMU_DIR lacks vmu_save_${slot}.bin (Flycast would move the real card)" >&2; exit 2; }
    done
    FLYCAST_CONFIG+=",config:Dreamcast.VMUPath=${DK_VMU_DIR}"
fi
# The disc: the music's audio tracks, then the ELF. Rebuilt when the ELF or
# any track is newer than the last image (~210 MB of audio, a few seconds).
CDI_PATH="${PROJECT_DIR}/build/cdi/${ELF_NAME%.elf}.cdi"
stale=false
if [[ ! -f "${CDI_PATH}" || "${ELF_PATH}" -nt "${CDI_PATH}" ]]; then stale=true; fi
for track in "${PROJECT_DIR}"/cdda/*.wav; do
    [[ "${track}" -nt "${CDI_PATH}" ]] && stale=true
done
if [[ "${stale}" == true ]]; then
    "${PROJECT_DIR}/../../../tools/flycast/elf-to-cdi.sh" --cdda "${PROJECT_DIR}/cdda" "${ELF_PATH}" "${CDI_PATH}"
fi

launcher_run "${CDI_PATH}"
