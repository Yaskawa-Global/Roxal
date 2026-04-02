#!/usr/bin/env bash
#
# Build and install from-source dependencies into deps/
#
# Usage:
#   bash install-deps.sh                # build core deps (eigen, antlr4)
#   bash install-deps.sh all            # build all deps (GPU ONNX by default)
#   bash install-deps.sh all --cpu-only # build all deps (CPU-only ONNX)
#   bash install-deps.sh eigen          # build only eigen
#   bash install-deps.sh onnxruntime    # just ONNX Runtime (GPU)
#   bash install-deps.sh onnxruntime --cpu-only
#
# Available targets: eigen antlr4 cyclonedds grpc media onnxruntime
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPS_DIR="${SCRIPT_DIR}/deps"
JOBS="${JOBS:-$(nproc)}"
BUILD_TMP="/tmp/roxal-deps-build"

CORE_TARGETS=(eigen antlr4)
ALL_TARGETS=(eigen antlr4 cyclonedds grpc media onnxruntime)

# Parse --cpu-only flag
ONNX_CPU_ONLY=false
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        --cpu-only) ONNX_CPU_ONLY=true ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

# If arguments given, build only those; otherwise build core
if [ $# -gt 0 ]; then
    if [ "$1" = "all" ]; then
        TARGETS=("${ALL_TARGETS[@]}")
    else
        TARGETS=("$@")
    fi
else
    TARGETS=("${CORE_TARGETS[@]}")
fi

should_build() { printf '%s\n' "${TARGETS[@]}" | grep -qx "$1"; }

mkdir -p "$DEPS_DIR" "$BUILD_TMP"

# --- Install system apt prerequisites if missing ---
APT_PACKAGES=(
    build-essential cmake pkg-config git wget ca-certificates
    python3 python3-pip default-jre-headless
    libicu-dev libffi-dev libboost-program-options-dev
    libpcre2-dev
)
# Optional apt deps based on targets
should_build media && APT_PACKAGES+=(libpng-dev libjpeg-dev)

MISSING=()
for pkg in "${APT_PACKAGES[@]}"; do
    if ! dpkg -s "$pkg" &>/dev/null; then
        MISSING+=("$pkg")
    fi
done
if [ ${#MISSING[@]} -gt 0 ]; then
    echo "=== Installing missing system packages: ${MISSING[*]} ==="
    sudo apt-get update
    sudo apt-get install -y "${MISSING[@]}"
fi

# --- ANTLR4 command-line tool (Java-based parser generator, used at build time) ---
# The pip package downloads the correct jar when invoked with -v 4.13.1.
if should_build antlr4; then
    if ! python3 -c "import antlr4_tools" &>/dev/null; then
        echo "=== Installing antlr4-tools (pip) ==="
        python3 -m pip install --user --upgrade antlr4-tools
    fi
fi

# ---------- Eigen 5.0.1 (header-only, installs cmake config + headers) ----------
if should_build eigen; then
    echo "=== Building Eigen 5.0.1 ==="
    cd "$BUILD_TMP"
    [ -d eigen ] || git clone --depth 1 --branch 5.0.1 https://gitlab.com/libeigen/eigen.git eigen
    cmake -B eigen/build -S eigen \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/eigen" \
        -DBUILD_TESTING=OFF \
        -DEIGEN_BUILD_BLAS=OFF \
        -DEIGEN_BUILD_LAPACK=OFF
    cmake --install eigen/build
    echo "  -> installed to $DEPS_DIR/eigen"
fi

# ---------- ANTLR4 C++ runtime 4.13.1 ----------
if should_build antlr4; then
    echo "=== Building antlr4-runtime 4.13.1 ==="
    cd "$BUILD_TMP"
    [ -d antlr4 ] || git clone --depth 1 --branch 4.13.1 https://github.com/antlr/antlr4.git
    cmake -B antlr4/build -S antlr4/runtime/Cpp \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/antlr4" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release \
        -DANTLR_BUILD_CPP_TESTS=OFF
    cmake --build antlr4/build -j"$JOBS"
    cmake --install antlr4/build
    echo "  -> installed to $DEPS_DIR/antlr4"
fi

# ---------- CycloneDDS (C library) + CycloneDDS-CXX ----------
if should_build cyclonedds; then
    echo "=== Building CycloneDDS 11.0.0 ==="
    cd "$BUILD_TMP"
    [ -d cyclonedds ] || git clone --depth 1 --branch 11.0.0 https://github.com/eclipse-cyclonedds/cyclonedds.git
    cmake -B cyclonedds/build -S cyclonedds \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/cyclonedds" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build cyclonedds/build -j"$JOBS"
    cmake --install cyclonedds/build

    echo "=== Building CycloneDDS-CXX 11.0.0 ==="
    [ -d cyclonedds-cxx ] || git clone --depth 1 --branch 11.0.0 https://github.com/eclipse-cyclonedds/cyclonedds-cxx.git
    cmake -B cyclonedds-cxx/build -S cyclonedds-cxx \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/cyclonedds" \
        -DCycloneDDS_DIR="$DEPS_DIR/cyclonedds/lib/cmake/CycloneDDS" \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build cyclonedds-cxx/build -j"$JOBS"
    cmake --install cyclonedds-cxx/build
    echo "  -> installed to $DEPS_DIR/cyclonedds"
fi

# ---------- gRPC 1.51.1 (includes protobuf) ----------
if should_build grpc; then
    echo "=== Building gRPC 1.51.1 + protobuf (this takes a while) ==="
    cd "$BUILD_TMP"
    if [ ! -d grpc ]; then
        git clone --recurse-submodules --depth 1 --branch v1.51.1 https://github.com/grpc/grpc.git
    fi
    cmake -B grpc/build -S grpc \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/grpc" \
        -DCMAKE_BUILD_TYPE=Release \
        -DgRPC_INSTALL=ON \
        -DgRPC_BUILD_TESTS=OFF \
        -DABSL_PROPAGATE_CXX_STD=ON
    cmake --build grpc/build -j"$JOBS"
    cmake --install grpc/build
    echo "  -> installed to $DEPS_DIR/grpc"

    # Also install Python gRPC packages (needed by runtests.py test server)
    python3 -m pip install --user --break-system-packages grpcio grpcio-tools 2>/dev/null \
        || python3 -m pip install --user grpcio grpcio-tools
fi

# ---------- Media (PNG/JPEG — apt only, no from-source build needed) ----------
if should_build media; then
    echo "=== Media dependencies (libpng, libjpeg) installed via apt ==="
fi

# ---------- ONNX Runtime 1.24.1 ----------
# Default: GPU (CUDA 12) pre-built binary. Use --cpu-only for CPU-only.
# arm64 GPU has no pre-built binary — must build from source (see comments below).
if should_build onnxruntime; then
    ONNX_VER="1.24.1"
    ARCH="$(uname -m)"

    if [ "$ARCH" = "x86_64" ]; then
        if [ "$ONNX_CPU_ONLY" = true ]; then
            ONNX_SLUG="onnxruntime-linux-x64-${ONNX_VER}"
            echo "=== Installing ONNX Runtime ${ONNX_VER} (x64, CPU-only) ==="
        else
            ONNX_SLUG="onnxruntime-linux-x64-gpu-${ONNX_VER}"
            echo "=== Installing ONNX Runtime ${ONNX_VER} (x64, GPU/CUDA 12) ==="
        fi
    elif [ "$ARCH" = "aarch64" ]; then
        if [ "$ONNX_CPU_ONLY" = true ]; then
            ONNX_SLUG="onnxruntime-linux-aarch64-${ONNX_VER}"
            echo "=== Installing ONNX Runtime ${ONNX_VER} (arm64, CPU-only) ==="
        else
            echo "ERROR: no pre-built GPU binary for arm64. Build from source (see install-deps.sh) or use --cpu-only."
            exit 1
        fi
    else
        echo "ERROR: unsupported architecture $ARCH for ONNX Runtime pre-built binary."
        exit 1
    fi

    ONNX_TAR="${ONNX_SLUG}.tgz"
    if [ ! -d "$DEPS_DIR/onnxruntime" ]; then
        cd "$BUILD_TMP"
        [ -f "$ONNX_TAR" ] || wget "https://github.com/microsoft/onnxruntime/releases/download/v${ONNX_VER}/$ONNX_TAR"
        tar -xzf "$ONNX_TAR"
        mv "$ONNX_SLUG" "$DEPS_DIR/onnxruntime"
        rm -f "$ONNX_TAR"
    fi
    echo "  -> installed to $DEPS_DIR/onnxruntime"
    #
    # arm64 GPU (CUDA) — build from source:
    #   Requires: cmake 3.31+, CUDA toolkit, cuDNN 9 (apt install cudnn9-cuda-13)
    #   git clone --depth 1 --branch v1.24.1 --recursive https://github.com/microsoft/onnxruntime.git /tmp/ort-build
    #   cd /tmp/ort-build
    #   ./build.sh --config Release --parallel \
    #     --use_cuda --cuda_home /usr/local/cuda --cudnn_home /usr \
    #     --build_shared_lib --skip_tests \
    #     --cmake_extra_defines CMAKE_CUDA_ARCHITECTURES=native
    #   mkdir -p deps/onnxruntime/lib
    #   cp -a build/Linux/Release/libonnxruntime*.so* deps/onnxruntime/lib/
    #   cp -r include deps/onnxruntime/
fi

echo ""
echo "=== All requested deps built ==="
echo "Contents of $DEPS_DIR:"
ls "$DEPS_DIR"
echo ""
echo "You can now clean up build temps with: rm -rf $BUILD_TMP"
