#!/usr/bin/env python3
"""
Minimal gRPC server for compiler/protos/arithematic.proto.

It compiles the proto on-the-fly using grpc_tools.protoc so it does not rely
on pre-generated Python stubs.  Requires `pip install grpcio grpcio-tools`.
"""

import argparse
import importlib
import os
import sys
import tempfile
from concurrent import futures

import grpc


def compile_proto(proto_path: str, workdir: str):
    from grpc_tools import protoc

    proto_path = os.path.abspath(proto_path)
    proto_dir = os.path.dirname(proto_path)
    proto_file = os.path.basename(proto_path)
    result = protoc.main(
        [
            "protoc",
            f"-I{proto_dir}",
            f"--python_out={workdir}",
            f"--grpc_python_out={workdir}",
            proto_file,
        ]
    )
    if result != 0:
        raise SystemExit(f"protoc compilation failed with exit code {result}")

    sys.path.insert(0, workdir)
    pb2 = importlib.import_module("arithematic_pb2")
    pb2_grpc = importlib.import_module("arithematic_pb2_grpc")
    return pb2, pb2_grpc


def serve(proto_path: str, address: str):
    with tempfile.TemporaryDirectory() as td:
        pb2, pb2_grpc = compile_proto(proto_path, td)

        class Arithmetic(pb2_grpc.ArithematicServicer):
            def Add(self, request, context):
                return pb2.NumResponse(ans=request.arg1 + request.arg2)

            def Subtract(self, request, context):
                return pb2.NumResponse(ans=request.arg1 - request.arg2)

            def Multiply(self, request, context):
                return pb2.NumResponse(ans=request.arg1 * request.arg2)

            def Divide(self, request, context):
                # mimic integer divide like sample expectations (10/7 -> 1)
                if request.arg2 == 0:
                    context.abort(grpc.StatusCode.INVALID_ARGUMENT, "divide by zero")
                return pb2.NumResponse(ans=request.arg1 / request.arg2)

        server = grpc.server(futures.ThreadPoolExecutor(max_workers=4))
        pb2_grpc.add_ArithematicServicer_to_server(Arithmetic(), server)
        server.add_insecure_port(address)
        server.start()
        print(f"Arithmetic gRPC server listening on {address}")
        try:
            server.wait_for_termination()
        except KeyboardInterrupt:
            print("Shutting down...")
            server.stop(None)


def main():
    parser = argparse.ArgumentParser(description="Run arithmetic gRPC server for Roxal sample.")
    parser.add_argument(
        "--proto",
        default=os.path.join(os.path.dirname(__file__), "..", "compiler", "grpc", "protos", "arithematic.proto"),
        help="Path to arithematic.proto",
    )
    parser.add_argument("--address", default="0.0.0.0:50051", help="Listen address")
    args = parser.parse_args()
    serve(args.proto, args.address)


if __name__ == "__main__":
    main()
