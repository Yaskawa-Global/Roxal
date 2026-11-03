#include "TritonEndpoint.h"

#include "SimpleMarkSweepGC.h"
#include "VM.h"

#include <google/protobuf/message.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/channel_arguments.h>

#include <climits>
#include <vector>

using namespace roxal;
using namespace roxal::nn;

namespace {

std::mutex registryMutex;

std::map<std::string, std::shared_ptr<TritonEndpoint>>& registry()
{
    static std::map<std::string, std::shared_ptr<TritonEndpoint>> endpoints;
    return endpoints;
}

std::string methodPath(const char* method)
{
    return std::string("/inference.GRPCInferenceService/") + method;
}

grpc::ByteBuffer bufferOf(const std::string& bytes)
{
    grpc::Slice slice(bytes);
    return grpc::ByteBuffer(&slice, 1);
}

std::string stringOf(grpc::ByteBuffer& buffer)
{
    std::vector<grpc::Slice> slices;
    buffer.Dump(&slices);
    std::string bytes;
    bytes.reserve(buffer.Length());
    for (const auto& slice : slices)
        bytes.append(reinterpret_cast<const char*>(slice.begin()), slice.size());
    return bytes;
}

} // namespace

struct TritonEndpoint::Call {
    grpc::ClientContext context;
    grpc::ByteBuffer reply;
    grpc::Status status;
    std::unique_ptr<grpc::GenericClientAsyncResponseReader> reader;
    Done done;
};

std::shared_ptr<TritonEndpoint> TritonEndpoint::get(const std::string& address)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    auto& endpoint = registry()[address];
    if (!endpoint)
        endpoint.reset(new TritonEndpoint(address));
    return endpoint;
}

void TritonEndpoint::shutdownAll()
{
    std::map<std::string, std::shared_ptr<TritonEndpoint>> endpoints;
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        endpoints.swap(registry());
    }
    for (auto& [address, endpoint] : endpoints)
        endpoint->stop();
}

TritonEndpoint::TritonEndpoint(std::string address)
    : address_(std::move(address))
{
    // gRPC starts its own threads on first use, and they inherit the
    // scheduling of the thread that makes the channel: it is made on this
    // endpoint's demoted thread, never on an embedding host's real-time one.
    thread_ = std::thread([this] {
        VM::demoteCurrentThreadToNonRT();
        run();
    });
    SimpleMarkSweepGC::GCBlockedScope blocked;
    std::unique_lock<std::mutex> lock(mutex_);
    started_.wait(lock, [this] { return ready_; });
}

TritonEndpoint::~TritonEndpoint()
{
    stop();
}

void TritonEndpoint::run()
{
    {
        grpc::ChannelArguments args;
        // Tensors routinely exceed gRPC's 4 MB default message size.
        args.SetMaxReceiveMessageSize(INT_MAX);
        args.SetMaxSendMessageSize(INT_MAX);
        auto channel = grpc::CreateCustomChannel(address_, grpc::InsecureChannelCredentials(), args);
        std::lock_guard<std::mutex> lock(mutex_);
        channel_ = std::move(channel);
        stub_ = std::make_unique<grpc::GenericStub>(channel_);
        ready_ = true;
    }
    started_.notify_all();

    void* tag = nullptr;
    bool ok = false;
    while (cq_.Next(&tag, &ok)) {
        std::unique_ptr<Call> call;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = inFlight_.find(static_cast<Call*>(tag));
            if (it == inFlight_.end())
                continue;
            call = std::move(it->second);
            inFlight_.erase(it);
        }
        try {
            call->done(call->status, call->reply);
        } catch (...) {
            // A completion must not end the thread every later call needs.
        }
    }
}

void TritonEndpoint::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_)
            return;
        stopping_ = true;
        for (auto& [raw, call] : inFlight_)
            call->context.TryCancel();
    }
    // Every call started has its completion queued (cancelled or not):
    // the thread runs them all, then Next() reports the queue drained.
    cq_.Shutdown();
    if (thread_.joinable())
        thread_.join();
}

grpc::Status TritonEndpoint::call(const char* method, const google::protobuf::Message& request,
                                  google::protobuf::Message& reply,
                                  std::chrono::milliseconds deadline)
{
    std::string bytes;
    if (!request.SerializeToString(&bytes))
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("cannot encode the ") + method + " request");
    return call(method, bufferOf(bytes), reply, deadline);
}

grpc::Status TritonEndpoint::call(const char* method, const grpc::ByteBuffer& request,
                                  google::protobuf::Message& reply,
                                  std::chrono::milliseconds deadline)
{
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + deadline);
    grpc::ByteBuffer replyBuffer;
    grpc::Status status;
    {
        // Waiting on the network, touching nothing GC-managed.
        SimpleMarkSweepGC::GCBlockedScope blocked;
        grpc::CompletionQueue cq;
        auto reader = stub_->PrepareUnaryCall(&context, methodPath(method), request, &cq);
        reader->StartCall();
        reader->Finish(&replyBuffer, &status, this);
        void* tag = nullptr;
        bool ok = false;
        cq.Next(&tag, &ok);
        cq.Shutdown();
        while (cq.Next(&tag, &ok)) {}
    }
    if (status.ok() && !reply.ParseFromString(stringOf(replyBuffer)))
        return grpc::Status(grpc::StatusCode::INTERNAL, std::string("malformed ") + method + " reply");
    return status;
}

bool TritonEndpoint::callAsync(const char* method, const grpc::ByteBuffer& request,
                               std::chrono::microseconds deadline, Done done)
{
    auto call = std::make_unique<Call>();
    call->context.set_deadline(std::chrono::system_clock::now() + deadline);
    call->done = std::move(done);

    // Held until the call is listed, so its completion -- which may arrive
    // at once -- finds it.
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_)
        return false;
    Call* raw = call.get();
    raw->reader = stub_->PrepareUnaryCall(&raw->context, methodPath(method), request, &cq_);
    raw->reader->StartCall();
    raw->reader->Finish(&raw->reply, &raw->status, raw);
    inFlight_.emplace(raw, std::move(call));
    return true;
}

bool TritonEndpoint::unreachable() const
{
    return channel_->GetState(false) == GRPC_CHANNEL_TRANSIENT_FAILURE;
}
