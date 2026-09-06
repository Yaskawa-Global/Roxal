#!/usr/bin/env bash
# Launch AI Studio as a desktop app on this workspace (or the one given).
#   examples/aistudio/aistudio.sh [workspace-dir | diagram.rox]
# Naming a .rox file opens it, with its folder as the workspace.
# Needs: build/roxal (cmake --build build/), web/node_modules (npm install),
# and the built page (npm run build, done here when missing).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
TARGET="${1:-$HERE}"
if [ ! -e "$TARGET" ]; then
    echo "aistudio: no such file or directory: $TARGET" >&2
    exit 1
fi
# an absolute path: the app is started from web/, so a relative one would move
TARGET="$(cd "$(dirname "$TARGET")" && pwd)/$(basename "$TARGET")"
cd "$REPO/web"
[ -d node_modules ] || npm install
[ -f dist/aistudio.html ] || npm run build
# (npm run app unsets ELECTRON_RUN_AS_NODE, which an IDE terminal may export)
exec npm run --silent app -- "$TARGET"
