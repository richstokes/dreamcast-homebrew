#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KOS_ENV="${KOS_ENV:-/Users/rich/.local/share/dreamcast/kos/environ.sh}"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"
ASSET_PATH="${PROJECT_DIR}/assets/generated/gravity_wave_music.adpcm"
WORK_DIR="$(mktemp -d /tmp/gravity-wave-music.XXXXXX)"
LOG_PATH="${WORK_DIR}/export.log"
PAYLOAD_PATH="${WORK_DIR}/payload.adpcm"
FINAL_PATH="${WORK_DIR}/gravity_wave_music.adpcm"
flycast_pid=""

cleanup() {
    if [[ -n "${flycast_pid}" ]] && kill -0 "${flycast_pid}" 2>/dev/null; then
        kill "${flycast_pid}" 2>/dev/null || true
        wait "${flycast_pid}" 2>/dev/null || true
    fi
    rm -f "${LOG_PATH}" "${PAYLOAD_PATH}" "${FINAL_PATH}"
    rmdir "${WORK_DIR}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if [[ ! -f "${KOS_ENV}" ]]; then
    echo "KallistiOS environment not found: ${KOS_ENV}" >&2
    exit 1
fi
if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    exit 1
fi

set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u

make -C "${PROJECT_DIR}" clean
make -C "${PROJECT_DIR}" \
    CFLAGS="${KOS_CFLAGS} -Wextra -Werror -DGRAVITY_WAVE_EXPORT_MUSIC_HEX -DGRAVITY_WAVE_AUTOTEST -DGRAVITY_WAVE_AUTOTEST_EXIT_SECONDS=0.2f"

"${FLYCAST_BIN}" \
    -config "config:Debug.SerialConsoleEnabled=yes,config:Dynarec.Enabled=no" \
    "${PROJECT_DIR}/gravity-wave.elf" > "${LOG_PATH}" 2>&1 &
flycast_pid=$!

echo "Baking 24 Dreamcast ADPCM sections in Flycast; this takes several minutes."
complete=false
for ((attempt = 0; attempt < 600; ++attempt)); do
    if rg -q '^Gravity Wave: shutting down cleanly\.' "${LOG_PATH}"; then
        complete=true
        break
    fi
    if ! kill -0 "${flycast_pid}" 2>/dev/null; then
        break
    fi
    sleep 1
done

if [[ "${complete}" != true ]]; then
    echo "Music bake did not complete cleanly." >&2
    tail -n 40 "${LOG_PATH}" >&2
    exit 1
fi
kill "${flycast_pid}" 2>/dev/null || true
wait "${flycast_pid}" 2>/dev/null || true
flycast_pid=""

section_count="$(awk '/^GW_MUSIC_SECTION / {count++} END {print count + 0}' "${LOG_PATH}")"
expected_bytes="$(awk '/^GW_MUSIC_SECTION / {sum += $4} END {print sum + 0}' "${LOG_PATH}")"
catalog_fingerprint="$(awk '/^GW_MUSIC_FINGERPRINT / {print $2; exit}' "${LOG_PATH}" | tr -d '\r')"
if [[ "${section_count}" != 24 || -z "${catalog_fingerprint}" ]]; then
    echo "Music bake manifest is incomplete (${section_count}/24 sections)." >&2
    exit 1
fi

awk '/^GWHEX / {print $2}' "${LOG_PATH}" | xxd -r -p > "${PAYLOAD_PATH}"
actual_bytes="$(wc -c < "${PAYLOAD_PATH}" | tr -d ' ')"
if [[ "${actual_bytes}" != "${expected_bytes}" ]]; then
    echo "Music payload size mismatch: ${actual_bytes}/${expected_bytes}." >&2
    exit 1
fi

payload_fingerprint="$(perl -e '
    use integer;
    my $hash = 2166136261;
    while(read(STDIN, my $chunk, 65536)) {
        for my $byte (unpack("C*", $chunk)) {
            $hash ^= $byte;
            $hash = ($hash * 16777619) & 0xffffffff;
        }
    }
    printf "%08x", $hash;
' < "${PAYLOAD_PATH}")"

perl -e 'print pack("V", hex($ARGV[0])), pack("V", hex($ARGV[1]))' \
    "${catalog_fingerprint}" "${payload_fingerprint}" > "${FINAL_PATH}"
dd if="${PAYLOAD_PATH}" bs=1048576 >> "${FINAL_PATH}" 2>/dev/null
final_bytes="$(wc -c < "${FINAL_PATH}" | tr -d ' ')"
if [[ "${final_bytes}" != "$((expected_bytes + 8))" ]]; then
    echo "Final music asset size mismatch: ${final_bytes}." >&2
    exit 1
fi

mv "${FINAL_PATH}" "${ASSET_PATH}"
chmod 644 "${ASSET_PATH}"
echo "Wrote ${ASSET_PATH} (${final_bytes} bytes, catalog ${catalog_fingerprint}, payload ${payload_fingerprint})."
shasum -a 256 "${ASSET_PATH}"

make -C "${PROJECT_DIR}" clean
make -C "${PROJECT_DIR}" CFLAGS="${KOS_CFLAGS} -Wextra -Werror"
