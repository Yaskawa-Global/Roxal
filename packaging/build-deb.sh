#!/usr/bin/env bash
#
# Build a .deb package for Roxal from an existing cmake build.
#
# Usage:  ./packaging/build-deb.sh [--distro noble] [--dds-lib-dir /usr/share/lib]
#                                   [--build-dir build] [--output-dir .]
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
DDS_LIB_DIR="${DDS_LIB_DIR:-/usr/share/lib}"
OUTPUT_DIR="${PROJECT_ROOT}"
ARCH="amd64"

# Modules to include (explicit list -- no globs)
ROX_MODULES="sys.rox math.rox fileio.rox regex.rox socket.rox dds.rox"

# ---------------------------------------------------------------------------
# Parse arguments
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --distro)       DISTRO="$2";       shift 2 ;;
        --dds-lib-dir)  DDS_LIB_DIR="$2";  shift 2 ;;
        --build-dir)    BUILD_DIR="$2";     shift 2 ;;
        --output-dir)   OUTPUT_DIR="$2";    shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--distro noble] [--dds-lib-dir /usr/share/lib]"
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
    cp "${MODULES_DIR}/${mod}" "${STAGING_DIR}/usr/share/roxal/"
done
chmod 644 "${STAGING_DIR}/usr/share/roxal/"*.rox

echo "  Modules: ${ROX_MODULES}"

# ---------------------------------------------------------------------------
# Step 3: Bundle CycloneDDS shared libraries
# ---------------------------------------------------------------------------
echo "Bundling CycloneDDS libraries..."

bundle_lib() {
    local soname="$1"
    local soname_path="${DDS_LIB_DIR}/${soname}"

    if [[ ! -e "${soname_path}" ]]; then
        echo "  WARNING: ${soname_path} not found -- skipping DDS bundling for ${soname}"
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

    echo "  ${soname} -> ${real_name}"
}

DDS_FOUND=true
bundle_lib "libddsc.so.0" || DDS_FOUND=false
bundle_lib "libcycloneddsidl.so.0" || DDS_FOUND=false

if [[ "${DDS_FOUND}" == "false" ]]; then
    echo "  WARNING: CycloneDDS libraries not fully bundled."
    echo "  The DDS module may not work. Set --dds-lib-dir if libs are elsewhere."
fi

# ---------------------------------------------------------------------------
# Step 4: Install ld.so.conf.d drop-in and maintainer scripts
# ---------------------------------------------------------------------------
cp "${SCRIPT_DIR}/templates/roxal.conf" "${STAGING_DIR}/etc/ld.so.conf.d/roxal.conf"
chmod 644 "${STAGING_DIR}/etc/ld.so.conf.d/roxal.conf"

cp "${SCRIPT_DIR}/templates/postinst" "${STAGING_DIR}/DEBIAN/postinst"
cp "${SCRIPT_DIR}/templates/postrm"   "${STAGING_DIR}/DEBIAN/postrm"
chmod 755 "${STAGING_DIR}/DEBIAN/postinst"
chmod 755 "${STAGING_DIR}/DEBIAN/postrm"

# ---------------------------------------------------------------------------
# Step 5: Generate copyright file
# ---------------------------------------------------------------------------
cat > "${STAGING_DIR}/usr/share/doc/roxal/copyright" <<'COPY'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/

Files: *
Copyright: 2024-2026 David Jung
License: Proprietary

Files: usr/lib/roxal/libddsc* usr/lib/roxal/libcycloneddsidl*
Copyright: Eclipse Foundation
License: EPL-2.0 OR BSD-3-Clause
Comment: CycloneDDS - https://github.com/eclipse-cyclonedds/cyclonedds
COPY

# ---------------------------------------------------------------------------
# Step 6: Generate DEBIAN/control
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
 and DDS communication.
EOF

# ---------------------------------------------------------------------------
# Step 7: Build the .deb
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
