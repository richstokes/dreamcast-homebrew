#!/usr/bin/env bash
# Build a native Flycast with the macOS document-open startup fix.
set -euo pipefail
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REVISION=5aa091fde632fb332c8d8c34e280d62dc951954c
WORK_DIR="${FLYCAST_BUILD_DIR:-${HOME}/Library/Caches/dreamcast-flycast-v2.7}"
INSTALL_DIR="${HOME}/.local/share/dreamcast/flycast"

[[ "$(uname -s)" == Darwin ]] || { echo 'This builder requires macOS.' >&2; exit 1; }
for tool in git cmake pkg-config; do
    command -v "$tool" >/dev/null || { echo "Missing $tool. Install the build dependencies described in tools/flycast/README.md." >&2; exit 1; }
done
pkg-config --exists sdl2 || { echo 'SDL2 development files are required (brew install sdl2).' >&2; exit 1; }
SDK_DIR="$(xcrun --show-sdk-path)"
mkdir -p "$WORK_DIR"
if [[ ! -d "$WORK_DIR/source/.git" ]]; then
    git clone --depth 1 --branch v2.7 https://github.com/flyinghead/flycast.git "$WORK_DIR/source"
fi
[[ "$(git -C "$WORK_DIR/source" rev-parse HEAD)" == "$REVISION" ]] || {
    echo "Unexpected source revision in $WORK_DIR/source; choose a fresh FLYCAST_BUILD_DIR." >&2; exit 1;
}
git -C "$WORK_DIR/source" submodule update --init --recursive --depth 1
for patch in macos-build.patch macos-startup.patch; do
    if git -C "$WORK_DIR/source" apply --check "$TOOLS_DIR/$patch" 2>/dev/null; then
        git -C "$WORK_DIR/source" apply "$TOOLS_DIR/$patch"
    else
        git -C "$WORK_DIR/source" apply --reverse --check "$TOOLS_DIR/$patch"
    fi
done
cmake -S "$WORK_DIR/source" -B "$WORK_DIR/build" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
    -DUSE_HOST_SDL=ON -DUSE_HOST_LIBZIP=OFF -DUSE_VULKAN=OFF \
    -DUSE_BREAKPAD=OFF -DUSE_LUA=OFF -DENABLE_LOG=OFF \
    -DFLYCAST_PRESEED_DARWIN_XCODE_CHECKS=OFF \
    -DZLIB_LIBRARY="$SDK_DIR/usr/lib/libz.tbd"
cmake --build "$WORK_DIR/build" --parallel "$(sysctl -n hw.ncpu)"
mkdir -p "$INSTALL_DIR"
ditto "$WORK_DIR/build/Flycast.app" "$INSTALL_DIR/Flycast.app"
file "$INSTALL_DIR/Flycast.app/Contents/MacOS/Flycast"
echo "Installed native Flycast. Project launchers will use $INSTALL_DIR/Flycast.app."
