#!/usr/bin/env bash
echo "updating package lists"
apt-get update
apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    python3 \
    python3-pip \
    libpcre2-dev
apt-get install -y \
    libicu-dev \
    libffi-dev \
    libboost-program-options-dev \
    git

echo "installing python packages"
# ANTLR4 command-line tool (Java-based parser generator, used at build time)
# The pip package downloads the correct jar when invoked with -v 4.13.1.
python3 -m pip install --user --upgrade antlr4-tools

# ANTLR4 C++ runtime library (build from source for exact version match)
# The runtime version MUST match the antlr4 tool version (4.13.1).
# Ubuntu apt ships an older version that is incompatible with the parser.
echo "building antlr4-runtime 4.13.1 from source into deps/antlr4"
git clone --depth 1 --branch 4.13.1 https://github.com/antlr/antlr4.git /tmp/antlr4-build
cmake -B /tmp/antlr4-build/build -S /tmp/antlr4-build/runtime/Cpp \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/deps/antlr4" -DCMAKE_BUILD_TYPE=Release \
    -DANTLR_BUILD_CPP_TESTS=OFF
cmake --build /tmp/antlr4-build/build -j$(nproc)
cmake --install /tmp/antlr4-build/build
rm -rf /tmp/antlr4-build

# Eigen 5.0.1 (header-only, installs cmake config + headers)
echo "building Eigen 5.0.1 from source into deps/eigen"
git clone --depth 1 --branch 5.0.1 https://gitlab.com/libeigen/eigen.git /tmp/eigen-build
cmake -B /tmp/eigen-build/build -S /tmp/eigen-build \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/deps/eigen" \
    -DCMAKE_BUILD_TYPE=Release \
    -DEIGEN_BUILD_BLAS=OFF \
    -DEIGEN_BUILD_LAPACK=OFF
cmake --install /tmp/eigen-build/build
rm -rf /tmp/eigen-build

# Optional features:
#   media (PNG/JPEG): apt-get install -y libpng-dev libjpeg-dev
#   gRPC/protobuf (C++ runtime):
#     apt-get install -y protobuf-compiler libprotobuf-dev libprotoc-dev libgrpc++-dev
#   gRPC/protobuf (Python, needed by runtests.py test server):
#     python3 -m pip install --user --break-system-packages grpcio grpcio-tools
#   CycloneDDS:    build from source (see packaging/Dockerfile for reference)
#
# ONNX Runtime (for ai.nn ML inference module):
#
#   x64 CPU-only:
#     mkdir -p deps
#     wget https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/onnxruntime-linux-x64-1.24.1.tgz
#     tar -xzf onnxruntime-linux-x64-1.24.1.tgz
#     mv onnxruntime-linux-x64-1.24.1 deps/onnxruntime
#     rm onnxruntime-linux-x64-1.24.1.tgz
#
#   x64 GPU (CUDA 12) — pre-built binary:
#     mkdir -p deps
#     wget https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/onnxruntime-linux-x64-gpu-1.24.1.tgz
#     tar -xzf onnxruntime-linux-x64-gpu-1.24.1.tgz
#     mv onnxruntime-linux-x64-gpu-1.24.1 deps/onnxruntime
#     rm onnxruntime-linux-x64-gpu-1.24.1.tgz
#
#   arm64 CPU-only — pre-built binary:
#     mkdir -p deps
#     wget https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/onnxruntime-linux-aarch64-1.24.1.tgz
#     tar -xzf onnxruntime-linux-aarch64-1.24.1.tgz
#     mv onnxruntime-linux-aarch64-1.24.1 deps/onnxruntime
#     rm onnxruntime-linux-aarch64-1.24.1.tgz
#
#   arm64 GPU (CUDA) — no pre-built binary; build from source:
#     Requires: cmake 3.31+ (apt.kitware.com), CUDA toolkit, cuDNN 9 (apt install cudnn9-cuda-13)
#     git clone --depth 1 --branch v1.24.1 --recursive https://github.com/microsoft/onnxruntime.git /tmp/ort-build
#     cd /tmp/ort-build
#     ./build.sh --config Release --parallel \
#       --use_cuda --cuda_home /usr/local/cuda --cudnn_home /usr \
#       --build_shared_lib --skip_tests \
#       --cmake_extra_defines CMAKE_CUDA_ARCHITECTURES=native
#     mkdir -p <roxal-dir>/deps/onnxruntime/lib
#     cp -a build/Linux/Release/libonnxruntime*.so* <roxal-dir>/deps/onnxruntime/lib/
#     rm -f <roxal-dir>/deps/onnxruntime/lib/libonnxruntime_runtime_path_test_shared_library.so
#     cp -r include <roxal-dir>/deps/onnxruntime/
#     # Flatten headers (from-source layout differs from pre-built tarballs)
#     for f in <roxal-dir>/deps/onnxruntime/include/onnxruntime/core/session/*.h \
#              <roxal-dir>/deps/onnxruntime/include/onnxruntime/core/providers/cpu/*.h \
#              <roxal-dir>/deps/onnxruntime/include/onnxruntime/core/providers/cuda/*.h; do
#       ln -sf "$f" <roxal-dir>/deps/onnxruntime/include/$(basename "$f")
#     done
#     cd - && rm -rf /tmp/ort-build
