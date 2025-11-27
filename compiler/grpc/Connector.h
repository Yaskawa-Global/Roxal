#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include <grpcpp/channel.h>
#include <string>
#include <memory>
#include <chrono>
#include <optional>
#include <stdexcept>

#include "ArgsView.h"
#include "Value.h"
#include "ClientCall.h"
#include "ProtoAdapter.h"

namespace roxal {

class Connector {
public:
    virtual ~Connector() = default;
    virtual Value call(const std::string& methodName,
                       ArgsView args,
                       std::optional<std::chrono::milliseconds> timeout = std::nullopt) = 0;

protected:
    std::unique_ptr<ClientCall> m_caller;
    ProtoAdapter* m_adapter { nullptr };
};

class GrpcStatusError : public std::runtime_error {
public:
    GrpcStatusError(const std::string& method,
                    const grpc::Status& status);
    grpc::StatusCode code() const noexcept { return statusCode; }
    const std::string& method() const noexcept { return methodName; }
    const std::string& grpcMessage() const noexcept { return statusMessage; }
private:
    grpc::StatusCode statusCode;
    std::string methodName;
    std::string statusMessage;
};

const char* grpcStatusCodeName(grpc::StatusCode code);


class ACUCommunicator : public Connector
{
public:
    ACUCommunicator(std::shared_ptr<grpc::Channel> channel, ProtoAdapter* adapter);
    Value call(const std::string& methodName,
               ArgsView args,
               std::optional<std::chrono::milliseconds> timeout = std::nullopt) override;
};

} // namespace roxal

#endif // ROXAL_ENABLE_GRPC
