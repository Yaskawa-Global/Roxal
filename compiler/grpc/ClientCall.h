#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/support/slice.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/completion_queue.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

// Stream handle for managing active gRPC streams
struct StreamHandle {
    std::unique_ptr<grpc::ClientContext> ctx;
    std::unique_ptr<grpc::GenericClientAsyncReaderWriter> stream;
    std::unique_ptr<grpc::CompletionQueue> cq;

    std::atomic<bool> clientDone{false};
    std::atomic<bool> serverDone{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> started{false};

    std::thread readerThread;
    std::mutex writeMutex;  // Protects writes to the stream

    // Callbacks
    std::function<void(const std::string&)> onServerMessage;
    std::function<void(const grpc::Status&)> onStreamEnd;

    ~StreamHandle() {
        cancelled = true;
        if (readerThread.joinable()) {
            readerThread.join();
        }
    }
};

class ClientCall
{
    using OutgoingMetaData = std::multimap<std::string, std::string>;
    using IncomingMetaData = std::multimap<grpc::string_ref, grpc::string_ref>;

public:
    ClientCall();
    explicit ClientCall(std::shared_ptr<grpc::Channel> channel);
    ~ClientCall();

    // Existing unary call
    grpc::Status Call(const std::string& methodName,
                      const std::string& request,
                      std::string& response,
                      OutgoingMetaData* initialMetaData = nullptr,
                      IncomingMetaData* server_trailing_data = nullptr,
                      std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    // Start a bidirectional stream (also used for client-only or server-only streaming)
    std::shared_ptr<StreamHandle> StartStream(
        const std::string& methodName,
        std::function<void(const std::string&)> onMessage,
        std::function<void(const grpc::Status&)> onEnd,
        std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    // Start a server streaming call (sends initial request, then receives stream)
    std::shared_ptr<StreamHandle> StartServerStream(
        const std::string& methodName,
        const std::string& request,
        std::function<void(const std::string&)> onMessage,
        std::function<void(const grpc::Status&)> onEnd,
        std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    // Write a message to an active stream
    bool WriteToStream(std::shared_ptr<StreamHandle> handle, const std::string& message);

    // Signal that client is done writing (half-close)
    void CloseClientStream(std::shared_ptr<StreamHandle> handle);

    // Cancel an active stream
    void CancelStream(std::shared_ptr<StreamHandle> handle);

private:
    std::shared_ptr<grpc::Channel> m_channel;
    std::unique_ptr<grpc::GenericStub> m_stub;

    // Background reader loop for receiving server messages
    void ServerReadLoop(std::shared_ptr<StreamHandle> handle);
};

#endif // ROXAL_ENABLE_GRPC
