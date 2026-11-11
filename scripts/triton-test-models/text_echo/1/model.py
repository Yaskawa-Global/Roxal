"""text (BYTES [-1]) -> upper (BYTES [-1]): each element upper-cased, with the
optional suffix appended -- for ai.nn's real-server tests (tests/triton_real_text.rox)."""
import numpy as np
import triton_python_backend_utils as pb_utils


class TritonPythonModel:
    def execute(self, requests):
        responses = []
        for request in requests:
            texts = pb_utils.get_input_tensor_by_name(request, "text").as_numpy()
            suffix = pb_utils.get_input_tensor_by_name(request, "suffix")
            suffix = suffix.as_numpy()[0].decode("utf-8") if suffix is not None else ""
            upper = np.array([(t.decode("utf-8").upper() + suffix).encode("utf-8") for t in texts], dtype=np.object_)
            responses.append(pb_utils.InferenceResponse(output_tensors=[pb_utils.Tensor("upper", upper)]))
        return responses
