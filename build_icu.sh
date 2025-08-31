# ~/Roxal/build_icu.sh
#! /bin/bash
echo "Building ICU"
cd External/icu
set -euo pipefail

if [ "${FULLCLEAN:-1}" = "1" ]; then
  echo "[Full clean] Removing build dir: build-vx-rpi4"
  rm -rf build-vx-rpi4
fi

#host utility setting
export SHELL=/bin/sh
export CONFIG_SHELL=/bin/sh
export GREP=/usr/bin/grep
export SED=/usr/bin/sed
export AWK=/usr/bin/awk
export PATH="/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

export WIND_BASE="$HOME/wrsdk-vxworks7-raspberrypi4b/vxsdk"
export WIND_VX7_HOST_TYPE=x86_64-linux
export WIND_CC_SYSROOT=$WIND_BASE/sysroot
export PATH=$WIND_BASE/host/$WIND_VX7_HOST_TYPE/bin:$PATH

which wr-c++
wr-c++ --version

cmake -S . -B build-vx-rpi4 \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_ICU=ON \
  -DICU_STATIC=ON \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DICU_BUILD_VERSION=74.2 \
  -DCMAKE_TOOLCHAIN_FILE=~/Roxal/toolchains/vxworks.cmake \
  -DICU_CROSS_ARCH=aarch64-wrs-vxworks7 \
  -DICU_CFG_OPTIONS:STRING="--disable-shared;--enable-static;--disable-tests;--disable-samples;--disable-extras;--with-data-packaging=static"

cmake --build build-vx-rpi4 -v


