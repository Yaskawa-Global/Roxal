#include "ModuleSocket.h"
#include "VM.h"
#include "Object.h"

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <cstring>
#include <stdexcept>

using namespace roxal;

ModuleSocket::ModuleSocket()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("socket")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleSocket::~ModuleSocket()
{
    stopAsyncThread();
    destroyModuleType(moduleTypeValue);
}

void ModuleSocket::startAsyncThread()
{
    if (!running) {
        running = true;
        asyncThread = std::thread(&ModuleSocket::asyncWorker, this);
    }
}

void ModuleSocket::stopAsyncThread()
{
    if (running) {
        running = false;
        if (asyncThread.joinable()) {
            asyncThread.join();
        }
    }
}

void ModuleSocket::asyncWorker()
{
    while (running) {
        std::list<PendingSocketOp> toProcess;

        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            toProcess = std::move(pendingOps);
            pendingOps.clear();
        }

        if (toProcess.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Build poll array
        std::vector<struct pollfd> fds;
        std::vector<PendingSocketOp*> ops;

        for (auto& op : toProcess) {
            if (op.type == PendingSocketOp::Type::Resolve) {
                // DNS resolution - do it synchronously in this thread
                struct addrinfo hints{}, *result;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_STREAM;

                int status = getaddrinfo(op.hostname.c_str(), nullptr, &hints, &result);
                if (status == 0 && result) {
                    char ipstr[INET_ADDRSTRLEN];
                    struct sockaddr_in* addr = (struct sockaddr_in*)result->ai_addr;
                    inet_ntop(AF_INET, &addr->sin_addr, ipstr, sizeof(ipstr));
                    freeaddrinfo(result);
                    op.promise->set_value(Value::stringVal(toUnicodeString(ipstr)));
                } else {
                    op.promise->set_value(Value::nilVal());
                }
                continue;
            }

            struct pollfd pfd{};
            pfd.fd = op.fd;
            if (op.type == PendingSocketOp::Type::Accept ||
                op.type == PendingSocketOp::Type::Recv ||
                op.type == PendingSocketOp::Type::RecvFrom) {
                pfd.events = POLLIN;
            } else if (op.type == PendingSocketOp::Type::Connect) {
                pfd.events = POLLOUT;
            }
            fds.push_back(pfd);
            ops.push_back(&op);
        }

        if (fds.empty()) {
            continue;
        }

        // Poll with timeout
        int timeout_ms = 100;  // Check periodically
        int ready = poll(fds.data(), fds.size(), timeout_ms);

        if (ready > 0) {
            for (size_t i = 0; i < fds.size(); i++) {
                if (fds[i].revents & (POLLIN | POLLOUT)) {
                    PendingSocketOp* op = ops[i];

                    switch (op->type) {
                        case PendingSocketOp::Type::Accept: {
                            struct sockaddr_in clientAddr{};
                            socklen_t addrLen = sizeof(clientAddr);
                            int clientFd = accept(op->fd, (struct sockaddr*)&clientAddr, &addrLen);

                            if (clientFd >= 0) {
                                // Set non-blocking
                                int flags = fcntl(clientFd, F_GETFL, 0);
                                fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

                                // Get client address
                                char ipstr[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &clientAddr.sin_addr, ipstr, sizeof(ipstr));
                                int port = ntohs(clientAddr.sin_port);

                                // Create result: [Socket, [host, port]]
                                Value clientSocket = const_cast<ModuleSocket*>(this)->createSocketObject(clientFd, SOCK_STREAM);
                                Value addrList = Value::listVal();
                                asList(addrList)->elts.push_back(Value::stringVal(toUnicodeString(ipstr)));
                                asList(addrList)->elts.push_back(Value::intVal(port));

                                Value resultList = Value::listVal();
                                asList(resultList)->elts.push_back(clientSocket);
                                asList(resultList)->elts.push_back(addrList);

                                op->promise->set_value(resultList);
                            } else {
                                op->promise->set_value(Value::nilVal());
                            }
                            break;
                        }

                        case PendingSocketOp::Type::Connect: {
                            int error = 0;
                            socklen_t len = sizeof(error);
                            getsockopt(op->fd, SOL_SOCKET, SO_ERROR, &error, &len);
                            op->promise->set_value(error == 0 ? Value::trueVal() : Value::falseVal());
                            break;
                        }

                        case PendingSocketOp::Type::Recv: {
                            std::vector<char> buffer(op->bufferSize);
                            ssize_t n = recv(op->fd, buffer.data(), buffer.size(), 0);
                            if (n > 0) {
                                std::string data(buffer.data(), n);
                                op->promise->set_value(Value::stringVal(toUnicodeString(data)));
                            } else if (n == 0) {
                                // Connection closed
                                op->promise->set_value(Value::stringVal(toUnicodeString("")));
                            } else {
                                op->promise->set_value(Value::nilVal());
                            }
                            break;
                        }

                        case PendingSocketOp::Type::RecvFrom: {
                            std::vector<char> buffer(op->bufferSize);
                            struct sockaddr_in senderAddr{};
                            socklen_t addrLen = sizeof(senderAddr);
                            ssize_t n = recvfrom(op->fd, buffer.data(), buffer.size(), 0,
                                                (struct sockaddr*)&senderAddr, &addrLen);
                            if (n > 0) {
                                std::string data(buffer.data(), n);
                                char ipstr[INET_ADDRSTRLEN];
                                inet_ntop(AF_INET, &senderAddr.sin_addr, ipstr, sizeof(ipstr));
                                int port = ntohs(senderAddr.sin_port);

                                // Return [data, host, port]
                                Value resultList = Value::listVal();
                                asList(resultList)->elts.push_back(Value::stringVal(toUnicodeString(data)));
                                asList(resultList)->elts.push_back(Value::stringVal(toUnicodeString(ipstr)));
                                asList(resultList)->elts.push_back(Value::intVal(port));
                                op->promise->set_value(resultList);
                            } else {
                                op->promise->set_value(Value::nilVal());
                            }
                            break;
                        }

                        default:
                            break;
                    }
                } else if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    // Error condition
                    ops[i]->promise->set_value(Value::nilVal());
                } else {
                    // Not ready yet, re-queue
                    std::lock_guard<std::mutex> lock(pendingMutex);
                    pendingOps.push_back(*ops[i]);
                }
            }
        } else {
            // Timeout or error - re-queue all
            std::lock_guard<std::mutex> lock(pendingMutex);
            for (auto& op : toProcess) {
                if (op.type != PendingSocketOp::Type::Resolve) {
                    pendingOps.push_back(op);
                }
            }
        }
    }
}

void ModuleSocket::registerBuiltins(VM& vm)
{
    setVM(vm);

    // Module-level functions
    link("tcp", [this](VM&, ArgsView a) { return socket_tcp_builtin(a); });
    link("udp", [this](VM&, ArgsView a) { return socket_udp_builtin(a); });
    link("gethostbyname", [this](VM&, ArgsView a) { return socket_gethostbyname_builtin(a); });

    // Socket object methods
    linkMethod("Socket", "bind", [this](VM&, ArgsView a) { return socket_bind_builtin(a); });
    linkMethod("Socket", "listen", [this](VM&, ArgsView a) { return socket_listen_builtin(a); });
    linkMethod("Socket", "accept", [this](VM&, ArgsView a) { return socket_accept_builtin(a); });
    linkMethod("Socket", "connect", [this](VM&, ArgsView a) { return socket_connect_builtin(a); });
    linkMethod("Socket", "send", [this](VM&, ArgsView a) { return socket_send_builtin(a); });
    linkMethod("Socket", "recv", [this](VM&, ArgsView a) { return socket_recv_builtin(a); });
    linkMethod("Socket", "sendto", [this](VM&, ArgsView a) { return socket_sendto_builtin(a); });
    linkMethod("Socket", "recvfrom", [this](VM&, ArgsView a) { return socket_recvfrom_builtin(a); });
    linkMethod("Socket", "close", [this](VM&, ArgsView a) { return socket_close_builtin(a); });
    linkMethod("Socket", "settimeout", [this](VM&, ArgsView a) { return socket_settimeout_builtin(a); });
    linkMethod("Socket", "setsockopt", [this](VM&, ArgsView a) { return socket_setsockopt_builtin(a); });
    linkMethod("Socket", "getsockname", [this](VM&, ArgsView a) { return socket_getsockname_builtin(a); });
    linkMethod("Socket", "getpeername", [this](VM&, ArgsView a) { return socket_getpeername_builtin(a); });
    linkMethod("Socket", "fileno", [this](VM&, ArgsView a) { return socket_fileno_builtin(a); });
}

void ModuleSocket::onModuleLoaded(VM& vm)
{
    // Start the async worker thread when module is loaded
    startAsyncThread();
}

void ModuleSocket::onModuleUnloading(VM& vm)
{
    // Stop the async worker thread when module is unloaded
    stopAsyncThread();
}

SocketState* ModuleSocket::getSocketState(ObjectInstance* inst)
{
    Value fpVal = inst->getProperty("_this");
    if (fpVal.isNil() || !isForeignPtr(fpVal)) {
        throw std::runtime_error("Socket object not properly initialized");
    }
    return static_cast<SocketState*>(asForeignPtr(fpVal)->ptr);
}

Value ModuleSocket::createSocketObject(int fd, int sockType)
{
    // Get the Socket type from the module
    auto typeVal = asModuleType(moduleType())->vars.load(toUnicodeString("Socket"));
    if (!typeVal.has_value() || !isObjectType(typeVal.value())) {
        throw std::runtime_error("Socket type not found in module");
    }

    // Create instance
    Value instance = Value::objVal(newObjectInstance(typeVal.value()));
    ObjectInstance* inst = asObjectInstance(instance);

    // Create and store socket state
    SocketState* state = new SocketState();
    state->fd = fd;
    state->sockType = sockType;
    state->family = AF_INET;

    Value fp = Value::foreignPtrVal(state);
    asForeignPtr(fp)->registerCleanup([](void* p) {
        SocketState* s = static_cast<SocketState*>(p);
        if (s->fd >= 0 && !s->closed) {
            ::close(s->fd);
        }
        delete s;
    });
    inst->setProperty("_this", fp);

    return instance;
}

Value ModuleSocket::socket_tcp_builtin(ArgsView args)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to create TCP socket: " + std::string(strerror(errno)));
    }

    // Set non-blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return createSocketObject(fd, SOCK_STREAM);
}

Value ModuleSocket::socket_udp_builtin(ArgsView args)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to create UDP socket: " + std::string(strerror(errno)));
    }

    // Set non-blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return createSocketObject(fd, SOCK_DGRAM);
}

Value ModuleSocket::socket_gethostbyname_builtin(ArgsView args)
{
    if (args.size() < 1 || !isString(args[0])) {
        throw std::invalid_argument("gethostbyname expects hostname string");
    }

    std::string hostname = toUTF8StdString(asStringObj(args[0])->s);

    // Create promise and future
    auto promise = std::make_shared<std::promise<Value>>();
    std::shared_future<Value> future = promise->get_future().share();

    // Queue the resolve operation
    PendingSocketOp op;
    op.type = PendingSocketOp::Type::Resolve;
    op.hostname = hostname;
    op.promise = promise;

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingOps.push_back(op);
    }

    return Value::futureVal(future);
}

Value ModuleSocket::socket_bind_builtin(ArgsView args)
{
    if (args.size() < 3 || !isObjectInstance(args[0]) || !isString(args[1]) || !args[2].isInt()) {
        throw std::invalid_argument("bind expects host string and port int");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    std::string host = toUTF8StdString(asStringObj(args[1])->s);
    int port = args[2].asInt();

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (host == "0.0.0.0" || host.empty()) {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            throw std::invalid_argument("Invalid address: " + host);
        }
    }

    if (bind(state->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        throw std::runtime_error("bind failed: " + std::string(strerror(errno)));
    }

    return Value::trueVal();
}

Value ModuleSocket::socket_listen_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("listen expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    int backlog = 5;
    if (args.size() >= 2 && args[1].isInt()) {
        backlog = args[1].asInt();
    }

    if (listen(state->fd, backlog) < 0) {
        throw std::runtime_error("listen failed: " + std::string(strerror(errno)));
    }

    state->listening = true;
    return Value::trueVal();
}

Value ModuleSocket::socket_accept_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("accept expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    // Create promise and future
    auto promise = std::make_shared<std::promise<Value>>();
    std::shared_future<Value> future = promise->get_future().share();

    // Queue the accept operation
    PendingSocketOp op;
    op.type = PendingSocketOp::Type::Accept;
    op.fd = state->fd;
    op.promise = promise;
    op.timeoutSeconds = state->timeoutSeconds;

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingOps.push_back(op);
    }

    return Value::futureVal(future);
}

Value ModuleSocket::socket_connect_builtin(ArgsView args)
{
    if (args.size() < 3 || !isObjectInstance(args[0]) || !isString(args[1]) || !args[2].isInt()) {
        throw std::invalid_argument("connect expects host string and port int");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    std::string host = toUTF8StdString(asStringObj(args[1])->s);
    int port = args[2].asInt();

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        // Try DNS resolution
        struct addrinfo hints{}, *result;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result) {
            addr.sin_addr = ((struct sockaddr_in*)result->ai_addr)->sin_addr;
            freeaddrinfo(result);
        } else {
            throw std::invalid_argument("Cannot resolve host: " + host);
        }
    }

    // Start non-blocking connect
    int ret = connect(state->fd, (struct sockaddr*)&addr, sizeof(addr));

    if (ret == 0) {
        // Connected immediately
        state->connected = true;
        auto promise = std::make_shared<std::promise<Value>>();
        promise->set_value(Value::trueVal());
        return Value::futureVal(promise->get_future().share());
    } else if (errno == EINPROGRESS) {
        // Connection in progress - return future
        auto promise = std::make_shared<std::promise<Value>>();
        std::shared_future<Value> future = promise->get_future().share();

        PendingSocketOp op;
        op.type = PendingSocketOp::Type::Connect;
        op.fd = state->fd;
        op.promise = promise;
        op.timeoutSeconds = state->timeoutSeconds;

        {
            std::lock_guard<std::mutex> lock(pendingMutex);
            pendingOps.push_back(op);
        }

        return Value::futureVal(future);
    } else {
        throw std::runtime_error("connect failed: " + std::string(strerror(errno)));
    }
}

Value ModuleSocket::socket_send_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0]) || !isString(args[1])) {
        throw std::invalid_argument("send expects data string");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    std::string data = toUTF8StdString(asStringObj(args[1])->s);

    ssize_t sent = send(state->fd, data.c_str(), data.length(), 0);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Value::intVal(0);
        }
        throw std::runtime_error("send failed: " + std::string(strerror(errno)));
    }

    return Value::intVal(sent);
}

Value ModuleSocket::socket_recv_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("recv expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    int size = 4096;
    if (args.size() >= 2 && args[1].isInt()) {
        size = args[1].asInt();
    }

    // Create promise and future
    auto promise = std::make_shared<std::promise<Value>>();
    std::shared_future<Value> future = promise->get_future().share();

    // Queue the recv operation
    PendingSocketOp op;
    op.type = PendingSocketOp::Type::Recv;
    op.fd = state->fd;
    op.bufferSize = size;
    op.promise = promise;
    op.timeoutSeconds = state->timeoutSeconds;

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingOps.push_back(op);
    }

    return Value::futureVal(future);
}

Value ModuleSocket::socket_sendto_builtin(ArgsView args)
{
    if (args.size() < 4 || !isObjectInstance(args[0]) || !isString(args[1]) ||
        !isString(args[2]) || !args[3].isInt()) {
        throw std::invalid_argument("sendto expects data, host, port");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    std::string data = toUTF8StdString(asStringObj(args[1])->s);
    std::string host = toUTF8StdString(asStringObj(args[2])->s);
    int port = args[3].asInt();

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        throw std::invalid_argument("Invalid address: " + host);
    }

    ssize_t sent = sendto(state->fd, data.c_str(), data.length(), 0,
                          (struct sockaddr*)&addr, sizeof(addr));
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return Value::intVal(0);
        }
        throw std::runtime_error("sendto failed: " + std::string(strerror(errno)));
    }

    return Value::intVal(sent);
}

Value ModuleSocket::socket_recvfrom_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("recvfrom expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    int size = 4096;
    if (args.size() >= 2 && args[1].isInt()) {
        size = args[1].asInt();
    }

    // Create promise and future
    auto promise = std::make_shared<std::promise<Value>>();
    std::shared_future<Value> future = promise->get_future().share();

    // Queue the recvfrom operation
    PendingSocketOp op;
    op.type = PendingSocketOp::Type::RecvFrom;
    op.fd = state->fd;
    op.bufferSize = size;
    op.promise = promise;
    op.timeoutSeconds = state->timeoutSeconds;

    {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pendingOps.push_back(op);
    }

    return Value::futureVal(future);
}

Value ModuleSocket::socket_close_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("close expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    if (state->fd >= 0 && !state->closed) {
        ::close(state->fd);
        state->closed = true;
    }

    return Value::nilVal();
}

Value ModuleSocket::socket_settimeout_builtin(ArgsView args)
{
    if (args.size() < 2 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("settimeout expects timeout value");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    if (args[1].isNil()) {
        state->timeoutSeconds = -1.0;  // Blocking
    } else if (args[1].isReal()) {
        state->timeoutSeconds = args[1].asReal();
    } else if (args[1].isInt()) {
        state->timeoutSeconds = static_cast<double>(args[1].asInt());
    } else {
        throw std::invalid_argument("settimeout expects number or nil");
    }

    return Value::nilVal();
}

Value ModuleSocket::socket_setsockopt_builtin(ArgsView args)
{
    if (args.size() < 3 || !isObjectInstance(args[0]) || !isString(args[1])) {
        throw std::invalid_argument("setsockopt expects option name and value");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    std::string option = toUTF8StdString(asStringObj(args[1])->s);
    int level = SOL_SOCKET;
    int optname = 0;
    int intval = 0;

    if (option == "reuseaddr") {
        optname = SO_REUSEADDR;
        intval = args[2].isBool() ? (args[2].asBool() ? 1 : 0) : (args[2].asInt() ? 1 : 0);
    } else if (option == "broadcast") {
        optname = SO_BROADCAST;
        intval = args[2].isBool() ? (args[2].asBool() ? 1 : 0) : (args[2].asInt() ? 1 : 0);
    } else if (option == "keepalive") {
        optname = SO_KEEPALIVE;
        intval = args[2].isBool() ? (args[2].asBool() ? 1 : 0) : (args[2].asInt() ? 1 : 0);
    } else if (option == "nodelay") {
        level = IPPROTO_TCP;
        optname = TCP_NODELAY;
        intval = args[2].isBool() ? (args[2].asBool() ? 1 : 0) : (args[2].asInt() ? 1 : 0);
    } else if (option == "rcvbuf") {
        optname = SO_RCVBUF;
        intval = args[2].asInt();
    } else if (option == "sndbuf") {
        optname = SO_SNDBUF;
        intval = args[2].asInt();
    } else {
        throw std::invalid_argument("Unknown socket option: " + option);
    }

    if (setsockopt(state->fd, level, optname, &intval, sizeof(intval)) < 0) {
        return Value::falseVal();
    }

    return Value::trueVal();
}

Value ModuleSocket::socket_getsockname_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("getsockname expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    struct sockaddr_in addr{};
    socklen_t addrLen = sizeof(addr);

    if (getsockname(state->fd, (struct sockaddr*)&addr, &addrLen) < 0) {
        throw std::runtime_error("getsockname failed: " + std::string(strerror(errno)));
    }

    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof(ipstr));
    int port = ntohs(addr.sin_port);

    Value result = Value::listVal();
    asList(result)->elts.push_back(Value::stringVal(toUnicodeString(ipstr)));
    asList(result)->elts.push_back(Value::intVal(port));

    return result;
}

Value ModuleSocket::socket_getpeername_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("getpeername expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    struct sockaddr_in addr{};
    socklen_t addrLen = sizeof(addr);

    if (getpeername(state->fd, (struct sockaddr*)&addr, &addrLen) < 0) {
        throw std::runtime_error("getpeername failed: " + std::string(strerror(errno)));
    }

    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ipstr, sizeof(ipstr));
    int port = ntohs(addr.sin_port);

    Value result = Value::listVal();
    asList(result)->elts.push_back(Value::stringVal(toUnicodeString(ipstr)));
    asList(result)->elts.push_back(Value::intVal(port));

    return result;
}

Value ModuleSocket::socket_fileno_builtin(ArgsView args)
{
    if (args.size() < 1 || !isObjectInstance(args[0])) {
        throw std::invalid_argument("fileno expects socket receiver");
    }

    ObjectInstance* inst = asObjectInstance(args[0]);
    SocketState* state = getSocketState(inst);

    return Value::intVal(state->fd);
}
