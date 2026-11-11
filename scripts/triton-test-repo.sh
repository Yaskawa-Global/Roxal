#!/usr/bin/env bash
# Build a Triton model repository from ai.nn's small test models, for
#   python3 runtests.py --triton-url HOST:PORT
# and print a command that serves it with NVIDIA's Triton container.
#
#   scripts/triton-test-repo.sh [REPO_DIR]     (default: build/triton-test-repo)
#
# The ONNX models need no config.pbtxt: Triton fills one in from each ONNX
# file.  The Python ones (scripts/triton-test-models: text in and out, and a
# decoupled model that streams) bring theirs.
set -euo pipefail

ROXAL="$(cd "$(dirname "$0")/.." && pwd)"
REPO="${1:-$ROXAL/build/triton-test-repo}"

for model in scale-shift add-sub shift-const3; do
    dir="$REPO/${model//-/_}/1"
    mkdir -p "$dir"
    cp "$ROXAL/modules/ai/$model.onnx" "$dir/model.onnx"
done
for model in text_echo text_stream; do
    rm -rf "$REPO/$model"
    cp -r "$ROXAL/scripts/triton-test-models/$model" "$REPO/$model"
done

REPO="$(cd "$REPO" && pwd)"
cat <<EOF
Model repository: $REPO

Serve it (CPU; add --gpus all for a GPU, with the NVIDIA container toolkit):
  docker run --rm -p 8001:8001 -v $REPO:/models:ro \\
      nvcr.io/nvidia/tritonserver:25.10-py3 tritonserver --model-repository=/models

then:
  python3 runtests.py --triton-url localhost:8001

For the deployment tests too (Model(path, server=...)), serve a shared
directory as a second repository, in explicit mode:
  mkdir -p ~/roxal-triton
  docker run --rm -p 8001:8001 -v $REPO:/models:ro -v ~/roxal-triton:/roxal-models:ro \\
      nvcr.io/nvidia/tritonserver:25.10-py3 tritonserver --model-repository=/models \\
      --model-repository=/roxal-models --model-control-mode=explicit --load-model='*'
  python3 runtests.py --triton-url localhost:8001 --triton-repository ~/roxal-triton
EOF
