#!/bin/bash
# Download the ONNX models used by the opencv module's DNN task wrappers
# (face detection, ALIKED/LightGlue features, VIT tracking).
# Run once after cloning; fetches ~55 MB into this directory.
set -e
cd "$(dirname "$0")"

fetch() {  # fetch <file> <min-bytes> <url>
    if [ -f "$1" ] && [ "$(stat -c%s "$1")" -ge "$2" ]; then
        echo "have     $1"
        return
    fi
    echo "fetching $1 ..."
    curl -sL --fail -o "$1.part" "$3"
    local size
    size=$(stat -c%s "$1.part")
    if [ "$size" -lt "$2" ]; then
        echo "ERROR: $1 downloaded only $size bytes (expected >= $2) — bad URL or LFS pointer?" >&2
        rm -f "$1.part"
        exit 1
    fi
    mv "$1.part" "$1"
}

fetch face_detection_yunet_2023mar.onnx 100000 \
    "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx"
fetch object_tracking_vittrack_2023sep.onnx 500000 \
    "https://github.com/opencv/opencv_zoo/raw/main/models/object_tracking_vittrack/object_tracking_vittrack_2023sep.onnx"
fetch aliked-n16rot-top1k-640.onnx 5000000 \
    "https://huggingface.co/bukuroo/ALIKED-LightGlue-ONNX/resolve/main/aliked-n16rot-top1k-640.onnx"
fetch lightglue_for_aliked.onnx 40000000 \
    "https://huggingface.co/bukuroo/ALIKED-LightGlue-ONNX/resolve/main/lightglue_for_aliked.onnx"

echo "all models present"
