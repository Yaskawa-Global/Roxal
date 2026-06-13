#!/usr/bin/env bash
#
# Build a .deb package for Roxal from an existing cmake build.
#
# Usage:  ./packaging/build-deb.sh [--distro noble]
#           [--antlr4-lib-dir /opt/antlr4/lib]
#           [--dds-lib-dir /usr/share/lib]
#           [--ort-lib-dir deps/onnxruntime/lib]
#           [--build-dir build] [--output-dir .]
#
# Run from the project root directory.

set -euo pipefail

# ---------------------------------------------------------------------------
# Defaults
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DISTRO="noble"
BUILD_DIR="${PROJECT_ROOT}/build"
MODULES_DIR="${PROJECT_ROOT}/modules"
ANTLR4_LIB_DIR="${ANTLR4_LIB_DIR:-${PROJECT_ROOT}/deps/antlr4/lib}"
DDS_LIB_DIR="${DDS_LIB_DIR:-${PROJECT_ROOT}/deps/cyclonedds/lib}"
ORT_LIB_DIR="${ORT_LIB_DIR:-${PROJECT_ROOT}/deps/onnxruntime/lib}"
OUTPUT_DIR="${PROJECT_ROOT}"
ARCH=$(dpkg --print-architecture 2>/dev/null || echo "amd64")

# Modules to include (explicit list -- no globs)
# Paths relative to MODULES_DIR; subdirectories are preserved.
ROX_MODULES="sys.rox math.rox fileio.rox regex.rox socket.rox dds.rox ai/nn.rox"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --distro)         DISTRO="$2";         shift 2 ;;
        --antlr4-lib-dir) ANTLR4_LIB_DIR="$2"; shift 2 ;;
        --dds-lib-dir)    DDS_LIB_DIR="$2";    shift 2 ;;
        --ort-lib-dir)  ORT_LIB_DIR="$2";  shift 2 ;;
        --build-dir)    BUILD_DIR="$2";     shift 2 ;;
        --output-dir)   OUTPUT_DIR="$2";    shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--distro noble] [--antlr4-lib-dir /opt/antlr4/lib]"
            echo "          [--dds-lib-dir /usr/share/lib] [--ort-lib-dir deps/onnxruntime/lib]"
            echo "          [--build-dir build] [--output-dir .]"
            exit 0
            ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# Validate prerequisites
# ---------------------------------------------------------------------------
die() { echo "ERROR: $*" >&2; exit 1; }

[[ -f "${BUILD_DIR}/roxal" ]] || die "Binary not found at ${BUILD_DIR}/roxal -- build first."
file "${BUILD_DIR}/roxal" | grep -q "ELF" || die "${BUILD_DIR}/roxal is not an ELF binary."

for tool in strip patchelf dpkg-deb; do
    command -v "$tool" >/dev/null 2>&1 || die "'${tool}' is required. Install with: sudo apt install ${tool}"
done

DEPS_FILE="${SCRIPT_DIR}/templates/deps-${DISTRO}.txt"
[[ -f "${DEPS_FILE}" ]] || die "No dependency file for distro '${DISTRO}' at ${DEPS_FILE}"

# ---------------------------------------------------------------------------
# Extract version from CMakeLists.txt
# ---------------------------------------------------------------------------
VERSION=$(grep -oP 'project\(roxal\s+VERSION\s+\K[0-9]+\.[0-9]+\.[0-9]+' "${PROJECT_ROOT}/CMakeLists.txt") \
    || die "Could not extract version from CMakeLists.txt"

PRERELEASE=$(grep -oP 'set\(ROXAL_PRERELEASE\s+"\K[^"]+' "${PROJECT_ROOT}/CMakeLists.txt" || true)

if [[ -n "${PRERELEASE}" && "${PRERELEASE}" != "none" && "${PRERELEASE}" != '""' ]]; then
    DEB_VERSION="${VERSION}~${PRERELEASE}"
else
    DEB_VERSION="${VERSION}"
fi

# Append build metadata: +YYYYMMDD.g<short-hash>
BUILD_DATE=$(date -u +%Y%m%d)
GIT_HASH=$(git -C "${PROJECT_ROOT}" rev-parse --short HEAD 2>/dev/null || echo "unknown")
DEB_VERSION="${DEB_VERSION}+${BUILD_DATE}.g${GIT_HASH}~${DISTRO}"

PKG_NAME="roxal_${DEB_VERSION}_${ARCH}"

echo "=== Building ${PKG_NAME}.deb ==="
echo "  Version:   ${DEB_VERSION}"
echo "  Distro:    ${DISTRO}"
echo "  Build dir: ${BUILD_DIR}"
echo "  DDS libs:  ${DDS_LIB_DIR}"
echo ""

# ---------------------------------------------------------------------------
# Create staging directory (cleaned up on exit)
# ---------------------------------------------------------------------------
STAGING_DIR=$(mktemp -d)
trap 'rm -rf "${STAGING_DIR}"' EXIT

mkdir -p "${STAGING_DIR}/DEBIAN"
mkdir -p "${STAGING_DIR}/usr/bin"
mkdir -p "${STAGING_DIR}/usr/share/roxal"
mkdir -p "${STAGING_DIR}/usr/share/doc/roxal"
mkdir -p "${STAGING_DIR}/usr/lib/roxal"
mkdir -p "${STAGING_DIR}/etc/ld.so.conf.d"

# ---------------------------------------------------------------------------
# Step 1: Copy, strip, and patch the binary
# ---------------------------------------------------------------------------
echo "Copying and stripping binary..."
cp "${BUILD_DIR}/roxal" "${STAGING_DIR}/usr/bin/roxal"
strip --strip-all "${STAGING_DIR}/usr/bin/roxal"
patchelf --set-rpath /usr/lib/roxal "${STAGING_DIR}/usr/bin/roxal"
chmod 755 "${STAGING_DIR}/usr/bin/roxal"

STRIPPED_SIZE=$(du -sh "${STAGING_DIR}/usr/bin/roxal" | cut -f1)
echo "  Stripped binary: ${STRIPPED_SIZE}"

# ---------------------------------------------------------------------------
# Step 2: Copy module .rox files
# ---------------------------------------------------------------------------
echo "Copying module files..."
for mod in ${ROX_MODULES}; do
    if [[ ! -f "${MODULES_DIR}/${mod}" ]]; then
        die "Module file not found: ${MODULES_DIR}/${mod}"
    fi
    # Preserve subdirectory structure (e.g. ai/nn.rox → usr/share/roxal/ai/nn.rox)
    mod_dest="${STAGING_DIR}/usr/share/roxal/${mod}"
    mkdir -p "$(dirname "${mod_dest}")"
    cp "${MODULES_DIR}/${mod}" "${mod_dest}"
    chmod 644 "${mod_dest}"
done

echo "  Modules: ${ROX_MODULES}"

# ---------------------------------------------------------------------------
# Step 3: Copy static libraries for VM integration
# ---------------------------------------------------------------------------
echo "Copying static libraries..."
for lib in libroxal.a dataflow/libdataflow.a; do
    libpath="${BUILD_DIR}/${lib}"
    if [[ -f "${libpath}" ]]; then
        cp "${libpath}" "${STAGING_DIR}/usr/lib/roxal/"
        chmod 644 "${STAGING_DIR}/usr/lib/roxal/$(basename "${lib}")"
        echo "  $(basename "${lib}") ($(du -sh "${libpath}" | cut -f1))"
    else
        echo "  WARNING: ${libpath} not found -- skipping"
    fi
done

# ---------------------------------------------------------------------------
# Helper: bundle a shared library + its SONAME symlink into usr/lib/roxal
# ---------------------------------------------------------------------------
bundle_lib() {
    local lib_dir="$1"
    local soname="$2"
    local soname_path="${lib_dir}/${soname}"

    if [[ ! -e "${soname_path}" ]]; then
        echo "  WARNING: ${soname_path} not found -- skipping"
        return 1
    fi

    # Resolve the real versioned file
    local real_file
    real_file=$(readlink -f "${soname_path}")
    local real_name
    real_name=$(basename "${real_file}")

    cp "${real_file}" "${STAGING_DIR}/usr/lib/roxal/${real_name}"
    chmod 644 "${STAGING_DIR}/usr/lib/roxal/${real_name}"

    # Create SONAME symlink if different from real file
    if [[ "${soname}" != "${real_name}" ]]; then
        ln -sf "${real_name}" "${STAGING_DIR}/usr/lib/roxal/${soname}"
    fi

    local size
    size=$(du -sh "${STAGING_DIR}/usr/lib/roxal/${real_name}" | cut -f1)
    echo "  ${soname} -> ${real_name} (${size})"
}

# ---------------------------------------------------------------------------
# Step 4a: Bundle ANTLR4 runtime shared library
# ---------------------------------------------------------------------------
if [[ -n "${ANTLR4_LIB_DIR}" ]]; then
    echo "Bundling ANTLR4 runtime library..."
    # ANTLR4 uses version-in-SONAME: libantlr4-runtime.so.4.13.1
    antlr4_so=$(ls "${ANTLR4_LIB_DIR}"/libantlr4-runtime.so.* 2>/dev/null | sort -V | tail -1)
    if [[ -n "${antlr4_so}" ]]; then
        bundle_lib "${ANTLR4_LIB_DIR}" "$(basename "${antlr4_so}")"
    else
        echo "  WARNING: No libantlr4-runtime.so.* found in ${ANTLR4_LIB_DIR}"
    fi
fi

# ---------------------------------------------------------------------------
# Step 4b: Bundle CycloneDDS shared libraries
# ---------------------------------------------------------------------------
echo "Bundling CycloneDDS libraries..."

# Auto-detect CycloneDDS SONAME (handles both old so.0 and new so.11+)
DDS_FOUND=true
for pattern in "libddsc.so.*[0-9]" "libcycloneddsidl.so.*[0-9]"; do
    # Find the SONAME symlink (e.g. libddsc.so.0 or libddsc.so.11)
    soname=$(ls "${DDS_LIB_DIR}"/${pattern} 2>/dev/null | grep -v '\.so\.[0-9]*\.[0-9]' | head -1)
    if [[ -n "${soname}" ]]; then
        bundle_lib "${DDS_LIB_DIR}" "$(basename "${soname}")" || DDS_FOUND=false
    else
        echo "  WARNING: No ${pattern} found in ${DDS_LIB_DIR} -- skipping"
        DDS_FOUND=false
    fi
done

if [[ "${DDS_FOUND}" == "false" ]]; then
    echo "  WARNING: CycloneDDS libraries not fully bundled."
    echo "  The DDS module may not work. Set --dds-lib-dir if libs are elsewhere."
fi

# ---------------------------------------------------------------------------
# Step 5: Bundle ONNX Runtime shared libraries
# ---------------------------------------------------------------------------
echo "Bundling ONNX Runtime libraries..."

ORT_FOUND=true

# Core library (always required for ai.nn)
bundle_lib "${ORT_LIB_DIR}" "libonnxruntime.so.1" || ORT_FOUND=false

# Shared provider utilities (needed by CUDA/TensorRT providers)
bundle_lib "${ORT_LIB_DIR}" "libonnxruntime_providers_shared.so" || true

# CUDA provider (optional -- enables GPU acceleration when CUDA is installed)
if [[ -f "${ORT_LIB_DIR}/libonnxruntime_providers_cuda.so" ]]; then
    bundle_lib "${ORT_LIB_DIR}" "libonnxruntime_providers_cuda.so" || true
else
    echo "  (CUDA provider not present -- GPU acceleration not bundled)"
fi

# Also create the unversioned symlink for the linker
if [[ "${ORT_FOUND}" == "true" && ! -e "${STAGING_DIR}/usr/lib/roxal/libonnxruntime.so" ]]; then
    local_soname=$(ls "${STAGING_DIR}/usr/lib/roxal/libonnxruntime.so."* 2>/dev/null | head -1 | xargs basename)
    if [[ -n "${local_soname}" ]]; then
        ln -sf "${local_soname}" "${STAGING_DIR}/usr/lib/roxal/libonnxruntime.so"
    fi
fi

if [[ "${ORT_FOUND}" == "false" ]]; then
    echo "  WARNING: ONNX Runtime core library not found."
    echo "  The ai.nn module will not work. Set --ort-lib-dir if libs are elsewhere."
fi

# ---------------------------------------------------------------------------
# Step 6: Install ld.so.conf.d drop-in and maintainer scripts
# ---------------------------------------------------------------------------
cp "${SCRIPT_DIR}/templates/roxal.conf" "${STAGING_DIR}/etc/ld.so.conf.d/roxal.conf"
chmod 644 "${STAGING_DIR}/etc/ld.so.conf.d/roxal.conf"

cp "${SCRIPT_DIR}/templates/postinst" "${STAGING_DIR}/DEBIAN/postinst"
cp "${SCRIPT_DIR}/templates/postrm"   "${STAGING_DIR}/DEBIAN/postrm"
chmod 755 "${STAGING_DIR}/DEBIAN/postinst"
chmod 755 "${STAGING_DIR}/DEBIAN/postrm"

# ---------------------------------------------------------------------------
# Step 7: Generate copyright file
# ---------------------------------------------------------------------------
cat > "${STAGING_DIR}/usr/share/doc/roxal/copyright" <<'COPY'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/

Files: *
Copyright: 2024-2026 David Jung
License: Proprietary

Files: usr/lib/roxal/libantlr4-runtime*
Copyright: The ANTLR Project
License: BSD-3-Clause
Comment: ANTLR4 C++ Runtime - https://github.com/antlr/antlr4

Files: usr/lib/roxal/libddsc* usr/lib/roxal/libcycloneddsidl*
Copyright: Eclipse Foundation
License: EPL-2.0 OR BSD-3-Clause
Comment: CycloneDDS - https://github.com/eclipse-cyclonedds/cyclonedds

Files: usr/lib/roxal/libonnxruntime*
Copyright: Microsoft Corporation
License: MIT
Comment: ONNX Runtime - https://github.com/microsoft/onnxruntime
COPY

# ---------------------------------------------------------------------------
# Step 8: Generate DEBIAN/control
# ---------------------------------------------------------------------------
DEPENDS=$(cat "${DEPS_FILE}" | tr -d '\n')
INSTALLED_KB=$(du -sk "${STAGING_DIR}" | cut -f1)

cat > "${STAGING_DIR}/DEBIAN/control" <<EOF
Package: roxal
Version: ${DEB_VERSION}
Architecture: ${ARCH}
Maintainer: David Jung <david@roxal.dev>
Installed-Size: ${INSTALLED_KB}
Depends: ${DEPENDS}
Section: devel
Priority: optional
Description: Roxal programming language runtime
 Roxal is a programming language designed for robotics applications.
 It includes a compiler and virtual machine with support for actors,
 signals, events, and modules for file I/O, regex, sockets, gRPC,
 DDS communication, and AI neural network inference (ONNX Runtime).
EOF

# ---------------------------------------------------------------------------
# Step 9: Build the .deb
# ---------------------------------------------------------------------------
echo ""
echo "Building .deb package..."

# Use --root-owner-group so files are owned by root in the archive
# (available in dpkg >= 1.19.0, present on Ubuntu 20.04+)
dpkg-deb --root-owner-group --build "${STAGING_DIR}" "${OUTPUT_DIR}/${PKG_NAME}.deb"

echo ""
echo "=== Package built successfully ==="
echo "  ${OUTPUT_DIR}/${PKG_NAME}.deb"
echo ""
echo "Inspect with:"
echo "  dpkg-deb --info ${PKG_NAME}.deb"
echo "  dpkg-deb --contents ${PKG_NAME}.deb"
echo ""
echo "Install with:"
echo "  sudo apt install ./${PKG_NAME}.deb"
echo ""
echo "Remove with:"
echo "  sudo apt remove roxal"
