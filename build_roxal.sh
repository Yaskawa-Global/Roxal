# ~/Roxal/build_roxal.sh
#!/bin/bash
set -euo pipefail

echo "Building Roxal"

# -------------------------------
# Roxal 루트 자동 감지
# -------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROXAL_ROOT="${SCRIPT_DIR}"

echo "Roxal root: ${ROXAL_ROOT}"

BUILD_DIR="${ROXAL_ROOT}/build"

# -------------------------------
# Full clean 옵션
# -------------------------------
if [ "${FULLCLEAN:-1}" = "1" ]; then
  echo "[Full clean] Removing build dir: ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi

# -------------------------------
# VxWorks 환경 설정
# -------------------------------
export WIND_BASE="$HOME/wrsdk-vxworks7-raspberrypi4b/vxsdk"
export WIND_VX7_HOST_TYPE=x86_64-linux
export WIND_CC_SYSROOT="$WIND_BASE/sysroot"
export PATH="$WIND_BASE/host/$WIND_VX7_HOST_TYPE/bin:$PATH"

which wr-c++
wr-c++ --version

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

