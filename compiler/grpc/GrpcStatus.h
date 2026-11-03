#pragma once

#include <grpcpp/support/status_code_enum.h>

namespace roxal {

// How a failed gRPC call surfaces as a Roxal exception -- shared by the grpc
// module's calls and by ai.nn's Triton client, so the same status means the
// same thing to a script wherever it comes from.
enum class GrpcFailureKind {
    // "Not now": the server unreachable or overloaded (UNAVAILABLE), or too
    // slow for the call's deadline (DEADLINE_EXCEEDED).  UnavailableError --
    // a dataflow node whose async body fails this way keeps its last outputs.
    Unavailable,
    // The call itself was refused: a bad argument, a missing entity, ...
    // ProgramException.  RESOURCE_EXHAUSTED is one: besides an exhausted
    // quota it reports a message over the size limit, which no retry fixes.
    Application,
    // Anything else, the server's own failures (INTERNAL) included.
    // RuntimeException.
    Runtime
};

GrpcFailureKind classifyGrpcStatus(grpc::StatusCode code);

// The name of the builtin exception type a call failing with `code` raises.
const char* grpcExceptionTypeName(grpc::StatusCode code);

// The status's name as gRPC spells it, e.g. "UNAVAILABLE".
const char* grpcStatusCodeName(grpc::StatusCode code);

} // namespace roxal
