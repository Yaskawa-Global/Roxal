#pragma once

#if defined(ROXAL_ENABLE_WEB) && !defined(__EMSCRIPTEN__)

// The native web host: a WebSocket server on localhost that carries the
// state-bridge protocol (compiler/web/JsBridge.h) between a native VM and a
// browser page, plus a small control channel for what the wasm build exposes
// as C entry points (submit a script, stop it, read its result).
//
// A WebSocket is a plain TCP socket with an HTTP handshake in front and a
// 2-14 byte header per message. That is the whole reason for the SHA-1 and
// base64 below: the handshake reply is base64(sha1(key + GUID)). No library.
//
// Threads:
//   accept thread  -- waits for connections; a new client displaces the old
//                     one (a page reload must not need a host restart)
//   reader thread  -- decodes frames; queues inbound work (JsBridge inbound
//                     queue) or handles control messages. Never touches VM
//                     state directly.
//   writer thread  -- drains the outbound queue to the socket, so a slow
//                     client never blocks the VM thread.
// The VM thread only ever enqueues (Transport::send) -- never blocks here.
//
// Message framing inside WebSocket binary frames:
//   host -> client   [u8 0][ops batch]                 the JS half runs the ops
//                    [u8 1][Str type][fields...]       control (hello, ended, output)
//   client -> host   [u8 0][u8 inKind][u32 id][Str name][Str member][args]
//                    [u8 1][Str type][fields...]       control (submit, stop, ...)
// Str is Tag::Str + u32 length + UTF-8, exactly as Encoder::str writes it.
// The JS side of this is web/src/lib/socket-host.js.

#include "JsBridge.h"
#include "core/Output.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace roxal {
namespace web {

class ScriptInbox;

class SocketHost final : public Transport, public OutputSink {
public:
    struct Options {
        std::string bindAddress = "127.0.0.1";
        std::uint16_t port = 8765;         // 0 = ephemeral; see listen()
        std::string root;                  // the user's files (web.data_dir)
        std::string stdlibDir;             // the module directory (web.stdlib_dir)
        bool verbose = false;
    };

    explicit SocketHost(Options options);
    ~SocketHost() override;

    SocketHost(const SocketHost&) = delete;
    SocketHost& operator=(const SocketHost&) = delete;

    // Bind and listen. Returns the port actually bound (meaningful for 0).
    std::uint16_t listen();

    // Start the accept thread, routing control messages to `inbox`. Also
    // installs this host as the bridge transport and as the output sink
    // (teeing to whatever sink was installed before, normally the console).
    void start(ScriptInbox& inbox);

    // Close the client, stop the threads, restore the previous output sink
    // and detach the transport. Idempotent.
    void shutdown();

    bool clientConnected() const { return clientFd_.load(std::memory_order_acquire) >= 0; }

    // --- Transport ---
    void send(const std::vector<uint8_t>& batch) override;
    std::vector<uint8_t> roundTrip(const std::vector<uint8_t>& batch) override;
    bool canIssueOps() override { return true; }
    bool takeResync() override { return resync_.exchange(false, std::memory_order_acq_rel); }

    // --- OutputSink ---
    OutputResult emit(const OutputEventView& event) override;

    // Host -> client control messages.
    void sendEnded(int rc, int completed);

private:
    struct Outbound {
        std::vector<uint8_t> bytes;
    };

    void acceptLoop();
    void serveClient(int fd);
    bool handshake(int fd);
    void readerLoop(int fd);
    void writerLoop();
    void closeClient();

    void enqueue(std::vector<uint8_t> message);
    void sendHello();
    void handleControl(const uint8_t* data, size_t len);
    void handleInbound(const uint8_t* data, size_t len);

    Options options_;
    ScriptInbox* inbox_ = nullptr;
    OutputSink* previousSink_ = nullptr;

    int listenFd_ = -1;
    std::atomic<int> clientFd_ { -1 };
    std::atomic<bool> stopping_ { false };
    std::atomic<bool> resync_ { false };

    std::thread acceptThread_;
    std::thread readerThread_;
    std::thread writerThread_;

    std::mutex outMutex_;
    std::condition_variable outCv_;
    std::deque<Outbound> outbound_;
    size_t outboundBytes_ { 0 };       // bytes queued (guarded by outMutex_); see enqueue()
    std::vector<uint8_t> pendingAfterHandshake_;   // frame bytes read with the HTTP head
    std::mutex writeMutex_;            // one frame at a time on the wire
};

} // namespace web
} // namespace roxal

#endif // ROXAL_ENABLE_WEB && !__EMSCRIPTEN__
