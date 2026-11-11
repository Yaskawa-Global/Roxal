#!/usr/bin/env python3
"""
A stand-in for an NVIDIA Triton Inference Server, for ai.nn's Triton tests:
the GRPCInferenceService methods the backend calls, over the messages
vendored in third_party/triton (compiled on the fly with grpc_tools.protoc).

Models:
  scale_shift  x FP32 [1,4] -> doubled = x*2, plus_one = x+1.  Its outputs are
               listed plus_one first, as Triton's autofilled config does for
               the ONNX file (whose order is doubled, plus_one).
  reject       x FP32 [1,4] -> y; every inference fails INVALID_ARGUMENT.
  echo_bytes   text BYTES [-1] -> echo BYTES [-1] (the elements, as sent),
               first BYTES [1] (the first element), count INT32 [1].
  optional_in  prompt BYTES [1], optional scale FP32 [1] and tag BYTES [1]
               -> described BYTES [1], saying which inputs arrived.
  countdown    DECOUPLED (answers on ModelStreamInfer only, as Triton's vLLM
               backend): text_input BYTES [1], optional n, gap_ms, fail_at
               INT32 [1] -> n responses text_output BYTES [1] "<text> <i>" and
               step INT32 [1], gap_ms apart (default 3 responses, 10 ms),
               then the final-response flag; fail_at=i reports an error
               instead of response i.
  __control    mode INT32 [1] -> ok INT32 [1].  Sets how the server answers
               every OTHER model's inferences: 0 up, 1 down (UNAVAILABLE),
               2 slow (each takes 2 s, past a client's deadline).  Itself
               always answers, so a test can bring the server back.  Mode 7
               answers how many streams were cancelled by their client since
               the last mode 7.

Requires `pip install grpcio grpcio-tools`.
"""

import argparse
import importlib
import os
import sys
import tempfile
import threading
import time
from concurrent import futures

import grpc

UP, DOWN, SLOW = 0, 1, 2
# Further control modes, for model loading: refuse loads as a server not in
# explicit mode does, or allow them (the default); fail to see symlinked
# model files (as a container that does not mount their targets), or see them
# (the default).
LOADS_REFUSED, LOADS_ALLOWED, SYMLINKS_UNSEEN, SYMLINKS_SEEN = 3, 4, 5, 6
CANCELLED_STREAMS = 7
DATATYPES = {"BOOL": 1, "UINT8": 2, "UINT16": 3, "UINT32": 4, "UINT64": 5, "INT8": 6, "INT16": 7,
             "INT32": 8, "INT64": 9, "FP16": 10, "FP32": 11, "FP64": 12, "BYTES": 13, "BF16": 14}


def bytes_elements(raw):
    """A BYTES tensor's elements: each a 4-byte little-endian length, then its bytes."""
    import struct
    elements, at = [], 0
    while at < len(raw):
        (n,) = struct.unpack_from("<I", raw, at)
        elements.append(raw[at + 4:at + 4 + n])
        at += 4 + n
    return elements


def bytes_contents(elements):
    import struct
    return b"".join(struct.pack("<I", len(e)) + e for e in elements)


def request_inputs(request):
    """name -> (datatype, shape, raw contents), in the request's input order."""
    return {i.name: (i.datatype, list(i.shape), request.raw_input_contents[k])
            for k, i in enumerate(request.inputs)}
SCALE_SHIFT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "modules", "ai", "scale-shift.onnx")
SERVICE = "inference.GRPCInferenceService"


def load_messages(workdir):
    from grpc_tools import protoc
    proto_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "triton")
    result = protoc.main(["protoc", f"-I{proto_dir}", f"--python_out={workdir}",
                          "triton_inference.proto"])
    if result != 0:
        raise RuntimeError("protoc failed on triton_inference.proto")
    sys.path.insert(0, workdir)
    return importlib.import_module("triton_inference_pb2")


class Mock:
    def __init__(self, pb, repository):
        self.pb = pb
        self.mode = UP
        self.repository = repository
        self.loads_refused = False
        self.symlinks_unseen = False
        with open(SCALE_SHIFT, "rb") as f:
            self.scale_shift_bytes = f.read()
        self.lock = threading.Lock()
        self.cancelled_streams = 0
        # name -> {"inputs": [(name, datatype, shape[, optional])], "outputs": [...],
        #          "decoupled": bool}
        self.models = {
            "scale_shift": {"inputs": [("x", "FP32", [1, 4])],
                            "outputs": [("plus_one", "FP32", [1, 4]), ("doubled", "FP32", [1, 4])]},
            "reject": {"inputs": [("x", "FP32", [1, 4])],
                       "outputs": [("y", "FP32", [1, 4])]},
            "__control": {"inputs": [("mode", "INT32", [1])],
                          "outputs": [("ok", "INT32", [1])]},
            "echo_bytes": {"inputs": [("text", "BYTES", [-1])],
                           "outputs": [("echo", "BYTES", [-1]), ("first", "BYTES", [1]),
                                       ("count", "INT32", [1])]},
            "optional_in": {"inputs": [("prompt", "BYTES", [1]), ("scale", "FP32", [1], True),
                                       ("tag", "BYTES", [1], True)],
                            "outputs": [("described", "BYTES", [1])]},
            "countdown": {"inputs": [("text_input", "BYTES", [1]), ("n", "INT32", [1], True),
                                     ("gap_ms", "INT32", [1], True), ("fail_at", "INT32", [1], True)],
                          "outputs": [("text_output", "BYTES", [1]), ("step", "INT32", [1])],
                          "decoupled": True},
        }

    def ServerLive(self, request, context):
        return self.pb.ServerLiveResponse(live=True)

    def ServerMetadata(self, request, context):
        return self.pb.ServerMetadataResponse(name="triton-mock", version="0.1",
                                              extensions=["model_configuration", "model_repository"])

    # Model loading.  The mock serves only scale-shift.onnx's content, under
    # whatever name it is loaded as: uploaded, or from <repository>/<name>/1/.
    def RepositoryIndex(self, request, context):
        reply = self.pb.RepositoryIndexResponse()
        for name in sorted(os.listdir(self.repository)):
            if not name.startswith(".") and os.path.isdir(os.path.join(self.repository, name)):
                reply.models.add(name=name, state="READY" if name in self.models else "")
        return reply

    def _refuse_if_polling(self, context):
        if self.loads_refused:
            context.abort(grpc.StatusCode.UNAVAILABLE,
                          "explicit model load / unload is not allowed if polling is enabled")

    def RepositoryModelLoad(self, request, context):
        self._refuse_if_polling(context)
        name = request.model_name
        files = {k[len("file:1/"):]: v.bytes_param
                 for k, v in request.parameters.items() if k.startswith("file:1/")}
        if files:
            model = files.get("model.onnx")
        else:
            path = os.path.join(self.repository, name, "1", "model.onnx")
            if not os.path.lexists(path):
                context.abort(grpc.StatusCode.INTERNAL, f"failed to load '{name}', no version is available")
            if os.path.islink(path) and self.symlinks_unseen:
                context.abort(grpc.StatusCode.INTERNAL, f"failed to load '{name}': unable to open '{path}'")
            with open(path, "rb") as f:
                model = f.read()
        if model != self.scale_shift_bytes:
            context.abort(grpc.StatusCode.INTERNAL, f"failed to load '{name}': the mock serves scale-shift.onnx only")
        with self.lock:
            self.models[name] = self.models["scale_shift"]
        return self.pb.RepositoryModelLoadResponse()

    def RepositoryModelUnload(self, request, context):
        self._refuse_if_polling(context)
        with self.lock:
            self.models.pop(request.model_name, None)
        return self.pb.RepositoryModelUnloadResponse()

    def ModelReady(self, request, context):
        return self.pb.ModelReadyResponse(ready=request.name in self.models)

    def ModelMetadata(self, request, context):
        model = self.models.get(request.name)
        if model is None:
            context.abort(grpc.StatusCode.NOT_FOUND, f"Request for unknown model: '{request.name}' is not found")
        reply = self.pb.ModelMetadataResponse(name=request.name, versions=["1"], platform="onnxruntime_onnx")
        for kind in ("inputs", "outputs"):
            for name, datatype, shape, *_ in model[kind]:
                getattr(reply, kind).add(name=name, datatype=datatype, shape=shape)
        return reply

    def ModelConfig(self, request, context):
        model = self.models.get(request.name)
        if model is None:
            context.abort(grpc.StatusCode.NOT_FOUND, f"Request for unknown model: '{request.name}' is not found")
        config = self.pb.ModelConfig(name=request.name, max_batch_size=0)
        for name, datatype, shape, *optional in model["inputs"]:
            config.input.add(name=name, data_type=DATATYPES[datatype], dims=shape,
                             optional=bool(optional and optional[0]))
        for name, datatype, shape, *_ in model["outputs"]:
            config.output.add(name=name, data_type=DATATYPES[datatype], dims=shape)
        config.model_transaction_policy.decoupled = model.get("decoupled", False)
        return self.pb.ModelConfigResponse(config=config)

    def ModelInfer(self, request, context):
        import struct
        if request.model_name == "__control":
            (mode,) = struct.unpack("<i", request.raw_input_contents[0][:4])
            with self.lock:
                if mode in (LOADS_REFUSED, LOADS_ALLOWED):
                    self.loads_refused = mode == LOADS_REFUSED
                elif mode in (SYMLINKS_UNSEEN, SYMLINKS_SEEN):
                    self.symlinks_unseen = mode == SYMLINKS_UNSEEN
                else:
                    self.mode = mode
            ok = 1
            with self.lock:
                if mode == CANCELLED_STREAMS:
                    ok, self.cancelled_streams = self.cancelled_streams, 0
            reply = self.pb.ModelInferResponse(model_name="__control")
            reply.outputs.add(name="ok", datatype="INT32", shape=[1])
            reply.raw_output_contents.append(struct.pack("<i", ok))
            return reply
        with self.lock:
            mode = self.mode
        if mode == DOWN:
            context.abort(grpc.StatusCode.UNAVAILABLE, "the mock server is down")
        if mode == SLOW:
            time.sleep(2.0)
        if request.model_name not in self.models:
            context.abort(grpc.StatusCode.NOT_FOUND, f"Request for unknown model: '{request.model_name}'")
        if request.model_name == "reject":
            context.abort(grpc.StatusCode.INVALID_ARGUMENT, "the mock rejects every input")
        if self.models[request.model_name].get("decoupled"):
            context.abort(grpc.StatusCode.UNIMPLEMENTED,
                          "ModelInfer RPC doesn't support models with decoupled transaction policy")
        if request.model_name == "echo_bytes":
            return self._echo_bytes(request)
        if request.model_name == "optional_in":
            return self._optional_in(request)

        x = struct.unpack("<4f", request.raw_input_contents[0])
        results = {"doubled": [v * 2 for v in x], "plus_one": [v + 1 for v in x]}
        wanted = [o.name for o in request.outputs] or ["plus_one", "doubled"]
        reply = self.pb.ModelInferResponse(model_name=request.model_name)
        # Answer in the server's own order, whatever order was asked for.
        for name in ("plus_one", "doubled"):
            if name in wanted:
                reply.outputs.add(name=name, datatype="FP32", shape=[1, 4])
                reply.raw_output_contents.append(struct.pack("<4f", *results[name]))
        return reply


    def _echo_bytes(self, request):
        import struct
        datatype, shape, raw = request_inputs(request)["text"]
        elements = bytes_elements(raw)
        wanted = [o.name for o in request.outputs] or ["echo", "first", "count"]
        reply = self.pb.ModelInferResponse(model_name=request.model_name)
        for name in ("echo", "first", "count"):
            if name not in wanted:
                continue
            if name == "echo":
                reply.outputs.add(name=name, datatype="BYTES", shape=shape)
                reply.raw_output_contents.append(bytes_contents(elements))
            elif name == "first":
                reply.outputs.add(name=name, datatype="BYTES", shape=[1])
                reply.raw_output_contents.append(bytes_contents(elements[:1]))
            else:
                reply.outputs.add(name=name, datatype="INT32", shape=[1])
                reply.raw_output_contents.append(struct.pack("<i", len(elements)))
        return reply

    def _optional_in(self, request):
        import struct
        given = request_inputs(request)
        parts = ["prompt=" + bytes_elements(given["prompt"][2])[0].decode()]
        parts.append("scale=" + (f"{struct.unpack('<f', given['scale'][2])[0]:g}" if "scale" in given else "none"))
        parts.append("tag=" + (bytes_elements(given["tag"][2])[0].decode() if "tag" in given else "none"))
        reply = self.pb.ModelInferResponse(model_name=request.model_name)
        reply.outputs.add(name="described", datatype="BYTES", shape=[1])
        reply.raw_output_contents.append(bytes_contents([" ".join(parts).encode()]))
        return reply

    # A decoupled model's stream: each request read is answered with its
    # responses, then the final flag (alone, when the request enabled empty
    # final responses, as ai.nn's do).
    def ModelStreamInfer(self, request_iterator, context):
        import struct
        finished = threading.Event()

        def on_done():
            if not finished.is_set():
                with self.lock:
                    self.cancelled_streams += 1
        context.add_callback(on_done)

        for request in request_iterator:
            with self.lock:
                mode = self.mode
            if mode == DOWN:
                finished.set()
                context.abort(grpc.StatusCode.UNAVAILABLE, "the mock server is down")
            model = self.models.get(request.model_name)
            if model is None or not model.get("decoupled"):
                yield self.pb.ModelStreamInferResponse(
                    error_message=f"the mock streams only decoupled models, not '{request.model_name}'")
                continue
            given = request_inputs(request)
            text = bytes_elements(given["text_input"][2])[0].decode()
            option = lambda name, default: struct.unpack("<i", given[name][2])[0] if name in given else default
            n, gap_ms, fail_at = option("n", 3), option("gap_ms", 10), option("fail_at", -1)
            empty_final = request.parameters["triton_enable_empty_final_response"].bool_param \
                if "triton_enable_empty_final_response" in request.parameters else False
            for i in range(n):
                time.sleep(gap_ms / 1000.0)
                if not context.is_active():
                    return
                if i == fail_at:
                    yield self.pb.ModelStreamInferResponse(error_message=f"the mock failed at step {i}")
                    break
                response = self.pb.ModelInferResponse(model_name=request.model_name, id=request.id)
                response.outputs.add(name="text_output", datatype="BYTES", shape=[1])
                response.raw_output_contents.append(bytes_contents([f"{text} {i}".encode()]))
                response.outputs.add(name="step", datatype="INT32", shape=[1])
                response.raw_output_contents.append(struct.pack("<i", i))
                response.parameters["triton_final_response"].bool_param = (i == n - 1 and not empty_final)
                yield self.pb.ModelStreamInferResponse(infer_response=response)
            else:
                if empty_final:
                    response = self.pb.ModelInferResponse(model_name=request.model_name, id=request.id)
                    response.parameters["triton_final_response"].bool_param = True
                    yield self.pb.ModelStreamInferResponse(infer_response=response)
        finished.set()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--address", default="127.0.0.1:28928")
    parser.add_argument("--repository", default=None,
                        help="the directory served as the model repository (default: a temporary one)")
    args = parser.parse_args()

    workdir = tempfile.mkdtemp(prefix="triton_mock_")
    pb = load_messages(workdir)
    mock = Mock(pb, args.repository or tempfile.mkdtemp(prefix="triton_mock_repo_"))
    methods = {
        "ServerLive": (pb.ServerLiveRequest, mock.ServerLive),
        "ServerMetadata": (pb.ServerMetadataRequest, mock.ServerMetadata),
        "RepositoryIndex": (pb.RepositoryIndexRequest, mock.RepositoryIndex),
        "RepositoryModelLoad": (pb.RepositoryModelLoadRequest, mock.RepositoryModelLoad),
        "RepositoryModelUnload": (pb.RepositoryModelUnloadRequest, mock.RepositoryModelUnload),
        "ModelReady": (pb.ModelReadyRequest, mock.ModelReady),
        "ModelMetadata": (pb.ModelMetadataRequest, mock.ModelMetadata),
        "ModelConfig": (pb.ModelConfigRequest, mock.ModelConfig),
        "ModelInfer": (pb.ModelInferRequest, mock.ModelInfer),
    }
    handlers = {
        name: grpc.unary_unary_rpc_method_handler(
            fn, request_deserializer=req.FromString,
            response_serializer=lambda reply: reply.SerializeToString())
        for name, (req, fn) in methods.items()
    }
    handlers["ModelStreamInfer"] = grpc.stream_stream_rpc_method_handler(
        mock.ModelStreamInfer, request_deserializer=pb.ModelInferRequest.FromString,
        response_serializer=lambda reply: reply.SerializeToString())
    server = grpc.server(futures.ThreadPoolExecutor(max_workers=16),
                         options=[("grpc.max_receive_message_length", -1),
                                  ("grpc.max_send_message_length", -1)])
    server.add_generic_rpc_handlers((grpc.method_handlers_generic_handler(SERVICE, handlers),))
    server.add_insecure_port(args.address)
    server.start()
    print(f"triton mock listening on {args.address}", flush=True)
    server.wait_for_termination()


if __name__ == "__main__":
    main()
