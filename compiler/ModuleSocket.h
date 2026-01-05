#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

#include <mutex>
#include <thread>
#include <atomic>
#include <list>
#include <memory>
#include <future>

namespace roxal {

// Forward declaration - socket state wrapper
struct SocketState;

// Pending async socket operation
struct PendingSocketOp {
    enum class Type { Accept, Connect, Recv, RecvFrom, Resolve };
    Type type;
    int fd;
    int bufferSize;
    std::shared_ptr<std::promise<Value>> promise;
    double timeoutSeconds;
    std::string hostname;  // For resolve operations
};

class ModuleSocket : public BuiltinModule {
public:
    ModuleSocket();
    virtual ~ModuleSocket();

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const { return moduleTypeValue; }

    // Module-level functions
    Value socket_tcp_builtin(ArgsView args);
    Value socket_udp_builtin(ArgsView args);
    Value socket_gethostbyname_builtin(ArgsView args);

    // Socket object methods
    Value socket_bind_builtin(ArgsView args);
    Value socket_listen_builtin(ArgsView args);
    Value socket_accept_builtin(ArgsView args);
    Value socket_connect_builtin(ArgsView args);
    Value socket_send_builtin(ArgsView args);
    Value socket_recv_builtin(ArgsView args);
    Value socket_sendto_builtin(ArgsView args);
    Value socket_recvfrom_builtin(ArgsView args);
    Value socket_close_builtin(ArgsView args);
    Value socket_settimeout_builtin(ArgsView args);
    Value socket_setsockopt_builtin(ArgsView args);
    Value socket_getsockname_builtin(ArgsView args);
    Value socket_getpeername_builtin(ArgsView args);
    Value socket_fileno_builtin(ArgsView args);

    // Helper to get SocketState from an object instance
    static SocketState* getSocketState(ObjectInstance* inst);

    // Helper to create a Socket object with given fd and type
    Value createSocketObject(int fd, int sockType);

private:
    Value moduleTypeValue; // ObjModuleType*

    // Background thread for async operations
    std::thread asyncThread;
    std::atomic<bool> running{false};
    std::mutex pendingMutex;
    std::list<PendingSocketOp> pendingOps;

    void asyncWorker();
    void startAsyncThread();
    void stopAsyncThread();
};

// Socket state stored in object instance
struct SocketState {
    int fd = -1;              // File descriptor
    int sockType = 0;         // SOCK_STREAM or SOCK_DGRAM
    int family = 0;           // AF_INET
    double timeoutSeconds = -1.0;  // -1 = blocking, 0 = non-blocking, >0 = timeout
    bool connected = false;
    bool listening = false;
    bool closed = false;
};

} // namespace roxal
