#!/usr/bin/env bash
# Build a native Flycast from the pinned upstream revision.
set -euo pipefail
TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REVISION=628bd3dbb160ea2750230fc6b00c0bb8173cb1f6
WORK_DIR="${FLYCAST_BUILD_DIR:-${HOME}/Library/Caches/dreamcast-flycast-${REVISION:0:7}}"
INSTALL_DIR="${HOME}/.local/share/dreamcast/flycast"

[[ "$(uname -s)" == Darwin ]] || { echo 'This builder requires macOS.' >&2; exit 1; }
for tool in git cmake pkg-config; do
    command -v "$tool" >/dev/null || { echo "Missing $tool. Install the build dependencies described in tools/flycast/README.md." >&2; exit 1; }
done
pkg-config --exists sdl2 || { echo 'SDL2 development files are required (brew install sdl2).' >&2; exit 1; }
SDK_DIR="$(xcrun --show-sdk-path)"
mkdir -p "$WORK_DIR"
if [[ ! -d "$WORK_DIR/source/.git" ]]; then
    git init -q "$WORK_DIR/source"
    git -C "$WORK_DIR/source" remote add origin https://github.com/flyinghead/flycast.git
    git -C "$WORK_DIR/source" fetch --depth 1 origin "$REVISION"
    git -C "$WORK_DIR/source" checkout -q FETCH_HEAD
fi
[[ "$(git -C "$WORK_DIR/source" rev-parse HEAD)" == "$REVISION" ]] || {
    echo "Unexpected source revision in $WORK_DIR/source; choose a fresh FLYCAST_BUILD_DIR." >&2; exit 1;
}
git -C "$WORK_DIR/source" submodule update --init --recursive --depth 1
for patch in macos-build.patch keyboard-input.patch; do
    if git -C "$WORK_DIR/source" apply --check "$TOOLS_DIR/$patch" 2>/dev/null; then
        git -C "$WORK_DIR/source" apply "$TOOLS_DIR/$patch"
    else
        git -C "$WORK_DIR/source" apply --reverse --check "$TOOLS_DIR/$patch"
    fi
done
"$TOOLS_DIR/test-keyboard-input.sh"
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
