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
#   bash install-deps.sh --help         # list every target + install status
#
# Available targets: eigen antlr4 cyclonedds grpc media pugixml onnxruntime
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEPS_DIR="${SCRIPT_DIR}/deps"
JOBS="${JOBS:-$(nproc)}"
BUILD_TMP="/tmp/roxal-deps-build"

CORE_TARGETS=(eigen antlr4)
ALL_TARGETS=(eigen antlr4 cyclonedds grpc media pugixml onnxruntime opencv librealsense)

# WebAssembly cross-build toolchain. Deliberately NOT part of 'all': these are a
# cross toolchain rather than native libraries, and emsdk alone is ~1GB, so
# pulling it into "build every native dep" would be a surprise. Use 'wasm'.
WASM_TARGETS=(emsdk antlr4-wasm pcre2-wasm pugixml-wasm)
KNOWN_TARGETS=("${ALL_TARGETS[@]}" "${WASM_TARGETS[@]}")

# emsdk installs OUTSIDE deps/ -- it is a toolchain, not a library, and this is
# the path wasm/build.sh already probes.
EMSDK_DIR="${EMSDK:-$HOME/dev/emsdk}"

# One-line description per target, shown by --help.
declare -A TARGET_DESC=(
    [eigen]="Eigen 5.0.1 - header-only linear algebra (core)"
    [antlr4]="ANTLR4 4.13.1 C++ runtime + antlr4-tools (core)"
    [cyclonedds]="CycloneDDS 11.0.0 + CycloneDDS-CXX (DDS pub/sub)"
    [grpc]="gRPC 1.51.1 + protobuf (RPC; slow build)"
    [media]="PNG/JPEG image libs (apt only; no deps/ folder)"
    [pugixml]="pugixml 1.15 (lightweight XML parser)"
    [onnxruntime]="ONNX Runtime 1.24.1 (ML inference; GPU default, --cpu-only)"
    [opencv]="OpenCV 5.0.0 (modules/opencv FFI binding; builds libcvxshim.so)"
    [librealsense]="librealsense 2.58.3 (modules/realsense FFI binding; builds librsshim.so)"
    [emsdk]="Emscripten SDK (WebAssembly toolchain; installs to \$HOME/dev/emsdk)"
    [antlr4-wasm]="ANTLR4 4.13.1 C++ runtime cross-built for wasm (needs emsdk)"
    [pcre2-wasm]="PCRE2 10.44 cross-built for wasm (regex module; needs emsdk)"
    [pugixml-wasm]="pugixml 1.15 cross-built for wasm (xml builtins; needs emsdk)"
)

print_help() {
    cat <<EOF
install-deps.sh - build/install Roxal's from-source dependencies into deps/

Usage:
  install-deps.sh [target ...] [--cpu-only]
  install-deps.sh all [--cpu-only]
  install-deps.sh --help

With no target, builds the core set: ${CORE_TARGETS[*]}
'all' builds every target:          ${ALL_TARGETS[*]}

Targets:
EOF
    local t status
    for t in "${KNOWN_TARGETS[@]}"; do
        if [ "$t" = media ]; then
            status="apt-only"
        elif [ "$t" = emsdk ]; then
            # A toolchain, not a deps/ library -- see EMSDK_DIR.
            [ -f "$EMSDK_DIR/emsdk_env.sh" ] && status="installed" || status="missing"
        elif [ "$t" = antlr4-wasm ]; then
            [ -d "$DEPS_DIR/antlr4-wasm-mt" ] && status="installed" || status="missing"
        elif [ "$t" = pcre2-wasm ]; then
            [ -d "$DEPS_DIR/pcre2-wasm" ] && status="installed" || status="missing"
        elif [ "$t" = pugixml-wasm ]; then
            [ -d "$DEPS_DIR/pugixml-wasm" ] && status="installed" || status="missing"
        elif [ -d "$DEPS_DIR/$t" ]; then
            status="installed"
        else
            status="missing"
        fi
        printf '  %-12s [%-9s] %s\n' "$t" "$status" "${TARGET_DESC[$t]:-}"
    done
    cat <<EOF

Options:
  --cpu-only   Build ONNX Runtime CPU-only (default is GPU / CUDA 12)
  -h, --help   Show this help and exit

deps/ dir: $DEPS_DIR

Notes:
  * [installed] means a deps/<name> folder already exists.  Re-running a target
    rebuilds/updates it in place; builds are NOT auto-skipped (except
    onnxruntime).  To force a clean rebuild, remove deps/<name> (and its
    $BUILD_TMP/<name> source checkout) first.

Examples:
  install-deps.sh                    # core deps (${CORE_TARGETS[*]})
  install-deps.sh all                # everything (GPU ONNX)
  install-deps.sh all --cpu-only     # everything (CPU ONNX)
  install-deps.sh grpc cyclonedds    # just these two
  install-deps.sh wasm               # WebAssembly toolchain (${WASM_TARGETS[*]})
EOF
}

# Parse --cpu-only flag
ONNX_CPU_ONLY=false
POSITIONAL=()
for arg in "$@"; do
    case "$arg" in
        -h|--help) print_help; exit 0 ;;
        --cpu-only) ONNX_CPU_ONLY=true ;;
        *) POSITIONAL+=("$arg") ;;
    esac
done
set -- "${POSITIONAL[@]+"${POSITIONAL[@]}"}"

# If arguments given, build only those; otherwise build core
if [ $# -gt 0 ]; then
    if [ "$1" = "all" ]; then
        TARGETS=("${ALL_TARGETS[@]}")
    elif [ "$1" = "wasm" ]; then
        TARGETS=("${WASM_TARGETS[@]}")
    else
        TARGETS=("$@")
    fi
else
    TARGETS=("${CORE_TARGETS[@]}")
fi

# Reject unknown target names up-front (a typo would otherwise silently
# build nothing).  'all'/'wasm' and the default core set are already known-good.
if [ $# -gt 0 ] && [ "$1" != "all" ] && [ "$1" != "wasm" ]; then
    for _t in "${TARGETS[@]}"; do
        if ! printf '%s\n' "${KNOWN_TARGETS[@]}" | grep -qx "$_t"; then
            echo "ERROR: unknown target '$_t'. Valid targets: ${KNOWN_TARGETS[*]}" >&2
            echo "Run '$(basename "$0") --help' to list targets and status." >&2
            exit 1
        fi
    done
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
should_build opencv && APT_PACKAGES+=(libpng-dev libjpeg-dev libavcodec-dev libavformat-dev libswscale-dev)
should_build librealsense && APT_PACKAGES+=(libudev-dev libusb-1.0-0-dev libssl-dev)

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

# ---------- Emscripten SDK (WebAssembly toolchain) ----------
if should_build emsdk; then
    echo "=== Installing emsdk ==="
    [ -d "$EMSDK_DIR" ] || git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$EMSDK_DIR"
    "$EMSDK_DIR/emsdk" install latest
    "$EMSDK_DIR/emsdk" activate latest
    echo "  -> installed to $EMSDK_DIR"
fi

# ---------- ANTLR4 C++ runtime 4.13.1, cross-built for wasm ----------
# A separate prefix from the native deps/antlr4 because -fwasm-exceptions and
# -pthread are ABI-level: they must match libroxal.a and the wasm host exactly, or
# class layouts diverge silently instead of failing to link.  The root CMakeLists
# applies the same two flags to every EMSCRIPTEN build -- keep these in step.
if should_build antlr4-wasm; then
    echo "=== Building antlr4-runtime 4.13.1 for wasm ==="
    [ -f "$EMSDK_DIR/emsdk_env.sh" ] || {
        echo "ERROR: emsdk not found at $EMSDK_DIR -- run '$(basename "$0") emsdk' first" >&2; exit 1; }
    # emsdk_env.sh trips over 'set -u'.
    set +u; source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1; set -u

    cd "$BUILD_TMP"
    # Same checkout the native antlr4 target uses, different build dir.
    [ -d antlr4 ] || git clone --depth 1 --branch 4.13.1 https://github.com/antlr/antlr4.git
    emcmake cmake -B antlr4/build-wasm-mt -S antlr4/runtime/Cpp \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/antlr4-wasm-mt" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release \
        -DANTLR4_INSTALL=ON \
        -DWITH_DEMO=OFF \
        -DANTLR_BUILD_CPP_TESTS=OFF \
        -DANTLR_BUILD_SHARED=OFF \
        -DCMAKE_CXX_FLAGS="-fwasm-exceptions -pthread"
    cmake --build antlr4/build-wasm-mt -j"$JOBS"
    cmake --install antlr4/build-wasm-mt
    echo "  -> installed to $DEPS_DIR/antlr4-wasm-mt"
fi

# ---------- PCRE2 10.44, cross-built for wasm ----------
# Its own prefix, like antlr4-wasm-mt: a native libpcre2 cannot link into a wasm
# build, and the -fwasm-exceptions/-pthread pair must match libroxal.a.
# Static only, 8-bit code units, no JIT (wasm cannot generate machine code) and
# no pcre2grep/pcre2test -- the module uses the library API and nothing else.
if should_build pcre2-wasm; then
    echo "=== Building PCRE2 10.44 for wasm ==="
    [ -f "$EMSDK_DIR/emsdk_env.sh" ] || {
        echo "ERROR: emsdk not found at $EMSDK_DIR -- run '$(basename "$0") emsdk' first" >&2; exit 1; }
    set +u; source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1; set -u

    cd "$BUILD_TMP"
    [ -d pcre2 ] || git clone --depth 1 --branch pcre2-10.44 https://github.com/PCRE2Project/pcre2.git
    emcmake cmake -B pcre2/build-wasm -S pcre2 \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/pcre2-wasm" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DPCRE2_BUILD_PCRE2_8=ON \
        -DPCRE2_SUPPORT_JIT=OFF \
        -DPCRE2_BUILD_PCRE2GREP=OFF \
        -DPCRE2_BUILD_TESTS=OFF \
        -DCMAKE_C_FLAGS="-fwasm-exceptions -pthread"
    cmake --build pcre2/build-wasm -j"$JOBS"
    cmake --install pcre2/build-wasm
    echo "  -> installed to $DEPS_DIR/pcre2-wasm"
fi

# ---------- pugixml 1.15, cross-built for wasm ----------
if should_build pugixml-wasm; then
    echo "=== Building pugixml 1.15 for wasm ==="
    [ -f "$EMSDK_DIR/emsdk_env.sh" ] || {
        echo "ERROR: emsdk not found at $EMSDK_DIR -- run '$(basename "$0") emsdk' first" >&2; exit 1; }
    set +u; source "$EMSDK_DIR/emsdk_env.sh" >/dev/null 2>&1; set -u

    cd "$BUILD_TMP"
    [ -d pugixml ] || git clone --depth 1 --branch v1.15 https://github.com/zeux/pugixml.git
    emcmake cmake -B pugixml/build-wasm -S pugixml \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/pugixml-wasm" \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DCMAKE_CXX_FLAGS="-fwasm-exceptions -pthread"
    cmake --build pugixml/build-wasm -j"$JOBS"
    cmake --install pugixml/build-wasm
    echo "  -> installed to $DEPS_DIR/pugixml-wasm"
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

# ---------- pugixml 1.15 ----------
if should_build pugixml; then
    echo "=== Building pugixml 1.15 ==="
    cd "$BUILD_TMP"
    [ -d pugixml ] || git clone --depth 1 --branch v1.15 https://github.com/zeux/pugixml.git
    cmake -B pugixml/build -S pugixml \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/pugixml" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DPUGIXML_BUILD_TESTS=OFF
    cmake --build pugixml/build -j"$JOBS"
    cmake --install pugixml/build
    echo "  -> installed to $DEPS_DIR/pugixml"
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

# ---------- OpenCV 5.0.0 (for the modules/opencv FFI binding) ----------
# Pinned: modules/opencv/init.rox and shim/cvx_shim.cpp are written against this
# exact version (the shim static_asserts 5.0 at compile time; init.rox re-checks
# at import). The source tree stays in deps/opencv because shim/generate.py
# reads hdr_parser.py and the module headers from it; libraries install to
# deps/opencv/install where modules/opencv/shim/build.sh expects them.
if should_build opencv; then
    OPENCV_VER="5.0.0"
    echo "=== Building OpenCV ${OPENCV_VER} ==="
    cd "$DEPS_DIR"
    [ -d opencv ] || git clone --depth 1 --branch "$OPENCV_VER" https://github.com/opencv/opencv.git opencv
    cmake -B opencv/build -S opencv \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/opencv/install" \
        -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
        -DBUILD_opencv_apps=OFF -DBUILD_opencv_python3=OFF -DBUILD_JAVA=OFF \
        -DBUILD_opencv_js=OFF -DBUILD_DOCS=OFF \
        -DOPENCV_GENERATE_PKGCONFIG=ON
    cmake --build opencv/build -j"$JOBS"
    cmake --install opencv/build
    bash "$SCRIPT_DIR/modules/opencv/shim/build.sh"
    echo "  -> installed to $DEPS_DIR/opencv/install (+ modules/opencv/libcvxshim.so)"
fi

# ---------- librealsense 2.58.3 (for the modules/realsense FFI binding) ----------
# Pinned: modules/realsense/shim/rs_shim.cpp static_asserts 2.58+ at compile time
# and init.rox re-checks the major version at import. BUILD_WITH_DDS=ON adds the
# Ethernet transport used by D555-class cameras (it FetchContents Fast-DDS, which
# is why this build is not quick); USB cameras do not need it.
#
# Note for non-root use: the SDK installs udev rules under
# deps/librealsense/config/99-realsense-libusb.rules. Without them the IMU's IIO
# nodes and some device metadata stay root-only:
#   sudo cp deps/librealsense/config/99-realsense-libusb.rules /etc/udev/rules.d/
#   sudo udevadm control --reload-rules && sudo udevadm trigger
if should_build librealsense; then
    LRS_VER="2.58.3"
    echo "=== Building librealsense ${LRS_VER} ==="
    cd "$DEPS_DIR"
    [ -d librealsense ] || git clone --depth 1 --branch "v${LRS_VER}" https://github.com/IntelRealSense/librealsense.git librealsense
    cmake -B librealsense/build -S librealsense \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX="$DEPS_DIR/librealsense/install" \
        -DBUILD_EXAMPLES=OFF -DBUILD_GRAPHICAL_EXAMPLES=OFF \
        -DBUILD_TOOLS=OFF -DBUILD_UNIT_TESTS=OFF \
        -DBUILD_PYTHON_BINDINGS=OFF \
        -DBUILD_WITH_DDS=ON
    cmake --build librealsense/build -j"$JOBS"
    cmake --install librealsense/build
    bash "$SCRIPT_DIR/modules/realsense/shim/build.sh"
    echo "  -> installed to $DEPS_DIR/librealsense/install (+ modules/realsense/librsshim.so)"
fi

echo ""
echo "=== All requested deps built ==="
echo "Contents of $DEPS_DIR:"
ls "$DEPS_DIR"
echo ""
echo "You can now clean up build temps with: rm -rf $BUILD_TMP"
