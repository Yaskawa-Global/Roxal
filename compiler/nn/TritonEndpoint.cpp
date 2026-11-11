#include "TritonEndpoint.h"

#include "SimpleMarkSweepGC.h"
#include "VM.h"

#include <google/protobuf/message.h>
#include <grpcpp/alarm.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/channel_arguments.h>

#include <algorithm>
#include <atomic>
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

struct TritonEndpoint::Call : TritonEndpoint::Op {
    TritonEndpoint* endpoint = nullptr;
    grpc::ClientContext context;
    grpc::ByteBuffer reply;
    grpc::Status status;
    std::unique_ptr<grpc::GenericClientAsyncResponseReader> reader;
    Done done;

    void complete(bool) override {
        std::unique_ptr<Call> self;
        {
            std::lock_guard<std::mutex> lock(endpoint->mutex_);
            auto it = endpoint->inFlight_.find(this);
            if (it == endpoint->inFlight_.end())
                return;
            self = std::move(it->second);
            endpoint->inFlight_.erase(it);
        }
        done(status, reply);
    }
};

// A stream's operations each complete on the endpoint's thread: StartCall,
// then WriteLast (the one request, closing the write side) alongside a Read
// loop, then Finish -- and an alarm, re-armed from the last message heard,
// for the gap.  It is freed once nothing of it is queued.
struct TritonEndpoint::StreamCall final : TritonEndpoint::Stream {
    struct Step : Op {
        StreamCall* stream;
        void (StreamCall::*on)(bool);
        Step(StreamCall* s, void (StreamCall::*f)(bool)) : stream(s), on(f) {}
        void complete(bool ok) override {
            stream->pending.fetch_sub(1, std::memory_order_acq_rel);
            (stream->*on)(ok);
            stream->releaseIfDone();
        }
    };

    TritonEndpoint* endpoint;
    grpc::ClientContext context;
    std::unique_ptr<grpc::GenericClientAsyncReaderWriter> rw;
    grpc::ByteBuffer request;
    grpc::ByteBuffer incoming;
    grpc::Status status;
    StreamCallbacks callbacks;
    std::chrono::microseconds gap;
    std::chrono::steady_clock::time_point lastHeard;
    grpc::Alarm alarm;
    std::atomic<int> pending { 0 };
    bool finishing = false;     // Finish queued (endpoint thread only)
    bool ended = false;         // onEnd ran
    bool timedOut = false;

    Step started { this, &StreamCall::onStarted };
    Step written { this, &StreamCall::onWritten };
    Step read { this, &StreamCall::onRead };
    Step finished { this, &StreamCall::onFinished };
    Step alarmed { this, &StreamCall::onAlarm };

    StreamCall(TritonEndpoint* e, const grpc::ByteBuffer& r, std::chrono::microseconds g, StreamCallbacks c)
        : endpoint(e), request(r), callbacks(std::move(c)), gap(g) {}

    void cancel() override { context.TryCancel(); }

    void armAlarm(std::chrono::steady_clock::time_point from) {
        const auto remaining = std::max(std::chrono::steady_clock::duration::zero(),
                                        from + gap - std::chrono::steady_clock::now());
        pending.fetch_add(1, std::memory_order_acq_rel);
        alarm.Set(&endpoint->cq_, std::chrono::system_clock::now() + remaining, &alarmed);
    }

    void readNext() {
        pending.fetch_add(1, std::memory_order_acq_rel);
        rw->Read(&incoming, &read);
    }

    void finish() {
        if (finishing)
            return;
        finishing = true;
        pending.fetch_add(1, std::memory_order_acq_rel);
        rw->Finish(&status, &finished);
    }

    void onStarted(bool ok) {
        if (!ok) {
            finish();
            return;
        }
        pending.fetch_add(1, std::memory_order_acq_rel);
        rw->WriteLast(request, grpc::WriteOptions(), &written);
        readNext();
    }

    void onWritten(bool) {
        // A failed write fails the reads too: they end the stream.
    }

    void onRead(bool ok) {
        if (!ok) {
            finish();
            return;
        }
        lastHeard = std::chrono::steady_clock::now();
        bool more = false;
        try {
            more = callbacks.onMessage(incoming);
        } catch (...) {
        }
        incoming.Clear();
        if (!more)
            context.TryCancel();
        readNext();
    }

    void onFinished(bool) {
        ended = true;
        alarm.Cancel();
        if (timedOut)
            status = grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                  "no response within " + std::to_string(gap.count() / 1000) + " ms");
        try {
            callbacks.onEnd(status);
        } catch (...) {
        }
        callbacks = {};
    }

    void onAlarm(bool ok) {
        if (!ok || ended || finishing)
            return;
        if (std::chrono::steady_clock::now() - lastHeard >= gap) {
            timedOut = true;
            context.TryCancel();
        } else {
            armAlarm(lastHeard);
        }
    }

    void releaseIfDone() {
        if (!ended || pending.load(std::memory_order_acquire) != 0)
            return;
        std::lock_guard<std::mutex> lock(endpoint->mutex_);
        auto it = endpoint->streams_.find(this);
        if (it == endpoint->streams_.end())
            return;
        endpoint->released_.push_back(std::move(it->second));
        endpoint->streams_.erase(it);
        endpoint->streamsDrained_.notify_all();
    }
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
        try {
            static_cast<Op*>(tag)->complete(ok);
        } catch (...) {
            // A completion must not end the thread every later call needs.
        }
        // A stream whose last operation just completed goes here, outside
        // its own code.
        std::vector<std::shared_ptr<StreamCall>> released;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released.swap(released_);
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
        for (auto& [raw, stream] : streams_)
            stream->context.TryCancel();
    }
    // A cancelled stream still queues operations as it winds down (its last
    // Read, Finish): the queue may only shut down once every stream is over.
    {
        SimpleMarkSweepGC::GCBlockedScope blocked;
        std::unique_lock<std::mutex> lock(mutex_);
        streamsDrained_.wait(lock, [this] { return streams_.empty(); });
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
    call->endpoint = this;
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

std::shared_ptr<TritonEndpoint::Stream> TritonEndpoint::startStream(const char* method,
                                                                    const grpc::ByteBuffer& request,
                                                                    std::chrono::microseconds gap,
                                                                    StreamCallbacks callbacks)
{
    auto stream = std::make_shared<StreamCall>(this, request, gap, std::move(callbacks));
    StreamCall* raw = stream.get();

    // Held until the stream is listed, so its completions -- which may
    // arrive at once -- find it.
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_)
        return nullptr;
    raw->rw = stub_->PrepareCall(&raw->context, methodPath(method), &cq_);
    raw->lastHeard = std::chrono::steady_clock::now();
    streams_.emplace(raw, stream);
    raw->pending.fetch_add(1, std::memory_order_acq_rel);
    raw->rw->StartCall(&raw->started);
    raw->armAlarm(raw->lastHeard);
    return stream;
}

bool TritonEndpoint::unreachable() const
{
    return channel_->GetState(false) == GRPC_CHANNEL_TRANSIENT_FAILURE;
}
