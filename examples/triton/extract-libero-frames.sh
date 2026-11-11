#!/usr/bin/env bash
# A few frames of a LIBERO demonstration (lerobot/libero, Apache-2.0,
# episode 0) for smolvla_libero.rox: per frame, both camera images (PNG) and
# sample.json -- the task, the robot's state, and the 50 actions recorded
# from that frame on.  Written to examples/triton/data/libero (git-ignored).
#
#   examples/triton/extract-libero-frames.sh [WEIGHTS_DIR]   (default ~/roxal-triton-weights)
#
# Runs in the server image (LeRobot decodes the dataset's videos); the
# episode's dataset files (1-2 GB) are cached in the weights directory.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
WEIGHTS="${1:-$HOME/roxal-triton-weights}"
IMAGE="${ROXAL_TRITON_IMAGE:-roxal-triton-ai}"
OUT="$HERE/data/libero"
mkdir -p "$WEIGHTS/hf" "$OUT"

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e HF_HUB_OFFLINE=0 -e HF_HOME=/weights/hf \
    -v "$WEIGHTS:/weights" -v "$OUT:/out" "$IMAGE" \
    python3 /opt/roxal/extract_libero.py --episode 0 --frames 0 40 80 --out /out

echo "Frames in $OUT"
