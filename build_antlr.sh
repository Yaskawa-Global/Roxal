# ~/Roxal/build_antlr.sh
#!/bin/bash
echo "Building antlr4-runtime"

cd External/antlr4-cpp-runtime
set -euo pipefail


export WIND_BASE="$HOME/wrsdk-vxworks7-raspberrypi4b/vxsdk"
export WIND_VX7_HOST_TYPE=x86_64-linux
export WIND_CC_SYSROOT=$WIND_BASE/sysroot
export PATH=$WIND_BASE/host/$WIND_VX7_HOST_TYPE/bin:$PATH


which wr-c++
wr-c++ --version


rm -rf ~/Roxal/External/antlr4-cpp-runtime/build
cmake -G Ninja \
  -S ~/Roxal/External/antlr4-cpp-runtime \
  -B ~/Roxal/External/antlr4-cpp-runtime/build \
  -DCMAKE_CXX_STANDARD=17 \
  -DCMAKE_CXX_STANDARD_REQUIRED=ON \
  -DCMAKE_TOOLCHAIN_FILE=~/Roxal/toolchains/vxworks.cmake \
  -DCMAKE_SYSTEM_NAME="VxWorks" \
  -DCMAKE_BUILD_TYPE=Release \


cmake --build ~/Roxal/External/antlr4-cpp-runtime/build -j


