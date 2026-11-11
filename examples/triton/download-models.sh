#!/usr/bin/env bash
# Fetch the weights the examples here use -- pinned revisions -- into a host
# directory the Triton server mounts read-only at /weights, laid out as a
# Hugging Face cache (the server reads it offline).  Nothing is written into
# the repository.
#
#   examples/triton/download-models.sh [WEIGHTS_DIR]   (default ~/roxal-triton-weights)
#
# Downloads with the server image's own huggingface_hub, so build the image
# first (docker build -t roxal-triton-ai examples/triton/server); the host
# needs only docker.  About 6.5 GB.  Set ROXAL_TRITON_IMAGE to use another
# image name.
set -euo pipefail

WEIGHTS="${1:-$HOME/roxal-triton-weights}"
IMAGE="${ROXAL_TRITON_IMAGE:-roxal-triton-ai}"
mkdir -p "$WEIGHTS/hf"

fetch() {   # fetch <repo> <revision> [pattern to leave out ...]
    local repo=$1 revision=$2
    shift 2
    echo "== $repo @ ${revision:0:12}"
    docker run --rm -i -u "$(id -u):$(id -g)" -e HOME=/tmp -e HF_HUB_OFFLINE=0 -e HF_HOME=/weights/hf \
        -v "$WEIGHTS:/weights" "$IMAGE" python3 - "$repo" "$revision" "$@" <<'PY'
import os, sys
from huggingface_hub import snapshot_download
repo, revision, ignore = sys.argv[1], sys.argv[2], sys.argv[3:]
path = snapshot_download(repo_id=repo, revision=revision, ignore_patterns=ignore or None)
# The pinned revision is 'main' too: whatever loads the model by its name
# alone, offline, finds it.
refs = os.path.join(os.environ["HF_HOME"], "hub", "models--" + repo.replace("/", "--"), "refs")
os.makedirs(refs, exist_ok=True)
with open(os.path.join(refs, "main"), "w") as f:
    f.write(revision)
print("   " + path)
PY
}

# Qwen3-VL-2B-Instruct (Apache-2.0), ~4.3 GB: the vision-language model.
fetch Qwen/Qwen3-VL-2B-Instruct 89644892e4d85e24eaac8bacfd4f463576704203
# SmolVLA fine-tuned on LIBERO (Apache-2.0), ~1.2 GB: the policy.
fetch HuggingFaceVLA/smolvla_libero 6721902bc4d61e50a3bfdb11dfb4cb626f05d102
# SmolVLM2-500M (Apache-2.0), ~1 GB: SmolVLA builds its backbone and its
# tokenizer from it, by this name.
fetch HuggingFaceTB/SmolVLM2-500M-Instruct 7b375e1b73b11138ff12fe22c8f2822d8fe03467 'onnx/*'

echo "Weights in $WEIGHTS"
