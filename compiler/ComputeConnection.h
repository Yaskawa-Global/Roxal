#pragma once

#ifdef ROXAL_COMPUTE_SERVER

#include <core/common.h>
#include <compiler/Value.h>
#include <compiler/Object.h>
#include <compiler/CallableInfo.h>
#include "ComputeProtocol.h"

#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace roxal {

constexpr std::uint8_t ComputeActorRefMarker = 0xFE;

// Forward declarations
class ComputeConnection;

// -----------------------------------------------------------------------
// NetworkSerializationContext
//
// Extends SerializationContext for cross-process Value serialization.
// Actor Values need special treatment:
//   - Remote actors (isRemote=true) are written as remote-ref markers
//   - Local actors are registered with the connection and written as
//     foreign-ref markers so the receiving side can route calls back
//
// NOTE: The isRemote field on ActorInstance is added in Phase 4.
//       This context is prepared now; actor interception is activated then.
// -----------------------------------------------------------------------
struct NetworkSerializationContext : public SerializationContext {
    ComputeConnection* conn { nullptr };

    // Foreign actor IDs assigned to local actors sent over this connection.
    std::unordered_map<const Obj*, int64_t> localActorToForeignId;
    std::unordered_map<int64_t, const Obj*> foreignIdToLocalActor;

    explicit NetworkSerializationContext(ComputeConnection* c) : conn(c) {}
};


// -----------------------------------------------------------------------
// ComputeConnection
//
// Manages one bidirectional TCP connection for the compute protocol.
// Used on both the client side (connecting to a server) and on the
// server side (one instance per accepted client socket).
//
// Thread-safety: all public methods are safe to call from any thread.
// The internal reader loop runs in its own thread.
// -----------------------------------------------------------------------
class ComputeConnection : public std::enable_shared_from_this<ComputeConnection> {
public:
    using PrintTarget = ActorInstance::MethodCallInfo::PrintTarget;

    // Client side: connect to "host:port"
    static ptr<ComputeConnection> connect(const std::string& hostPort);

    ~ComputeConnection();

    // Spawn an actor on the remote end.
    // actorTypeVal: Value holding the ObjObjectType to ship
    // initArgs:     evaluated constructor arguments
    // Returns a remote-actor-proxy Value (ActorInstance with isRemote=true).
    // Blocks until the server responds (or throws on error).
    Value spawnActor(const Value& actorTypeVal,
                     const std::vector<Value>& initArgs,
                     const CallSpec& initCallSpec);

    // Send a method call to a remote actor and block until the result arrives.
    // Called by the remote-actor proxy thread (Thread::act) instead of local dispatch.
    Value callRemoteMethod(int64_t remoteActorId,
                           const icu::UnicodeString& methodName,
                           const std::vector<Value>& args,
                           const CallSpec& callSpec);

    // Register a local actor so that back-channel CALL_METHOD frames can be
    // routed back to it.  Returns the foreign ID assigned to this actor.
    // The Value is stored as a weak ref so the actor can be GC-ed independently.
    int64_t registerLocalActor(const Value& actorVal);
    void unregisterLocalActor(int64_t id);
    Value resolveLocalActor(int64_t id);
    void sendActorDropped(int64_t actorId);
    void sendPrintOutput(uint64_t callId, const std::string& text, bool flush);

    // Send a BYE frame and close the connection gracefully.
    void sendBye();

    // Abort the connection immediately, rejecting any pending outbound calls.
    // Used during shutdown so proxy helper threads blocked in future.get()
    // unwind promptly instead of stalling Thread::join().
    void abort();

    // True until the connection is closed or an error occurs.
    bool isAlive() const { return alive_.load(); }

    // Server-side constructor: wrap an already-accepted socket fd.
    // (Client side uses the static connect() factory.)
    explicit ComputeConnection(int fd, bool startReader = true);

    // Public wrapper so server mode can share the same write mutex/ framing.
    void sendMessage(ComputeMsg type, const std::vector<std::uint8_t>& payload) {
        sendFrame(type, payload);
    }

    // Resolve / reject a pending outgoing call.
    // Public so server-side code can resolve back-channel CALL_RESULT frames.
    void resolveCall(uint64_t callId, Value result);
    void rejectCall(uint64_t callId, const std::string& errorMsg);
    void deliverPrintOutput(uint64_t callId, const std::string& text, bool flush);

    // Handle an incoming CALL_METHOD for a locally-registered actor.
    // Public so server-side code can delegate back-channel calls.
    void handleIncomingCall(uint64_t callId, int64_t actorId,
                            const icu::UnicodeString& method,
                            const std::vector<Value>& args,
                            const CallSpec& callSpec);

private:
    struct PendingCall {
        ptr<std::promise<Value>> promise;
        PrintTarget printTarget;
    };

    int fd_;
    std::atomic<bool> alive_{ true };
    ptr<std::thread> readerThread_;

    // Outgoing call tracking
    std::atomic<uint64_t> nextCallId_{ 1 };
    std::unordered_map<uint64_t, PendingCall> pendingCalls_;
    std::mutex pendingMu_;

    // Local actor registry for back-channel calls
    std::atomic<int64_t> nextLocalActorId_{ 1 };
    std::unordered_map<int64_t, Value> localActors_;  // weakRef values
    std::mutex localActorsMu_;

    // Write mutex: only one thread may write to the socket at a time
    std::mutex writeMu_;

    void readerLoop();
    void sendFrame(ComputeMsg type, const std::vector<std::uint8_t>& payload);
    bool readExact(void* buf, std::size_t n);
    void closeSocket();

    // Serialization helpers: Value ↔ byte buffer via stringstream
    static std::vector<std::uint8_t> serializeValue(const Value& v,
                                                     ptr<SerializationContext> ctx = nullptr);
    static Value deserializeValue(const std::uint8_t*& p, const std::uint8_t* end,
                                  ptr<SerializationContext> ctx = nullptr);

    static std::vector<std::uint8_t> serializeValueList(const std::vector<Value>& vals,
                                                         ptr<SerializationContext> ctx = nullptr);
    static std::vector<Value> deserializeValueList(const std::uint8_t*& p,
                                                    const std::uint8_t* end,
                                                    ptr<SerializationContext> ctx = nullptr);
};

} // namespace roxal

#endif // ROXAL_COMPUTE_SERVER
