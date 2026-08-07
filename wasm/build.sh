#!/usr/bin/env bash
# Convenience wrapper: configure and build the Roxal wasm host.
#
#   ./build.sh              configure (if needed) + build  -> build-wasm-mt/dist/
#   ./build.sh --clean      wipe the build dir first
#
# The build itself is plain CMake -- wasm/CMakeLists.txt is a child of the root
# project, so the ABI-affecting compile definitions, include dirs, ANTLR4 and
# dataflow all arrive by linking roxalcore. There is nothing for this script to
# keep in sync; it only spells out the configure flags.
#
# Prerequisites (see ../install-deps.sh emsdk antlr4-wasm):
#   emsdk           ~/dev/emsdk or ~/emsdk, or $EMSDK
#   deps/antlr4-wasm-mt   ANTLR4 C++ runtime cross-built with matching flags
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROXAL="${ROXAL:-$(cd "$HERE/.." && pwd)}"
BUILD="${BUILD:-$ROXAL/build-wasm-mt}"

if [ "${1:-}" = "--clean" ]; then
    echo "removing $BUILD"
    rm -rf "$BUILD"
fi

if [ -n "${EMSDK:-}" ] && [ -f "$EMSDK/emsdk_env.sh" ]; then
    source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
elif [ -f "$HOME/dev/emsdk/emsdk_env.sh" ]; then
    source "$HOME/dev/emsdk/emsdk_env.sh" >/dev/null 2>&1
elif [ -f "$HOME/emsdk/emsdk_env.sh" ]; then
    source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
command -v em++ >/dev/null || { echo "em++ not found -- run ../install-deps.sh emsdk, or set EMSDK" >&2; exit 1; }

[ -d "$ROXAL/deps/antlr4-wasm-mt" ] || {
    echo "missing $ROXAL/deps/antlr4-wasm-mt -- run ../install-deps.sh antlr4-wasm" >&2; exit 1; }

# -fwasm-exceptions and -pthread are NOT passed here: the root CMakeLists applies
# them for every EMSCRIPTEN build, so they cannot drift from what libroxal.a and
# the ANTLR4 runtime were compiled with.
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
    emcmake cmake -B "$BUILD" -S "$ROXAL" \
        -DCMAKE_BUILD_TYPE=Release \
        -DROXAL_ENABLE_FFI=OFF \
        -DROXAL_UNICODE_BACKEND=builtin \
        -DROXAL_ENABLE_SOCKET=OFF \
        -DROXAL_ENABLE_FILEIO=OFF \
        -DROXAL_ENABLE_LTO=OFF \
        -Dantlr4_ROOT="$ROXAL/deps/antlr4-wasm-mt" \
        -DEigen3_DIR="$ROXAL/deps/eigen/share/eigen3/cmake" \
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH
fi

cmake --build "$BUILD" -j"${JOBS:-4}" --target roxal_wasm

echo "built: $BUILD/dist/roxal.js  ($(du -h "$BUILD/dist/roxal.wasm" | cut -f1) wasm)"
