#!/usr/bin/env bash
#
# Build a Roxal .deb inside Docker.
# Defaults to arm64; also works for amd64.
#
# Usage:
#   ./packaging/docker-build.sh                           # arm64 (default)
#   ./packaging/docker-build.sh --platform linux/amd64    # amd64
#
# Prerequisites:
#   docker buildx (usually included with Docker Desktop or docker-ce)
#   For arm64 on an x86 host: docker run --rm --privileged multiarch/qemu-user-static --reset -p yes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PLATFORM="linux/arm64"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform) PLATFORM="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--platform linux/arm64|linux/amd64]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

echo "=== Building Roxal .deb for ${PLATFORM} ==="
echo ""

HOST_ARCH="linux/$(dpkg --print-architecture 2>/dev/null || echo amd64)"

# Determine parallel job count: use half cores for native, 1 for cross/QEMU
if [[ "${PLATFORM}" == "${HOST_ARCH}" ]]; then
    JOBS=$(( $(nproc) / 2 > 0 ? $(nproc) / 2 : 1 ))
else
    JOBS=1
    echo "Cross-build detected — using -j1 (QEMU is crash-prone with parallelism)"
    echo "Setting up QEMU user-mode emulation..."
    docker run --rm --privileged multiarch/qemu-user-static --reset -p yes 2>/dev/null || true
fi

echo "Build jobs: ${JOBS}"
echo ""

docker buildx build \
    --platform "${PLATFORM}" \
    --build-arg JOBS="${JOBS}" \
    -f "${SCRIPT_DIR}/Dockerfile" \
    --target output \
    --output "type=local,dest=${PROJECT_ROOT}" \
    "${PROJECT_ROOT}"

DEB_ARCH="${PLATFORM#linux/}"

echo ""
echo "=== Done ==="
ls -lht "${PROJECT_ROOT}"/roxal_*_${DEB_ARCH}.deb 2>/dev/null | head -1 || echo "WARNING: No .deb found in ${PROJECT_ROOT}"
