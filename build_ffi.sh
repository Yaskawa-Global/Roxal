#!/bin/bash
set -euo pipefail

echo "Building libffi"

FFI_VERSION="3.4.4"
FFI_REPO="https://github.com/libffi/libffi.git"

ROXAL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

EXTERNAL_DIR="$ROXAL_ROOT/External"
FFI_DIR="$EXTERNAL_DIR/ffi"
FFI_BUILD_DIR="$FFI_DIR/build"
FFI_LIB_DST="$ROXAL_ROOT/External/libraries/ffi"
FFI_CMAKE_DIR="$EXTERNAL_DIR/cmake/ffi"

TOOLCHAIN_FILE="$ROXAL_ROOT/toolchains/vxworksffi.cmake"
FFI_CLONED=0

# -------------------------------
# libffi clone
# -------------------------------
if [ ! -d "$FFI_DIR" ]; then
  echo "[INFO] libffi not found. Cloning FFI $FFI_VERSION..."
  mkdir -p "$EXTERNAL_DIR"
  git clone --branch "v$FFI_VERSION" --depth 1 "$FFI_REPO" "$FFI_DIR"
  FFI_CLONED=1
else
  echo "[INFO] libffi already exists. Skipping clone."
fi

if [ "$FFI_CLONED" -eq 1 ] && [ -d "$FFI_CMAKE_DIR" ]; then
  echo "[INFO] Overlay only copied files (no delete)"
  rsync -a "$FFI_CMAKE_DIR"/ "$FFI_DIR"/
fi

# -------------------------------
# Sanity check
# -------------------------------
if [ ! -d "$FFI_DIR" ]; then
  echo "[ERROR] $FFI_DIR does not exist"
  exit 1
fi

if [ ! -f "$TOOLCHAIN_FILE" ]; then
  echo "[ERROR] Toolchain file not found: $TOOLCHAIN_FILE"
  exit 1
fi

# -------------------------------
# VxWorks environment
# -------------------------------
export WIND_BASE="$HOME/wrsdk-vxworks7-raspberrypi4b/vxsdk"
export WIND_VX7_HOST_TYPE=x86_64-linux
export WIND_CC_SYSROOT="$WIND_BASE/sysroot"
export PATH="$WIND_BASE/host/$WIND_VX7_HOST_TYPE/bin:$PATH"

which wr-c++
wr-c++ --version

# -------------------------------
# Build libffi
# -------------------------------
mkdir -p "$FFI_BUILD_DIR"
cd "$FFI_BUILD_DIR"

cmake -G Ninja \
  -S "$FFI_DIR" \
  -B "$FFI_BUILD_DIR" \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_SYSTEM_NAME=VxWorks \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$FFI_BUILD_DIR" -j

# -------------------------------
# Copy only libffi.a
# -------------------------------
FFI_LIB_FILE="$(find "$FFI_BUILD_DIR" -name 'libffi.a' -type f | head -n 1)"
if [ -z "$FFI_LIB_FILE" ]; then
  echo "[ERROR] libffi.a not found"
  exit 1
fi

mkdir -p "$FFI_LIB_DST"
echo "[INFO] Copying libffi.a -> $FFI_LIB_DST"
cp -f "$FFI_LIB_FILE" "$FFI_LIB_DST/libffi.a"

echo "[INFO] libffi build finished successfully."
