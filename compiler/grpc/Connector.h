#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include <grpcpp/channel.h>
#include <string>
#include <memory>
#include <chrono>
#include <optional>

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
