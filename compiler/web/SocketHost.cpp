#if defined(ROXAL_ENABLE_WEB) && !defined(__EMSCRIPTEN__)

#include "SocketHost.h"
#include "ScriptInbox.h"
#include "WebHostLoop.h"

#include "ModuleDebug.h"
#include "RuntimeConfig.h"
#include "SimpleMarkSweepGC.h"
#include "VM.h"
#include "debug/BreakpointManager.h"
#include "debug/StopCoordinator.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace roxal;
using namespace roxal::web;

namespace {

// ------------------------------------------------------------------ SHA-1
// RFC 3174, needed for exactly one thing: the WebSocket handshake reply.
struct Sha1 {
    static std::array<uint8_t, 20> digest(const std::string& msg)
    {
        uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
        std::vector<uint8_t> data(msg.begin(), msg.end());
        const uint64_t bitLen = static_cast<uint64_t>(data.size()) * 8;
        data.push_back(0x80);
        while (data.size() % 64 != 56) data.push_back(0);
        for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>(bitLen >> (i * 8)));

        auto rol = [](uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
        for (size_t chunk = 0; chunk < data.size(); chunk += 64) {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = (uint32_t(data[chunk + i * 4]) << 24) | (uint32_t(data[chunk + i * 4 + 1]) << 16)
                     | (uint32_t(data[chunk + i * 4 + 2]) << 8) | uint32_t(data[chunk + i * 4 + 3]);
            for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
            uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
            for (int i = 0; i < 80; ++i) {
                uint32_t f, k;
                if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
                else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
                else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
                const uint32_t t = rol(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rol(b, 30); b = a; a = t;
            }
            h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
        }
        std::array<uint8_t, 20> out;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
        return out;
    }
};

std::string base64(const uint8_t* data, size_t len)
{
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        const uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += tbl[(n >> 6) & 63];  out += tbl[n & 63];
    }
    if (i < len) {
        uint32_t n = uint32_t(data[i]) << 16;
        if (i + 1 < len) n |= uint32_t(data[i + 1]) << 8;
        out += tbl[(n >> 18) & 63]; out += tbl[(n >> 12) & 63];
        out += (i + 1 < len) ? tbl[(n >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

// ------------------------------------------------------------------ socket I/O

bool sendAll(int fd, const uint8_t* data, size_t len)
{
    while (len > 0) {
        const ssize_t n = ::send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

// Bytes read past what the handshake consumed belong to the first frame.
struct Conn {
    int fd;
    std::vector<uint8_t> pending;
    size_t pendingPos = 0;

    bool readExact(uint8_t* out, size_t len)
    {
        while (len > 0) {
            if (pendingPos < pending.size()) {
                const size_t take = std::min(len, pending.size() - pendingPos);
                std::memcpy(out, pending.data() + pendingPos, take);
                pendingPos += take; out += take; len -= take;
                continue;
            }
            const ssize_t n = ::recv(fd, out, len, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            out += n;
            len -= static_cast<size_t>(n);
        }
        return true;
    }
};

// One WebSocket frame, server -> client: never masked.
std::vector<uint8_t> frame(uint8_t opcode, const uint8_t* payload, size_t len)
{
    std::vector<uint8_t> out;
    out.reserve(len + 10);
    out.push_back(static_cast<uint8_t>(0x80 | opcode));
    if (len < 126) {
        out.push_back(static_cast<uint8_t>(len));
    } else if (len < 65536) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(len));
    } else {
        out.push_back(127);
        for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(uint64_t(len) >> (i * 8)));
    }
    out.insert(out.end(), payload, payload + len);
    return out;
}

// A field reader for control messages. Deliberately NOT the bridge Decoder:
// that one builds Roxal Values, and this runs on the socket reader thread,
// which is no Roxal thread and must allocate nothing the GC would need to see.
struct FieldReader {
    const uint8_t* p;
    const uint8_t* end;

    uint8_t u8()
    {
        if (p >= end) throw std::runtime_error("truncated control message");
        return *p++;
    }
    uint32_t u32()
    {
        const uint32_t a = u8(), b = u8(), c = u8(), d = u8();
        return a | (b << 8) | (c << 16) | (d << 24);
    }
    std::string str()
    {
        if (static_cast<Tag>(u8()) != Tag::Str)
            throw std::runtime_error("expected a string field in control message");
        const uint32_t len = u32();
        if (static_cast<size_t>(end - p) < len) throw std::runtime_error("truncated string field");
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        return s;
    }
};

constexpr uint8_t kMsgOps = 0;
constexpr uint8_t kMsgControl = 1;
constexpr size_t kMaxFrame = 256u * 1024u * 1024u;   // a bug, not a workload

} // namespace

// ------------------------------------------------------------------ lifecycle

SocketHost::SocketHost(Options options) : options_(std::move(options)) {}

SocketHost::~SocketHost()
{
    shutdown();
}

std::uint16_t SocketHost::listen()
{
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
        throw std::runtime_error("web host: socket(): " + std::string(std::strerror(errno)));
    int enable = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(options_.port);
    if (::inet_pton(AF_INET, options_.bindAddress.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("web host: bad bind address '" + options_.bindAddress + "'");
    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        const std::string why = std::strerror(errno);
        ::close(listenFd_); listenFd_ = -1;
        throw std::runtime_error("web host: bind(" + options_.bindAddress + ":"
                                 + std::to_string(options_.port) + "): " + why);
    }
    if (::listen(listenFd_, 4) < 0) {
        const std::string why = std::strerror(errno);
        ::close(listenFd_); listenFd_ = -1;
        throw std::runtime_error("web host: listen(): " + why);
    }
    sockaddr_in bound {};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0)
        options_.port = ntohs(bound.sin_port);
    return options_.port;
}

void SocketHost::start(ScriptInbox& inbox)
{
    if (listenFd_ < 0)
        throw std::runtime_error("web host: start() before listen()");
    inbox_ = &inbox;
    previousSink_ = OutputRouter::sink();
    OutputRouter::setSink(this);
    setTransport(this);
    stopping_.store(false, std::memory_order_release);
    writerThread_ = std::thread([this] { writerLoop(); });
    acceptThread_ = std::thread([this] { acceptLoop(); });
}

void SocketHost::shutdown()
{
    if (stopping_.exchange(true, std::memory_order_acq_rel))
        return;
    if (transport() == this) setTransport(nullptr);
    if (OutputRouter::sink() == this) OutputRouter::setSink(previousSink_);
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    closeClient();
    outCv_.notify_all();
    if (acceptThread_.joinable()) acceptThread_.join();
    if (readerThread_.joinable()) readerThread_.join();
    if (writerThread_.joinable()) writerThread_.join();
}

void SocketHost::closeClient()
{
    const int fd = clientFd_.exchange(-1, std::memory_order_acq_rel);
    if (fd < 0) return;
    // A close frame first, best effort, so a well-behaved client sees a clean
    // close rather than a reset -- but never by WAITING for the writer: it
    // may be blocked in sendAll() on a client that has stopped reading,
    // holding writeMutex_, and only the shutdown() below unblocks it.  A
    // client like that is not reading a close frame anyway.
    if (writeMutex_.try_lock()) {
        const std::vector<uint8_t> close = frame(0x8, nullptr, 0);
        sendAll(fd, close.data(), close.size());
        writeMutex_.unlock();
    }
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
}

// ------------------------------------------------------------------ accept

void SocketHost::acceptLoop()
{
    for (;;) {
        sockaddr_in peer {};
        socklen_t peerLen = sizeof(peer);
        const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;   // listening socket closed: shutting down
        }
        if (stopping_.load(std::memory_order_acquire)) { ::close(fd); return; }
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        serveClient(fd);
    }
}

void SocketHost::serveClient(int fd)
{
    if (!handshake(fd)) { ::close(fd); return; }

    // The new client displaces the old one: close it and wait for its reader
    // to notice before the new reader starts, so exactly one reader lives.
    closeClient();
    if (readerThread_.joinable()) readerThread_.join();

    // Drop anything queued for the previous client; it starts from hello.
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        outbound_.clear();
        outboundBytes_ = 0;
    }
    clientFd_.store(fd, std::memory_order_release);
    if (options_.verbose)
        std::cerr << "web host: client connected" << std::endl;
    sendHello();
    // Stores exposed before this client arrived are unknown to it: ask the
    // VM thread to redefine them on its next host-loop turn.
    resync_.store(true, std::memory_order_release);
    readerThread_ = std::thread([this, fd] { readerLoop(fd); });
}

bool SocketHost::handshake(int fd)
{
    // Read the HTTP request head. Anything after the blank line is frame data.
    std::string head;
    std::vector<uint8_t> buf(4096);
    size_t headEnd = std::string::npos;
    std::vector<uint8_t> leftover;
    while (headEnd == std::string::npos) {
        const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
        if (n <= 0) return false;
        head.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
        headEnd = head.find("\r\n\r\n");
        if (head.size() > 16384) return false;
    }
    leftover.assign(head.begin() + static_cast<long>(headEnd + 4), head.end());
    head.resize(headEnd);

    // Case-insensitive header lookup.
    auto header = [&](const char* name) -> std::string {
        std::string lower = head;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        std::string key = name;
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        key += ":";
        const size_t at = lower.find("\r\n" + key);
        if (at == std::string::npos) return {};
        const size_t start = at + 2 + key.size();
        const size_t eol = head.find("\r\n", start);
        std::string v = head.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
        return v;
    };

    const std::string key = header("Sec-WebSocket-Key");
    std::string upgrade = header("Upgrade");
    std::transform(upgrade.begin(), upgrade.end(), upgrade.begin(), ::tolower);
    if (key.empty() || upgrade != "websocket") {
        // A plain HTTP probe (a browser tab, curl): answer, so the port is
        // recognisably ours, then close.
        const std::string body = "roxal web host: connect with a WebSocket client\n";
        const std::string resp = "HTTP/1.1 426 Upgrade Required\r\nContent-Type: text/plain\r\n"
                                 "Content-Length: " + std::to_string(body.size()) + "\r\n"
                                 "Connection: close\r\n\r\n" + body;
        sendAll(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size());
        return false;
    }
    const auto sha = Sha1::digest(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    const std::string resp = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                             "Connection: Upgrade\r\nSec-WebSocket-Accept: "
                             + base64(sha.data(), sha.size()) + "\r\n\r\n";
    if (!sendAll(fd, reinterpret_cast<const uint8_t*>(resp.data()), resp.size()))
        return false;
    // Hand the leftover to the reader through a tiny stash keyed by fd.
    if (!leftover.empty()) {
        std::lock_guard<std::mutex> lock(outMutex_);
        pendingAfterHandshake_ = std::move(leftover);
    }
    return true;
}

// ------------------------------------------------------------------ reader

void SocketHost::readerLoop(int fd)
{
    Conn conn { fd, {}, 0 };
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        conn.pending = std::move(pendingAfterHandshake_);
        pendingAfterHandshake_.clear();
    }
    std::vector<uint8_t> message;
    uint8_t messageOpcode = 0;
    for (;;) {
        uint8_t hdr[2];
        if (!conn.readExact(hdr, 2)) break;
        const bool fin = (hdr[0] & 0x80) != 0;
        const uint8_t opcode = hdr[0] & 0x0f;
        const bool masked = (hdr[1] & 0x80) != 0;
        uint64_t len = hdr[1] & 0x7f;
        if (len == 126) {
            uint8_t ext[2];
            if (!conn.readExact(ext, 2)) break;
            len = (uint64_t(ext[0]) << 8) | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!conn.readExact(ext, 8)) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | ext[i];
        }
        if (!masked || len > kMaxFrame) break;    // protocol violation: drop the client
        uint8_t mask[4];
        if (!conn.readExact(mask, 4)) break;
        std::vector<uint8_t> payload(static_cast<size_t>(len));
        if (len > 0 && !conn.readExact(payload.data(), payload.size())) break;
        for (size_t i = 0; i < payload.size(); ++i) payload[i] ^= mask[i & 3];

        if (opcode == 0x8) break;                                   // close
        if (opcode == 0x9) {                                        // ping -> pong
            std::lock_guard<std::mutex> lock(writeMutex_);
            const std::vector<uint8_t> pong = frame(0xA, payload.data(), payload.size());
            if (!sendAll(fd, pong.data(), pong.size())) break;
            continue;
        }
        if (opcode == 0xA) continue;                                // pong
        if (opcode == 0x1 || opcode == 0x2) {
            messageOpcode = opcode;
            message = std::move(payload);
        } else if (opcode == 0x0) {
            message.insert(message.end(), payload.begin(), payload.end());
        } else {
            break;
        }
        if (!fin) continue;
        if (messageOpcode == 0x2 && !message.empty()) {
            try {
                const uint8_t kind = message[0];
                if (kind == kMsgOps)          handleInbound(message.data() + 1, message.size() - 1);
                else if (kind == kMsgControl) handleControl(message.data() + 1, message.size() - 1);
            } catch (const std::exception& e) {
                std::cerr << "web host: bad message from client: " << e.what() << std::endl;
            }
        }
        message.clear();
    }
    // Only tear down if this is still the current client; a displaced reader
    // must not close its successor.
    int expected = fd;
    if (clientFd_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        if (options_.verbose)
            std::cerr << "web host: client disconnected" << std::endl;
    }
}

void SocketHost::handleInbound(const uint8_t* data, size_t len)
{
    FieldReader r { data, data + len };
    const Inbound kind = static_cast<Inbound>(r.u8());
    const uint32_t id = r.u32();
    const std::string name = r.str();
    const std::string member = r.str();
    queueInboundFromMainThread(kind, id, name.c_str(), member.c_str(),
                               r.p, static_cast<uint32_t>(r.end - r.p));
}

void SocketHost::handleControl(const uint8_t* data, size_t len)
{
    FieldReader r { data, data + len };
    const std::string type = r.str();
    if (type == "submit") {
        const std::string name = r.str();
        const std::string source = r.str();
        if (inbox_) inbox_->submit(source, name);
    } else if (type == "stop") {
        requestStop();
    } else if (type == "interrupt") {
        VM::instance().requestExit(0);
    } else if (type == "quit") {
        if (inbox_) inbox_->requestQuit();
        requestStop();
        VM::instance().requestExit(0);
    } else if (type == "config") {
        const std::string key = r.str();
        const std::string value = r.str();
        if (key == "gc.disabled") {
            SimpleMarkSweepGC::instance().setEnabled(value != "true");
        } else if (key == "gc.threshold") {
            SimpleMarkSweepGC::instance().setAutoTriggerThreshold(
                std::strtoull(value.c_str(), nullptr, 10));
        } else if (key.rfind("env.", 0) == 0) {
            ::setenv(key.c_str() + 4, value.c_str(), 1);
        } else {
            RuntimeConfig::set(key, value);
        }
    } else if (type == "debug_session") {
        ModuleDebug::setSessionPending(r.u8() != 0);
    } else if (type == "debug_arm") {
        const bool stopOnFatal = r.u8() != 0;
        ScopedGCMutatorCover cover;
        auto& coord = VM::instance().stopCoordinator();
        coord.ensureWorker();
        coord.setStopOnFatal(stopOnFatal);
    } else if (type == "breakpoints") {
        const std::string file = r.str();
        const std::string csv = r.str();
        std::vector<int> lines;
        const char* p = csv.c_str();
        while (*p) {
            int v = 0; bool any = false;
            while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; any = true; }
            if (any) lines.push_back(v);
            if (*p) ++p;
        }
        ScopedGCMutatorCover cover;
        BreakpointManager::instance().setBreakpoints(file, lines);
    } else {
        std::cerr << "web host: unknown control message '" << type << "'" << std::endl;
    }
}

// ------------------------------------------------------------------ writer

// The outbound queue is bounded.  Store coalescing happens BEFORE this
// queue, so frames already queued behind a client that has stopped reading
// (a stalled tab, a slow link) would pile up without limit under a sustained
// feed.  Past the cap, the queued store batches are dropped and the client
// is brought back to a consistent state the way a reconnecting one is: a
// full redefinition of every store on the VM thread's next host-loop turn
// (resync_).  Control messages (hello, ended, output) are never dropped --
// they carry events, not state.
constexpr size_t kOutboundCapBytes = size_t(64) << 20;

void SocketHost::enqueue(std::vector<uint8_t> message)
{
    if (!clientConnected()) return;   // nobody to tell; the next client resyncs
    bool overflowed = false;
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        outboundBytes_ += message.size();
        if (outboundBytes_ > kOutboundCapBytes) {
            size_t kept = 0;
            std::deque<Outbound> control;
            for (auto& item : outbound_) {
                if (!item.bytes.empty() && item.bytes[0] != kMsgOps) {
                    kept += item.bytes.size();
                    control.push_back(std::move(item));
                }
            }
            outbound_.swap(control);
            outboundBytes_ = kept + message.size();
            overflowed = true;
        }
        outbound_.push_back(Outbound { std::move(message) });
    }
    if (overflowed) {
        resync_.store(true, std::memory_order_release);
        if (options_.verbose)
            std::cerr << "web host: outbound queue overflowed (client not reading); "
                         "dropped queued store batches, resyncing" << std::endl;
    }
    outCv_.notify_one();
}

void SocketHost::writerLoop()
{
    for (;;) {
        Outbound item;
        {
            std::unique_lock<std::mutex> lock(outMutex_);
            outCv_.wait(lock, [this] {
                return !outbound_.empty() || stopping_.load(std::memory_order_acquire);
            });
            if (outbound_.empty()) return;   // stopping
            item = std::move(outbound_.front());
            outbound_.pop_front();
            outboundBytes_ -= std::min(outboundBytes_, item.bytes.size());
        }
        const int fd = clientFd_.load(std::memory_order_acquire);
        if (fd < 0) continue;              // client went away; drop
        const std::vector<uint8_t> f = frame(0x2, item.bytes.data(), item.bytes.size());
        std::lock_guard<std::mutex> lock(writeMutex_);
        if (!sendAll(fd, f.data(), f.size())) {
            int expected = fd;
            if (clientFd_.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
            }
        }
    }
}

// ------------------------------------------------------------------ transport

void SocketHost::send(const std::vector<uint8_t>& batch)
{
    std::vector<uint8_t> msg;
    msg.reserve(batch.size() + 1);
    msg.push_back(kMsgOps);
    msg.insert(msg.end(), batch.begin(), batch.end());
    enqueue(std::move(msg));
}

std::vector<uint8_t> SocketHost::roundTrip(const std::vector<uint8_t>& batch)
{
    (void)batch;
    throw std::runtime_error(
        "web: this host has no JavaScript surface to call into "
        "(the dom module needs the wasm host)");
}

// ------------------------------------------------------------------ control out

void SocketHost::sendHello()
{
    Encoder e;
    e.u8(kMsgControl);
    e.str(std::string("hello"));
    e.str(std::string(ROXAL_VERSION));
    std::string features;
    for (const auto& f : VM::featureStrings()) {
        if (!features.empty()) features += ",";
        features += f;
    }
    e.str(features);
    e.str(options_.root);
    e.str(options_.stdlibDir);
    e.u32(static_cast<uint32_t>(inbox_ ? inbox_->completedCount() : 0));
    e.u32(static_cast<uint32_t>(inbox_ ? inbox_->lastResult() : 0));
    e.u32(inbox_ && inbox_->running() ? 1 : 0);
    enqueue(e.bytes());
}

void SocketHost::sendEnded(int rc, int completed)
{
    Encoder e;
    e.u8(kMsgControl);
    e.str(std::string("ended"));
    e.u32(static_cast<uint32_t>(rc));
    e.u32(static_cast<uint32_t>(completed));
    enqueue(e.bytes());
}

OutputResult SocketHost::emit(const OutputEventView& event)
{
    // Tee: the console still sees everything (a host run from a terminal
    // should read like the CLI), and the client gets the same text.
    if (previousSink_) previousSink_->emit(event);
    if (clientConnected()) {
        std::string text(event.text);
        if (event.kind != OutputKind::Print) text += '\n';
        Encoder e;
        e.u8(kMsgControl);
        e.str(std::string("output"));
        e.u8(event.channel == "stderr" ? 1 : 0);
        e.str(text);
        enqueue(e.bytes());
    }
    return OutputResult::Accepted;
}

#endif // ROXAL_ENABLE_WEB && !__EMSCRIPTEN__
