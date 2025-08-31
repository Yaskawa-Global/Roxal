# ~/Roxal/build_roxal.sh
#!/bin/bash
echo "Building ICU"

cd External/ffi
set -euo pipefail

export WIND_HOME="$HOME/WindRiver"
export WIND_BASE="$HOME/wrsdk-vxworks7-raspberrypi4b/vxsdk"
export WIND_VX7_HOST_TYPE=x86_64-linux
export WIND_CC_SYSROOT=$WIND_BASE/sysroot
export PATH=$WIND_BASE/host/$WIND_VX7_HOST_TYPE/bin:$PATH


which wr-c++
wr-c++ --version

rm -rf ~/Roxal/External/ffi/build
cmake -G Ninja \
  -S ~/Roxal/External/ffi \
  -B ~/Roxal/External/ffi/build \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_TOOLCHAIN_FILE=~/Roxal/toolchains/vxworksffi.cmake \
  -DCMAKE_BUILD_TYPE=Release \
 
cmake --build ~/Roxal/External/ffi/build -j
