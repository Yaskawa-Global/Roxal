# Multi-stage Dockerfile for building Roxal .deb packages.
# Works for both amd64 and arm64 via: --platform linux/arm64
#
# Usage (via docker-build.sh wrapper):
#   ./packaging/docker-build.sh --platform linux/arm64
#   ./packaging/docker-build.sh --platform linux/amd64
#
# ONNX Runtime is built from source with CUDA support on both architectures.
# The CUDA_ARCHITECTURES build arg controls which GPU targets are compiled.

# ============================================================================
# Stage 1: deps — apt packages + from-source dependencies (cached layer)
# ============================================================================
FROM nvidia/cuda:13.0.2-cudnn-devel-ubuntu24.04 AS deps

ENV DEBIAN_FRONTEND=noninteractive

# Parallel job count for cmake --build.  Default 1 is safe for QEMU
# cross-builds; docker-build.sh passes a higher value for native builds.
ARG JOBS=1

# CUDA architectures to compile ONNX Runtime for.
# 70=V100 75=T4 80=A100 86=RTX3090 87=Orin 89=L40/RTX4090 90=H100
# 100=B100/B200 120=RTX5090 121=GB10/DGX-Spark
ARG CUDA_ARCHITECTURES="70;75;80;86;87;89;90;100;120;121"

# cmake from Kitware APT repo (Ubuntu 24.04's cmake 3.28 is too old for
# ONNX Runtime's CUDA build; we need 3.31+)
RUN apt-get update && apt-get install -y --no-install-recommends gpg wget ca-certificates \
    && wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
       | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg \
    && echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ noble main' \
       > /etc/apt/sources.list.d/kitware.list \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake pkg-config git wget ca-certificates \
    python3 python3-pip default-jre-headless \
    libicu-dev libffi-dev libboost-program-options-dev libeigen3-dev \
    libpcre2-dev protobuf-compiler libprotobuf-dev libprotoc-dev libgrpc++-dev \
    patchelf file \
    && rm -rf /var/lib/apt/lists/*

# antlr4 command-line tool (Java-based parser generator)
RUN pip install --break-system-packages antlr4-tools

# antlr4-runtime 4.13.1 C++ library (from source — version must match tool)
# Split clone/build so Docker caches the clone even if build fails.
# Retry loop: QEMU user-mode emulation can randomly segfault cc1plus.
RUN git clone --depth 1 --branch 4.13.1 https://github.com/antlr/antlr4.git /tmp/antlr4

RUN cmake -B /tmp/antlr4/build -S /tmp/antlr4/runtime/Cpp \
       -DCMAKE_INSTALL_PREFIX=/opt/antlr4 -DCMAKE_INSTALL_LIBDIR=lib \
       -DCMAKE_BUILD_TYPE=Release -DANTLR_BUILD_CPP_TESTS=OFF \
    && for attempt in 1 2 3 4 5; do \
         echo "=== antlr4 build attempt ${attempt}/5 ===" && \
         cmake --build /tmp/antlr4/build -j${JOBS} && break; \
         [ $attempt -eq 5 ] && exit 1; \
         echo "Build failed (likely QEMU segfault), retrying..."; \
       done \
    && cmake --install /tmp/antlr4/build \
    && rm -rf /tmp/antlr4

# CycloneDDS from source (not in Ubuntu 24.04 repos)
RUN git clone --depth 1 \
      https://github.com/eclipse-cyclonedds/cyclonedds.git /tmp/cdds

RUN cmake -B /tmp/cdds/build -S /tmp/cdds \
       -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release \
    && for attempt in 1 2 3 4 5; do \
         echo "=== CycloneDDS build attempt ${attempt}/5 ===" && \
         cmake --build /tmp/cdds/build -j${JOBS} && break; \
         [ $attempt -eq 5 ] && exit 1; \
         echo "Build failed (likely QEMU segfault), retrying..."; \
       done \
    && cmake --install /tmp/cdds/build \
    && rm -rf /tmp/cdds

# ONNX Runtime — built from source with CUDA support
# Clone and build are separate layers so the ~2GB recursive clone is cached.
RUN git clone --depth 1 --branch v1.24.1 --recursive \
      https://github.com/microsoft/onnxruntime.git /tmp/ort

RUN cd /tmp/ort \
    && for attempt in 1 2 3 4 5; do \
         echo "=== ONNX Runtime build attempt ${attempt}/5 ===" && \
         ./build.sh --config Release --parallel \
           --use_cuda --cuda_home /usr/local/cuda --cudnn_home /usr \
           --build_shared_lib --skip_tests --allow_running_as_root \
           --cmake_extra_defines CMAKE_CUDA_ARCHITECTURES="${CUDA_ARCHITECTURES}" \
         && break; \
         [ $attempt -eq 5 ] && exit 1; \
         echo "Build failed (likely QEMU segfault), retrying..."; \
       done \
    && mkdir -p /opt/onnxruntime/lib /opt/onnxruntime/include \
    && cp -a build/Linux/Release/libonnxruntime*.so* /opt/onnxruntime/lib/ \
    && rm -f /opt/onnxruntime/lib/libonnxruntime_runtime_path_test_shared_library.so \
    && cp -r include/onnxruntime /opt/onnxruntime/include/ \
    && for f in /opt/onnxruntime/include/onnxruntime/core/session/*.h \
               /opt/onnxruntime/include/onnxruntime/core/providers/cpu/*.h \
               /opt/onnxruntime/include/onnxruntime/core/providers/cuda/*.h; do \
         ln -sf "$f" /opt/onnxruntime/include/$(basename "$f"); \
       done \
    && cd / && rm -rf /tmp/ort

# ============================================================================
# Stage 2: builder — compile Roxal and package the .deb
# ============================================================================
FROM deps AS builder

# Re-declare ARG so it's available in this stage
ARG JOBS=1

COPY . /src
WORKDIR /src

# --fresh clears any stale CMakeCache.txt copied from host
RUN cmake -B build --fresh \
      -DCMAKE_BUILD_TYPE=Release \
      -DROXAL_ANTLR4_ROOT=/opt/antlr4 \
      -DROXAL_ENABLE_FILEIO=ON \
      -DROXAL_ENABLE_REGEX=ON \
      -DROXAL_ENABLE_SOCKET=ON \
      -DROXAL_ENABLE_GRPC=ON \
      -DROXAL_ENABLE_DDS=ON \
      -DROXAL_DDS_ROOT=/usr/local \
      -DROXAL_ENABLE_AI_NN=ON \
      -DROXAL_ONNX_ROOT=/opt/onnxruntime \
    && for attempt in 1 2 3 4 5; do \
         echo "=== Roxal build attempt ${attempt}/5 ===" && \
         cmake --build build -j${JOBS} && break; \
         [ $attempt -eq 5 ] && exit 1; \
         echo "Build failed (likely QEMU segfault), retrying..."; \
       done

RUN mkdir -p /out && \
    ./packaging/build-deb.sh \
      --antlr4-lib-dir /opt/antlr4/lib \
      --dds-lib-dir /usr/local/lib \
      --ort-lib-dir /opt/onnxruntime/lib \
      --output-dir /out

# ============================================================================
# Stage 3: output — extract just the .deb artifact
# ============================================================================
FROM scratch AS output
COPY --from=builder /out/*.deb /
