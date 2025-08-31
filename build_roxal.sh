# ~/Roxal/build_roxal.sh
#!/bin/bash
set -euo pipefail
echo "Building Roxal"

if [ "${FULLCLEAN:-1}" = "1" ]; then
  echo "[Full clean] Removing build dir: build"
  rm -rf build
fi

export WIND_BASE="$HOME/wrsdk-vxworks7-raspberrypi4b/vxsdk"
export WIND_VX7_HOST_TYPE=x86_64-linux
export WIND_CC_SYSROOT=$WIND_BASE/sysroot
export PATH=$WIND_BASE/host/$WIND_VX7_HOST_TYPE/bin:$PATH


which wr-c++
wr-c++ --version


rm -rf ~/Roxal-main/build
cmake -G Ninja \
  -S ~/Roxal\
  -B ~/Roxal/build \
  -DCMAKE_TOOLCHAIN_FILE=~/Roxal/toolchains/vxworks.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DEIGEN3_INCLUDE_DIR=/usr/local/include/eigen3


cmake --build ~/Roxal/build -j

