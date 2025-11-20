#ifdef ROXAL_ENABLE_GRPC

#include "Connector.h"

#include <stdexcept>
#include <grpcpp/support/status_code_enum.h>

using namespace roxal;

GrpcStatusError::GrpcStatusError(const std::string& method,
                                 const grpc::Status& status)
    : std::runtime_error("gRPC call '" + method + "' failed [" +
                         std::string(grpcStatusCodeName(status.error_code())) + "]: " +
                         status.error_message()),
      statusCode(status.error_code()),
      methodName(method),
      statusMessage(status.error_message())
{}

ACUCommunicator::ACUCommunicator(std::shared_ptr<grpc::Channel> channel, ProtoAdapter* adapter)
{
    if (!adapter)
        throw std::invalid_argument("ACUCommunicator requires a ProtoAdapter");
    m_caller = std::make_unique<ClientCall>(channel);
    m_adapter = adapter;
}

Value ACUCommunicator::call(const std::string& methodName,
                            ArgsView args,
                            std::optional<std::chrono::milliseconds> timeout)
{
    if (!m_adapter || !m_caller)
        throw std::runtime_error("gRPC connector is not initialized");

    if (args.size() != 1)
        throw std::invalid_argument("gRPC service calls expect a single request object argument");

    std::string request = m_adapter->generateProtocRequest(methodName, args[0]);
    std::string response;

    grpc::Status status = m_caller->Call(m_adapter->getFormattedMethodName(methodName),
                                         request,
                                         response,
                                         nullptr,
                                         nullptr,
                                         timeout);

    if (!status.ok())
        throw GrpcStatusError(methodName, status);

    return m_adapter->generateRoxalResponse(methodName, response);
}

const char* roxal::grpcStatusCodeName(grpc::StatusCode code)
{
    switch (code) {
        case grpc::StatusCode::OK: return "OK";
        case grpc::StatusCode::CANCELLED: return "CANCELLED";
        case grpc::StatusCode::UNKNOWN: return "UNKNOWN";
        case grpc::StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
        case grpc::StatusCode::DEADLINE_EXCEEDED: return "DEADLINE_EXCEEDED";
        case grpc::StatusCode::NOT_FOUND: return "NOT_FOUND";
        case grpc::StatusCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
        case grpc::StatusCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
        case grpc::StatusCode::UNAUTHENTICATED: return "UNAUTHENTICATED";
        case grpc::StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
        case grpc::StatusCode::FAILED_PRECONDITION: return "FAILED_PRECONDITION";
        case grpc::StatusCode::ABORTED: return "ABORTED";
        case grpc::StatusCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
        case grpc::StatusCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
        case grpc::StatusCode::INTERNAL: return "INTERNAL";
        case grpc::StatusCode::UNAVAILABLE: return "UNAVAILABLE";
        case grpc::StatusCode::DATA_LOSS: return "DATA_LOSS";
        case grpc::StatusCode::DO_NOT_USE: return "DO_NOT_USE";
        default: return "UNKNOWN";
    }
}

#endif // ROXAL_ENABLE_GRPC
