#ifdef ROXAL_ENABLE_GRPC

#include "ClientCall.h"
#include <stdexcept>
#include <chrono>

namespace {
void* tag(intptr_t t)
{
    return reinterpret_cast<void*>(t);
}

// Tag values for completion queue operations
constexpr intptr_t TAG_START = 1;
constexpr intptr_t TAG_READ = 2;
constexpr intptr_t TAG_WRITE = 3;
constexpr intptr_t TAG_WRITES_DONE = 4;
constexpr intptr_t TAG_FINISH = 5;
}

ClientCall::ClientCall() = default;

ClientCall::ClientCall(std::shared_ptr<grpc::Channel> channel)
    : m_channel(channel)
{
    m_stub = std::make_unique<grpc::GenericStub>(channel);
}

ClientCall::~ClientCall()
{
}


grpc::Status ClientCall::Call(const std::string& methodName,
                              const std::string& request,
                              std::string& response,
                              OutgoingMetaData* metadata,
                              IncomingMetaData* server_trailing,
                              std::optional<std::chrono::milliseconds> timeout)
{
    grpc::ClientContext ctx;
    if (metadata) {
        for (const auto& entry : *metadata)
            ctx.AddMetadata(entry.first, entry.second);
    }
    if (timeout.has_value()) {
        ctx.set_deadline(std::chrono::system_clock::now() + timeout.value());
    }

    grpc_slice raw = grpc::SliceFromCopiedString(request);
    grpc::Slice slice(raw, grpc::Slice::STEAL_REF);
    grpc::ByteBuffer reqBuffer(&slice, 1);

    grpc::ByteBuffer respBuffer;
    grpc::CompletionQueue cq;
    void* tag1 = reinterpret_cast<void*>(1);
    auto respReader = m_stub->PrepareUnaryCall(&ctx, methodName, reqBuffer, &cq);
    respReader->StartCall();
    grpc::Status status;
    respReader->Finish(&respBuffer, &status, tag1);

    void* got_tag = nullptr;
    bool ok = false;
    cq.Next(&got_tag, &ok);
    if (!ok || got_tag != tag1)
        return grpc::Status(grpc::StatusCode::UNKNOWN, "gRPC unary call failed");

    if (status.ok()) {
        std::vector<grpc::Slice> slices;
        respBuffer.Dump(&slices);
        response.clear();
        for (const auto& s : slices)
            response.append(reinterpret_cast<const char*>(s.begin()), s.size());
    }

    if (server_trailing)
        *server_trailing = ctx.GetServerTrailingMetadata();

    return status;
}

std::shared_ptr<StreamHandle> ClientCall::StartStream(
    const std::string& methodName,
    std::function<void(const std::string&)> onMessage,
    std::function<void(const grpc::Status&)> onEnd,
    std::optional<std::chrono::milliseconds> timeout)
{
    auto handle = std::make_shared<StreamHandle>();
    handle->ctx = std::make_unique<grpc::ClientContext>();
    handle->cq = std::make_unique<grpc::CompletionQueue>();
    handle->onServerMessage = std::move(onMessage);
    handle->onStreamEnd = std::move(onEnd);

    if (timeout.has_value()) {
        handle->ctx->set_deadline(std::chrono::system_clock::now() + timeout.value());
    }

    // PrepareCall returns a GenericClientAsyncReaderWriter for bidirectional streaming
    handle->stream = m_stub->PrepareCall(handle->ctx.get(), methodName, handle->cq.get());

    // Start the call
    handle->stream->StartCall(tag(TAG_START));

    void* gotTag;
    bool ok;
    if (!handle->cq->Next(&gotTag, &ok) || !ok) {
        return nullptr;
    }

    handle->started = true;

    // Start background reader thread
    handle->readerThread = std::thread(&ClientCall::ServerReadLoop, this, handle);

    return handle;
}

std::shared_ptr<StreamHandle> ClientCall::StartServerStream(
    const std::string& methodName,
    const std::string& request,
    std::function<void(const std::string&)> onMessage,
    std::function<void(const grpc::Status&)> onEnd,
    std::optional<std::chrono::milliseconds> timeout)
{
    // For server streaming, we need to do all writes BEFORE starting the reader thread
    // to avoid race conditions on the completion queue
    auto handle = std::make_shared<StreamHandle>();
    handle->ctx = std::make_unique<grpc::ClientContext>();
    handle->cq = std::make_unique<grpc::CompletionQueue>();
    handle->onServerMessage = std::move(onMessage);
    handle->onStreamEnd = std::move(onEnd);

    if (timeout.has_value()) {
        handle->ctx->set_deadline(std::chrono::system_clock::now() + timeout.value());
    }

    handle->stream = m_stub->PrepareCall(handle->ctx.get(), methodName, handle->cq.get());

    // Start the call
    handle->stream->StartCall(tag(TAG_START));

    void* gotTag;
    bool ok;
    if (!handle->cq->Next(&gotTag, &ok) || !ok) {
        return nullptr;
    }

    handle->started = true;

    // Write the request (before starting reader thread)
    {
        grpc_slice raw = grpc::SliceFromCopiedString(request);
        grpc::Slice slice(raw, grpc::Slice::STEAL_REF);
        grpc::ByteBuffer buffer(&slice, 1);

        handle->stream->Write(buffer, tag(TAG_WRITE));

        if (!handle->cq->Next(&gotTag, &ok) || !ok) {
            return nullptr;
        }
    }

    // Signal that client is done writing
    handle->clientDone = true;
    handle->stream->WritesDone(tag(TAG_WRITES_DONE));
    handle->cq->Next(&gotTag, &ok);

    // NOW start the background reader thread (after writes are complete)
    handle->readerThread = std::thread(&ClientCall::ServerReadLoop, this, handle);

    return handle;
}

void ClientCall::ServerReadLoop(std::shared_ptr<StreamHandle> handle)
{
    grpc::ByteBuffer readBuffer;

    while (!handle->cancelled && !handle->serverDone) {
        // Start a read operation
        handle->stream->Read(&readBuffer, tag(TAG_READ));

        void* gotTag;
        bool ok;

        // Wait for the read to complete
        if (!handle->cq->Next(&gotTag, &ok)) {
            // Completion queue shutdown
            handle->serverDone = true;
            break;
        }

        if (!ok) {
            // Read failed - server closed the stream or error
            handle->serverDone = true;
            break;
        }

        // Convert ByteBuffer to string
        std::vector<grpc::Slice> slices;
        readBuffer.Dump(&slices);
        std::string message;
        for (const auto& s : slices) {
            message.append(reinterpret_cast<const char*>(s.begin()), s.size());
        }

        // Invoke callback with the received message
        if (handle->onServerMessage) {
            handle->onServerMessage(message);
        }

        readBuffer.Clear();
    }

    // Get final status
    grpc::Status status;
    handle->stream->Finish(&status, tag(TAG_FINISH));

    void* gotTag;
    bool ok;
    handle->cq->Next(&gotTag, &ok);

    // Invoke end callback
    if (handle->onStreamEnd) {
        handle->onStreamEnd(status);
    }
}

bool ClientCall::WriteToStream(std::shared_ptr<StreamHandle> handle, const std::string& message)
{
    if (!handle || !handle->started || handle->clientDone || handle->cancelled) {
        return false;
    }

    std::lock_guard<std::mutex> lock(handle->writeMutex);

    grpc_slice raw = grpc::SliceFromCopiedString(message);
    grpc::Slice slice(raw, grpc::Slice::STEAL_REF);
    grpc::ByteBuffer buffer(&slice, 1);

    handle->stream->Write(buffer, tag(TAG_WRITE));

    void* gotTag;
    bool ok;
    if (!handle->cq->Next(&gotTag, &ok) || !ok) {
        return false;
    }

    return true;
}

void ClientCall::CloseClientStream(std::shared_ptr<StreamHandle> handle)
{
    if (!handle || !handle->started || handle->clientDone) {
        return;
    }

    std::lock_guard<std::mutex> lock(handle->writeMutex);

    handle->clientDone = true;
    handle->stream->WritesDone(tag(TAG_WRITES_DONE));

    void* gotTag;
    bool ok;
    handle->cq->Next(&gotTag, &ok);
}

void ClientCall::CancelStream(std::shared_ptr<StreamHandle> handle)
{
    if (!handle) {
        return;
    }

    handle->cancelled = true;
    if (handle->ctx) {
        handle->ctx->TryCancel();
    }
}

#endif // ROXAL_ENABLE_GRPC
