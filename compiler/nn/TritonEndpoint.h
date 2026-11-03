#pragma once

// One NVIDIA Triton Inference Server, as ai.nn's Triton backend reaches it:
// the gRPC channel, and the thread on which asynchronous calls complete.
// Every model on the same host:port shares one endpoint.  It knows nothing of
// Roxal values -- the session above it turns replies into tensors.

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/channel.h>
#include <grpcpp/completion_queue.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/status.h>

namespace google { namespace protobuf { class Message; } }

namespace roxal::nn {

class TritonEndpoint {
public:
    // A completed asynchronous call: the status, and the serialized reply
    // (as gRPC received it, in pieces) when it is OK.  Runs on the
    // endpoint's thread.
    using Done = std::function<void(const grpc::Status&, grpc::ByteBuffer& reply)>;

    // The endpoint for "host:port", created on first use and shared until
    // shutdownAll().
    static std::shared_ptr<TritonEndpoint> get(const std::string& address);

    // VM shutdown, before any object is freed: every call in flight is
    // cancelled and its Done runs (with CANCELLED) before this returns, and
    // the endpoints' threads are joined.
    static void shutdownAll();

    const std::string& address() const { return address_; }

    // A blocking unary call -- `method` is a GRPCInferenceService method
    // name, e.g. "ModelMetadata".  The wait is GC-blocked: collections go
    // on without the calling thread.
    grpc::Status call(const char* method, const google::protobuf::Message& request,
                      google::protobuf::Message& reply, std::chrono::milliseconds deadline);

    // The same, for a request already encoded (e.g. a model upload whose file
    // bytes are slices over mappings, see TritonDeploy).
    grpc::Status call(const char* method, const grpc::ByteBuffer& request,
                      google::protobuf::Message& reply, std::chrono::milliseconds deadline);

    // An asynchronous unary call on an already serialized request; `done`
    // runs on the endpoint's thread.  False, and `done` never runs, once the
    // endpoint is stopping.
    bool callAsync(const char* method, const grpc::ByteBuffer& request,
                   std::chrono::microseconds deadline, Done done);

    // The channel is failing to connect: a call now would fail at once.
    bool unreachable() const;

    ~TritonEndpoint();

private:
    explicit TritonEndpoint(std::string address);
    void run();
    void stop();

    struct Call;

    std::string address_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<grpc::GenericStub> stub_;
    grpc::CompletionQueue cq_;
    std::thread thread_;

    std::mutex mutex_;
    std::condition_variable started_;
    bool ready_ = false;            // the thread made the channel
    bool stopping_ = false;
    std::map<Call*, std::unique_ptr<Call>> inFlight_;
};

} // namespace roxal::nn
