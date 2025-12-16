# ~/Roxal/build_roxal.sh
#!/bin/bash
set -euo pipefail

echo "Building Roxal"

# -------------------------------
# Roxal Root detect
# -------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROXAL_ROOT="${SCRIPT_DIR}"

echo "Roxal root: ${ROXAL_ROOT}"

BUILD_DIR="${ROXAL_ROOT}/build_vxworks"

# -------------------------------
# Full clean option
# -------------------------------
if [ "${FULLCLEAN:-1}" = "1" ]; then
  echo "[Full clean] Removing build dir: ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

# -------------------------------
# VxWorks environment setting
# -------------------------------
export WIND_BASE="$HOME/wrsdk-vxworks7-raspberrypi4b/vxsdk"
export WIND_VX7_HOST_TYPE=x86_64-linux
export WIND_CC_SYSROOT="$WIND_BASE/sysroot"
export PATH="$WIND_BASE/host/$WIND_VX7_HOST_TYPE/bin:$PATH"

which wr-c++
wr-c++ --version

ROXAL_BUILD=ON

# -------------------------------
# CMake configure
# -------------------------------
cmake -G Ninja \
  -S "${ROXAL_ROOT}" \
  -B "${BUILD_DIR}" \
  -DCMAKE_TOOLCHAIN_FILE="${ROXAL_ROOT}/toolchains/vxworks.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DEIGEN3_INCLUDE_DIR=/usr/local/include/eigen3

# -------------------------------
# Build
# -------------------------------
cmake --build "${BUILD_DIR}"
echo "Roxal build completed."

