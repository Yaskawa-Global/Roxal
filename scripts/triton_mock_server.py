#!/usr/bin/env python3
"""
A stand-in for an NVIDIA Triton Inference Server, for ai.nn's Triton tests:
the four GRPCInferenceService methods the backend calls, over the messages
vendored in third_party/triton (compiled on the fly with grpc_tools.protoc).

Models:
  scale_shift  x FP32 [1,4] -> doubled = x*2, plus_one = x+1.  Its outputs are
               listed plus_one first, as Triton's autofilled config does for
               the ONNX file (whose order is doubled, plus_one).
  reject       x FP32 [1,4] -> y; every inference fails INVALID_ARGUMENT.
  __control    mode INT32 [1] -> ok INT32 [1].  Sets how the server answers
               every OTHER model's inferences: 0 up, 1 down (UNAVAILABLE),
               2 slow (each takes 2 s, past a client's deadline).  Itself
               always answers, so a test can bring the server back.

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
        self.models = {
            "scale_shift": {"inputs": [("x", "FP32", [1, 4])],
                            "outputs": [("plus_one", "FP32", [1, 4]), ("doubled", "FP32", [1, 4])]},
            "reject": {"inputs": [("x", "FP32", [1, 4])],
                       "outputs": [("y", "FP32", [1, 4])]},
            "__control": {"inputs": [("mode", "INT32", [1])],
                          "outputs": [("ok", "INT32", [1])]},
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
            for name, datatype, shape in model[kind]:
                getattr(reply, kind).add(name=name, datatype=datatype, shape=shape)
        return reply

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
            reply = self.pb.ModelInferResponse(model_name="__control")
            reply.outputs.add(name="ok", datatype="INT32", shape=[1])
            reply.raw_output_contents.append(struct.pack("<i", 1))
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
        "ModelInfer": (pb.ModelInferRequest, mock.ModelInfer),
    }
    handlers = {
        name: grpc.unary_unary_rpc_method_handler(
            fn, request_deserializer=req.FromString,
            response_serializer=lambda reply: reply.SerializeToString())
        for name, (req, fn) in methods.items()
    }
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
