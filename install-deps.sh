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
    libantlr4-runtime-dev \
    libicu-dev \
    libffi-dev \
    libboost-program-options-dev \
    libeigen3-dev

echo "installing python packages"
# ANTLR tool (used at build time to generate the parser)
python3 -m pip install --user --upgrade antlr4-tools

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
