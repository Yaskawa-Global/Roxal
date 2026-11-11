"""SmolVLA -- a vision-language-action policy (Hugging Face LeRobot) -- as a
Triton Python-backend model: camera images, the robot's state and a task in;
the next chunk of actions out.

Deployed by smolvla_libero.rox, with ai.nn.Model(<this directory>, server=...).
The weights come from the server's offline Hugging Face cache (/weights,
filled by download-models.sh).

Configuration parameters:
  checkpoint  the policy's Hugging Face repo id (default HuggingFaceVLA/smolvla_libero)

Inputs and output follow the checkpoint's own config (smolvla_libero's shown):
  image, image2  UINT8 [256, 256, 3]  each camera's RGB image
  state          FP32 [8]             the robot's state
  task           BYTES [1]            the instruction
  actions        FP32 [50, 7]         the next 50 actions, unnormalized
"""
import json

import numpy as np
import triton_python_backend_utils as pb_utils

DEFAULT_CHECKPOINT = "HuggingFaceVLA/smolvla_libero"


def _parameter(config, name, default):
    value = config.get("parameters", {}).get(name, {}).get("string_value")
    return value or default


def _features(checkpoint):
    """The checkpoint's cameras -- (feature key, input name, height, width) --
    state size, action size and chunk length."""
    from huggingface_hub import hf_hub_download

    with open(hf_hub_download(checkpoint, "config.json")) as f:
        config = json.load(f)
    cameras, state = [], 0
    for key, feature in config["input_features"].items():
        if feature["type"] == "VISUAL":
            _, height, width = feature["shape"]
            cameras.append((key, key.rsplit(".", 1)[-1], height, width))
        elif feature["type"] == "STATE":
            state = feature["shape"][0]
    action = config["output_features"]["action"]["shape"][0]
    return cameras, state, action, config["chunk_size"]


class TritonPythonModel:
    @staticmethod
    def auto_complete_config(auto_complete_model_config):
        config = auto_complete_model_config.as_dict()
        cameras, state, action, chunk = _features(_parameter(config, "checkpoint", DEFAULT_CHECKPOINT))
        for _, name, height, width in cameras:
            auto_complete_model_config.add_input({"name": name, "data_type": "TYPE_UINT8",
                                                  "dims": [height, width, 3]})
        auto_complete_model_config.add_input({"name": "state", "data_type": "TYPE_FP32", "dims": [state]})
        auto_complete_model_config.add_input({"name": "task", "data_type": "TYPE_STRING", "dims": [1]})
        auto_complete_model_config.add_output({"name": "actions", "data_type": "TYPE_FP32",
                                               "dims": [chunk, action]})
        auto_complete_model_config.set_max_batch_size(0)
        return auto_complete_model_config

    def initialize(self, args):
        import torch
        from lerobot.policies.factory import make_pre_post_processors
        from lerobot.policies.smolvla.modeling_smolvla import SmolVLAPolicy

        config = json.loads(args["model_config"])
        checkpoint = _parameter(config, "checkpoint", DEFAULT_CHECKPOINT)
        if args.get("model_instance_kind") == "GPU":
            self.device = torch.device("cuda", int(args["model_instance_device_id"]))
        else:
            self.device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
        self.torch = torch
        self.cameras = _features(checkpoint)[0]
        # FP32: small enough for any GPU, and needs no BF16 hardware (Turing has none).
        self.policy = SmolVLAPolicy.from_pretrained(checkpoint).to(self.device).eval()
        self.preprocess, self.postprocess = make_pre_post_processors(
            self.policy.config, checkpoint,
            preprocessor_overrides={"device_processor": {"device": str(self.device)}})

    def execute(self, requests):
        torch = self.torch
        responses = []
        for request in requests:
            try:
                frame = {}
                for key, name, _, _ in self.cameras:
                    pixels = pb_utils.get_input_tensor_by_name(request, name).as_numpy()
                    frame[key] = torch.from_numpy(np.ascontiguousarray(pixels)).permute(2, 0, 1).float() / 255.0
                state = pb_utils.get_input_tensor_by_name(request, "state").as_numpy()
                frame["observation.state"] = torch.from_numpy(state.astype(np.float32))
                task = pb_utils.get_input_tensor_by_name(request, "task").as_numpy()[0]
                frame["task"] = task.decode("utf-8") if isinstance(task, bytes) else str(task)

                batch = self.preprocess(frame)
                with torch.inference_mode():
                    actions = self.postprocess(self.policy.predict_action_chunk(batch))
                actions = actions.squeeze(0).float().cpu().numpy().astype(np.float32)
                responses.append(pb_utils.InferenceResponse(output_tensors=[pb_utils.Tensor("actions", actions)]))
            except Exception as e:  # one request's failure, not the model's
                responses.append(pb_utils.InferenceResponse(output_tensors=[], error=pb_utils.TritonError(str(e))))
        return responses
