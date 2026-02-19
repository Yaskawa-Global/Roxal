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
    libeigen3-dev \
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

# Optional features:
#   gRPC/protobuf: apt-get install -y protobuf-compiler libprotobuf-dev libgrpc++-dev
#   CycloneDDS:    apt-get install -y libcyclonedds-dev
#   ONNX Runtime (for ai.nn ML inference module):
#     mkdir -p deps && cd deps
#
#     CPU-only:
#       wget https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/onnxruntime-linux-x64-1.24.1.tgz
#       tar -xzf onnxruntime-linux-x64-1.24.1.tgz
#       mv onnxruntime-linux-x64-1.24.1 onnxruntime
#       rm onnxruntime-linux-x64-1.24.1.tgz
#
#     GPU (CUDA) — requires NVIDIA driver, CUDA toolkit, and cuDNN 9:
#       apt-get install -y nvidia-cuda-toolkit libcudnn9-cuda-12
#       wget https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/onnxruntime-linux-x64-gpu-1.24.1.tgz
#       tar -xzf onnxruntime-linux-x64-gpu-1.24.1.tgz
#       mv onnxruntime-linux-x64-gpu-1.24.1 onnxruntime
#       rm onnxruntime-linux-x64-gpu-1.24.1.tgz

# Download MNIST ONNX model from the official ONNX Model Zoo
#  (added to repo)
#echo "downloading MNIST ONNX model"
#mkdir -p modules/ai && cd modules/ai
#wget -N https://huggingface.co/onnxmodelzoo/mnist-8/resolve/main/mnist-8.onnx -O mnist-8.onnx
#cd -
