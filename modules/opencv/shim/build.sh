#!/bin/bash
# Build libcvxshim.so against the OpenCV 5 install in deps/opencv/install.
# The rpath is $ORIGIN-relative so the shim finds the OpenCV libs from its
# location in modules/opencv/ without any environment setup.
set -e
cd "$(dirname "$0")"
PREFIX=../../../deps/opencv/install
g++ -shared -fPIC -O2 -std=c++17 -o ../libcvxshim.so cvx_shim.cpp \
  -I"$PREFIX/include/opencv5" \
  -L"$PREFIX/lib" \
  -Wl,-rpath,'$ORIGIN/../../deps/opencv/install/lib' \
  -lopencv_core -lopencv_imgproc -lopencv_geometry -lopencv_imgcodecs -lopencv_videoio -lopencv_objdetect -lopencv_calib -lopencv_features -lopencv_stereo -lopencv_video
echo "built modules/opencv/libcvxshim.so"
