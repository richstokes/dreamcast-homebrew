#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KOS_ENV="${KOS_ENV:-/Users/rich/.local/share/dreamcast/kos/environ.sh}"
FLYCAST_BIN="${FLYCAST_BIN:-/Applications/Flycast.app/Contents/MacOS/Flycast}"
ASSET_PATH="${PROJECT_DIR}/assets/generated/gravity_wave_music.adpcm"
RENDER_TOOL="${PROJECT_DIR}/tools/render_soundtrack.py"
QA_TOOL="${PROJECT_DIR}/tools/analyze_soundtrack.py"
ALBUM_TOOL="${PROJECT_DIR}/tools/build_music_album.py"
WORK_DIR="$(mktemp -d /tmp/gravity-wave-music.XXXXXX)"
RENDER_DIR="${WORK_DIR}/rendered"
MANIFEST_PATH="${RENDER_DIR}/soundtrack_manifest.json"
QA_REPORT_PATH="${WORK_DIR}/soundtrack-qa.json"
LOG_PATH="${WORK_DIR}/catalog.log"
FINAL_PATH="${WORK_DIR}/gravity_wave_music.adpcm"
install_path=""
flycast_pid=""

cleanup() {
    if [[ -n "${flycast_pid}" ]] && kill -0 "${flycast_pid}" 2>/dev/null; then
        kill "${flycast_pid}" 2>/dev/null || true
        wait "${flycast_pid}" 2>/dev/null || true
    fi
    if [[ -n "${install_path}" && -f "${install_path}" ]]; then
        rm -f -- "${install_path}"
    fi
    case "${WORK_DIR}" in
        /tmp/gravity-wave-music.*)
            rm -rf -- "${WORK_DIR}"
            ;;
        *)
            echo "Refusing to remove unexpected work directory: ${WORK_DIR}" >&2
            ;;
    esac
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if [[ ! -f "${KOS_ENV}" ]]; then
    echo "KallistiOS environment not found: ${KOS_ENV}" >&2
    exit 1
fi
if [[ ! -x "${FLYCAST_BIN}" ]]; then
    echo "Flycast executable not found: ${FLYCAST_BIN}" >&2
    exit 1
fi
for required_tool in "${RENDER_TOOL}" "${QA_TOOL}" "${ALBUM_TOOL}"; do
    if [[ ! -f "${required_tool}" ]]; then
        echo "Soundtrack tool not found: ${required_tool}" >&2
        exit 1
    fi
done
if ! command -v uv >/dev/null 2>&1; then
    echo "uv is required to render and analyze the soundtrack." >&2
    exit 1
fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "python3 is required to package the soundtrack." >&2
    exit 1
fi

mkdir -p "${RENDER_DIR}"
echo "Rendering eight continuous 36-bar soundtrack masters."
uv run --with 'numpy==2.5.2' --with 'scipy==1.18.1' \
    "${RENDER_TOOL}" --output-dir "${RENDER_DIR}"

if [[ ! -f "${MANIFEST_PATH}" ]]; then
    echo "Renderer did not write ${MANIFEST_PATH}." >&2
    exit 1
fi
echo "Running soundtrack signal, structure, and identity QA."
uv run --with 'numpy==2.5.2' --with 'scipy==1.18.1' \
    "${QA_TOOL}" "${MANIFEST_PATH}" --json-output "${QA_REPORT_PATH}"

set +u
# shellcheck disable=SC1090
source "${KOS_ENV}"
set -u

make -C "${PROJECT_DIR}" clean
make -C "${PROJECT_DIR}" \
    CFLAGS="${KOS_CFLAGS} -Wextra -Werror -DGRAVITY_WAVE_EXPORT_MUSIC_CATALOG_ONLY"

"${FLYCAST_BIN}" \
    -config "config:Debug.SerialConsoleEnabled=yes,config:Dynarec.Enabled=no" \
    "${PROJECT_DIR}/gravity-wave.elf" > "${LOG_PATH}" 2>&1 &
flycast_pid=$!

echo "Booting the SH-4 catalog validator to obtain the runtime fingerprint."
fingerprint_ready=false
for ((attempt = 0; attempt < 60; ++attempt)); do
    if rg -q '^GW_MUSIC_FINGERPRINT [0-9a-fA-F]{1,8}\r?$' "${LOG_PATH}"; then
        fingerprint_ready=true
        break
    fi
    if ! kill -0 "${flycast_pid}" 2>/dev/null; then
        break
    fi
    sleep 1
done
if [[ "${fingerprint_ready}" != true ]]; then
    echo "Dreamcast catalog fingerprint was not emitted." >&2
    tail -n 60 "${LOG_PATH}" >&2
    exit 1
fi
kill "${flycast_pid}" 2>/dev/null || true
wait "${flycast_pid}" 2>/dev/null || true
flycast_pid=""

catalog_fingerprint="$(
    awk '/^GW_MUSIC_FINGERPRINT / {print $2}' "${LOG_PATH}" |
        tr -d '\r' | sort -u
)"
if [[ ! "${catalog_fingerprint}" =~ ^[0-9a-fA-F]{1,8}$ ]]; then
    echo "Catalog fingerprint output is missing or contradictory: ${catalog_fingerprint}" >&2
    exit 1
fi

echo "Encoding eight uninterrupted interleaved AICA ADPCM streams."
python3 "${ALBUM_TOOL}" \
    --manifest "${MANIFEST_PATH}" \
    --catalog-fingerprint "${catalog_fingerprint}" \
    --output "${FINAL_PATH}"
final_bytes="$(wc -c < "${FINAL_PATH}" | tr -d ' ')"
if [[ "${final_bytes}" -le 160 ]]; then
    echo "Final streaming album is unexpectedly small: ${final_bytes}." >&2
    exit 1
fi

# Copy into the destination directory first, then rename on that filesystem.
# This makes installation atomic even when /tmp and the project are on
# different volumes.
install_path="$(mktemp "${ASSET_PATH}.tmp.XXXXXX")"
cp "${FINAL_PATH}" "${install_path}"
chmod 644 "${install_path}"
mv -f "${install_path}" "${ASSET_PATH}"
install_path=""
echo "Installed ${ASSET_PATH} (${final_bytes} bytes)."
shasum -a 256 "${ASSET_PATH}"

make -C "${PROJECT_DIR}" clean
make -C "${PROJECT_DIR}" CFLAGS="${KOS_CFLAGS} -Wextra -Werror"
