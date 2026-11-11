"""A decoupled model, as Triton's vLLM backend is: text (BYTES [1]) and an
optional n (INT32 [1], default 3) -> n responses, piece (BYTES [1]) "<text> <i>"
50 ms apart, then the final flag -- for tests/triton_real_text.rox.  Stops
early when the client cancels."""
import time

import numpy as np
import triton_python_backend_utils as pb_utils


class TritonPythonModel:
    def execute(self, requests):
        for request in requests:
            sender = request.get_response_sender()
            text = pb_utils.get_input_tensor_by_name(request, "text").as_numpy()[0].decode("utf-8")
            n = pb_utils.get_input_tensor_by_name(request, "n")
            n = int(n.as_numpy()[0]) if n is not None else 3
            for i in range(n):
                time.sleep(0.05)
                if sender.is_cancelled():
                    break
                piece = np.array([f"{text} {i}".encode("utf-8")], dtype=np.object_)
                sender.send(pb_utils.InferenceResponse(output_tensors=[pb_utils.Tensor("piece", piece)]))
            sender.send(flags=pb_utils.TRITONSERVER_RESPONSE_COMPLETE_FINAL)
        return None
