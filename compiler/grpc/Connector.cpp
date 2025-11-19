#ifdef ROXAL_ENABLE_GRPC

#include "Connector.h"

#include <stdexcept>

using namespace roxal;

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
        throw std::runtime_error("gRPC call '" + methodName + "' failed: " + status.error_message());

    return m_adapter->generateRoxalResponse(methodName, response);
}

#endif // ROXAL_ENABLE_GRPC
