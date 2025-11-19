#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/slice.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/completion_queue.h>

#include <map>
#include <memory>
#include <string>

class ClientCall
{
    using OutgoingMetaData = std::multimap<std::string, std::string>;
    using IncomingMetaData = std::multimap<grpc::string_ref, grpc::string_ref>;

public:
    ClientCall();
    explicit ClientCall(std::shared_ptr<grpc::Channel> channel);
    ~ClientCall();

    grpc::Status Call(const std::string& methodName,
                      const std::string& request,
                      std::string& response,
                      OutgoingMetaData* initialMetaData = nullptr,
                      IncomingMetaData* server_trailing_data = nullptr,
                      std::optional<std::chrono::milliseconds> timeout = std::nullopt);

private:
    std::unique_ptr<grpc::GenericStub> m_stub;
};

#endif // ROXAL_ENABLE_GRPC
