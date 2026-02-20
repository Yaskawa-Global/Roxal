# Multi-stage Dockerfile for building Roxal .deb packages.
# Works for both amd64 and arm64 via: --platform linux/arm64
#
# Usage (via docker-build.sh wrapper):
#   ./packaging/docker-build.sh --platform linux/arm64
#   ./packaging/docker-build.sh --platform linux/amd64

# ============================================================================
# Stage 1: deps — apt packages + from-source dependencies (cached layer)
# ============================================================================
FROM ubuntu:24.04 AS deps

ENV DEBIAN_FRONTEND=noninteractive

# Parallel job count for cmake --build.  Default 1 is safe for QEMU
# cross-builds; docker-build.sh passes a higher value for native builds.
ARG JOBS=1

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
# Retry loop: QEMU user-mode emulation can randomly segfault cc1plus.
# cmake --build resumes from where it left off, so retries are cheap.
RUN git clone --depth 1 --branch 4.13.1 https://github.com/antlr/antlr4.git /tmp/antlr4 \
    && cmake -B /tmp/antlr4/build -S /tmp/antlr4/runtime/Cpp \
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
      https://github.com/eclipse-cyclonedds/cyclonedds.git /tmp/cdds \
    && cmake -B /tmp/cdds/build -S /tmp/cdds \
       -DCMAKE_INSTALL_PREFIX=/usr/local -DCMAKE_BUILD_TYPE=Release \
    && for attempt in 1 2 3 4 5; do \
         echo "=== CycloneDDS build attempt ${attempt}/5 ===" && \
         cmake --build /tmp/cdds/build -j${JOBS} && break; \
         [ $attempt -eq 5 ] && exit 1; \
         echo "Build failed (likely QEMU segfault), retrying..."; \
       done \
    && cmake --install /tmp/cdds/build \
    && rm -rf /tmp/cdds

# ONNX Runtime pre-built binary
# x64: GPU build (includes CUDA + TensorRT providers)
# aarch64: CPU-only (no pre-built GPU variant available)
RUN ARCH=$(uname -m) && \
    if [ "$ARCH" = "aarch64" ]; then ORT_PKG="onnxruntime-linux-aarch64-1.24.1.tgz"; \
    else ORT_PKG="onnxruntime-linux-x64-gpu-1.24.1.tgz"; fi && \
    wget -q "https://github.com/microsoft/onnxruntime/releases/download/v1.24.1/${ORT_PKG}" \
      -O /tmp/ort.tgz && \
    mkdir -p /opt/onnxruntime && \
    tar -xzf /tmp/ort.tgz -C /opt/onnxruntime --strip-components=1 && \
    rm /tmp/ort.tgz

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
