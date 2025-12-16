#!/bin/bash
set -euo pipefail

echo "Building antlr4-runtime"

ANTLR_VERSION="4.13.1"
ANTLR_REPO="https://github.com/antlr/antlr4.git"

ROXAL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

ANTLR_BUILD=ON
EXTERNAL_DIR="$ROXAL_ROOT/External"
ANTLR_DIR="$EXTERNAL_DIR/antlr4"
ANTLR_CPP_DIR="$ANTLR_DIR/runtime/Cpp"
ANTLR_CMAKE_DIR="$EXTERNAL_DIR/cmake/antlr4"
BUILD_DIR="$ANTLR_CPP_DIR/build"
ANTLR_LIB_DST="$ROXAL_ROOT/External/libraries/antlr4"

TOOLCHAIN_FILE="$ROXAL_ROOT/toolchains/vxworks.cmake"
ANTLR_CLONED=0

# -------------------------------
# antlr4 clone 
# -------------------------------
if [ ! -d "$ANTLR_DIR" ]; then
  echo "[INFO] antlr4 not found. Cloning ANTLR $ANTLR_VERSION..."
  mkdir -p "$EXTERNAL_DIR"
  git clone --branch "$ANTLR_VERSION" --depth 1 "$ANTLR_REPO" "$ANTLR_DIR"
  ANTLR_CLONED=1  
else
  echo "[INFO] antlr4 already exists. Skipping clone."
fi

if [ "$ANTLR_CLONED" -eq 1 ] && [ -d "$ANTLR_CMAKE_DIR" ]; then
  echo "[INFO] Overlay only copied files (no delete)"
  rsync -a "$ANTLR_CMAKE_DIR"/ "$ANTLR_DIR"/
fi

# -------------------------------
# sanity check
# -------------------------------
if [ ! -d "$ANTLR_CPP_DIR" ]; then
  echo "[ERROR] $ANTLR_CPP_DIR does not exist"
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
# Build
# -------------------------------
rm -rf "$BUILD_DIR"

cmake -G Ninja \
  -S "$ANTLR_CPP_DIR" \
  -B "$BUILD_DIR" \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DANTLR_BUILD_CPP_TESTS=OFF \
  -DANTLR_BUILD_CPP_EXAMPLES=OFF \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DCMAKE_SYSTEM_NAME=VxWorks \
  -DCMAKE_BUILD_TYPE=Release

cmake --build "$BUILD_DIR" -j

ANTLR_LIB_FILE="$(find "$BUILD_DIR" -name 'libantlr4-runtime.a' -type f | head -n 1)"
cp -f "$ANTLR_LIB_FILE" "$ANTLR_LIB_DST/libantlr4-runtime.a"




