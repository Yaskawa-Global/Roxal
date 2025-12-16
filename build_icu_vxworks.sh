#!/bin/bash
echo "Building ICU"
set -euo pipefail

ICU_BUILD=ON
ROXAL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXTERNAL_DIR="$ROXAL_ROOT/External"
ICU_DIR="$EXTERNAL_DIR/icu"
BUILD_DIR="$ICU_DIR/build-vx-rpi4"
ICU_LIB_DST="$EXTERNAL_DIR/libraries/icu"
ICU_CMAKE_DIR="$EXTERNAL_DIR/cmake/icu"
TOOLCHAIN_FILE="$ROXAL_ROOT/toolchains/vxworks.cmake"

ICU_REPO="https://github.com/unicode-org/icu.git"
ICU_BRANCH="release-74-2"
ICU_CLONED=0

# -------------------------------
# Clone ICU from GitHub if not exists
# -------------------------------
if [ ! -d "$ICU_DIR" ]; then
  echo "[INFO] ICU not found. Cloning branch $ICU_BRANCH..."
  mkdir -p "$EXTERNAL_DIR"
  git clone --branch "$ICU_BRANCH" --depth 1 "$ICU_REPO" "$ICU_DIR" || {
    echo "[ERROR] Failed to clone ICU branch $ICU_BRANCH"
    exit 1
  }
  ICU_CLONED=1
else
  echo "[INFO] ICU already exists. Skipping clone."
fi

# -------------------------------
# Apply overlay patch if exists
# -------------------------------
if [ "$ICU_CLONED" -eq 1 ] && [ -d "$ICU_CMAKE_DIR" ]; then
  echo "[INFO] Applying overlay from $ICU_CMAKE_DIR -> $ICU_DIR"
  rsync -a "$ICU_CMAKE_DIR"/ "$ICU_DIR"/
fi

# -------------------------------
# Full clean option
# -------------------------------
if [ "${FULLCLEAN:-1}" = "1" ]; then
  echo "[Full clean] Removing build dir: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

# -------------------------------
# Host utility setting
# -------------------------------
export SHELL=/bin/sh
export CONFIG_SHELL=/bin/sh
export GREP=/usr/bin/grep
export SED=/usr/bin/sed
export AWK=/usr/bin/awk
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

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
# Build ICU
# -------------------------------
mkdir -p "$BUILD_DIR"

cmake -S "$ICU_DIR" -B "$BUILD_DIR" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_ICU=ON \
  -DICU_STATIC=ON \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DICU_BUILD_VERSION="74.2" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
  -DICU_CROSS_ARCH=aarch64-wrs-vxworks7 \
  -DICU_CFG_OPTIONS:STRING="--disable-shared;--enable-static;--disable-tests;--disable-samples;--disable-extras;--with-data-packaging=static"

cmake --build "$BUILD_DIR" -v

# -------------------------------
# Copy only static ICU libraries
# -------------------------------
mkdir -p "$ICU_LIB_DST"

ICU_LIB_FILES=$(find "$BUILD_DIR" -name 'libicu*.a' -type f)
if [ -z "$ICU_LIB_FILES" ]; then
  echo "[ERROR] ICU static libraries not found"
  exit 1
fi

for lib in $ICU_LIB_FILES; do
  echo "[INFO] Copying $(basename "$lib") -> $ICU_LIB_DST"
  cp -f "$lib" "$ICU_LIB_DST/"
done

echo "[INFO] ICU build finished successfully."
