#!/bin/bash
# Build librsshim.so against the librealsense install in deps/librealsense/install.
# The rpath is $ORIGIN-relative so the shim finds the SDK from its location in
# modules/realsense/ without any environment setup.
set -e
cd "$(dirname "$0")"
PREFIX=../../../deps/librealsense/install
g++ -shared -fPIC -O2 -std=c++17 -o ../librsshim.so rs_shim.cpp \
  -I"$PREFIX/include" \
  -L"$PREFIX/lib" \
  -Wl,-rpath,'$ORIGIN/../../deps/librealsense/install/lib' \
  -lrealsense2
echo "built modules/realsense/librsshim.so"
