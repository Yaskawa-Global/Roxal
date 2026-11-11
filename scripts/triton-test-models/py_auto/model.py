"""A Python model that declares its own inputs and outputs (auto_complete_config),
so a script can deploy its directory with config={'backend': 'python'} alone --
as examples/triton/models/smolvla does.  name (BYTES [1]) -> greeting (BYTES [1])
"Hello, <name>!", length (INT32 [1])."""
import numpy as np
import triton_python_backend_utils as pb_utils


class TritonPythonModel:
    @staticmethod
    def auto_complete_config(config):
        config.add_input({"name": "name", "data_type": "TYPE_STRING", "dims": [1]})
        config.add_output({"name": "greeting", "data_type": "TYPE_STRING", "dims": [1]})
        config.add_output({"name": "length", "data_type": "TYPE_INT32", "dims": [1]})
        config.set_max_batch_size(0)
        return config

    def execute(self, requests):
        responses = []
        for request in requests:
            name = pb_utils.get_input_tensor_by_name(request, "name").as_numpy()[0].decode("utf-8")
            greeting = f"Hello, {name}!"
            responses.append(pb_utils.InferenceResponse(output_tensors=[
                pb_utils.Tensor("greeting", np.array([greeting.encode("utf-8")], dtype=np.object_)),
                pb_utils.Tensor("length", np.array([len(greeting)], dtype=np.int32))]))
        return responses
