#!/usr/bin/env bash
# Builds the Flycast WebAssembly core that dcvmu.com/play runs: Flycast at the
# commit flycast-wasm v1.0 pins, with that release's WebAssembly JIT and our
# patches/, linked against EmulatorJS's RetroArch fork. The link step is
# flycast-wasm's build-prod.sh. Then run
#   projects/dcvmu.com/service/package_flycast_core.py --build
# to package $FLYCAST_WASM_WORK/out into static/play/flycast-wasm.data.
set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
work=${FLYCAST_WASM_WORK:-$HOME/.local/share/dreamcast/flycast-wasm}
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc)

# EmulatorJS builds its cores with this emsdk.
EMSDK_VERSION=3.1.74
EMSDK_COMMIT=3d6d8ee910466516a53e665b86458faa81dae9ba
FLYCAST_COMMIT=2c48c0188a2afc158b02b6d1865d898756a03071
FLYCAST_WASM_COMMIT=16e9c7b4b7065a4bd42e7490e8aab3e9283d5415  # v1.0
# The fork's v1.22.2 branch as of the release's build date (Feb 19 2026),
# well before 2026-05-12 when it replaced the C exports EmulatorJS 4.2.3
# calls (toggleMainLoop, ejs_set_variable, ...).
RETROARCH_COMMIT=ed3265745eccec99b48f99e2a2ffc8a6a93823bb

checkout() {
  local url=$1 dir=$2 commit=$3
  if [ ! -d "$dir/.git" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$url"
  fi
  if [ "$(git -C "$dir" rev-parse -q --verify HEAD || true)" != "$commit" ]; then
    git -C "$dir" fetch -q --depth 1 origin "$commit"
    git -C "$dir" checkout -q --force "$commit"
  fi
}

mkdir -p "$work"
checkout https://github.com/emscripten-core/emsdk.git "$work/emsdk" "$EMSDK_COMMIT"
checkout https://github.com/nasomers/flycast-wasm.git "$work/flycast-wasm" "$FLYCAST_WASM_COMMIT"
checkout https://github.com/flyinghead/flycast.git "$work/flycast" "$FLYCAST_COMMIT"
checkout https://github.com/EmulatorJS/RetroArch.git "$work/retroarch" "$RETROARCH_COMMIT"

"$work/emsdk/emsdk" install "$EMSDK_VERSION" >/dev/null
"$work/emsdk/emsdk" activate "$EMSDK_VERSION" >/dev/null
# shellcheck disable=SC1091
EMSDK_QUIET=1 source "$work/emsdk/emsdk_env.sh"

# Start Flycast from a clean tree each run so patches apply exactly once.
git -C "$work/flycast" checkout -q --force "$FLYCAST_COMMIT"
git -C "$work/flycast" clean -q -fd -e build-wasm-prod
git -C "$work/flycast" submodule update -q --init --recursive --depth 1
# Upstream's shell/libretro/audiostream.cpp has CRLF line endings; the patch doesn't.
git -C "$work/flycast" apply --ignore-whitespace "$work/flycast-wasm/patches/wasm-jit-phase1-modified.patch"
mkdir -p "$work/flycast/core/rec-wasm"
cp "$work/flycast-wasm/patches/"{rec_wasm.cpp,wasm_emit.h,wasm_module_builder.h,fly_instrument.h} \
  "$work/flycast/core/rec-wasm/"
for patch in "$here"/patches/*.patch; do
  [ -e "$patch" ] || continue
  git -C "$work/flycast" apply "$patch"
done

# RetroArch's frontend objects; the target link fails without a core, so
# build only the objects. These flags give the same wasm imports and strings
# as the v1.0 release: full ASYNCIFY, WebGL 2, OpenAL audio, CHD.
ra_flags=(HAVE_CHD=1 HAVE_THREADS=0 ASYNC=1 HAVE_OPENGLES3=1 HAVE_AL=1 HAVE_RWEBAUDIO=0)
if [ ! -e "$work/retroarch/obj-emscripten/.built" ]; then
  printf 'include Makefile.emulatorjs\nobjects: $(RARCH_OBJ)\n' > "$work/retroarch-objects.mk"
  emmake make -C "$work/retroarch" -f "$work/retroarch-objects.mk" "${ra_flags[@]}" -j"$jobs" objects
  touch "$work/retroarch/obj-emscripten/.built"
fi

# flycast_stubs.c reproduces the prebuilt flycast_stubs.o that build-prod.sh links.
emcc -O3 -flto -c "$here/flycast_stubs.c" -o "$work/flycast_stubs.o"

# build-prod.sh calls nproc, which macOS lacks.
mkdir -p "$work/bin"
printf '#!/bin/sh\necho %s\n' "$jobs" > "$work/bin/nproc"
chmod +x "$work/bin/nproc"
cd "$work/flycast-wasm"
PATH="$work/bin:$PATH" SOURCE_DIR="$work/flycast" BUILD_DIR="$work/flycast/build-wasm-prod" \
  EJS_RA="$work/retroarch" STUBS="$work/flycast_stubs.o" OUT_DIR="$work/out" \
  bash build-prod.sh
