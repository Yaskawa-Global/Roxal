#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include <grpcpp/channel.h>
#include <string>
#include <memory>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <vector>
#include <mutex>
#include <functional>

#include "ArgsView.h"
#include "Value.h"
#include "ClientCall.h"
#include "ProtoAdapter.h"

namespace df {
    class Signal;
}

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


// State for an active streaming RPC
struct ActiveStreamState {
    std::shared_ptr<StreamHandle> handle;
    std::string methodName;

    // Input signal tracking (for client streaming)
    std::vector<Value> inputSignalValues;  // Weak refs to input signals
    std::vector<Value> frozenArgs;         // Original call arguments (for non-signal params)
    std::vector<size_t> signalArgIndices;  // Which args are signals
    std::atomic<int> stoppedSignalCount{0};
    int totalSignalCount{0};

    // Output signal (for server streaming)
    Value outputSignal;  // The output signal value

    // Cleanup callbacks registered with signals
    std::vector<std::function<void()>> cleanupCallbacks;

    // Promise for client-streaming response (resolves when stream closes)
    std::shared_ptr<std::promise<Value>> responsePromise;
};


class ACUCommunicator : public Connector
{
public:
    ACUCommunicator(std::shared_ptr<grpc::Channel> channel, ProtoAdapter* adapter);
    ~ACUCommunicator();

    Value call(const std::string& methodName,
               ArgsView args,
               std::optional<std::chrono::milliseconds> timeout = std::nullopt) override;

    // Streaming call - detects signal arguments and sets up appropriate streaming
    // Returns: for server streaming -> output signal; for client-only streaming -> future
    Value callStreaming(const std::string& methodName,
                        ArgsView args,
                        bool clientStreaming,
                        bool serverStreaming,
                        std::optional<std::chrono::milliseconds> timeout = std::nullopt);

private:
    // Build a request message from current argument values
    std::string buildRequestFromArgs(const std::string& methodName,
                                     const std::vector<Value>& args,
                                     const std::vector<size_t>& signalIndices,
                                     const std::vector<Value>& signalValues);

    // Called when an input signal value changes
    void onInputSignalChanged(std::shared_ptr<ActiveStreamState> state);

    // Called when an input signal is stopped
    void onInputSignalStopped(std::shared_ptr<ActiveStreamState> state);

    // Active streams
    std::vector<std::shared_ptr<ActiveStreamState>> m_activeStreams;
    std::mutex m_streamsMutex;
};

} // namespace roxal

#endif // ROXAL_ENABLE_GRPC
