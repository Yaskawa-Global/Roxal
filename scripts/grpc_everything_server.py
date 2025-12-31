#!/usr/bin/env python3
"""
gRPC server for compiler/grpc/protos/alltypes.proto.

Compiles the proto on the fly using grpc_tools.protoc so no pre-generated
Python stubs are required. Requires `pip install grpcio grpcio-tools`.
"""

import argparse
import importlib
import os
import sys
import tempfile
import subprocess
import shutil
from concurrent import futures

import grpc


def compile_proto(proto_path: str, workdir: str):
    try:
        from grpc_tools import protoc
    except Exception:
        protoc = None

    proto_path = os.path.abspath(proto_path)
    proto_dir = os.path.dirname(proto_path)
    proto_file = os.path.basename(proto_path)

    if protoc is not None:
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
    else:
        # Fall back to invoking the protoc binary with the Python gRPC plugin.
        plugin = shutil.which("grpc_python_plugin")
        plugin_arg = [f"--plugin=protoc-gen-grpc_python={plugin}"] if plugin else []
        cmd = [
            "protoc",
            f"-I{proto_dir}",
            f"--python_out={workdir}",
            f"--grpc_python_out={workdir}",
            *plugin_arg,
            proto_file,
        ]
        result = subprocess.run(cmd, cwd=proto_dir)
        if result.returncode != 0:
            raise SystemExit(f"protoc compilation failed with exit code {result.returncode}")

    sys.path.insert(0, workdir)
    module_basename = os.path.splitext(proto_file)[0]
    pb2 = importlib.import_module(f"{module_basename}_pb2")
    pb2_grpc = importlib.import_module(f"{module_basename}_pb2_grpc")
    return pb2, pb2_grpc


def reverse_string(s: str) -> str:
    return s[::-1]


def mutate_everything(pb2, payload, reverse: bool):
    """Return a mutated Everything message."""
    result = pb2.Everything()
    result.CopyFrom(payload)

    if reverse:
        result.text = reverse_string(payload.text)
        result.tags[:] = list(reversed(payload.tags))
        result.coords[:] = list(reversed(payload.coords))
        result.palette[:] = list(reversed(payload.palette))
        result.data = payload.data[::-1]
        if payload.HasField("nest"):
            result.nest.note = reverse_string(payload.nest.note)
        for note in result.notes:
            note.note = reverse_string(note.note)
    else:
        result.f64 += 1.0
        result.f32 += 1.0
        result.i32 += 1
        result.i64 += 1
        result.u32 += 1
        result.u64 += 1
        result.si32 += 1
        result.si64 += 1
        result.fx32 += 1
        result.fx64 += 1
        result.sfx32 += 1
        result.sfx64 += 1
        result.flag = not payload.flag
        if payload.HasField("nest"):
            result.nest.count += 1
        for note in result.notes:
            note.count += 1
        result.tags.append("server-tag")
        result.attrs["server"] = 1
        nested = pb2.Nested(note="from-server", count=42)
        entry = result.named["server"]
        entry.CopyFrom(nested)

    return result


def serve(proto_path: str, address: str):
    with tempfile.TemporaryDirectory() as td:
        pb2, pb2_grpc = compile_proto(proto_path, td)

        class EverythingService(pb2_grpc.EverythingServiceServicer):
            def Echo(self, request, context):
                response = pb2.EchoResponse()
                response.payload.CopyFrom(mutate_everything(pb2, request.payload, reverse=False))
                response.status = "echo"
                return response

            def Reverse(self, request, context):
                response = pb2.EchoResponse()
                response.payload.CopyFrom(mutate_everything(pb2, request.payload, reverse=True))
                response.status = "reverse"
                return response

            def Status(self, request, context):
                response = pb2.StatusResponse()
                prefix = request.message or ""
                if prefix:
                    response.status = f"status:{prefix}"
                else:
                    response.status = "status:default"
                return response

            def Ping(self, request, context):
                return pb2.PingResponse()

            def ServerStream(self, request, context):
                """Stream back 'count' responses with incrementing values."""
                count = max(1, request.count)
                for i in range(count):
                    response = pb2.StreamResponse(
                        index=i,
                        value=i * 10,
                        status=f"item_{i}"
                    )
                    yield response

            def ClientStream(self, request_iterator, context):
                """Accumulate values from client stream, return sum."""
                total = 0
                count = 0
                for request in request_iterator:
                    total += request.value
                    count += 1
                return pb2.StreamResponse(
                    index=count,
                    value=total,
                    status=f"accumulated_{count}_items"
                )

            def BiStream(self, request_iterator, context):
                """Echo each request with value doubled."""
                idx = 0
                for request in request_iterator:
                    response = pb2.StreamResponse(
                        index=idx,
                        value=request.value * 2,
                        status=f"echo_{idx}"
                    )
                    yield response
                    idx += 1

        server = grpc.server(futures.ThreadPoolExecutor(max_workers=8))
        pb2_grpc.add_EverythingServiceServicer_to_server(EverythingService(), server)
        server.add_insecure_port(address)
        server.start()
        print(f"EverythingService gRPC server listening on {address}")
        try:
            server.wait_for_termination()
        except KeyboardInterrupt:
            print("Shutting down...")
            server.stop(None)


def main():
    parser = argparse.ArgumentParser(description="Run EverythingService gRPC server for Roxal testing.")
    parser.add_argument(
        "--proto",
        default=os.path.join(os.path.dirname(__file__), "..", "compiler", "grpc", "protos", "roxal_examples.proto"),
        help="Path to roxal_examples.proto",
    )
    parser.add_argument("--address", default="0.0.0.0:50051", help="Listen address")
    args = parser.parse_args()
    serve(args.proto, args.address)


if __name__ == "__main__":
    main()
