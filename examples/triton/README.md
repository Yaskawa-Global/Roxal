# AI models on a Triton server

Two models served by an [NVIDIA Triton Inference Server](https://github.com/triton-inference-server/server)
and deployed by the Roxal scripts that use them (`ai.nn.Model(<model directory>, server=...)`):

| Script | Model | Served by |
|---|---|---|
| `vlm_describe.rox` | Qwen3-VL-2B-Instruct, a vision-language model: describes an image, its answer streamed | Triton's stock vLLM backend (`models/qwen3_vl/model.json`) |
| `vlm_signal.rox` | the same, in a dataflow network: a signal of questions in, a signal of answers out | |
| `smolvla_libero.rox` | SmolVLA (LeRobot), a vision-language-action policy: predicts a robot's next 50 actions from camera images, its state and a task, compared with a recorded LIBERO demonstration | a Python-backend model (`models/smolvla/model.py`) |

They need a Roxal built with Triton support (`ROXAL_ENABLE_TRITON`), an NVIDIA GPU, and Docker with
the [NVIDIA Container Toolkit](https://docs.nvidia.com/datacenter/cloud-native/container-toolkit/).
The weights (about 6.5 GB) are downloaded to a directory outside the repository.

## 1. Build the server image

```
docker build -t roxal-triton-ai examples/triton/server
```

It is NVIDIA's `tritonserver:26.10-vllm-python-py3` image (about 20 GB) with LeRobot added.

- **Driver.** 26.x images are built for newer drivers than R580.  On a GeForce card an older
  driver runs them in CUDA's minor-version compatibility mode (they print a warning but run).  If
  the server fails to start, update the driver, or build from an older image:
  `docker build --build-arg TRITON_TAG=26.02 ...` (vLLM 0.15).
- **If LeRobot's and vLLM's packages conflict** (the build fails installing LeRobot), serve SmolVLA
  from a second image instead -- the same Dockerfile `FROM nvcr.io/nvidia/tritonserver:25.10-py3`,
  without the constraint on torch -- on another port.

## 2. Download the weights

```
examples/triton/download-models.sh            # into ~/roxal-triton-weights, or give a directory
examples/triton/extract-libero-frames.sh      # for smolvla_libero.rox: a few LIBERO frames
```

Both run in the server image, so the host needs nothing but Docker.  The weights are kept as a
Hugging Face cache, at pinned revisions; the server reads it offline.

## 3. Start the server

```
mkdir -p ~/roxal-triton      # an (empty) model repository: models arrive from the scripts
docker run -d --name roxal-triton --gpus device=<GPU UUID> --shm-size=1g \
    -p 18001:8001 -p 18000:8000 \
    -v ~/roxal-triton-weights:/weights:ro -v ~/roxal-triton:/models \
    roxal-triton-ai tritonserver --model-repository=/models --model-control-mode=explicit
curl -sf localhost:18000/v2/health/ready && echo ready
```

- `--model-control-mode=explicit` lets scripts load models.
- Pick the GPU by UUID (`nvidia-smi -L`): `nvidia-smi` and CUDA can number GPUs differently.
- Memory: Qwen3-VL takes 60% of the GPU (`gpu_memory_utilization` in `model.json`: weights plus its
  KV cache, about 6.5 GB of an 11 GB card); SmolVLA runs in FP32, 2-3 GB.  On a smaller GPU lower
  the 0.6, or run one model at a time (`model.unload()`).
- Stop it with `docker rm -f roxal-triton`.

## 4. Run the examples

```
roxal examples/triton/vlm_describe.rox [image] [question] [host:port]
roxal examples/triton/vlm_signal.rox [image] [host:port]
roxal examples/triton/smolvla_libero.rox [host:port]
```

The server defaults to `localhost:18001`.  A model's first deployment loads it on the server --
a minute or more for Qwen3-VL -- and later runs find it loaded (models are deployed as
`roxal.<directory>.<content hash>`).

## Notes

- **Text.** Prompts, task instructions and answers are Triton `BYTES` tensors: strings in Roxal.
  `text_output` is declared `[-1]`, so each answer is a list of one string.
- **Streaming.** The vLLM backend is a *decoupled* model: it answers on a stream.  `predict`
  returns a future that resolves, with the first response, to a signal that each later response
  sets; it stops running after the last, and stopping it cancels the generation.  Streamed, each
  response holds only the newly generated text.
- **Prompts** go to the vLLM backend as they are -- in the model's own chat format, with
  `<|vision_start|><|image_pad|><|vision_end|>` where the image goes.  Images travel as base64
  JPEG (`media.Image.encode`, `base64_encode`).
- **Turing GPUs** (RTX 20xx, T4) have no BF16: Qwen3-VL runs in FP16 (`"dtype": "half"`), SmolVLA in
  FP32.
