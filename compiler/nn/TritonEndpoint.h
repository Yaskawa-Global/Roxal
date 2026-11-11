#pragma once

// One NVIDIA Triton Inference Server, as ai.nn's Triton backend reaches it:
// the gRPC channel, and the thread on which asynchronous calls -- and the
// streams of a decoupled model's responses -- complete.
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
#include <vector>

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

    // VM shutdown, before any object is freed: every call and stream in
    // flight is cancelled and its Done (onEnd) runs (with CANCELLED) before
    // this returns, and the endpoints' threads are joined.
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

    // A call answered by a stream of messages (Triton's ModelStreamInfer, for
    // a decoupled model): one request is written, the write side closed, and
    // the server's messages read until it ends the stream.  Both callbacks
    // run on the endpoint's thread.
    struct StreamCallbacks {
        // Each message.  False: no more are wanted (the call is cancelled).
        std::function<bool(grpc::ByteBuffer& message)> onMessage;
        // Exactly once, when the call is over: OK when the server ended it,
        // CANCELLED when cancelled, DEADLINE_EXCEEDED when `gap` passed with
        // nothing heard.
        std::function<void(const grpc::Status&)> onEnd;
    };

    class Stream {
    public:
        virtual ~Stream() = default;
        // From any thread, any number of times: the call ends, onEnd seeing
        // CANCELLED (unless it already ended).
        virtual void cancel() = 0;
    };

    // Start a stream; `gap` is the longest wait for the next message, the
    // first included.  Null, and no callback ever runs, once the endpoint is
    // stopping.
    std::shared_ptr<Stream> startStream(const char* method, const grpc::ByteBuffer& request,
                                        std::chrono::microseconds gap, StreamCallbacks callbacks);

    // The channel is failing to connect: a call now would fail at once.
    bool unreachable() const;

    ~TritonEndpoint();

private:
    explicit TritonEndpoint(std::string address);
    void run();
    void stop();

    // What a completion-queue tag points at: one asynchronous operation.
    struct Op {
        virtual ~Op() = default;
        virtual void complete(bool ok) = 0;
    };
    struct Call;
    struct StreamCall;

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
    // Streams live until their last operation completes; then they move to
    // released_, freed by the thread between completions.
    std::map<StreamCall*, std::shared_ptr<StreamCall>> streams_;
    std::vector<std::shared_ptr<StreamCall>> released_;
    std::condition_variable streamsDrained_;
};

} // namespace roxal::nn
