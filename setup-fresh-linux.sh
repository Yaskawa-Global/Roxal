#!/usr/bin/env bash
#
# Prepare a freshly cloned Roxal checkout on Debian/Ubuntu.
# This matches the current Codex `codex-universal` reference image, which is
# based on Ubuntu 24.04.
#
# Default behavior:
#   - install OS packages with apt
#   - build/install the core source dependencies into ./deps via install-deps.sh
#   - configure CMake into ./build
#
# This supersedes the older vcpkg-based Codex setup:
#   - dependencies are installed under this checkout's ./deps
#   - antlr4 runtime is built by install-deps.sh, not vcpkg
#   - parser generation is handled by CMake, so there is no manual
#     compiler/cpp-gen generation or copy step
#
# Examples:
#   ./setup-fresh-linux.sh
#   ./setup-fresh-linux.sh --build
#   ./setup-fresh-linux.sh --all --cpu-only --build
#   ./setup-fresh-linux.sh --all --gpu-onnx
#   ./setup-fresh-linux.sh -- -DROXAL_ENABLE_XML=ON
#
# Notes:
#   - This is intended for apt-based environments, including codex-universal.
#   - The heavy optional dependencies are enabled with --all.
#   - --all defaults to CPU-only ONNX Runtime because most fresh cloud VMs do
#     not have CUDA installed. Use --gpu-onnx to request the GPU ONNX tarball.
#   - Extra arguments after "--" are passed directly to cmake.
#   - Environment variables exported by Codex setup scripts do not persist into
#     the agent phase, so this script also writes Roxal paths to ~/.bashrc.

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./setup-fresh-linux.sh [options] [-- extra-cmake-args...]

Options:
  --core              Configure the default core feature set (default)
  --all               Build all deps and enable optional modules
  --cpu-only          Use CPU-only ONNX Runtime when --all is set (default)
  --gpu-onnx          Use GPU/CUDA ONNX Runtime when --all is set
  --build             Run cmake --build after configure
  --build-type TYPE   CMake build type (default: RelWithDebInfo)
  --skip-apt          Do not install apt packages
  --skip-deps         Do not run install-deps.sh
  --no-persist-env    Do not append Roxal PATH/LD_LIBRARY_PATH setup to ~/.bashrc
  -h, --help          Show this help

Any arguments after "--" are appended to the cmake configure command.
EOF
}

MODE="core"
ONNX_MODE="cpu"
RUN_BUILD=false
RUN_APT=true
RUN_DEPS=true
PERSIST_ENV=true
BUILD_TYPE="RelWithDebInfo"
EXTRA_CMAKE_ARGS=()

while [ "$#" -gt 0 ]; do
    case "$1" in
        --core)
            MODE="core"
            shift
            ;;
        --all)
            MODE="all"
            shift
            ;;
        --cpu-only)
            ONNX_MODE="cpu"
            shift
            ;;
        --gpu-onnx)
            ONNX_MODE="gpu"
            shift
            ;;
        --build)
            RUN_BUILD=true
            shift
            ;;
        --build-type)
            if [ "$#" -lt 2 ]; then
                echo "ERROR: --build-type requires a value" >&2
                exit 2
            fi
            BUILD_TYPE="$2"
            shift 2
            ;;
        --skip-apt)
            RUN_APT=false
            shift
            ;;
        --skip-deps)
            RUN_DEPS=false
            shift
            ;;
        --no-persist-env)
            PERSIST_ENV=false
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            EXTRA_CMAKE_ARGS+=("$@")
            break
            ;;
        *)
            echo "ERROR: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ ! -f "$SCRIPT_DIR/CMakeLists.txt" ] || [ ! -f "$SCRIPT_DIR/install-deps.sh" ]; then
    echo "ERROR: run this script from a Roxal checkout, or keep it in the repo root." >&2
    exit 1
fi

if [ "$(id -u)" -eq 0 ]; then
    SUDO=()
    sudo() { "$@"; }
    export -f sudo
else
    if ! command -v sudo >/dev/null 2>&1; then
        echo "ERROR: sudo is required when running as a non-root user." >&2
        exit 1
    fi
    SUDO=(sudo)
fi

JOBS="${JOBS:-$(nproc)}"

APT_PACKAGES=(
    autoconf
    automake
    build-essential
    ca-certificates
    cmake
    git
    libboost-program-options-dev
    libffi-dev
    libicu-dev
    libpcre2-dev
    libssl-dev
    libtool
    pkg-config
    python3
    python3-pip
    default-jre-headless
    gdb
    unzip
    wget
    zlib1g-dev
)

if [ "$MODE" = "all" ]; then
    APT_PACKAGES+=(
        libjpeg-dev
        libpng-dev
    )
fi

if [ "$RUN_APT" = true ]; then
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "ERROR: this setup script expects apt-get (Debian/Ubuntu, including codex-universal)." >&2
        exit 1
    fi

    echo "=== Installing OS packages ==="
    "${SUDO[@]}" apt-get update
    "${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${APT_PACKAGES[@]}"
fi

export PATH="$HOME/.local/bin:$PATH"
export JOBS

# Pre-install the ANTLR tool with a fallback for Ubuntu's externally-managed
# Python environments. install-deps.sh will skip this if the module is present.
if ! python3 -c 'import antlr4_tools' >/dev/null 2>&1; then
    echo "=== Installing antlr4-tools for the current user ==="
    python3 -m pip install --user --upgrade --break-system-packages antlr4-tools 2>/dev/null \
        || python3 -m pip install --user --upgrade antlr4-tools
fi

if [ "$RUN_DEPS" = true ]; then
    echo "=== Installing source dependencies into ./deps ==="
    if [ "$MODE" = "all" ]; then
        if [ "$ONNX_MODE" = "cpu" ]; then
            bash "$SCRIPT_DIR/install-deps.sh" all --cpu-only
        else
            bash "$SCRIPT_DIR/install-deps.sh" all
        fi
    else
        bash "$SCRIPT_DIR/install-deps.sh"
    fi
fi

ANTLR4_BIN="$(command -v antlr4 || true)"
if [ -z "$ANTLR4_BIN" ] && [ -x "$HOME/.local/bin/antlr4" ]; then
    ANTLR4_BIN="$HOME/.local/bin/antlr4"
fi

CMAKE_ARGS=(
    -B "$SCRIPT_DIR/build"
    -S "$SCRIPT_DIR"
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
    -DEigen3_ROOT="$SCRIPT_DIR/deps/eigen"
    -Dantlr4_ROOT="$SCRIPT_DIR/deps/antlr4"
    -DROXAL_ENABLE_FILEIO=ON
    -DROXAL_ENABLE_REGEX=ON
    -DROXAL_ENABLE_SOCKET=ON
)

if [ -n "$ANTLR4_BIN" ]; then
    CMAKE_ARGS+=(-DANTLR4="$ANTLR4_BIN")
fi

if [ "$MODE" = "all" ]; then
    CMAKE_ARGS+=(
        -DgRPC_ROOT="$SCRIPT_DIR/deps/grpc"
        -DCycloneDDS_ROOT="$SCRIPT_DIR/deps/cyclonedds"
        -Dpugixml_ROOT="$SCRIPT_DIR/deps/pugixml"
        -DOnnxRuntime_ROOT="$SCRIPT_DIR/deps/onnxruntime"
        -DROXAL_ENABLE_GRPC=ON
        -DROXAL_ENABLE_DDS=ON
        -DROXAL_ENABLE_XML=ON
        -DROXAL_ENABLE_ONNX=ON
        -DROXAL_ENABLE_AI_NN=ON
        -DROXAL_ENABLE_MEDIA=ON
        -DROXAL_COMPUTE_SERVER=ON
    )
else
    CMAKE_ARGS+=(
        -DROXAL_ENABLE_GRPC=OFF
        -DROXAL_ENABLE_DDS=OFF
        -DROXAL_ENABLE_XML=OFF
        -DROXAL_ENABLE_ONNX=OFF
        -DROXAL_ENABLE_AI_NN=OFF
        -DROXAL_ENABLE_MEDIA=OFF
        -DROXAL_COMPUTE_SERVER=OFF
    )
fi

CMAKE_ARGS+=("${EXTRA_CMAKE_ARGS[@]}")

echo "=== Configuring Roxal ==="
cmake "${CMAKE_ARGS[@]}"

mkdir -p "$SCRIPT_DIR/build"
cat > "$SCRIPT_DIR/build/roxal-env.sh" <<EOF
# Source this before running build/roxal if your shell cannot find bundled deps.
export PATH="$HOME/.local/bin:\$PATH"
export LD_LIBRARY_PATH="$SCRIPT_DIR/deps/antlr4/lib:$SCRIPT_DIR/deps/cyclonedds/lib:$SCRIPT_DIR/deps/grpc/lib:$SCRIPT_DIR/deps/onnxruntime/lib:\${LD_LIBRARY_PATH:-}"
EOF

if [ "$PERSIST_ENV" = true ]; then
    BASHRC="$HOME/.bashrc"
    START_MARKER="# >>> roxal setup-fresh-linux.sh >>>"
    END_MARKER="# <<< roxal setup-fresh-linux.sh <<<"
    touch "$BASHRC"
    if ! grep -Fq "$START_MARKER" "$BASHRC"; then
        cat >> "$BASHRC" <<EOF

$START_MARKER
if [ -f "$SCRIPT_DIR/build/roxal-env.sh" ]; then
    . "$SCRIPT_DIR/build/roxal-env.sh"
fi
$END_MARKER
EOF
    fi
fi

if [ "$RUN_BUILD" = true ]; then
    echo "=== Building Roxal ==="
    cmake --build "$SCRIPT_DIR/build" -j"$JOBS"
fi

echo ""
echo "=== Setup complete ==="
echo "Configured build directory: $SCRIPT_DIR/build"
echo "Environment helper: source $SCRIPT_DIR/build/roxal-env.sh"
if [ "$RUN_BUILD" = false ]; then
    echo "Build command: cmake --build $SCRIPT_DIR/build -j$JOBS"
fi
