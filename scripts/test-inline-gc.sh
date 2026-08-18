#!/usr/bin/env bash
# Run the test suite against the INLINE-COLLECTION GC configuration
# (ROXAL_GC_DEDICATED_THREAD=OFF) -- the configuration wasm ships.
#
# Why this exists: ROXAL_GC_DEDICATED_THREAD defaults ON on Linux, so an
# ordinary native run exercises collector semantics the browser never uses.
# A GC coordination bug that only bites when mutators self-elect (mark, sweep
# and reclaim spread across mutator threads instead of one collector thread)
# is invisible to the default suite and reproducible only in a browser.  That
# is exactly what happened: a stop-the-world barrier hole and a collector
# double-election cost days of browser-only debugging, and both fail this leg
# in seconds.  tests/gc_liveness.rox in particular is a canary here -- it
# HANGS outright if the collector's idle predicate regresses.
#
# Usage: scripts/test-inline-gc.sh [extra runtests.py args...]
#   e.g. scripts/test-inline-gc.sh -t 'gc_*'
#
# Add -DCMAKE_CXX_FLAGS=-DROXAL_GC_FORENSICS=1 to the configure below (or use
# a separate build dir) to also get the GC tripwires; then set
# ROXAL_FC_FLAGS=15 to enable mark verification -- see
# implementation-notes.md, "GC coordination invariants".

set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${ROXAL_INLINE_GC_BUILD_DIR:-build-inline-gc}"
JOBS="${ROXAL_BUILD_JOBS:-4}"

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "== configuring $BUILD_DIR (inline-collection GC) =="
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
          -DROXAL_GC_DEDICATED_THREAD=OFF
fi

# Guard against a stale/mis-configured directory silently testing the DEFAULT
# collector: the whole point of this leg is the other configuration.
if grep -q '^ROXAL_GC_DEDICATED_THREAD:BOOL=ON' "$BUILD_DIR/CMakeCache.txt"; then
    echo "ERROR: $BUILD_DIR is configured with the DEDICATED collector;" >&2
    echo "       delete it or set ROXAL_INLINE_GC_BUILD_DIR elsewhere." >&2
    exit 2
fi

echo "== building $BUILD_DIR =="
cmake --build "$BUILD_DIR" -j"$JOBS"

echo "== running the suite against $BUILD_DIR =="
ROXAL_BUILD_DIR="$BUILD_DIR" python3 runtests.py "$@"
