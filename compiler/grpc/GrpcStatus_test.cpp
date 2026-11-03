// The gRPC status classifier: which failed calls are temporary (a dataflow
// node holds its last outputs through them) and which raise for good.

#include "GrpcStatus.h"

#include <cstring>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(grpc::StatusCode code, const char* typeName)
{
    const char* actual = roxal::grpcExceptionTypeName(code);
    if (std::strcmp(actual, typeName) != 0) {
        std::cerr << "FAIL: status " << static_cast<int>(code) << " raises " << actual
                  << ", expected " << typeName << "\n";
        ++failures;
    }
}

} // namespace

int main()
{
    using grpc::StatusCode;

    // temporary: the server unreachable or overloaded, or too slow
    expect(StatusCode::UNAVAILABLE, "UnavailableError");
    expect(StatusCode::DEADLINE_EXCEEDED, "UnavailableError");

    // the call refused -- RESOURCE_EXHAUSTED included: it also reports a
    // message over the size limit, which waiting does not fix
    expect(StatusCode::INVALID_ARGUMENT, "ProgramException");
    expect(StatusCode::NOT_FOUND, "ProgramException");
    expect(StatusCode::RESOURCE_EXHAUSTED, "ProgramException");
    expect(StatusCode::UNKNOWN, "ProgramException");

    // the server's own failures
    expect(StatusCode::INTERNAL, "RuntimeException");
    expect(StatusCode::UNIMPLEMENTED, "RuntimeException");
    expect(StatusCode::CANCELLED, "RuntimeException");

    if (failures == 0)
        std::cout << "grpc status classification: ok\n";
    return failures == 0 ? 0 : 1;
}
