# A Triton Inference Server for the examples in examples/triton: NVIDIA's
# vLLM container (it serves Qwen3-VL with the stock vLLM backend) plus
# LeRobot, for the SmolVLA model's Python backend.  Model weights are not in
# the image: download-models.sh puts them in a host directory the server
# mounts at /weights (a Hugging Face cache, read offline).
#
#   docker build -t roxal-triton-ai examples/triton/server
#   docker build --build-arg TRITON_TAG=26.02 -t roxal-triton-ai examples/triton/server
#
# Running it: see examples/triton/README.md.
ARG TRITON_TAG=26.10
FROM nvcr.io/nvidia/tritonserver:${TRITON_TAG}-vllm-python-py3

# LeRobot decodes its datasets' videos with torchcodec, which needs FFmpeg.
RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg \
    && rm -rf /var/lib/apt/lists/*

# LeRobot next to vLLM, keeping the torch vLLM was built against: pip may not
# replace it.  (If the two cannot agree, serve SmolVLA from a second image
# built FROM tritonserver:<tag>-py3 instead -- see the README.)
RUN pip freeze | grep -iE '^(torch|torchvision|torchaudio|vllm|triton)==' > /tmp/keep.txt \
    && pip install --no-cache-dir -c /tmp/keep.txt "lerobot[smolvla]==0.7.0" \
    && rm /tmp/keep.txt

COPY extract_libero.py /opt/roxal/extract_libero.py

# The weights' Hugging Face cache, read offline: a server never downloads.
ENV HF_HOME=/weights/hf \
    HF_HUB_OFFLINE=1
