#!/usr/bin/env bash
# Fetch the AI Studio demo models into this directory (pinned sources, all
# permissively licensed), then refresh catalog.json from the files.
#   examples/aistudio/models/download-models.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
cd "$HERE"

fetch() {  # fetch <file> <url>
    if [ -s "$1" ]; then echo "  $1: present"; return; fi
    echo "  $1 <- $2"
    curl -fL --retry 3 -o "$1.part" "$2" && mv "$1.part" "$1"
}

# D-FINE nano (Apache-2.0): ships with the ai module
cp -n "$REPO/modules/ai/dfine_nano_fp32.onnx" dfine_nano_fp32.onnx 2>/dev/null || true
# Depth Anything V2 small (Apache-2.0), ONNX export by onnx-community
fetch depth_anything_v2_small.onnx \
      "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx"
# fast-neural-style mosaic (BSD-3, ONNX model zoo)
fetch fast_neural_style_mosaic.onnx \
      "https://github.com/onnx/models/raw/main/validated/vision/style_transfer/fast_neural_style/model/mosaic-9.onnx"
# MobileNet v2 (Apache-2.0, ONNX model zoo)
fetch mobilenetv2.onnx \
      "https://github.com/onnx/models/raw/main/validated/vision/classification/mobilenet/model/mobilenetv2-12.onnx"
# DeepLabV3 MobileNetV2, Pascal VOC (Apache-2.0): semantic segmentation.
# (SegFormer would be the obvious modern choice, but the NVIDIA weights every
# ADE20K export descends from are research/non-commercial only.)
fetch deeplabv3_mobilenetv2_voc.onnx \
      "https://huggingface.co/ketiswp/google-coral-DeepLabV3-MobileNetV2-1.0-PascalVOC-fp32-onnx/resolve/main/model.onnx"

python3 "$REPO/tools/nn-catalog.py" "$HERE"
