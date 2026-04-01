#include "ComputeConnection.h"

#ifdef ROXAL_COMPUTE_SERVER

#include <compiler/VM.h>
#include <compiler/Thread.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace roxal {

// ---------------------------------------------------------------------------
// helpers: parse "host[:port]"
// ---------------------------------------------------------------------------
static std::pair<std::string,uint16_t> parseHostPort(const std::string& hostPort)
{
    auto colon = hostPort.rfind(':');
    std::string host;
    std::uint16_t port = ComputeDefaultPort;

    if (colon == std::string::npos) {
        host = hostPort;
    } else {
        host = hostPort.substr(0, colon);
        std::string portText = hostPort.substr(colon + 1);
        if (portText.empty())
            throw std::runtime_error("ComputeConnection: invalid port in '" + hostPort + "'");
        int parsedPort = std::stoi(portText);
        if (parsedPort <= 0 || parsedPort > 65535)
            throw std::runtime_error("ComputeConnection: invalid port in '" + hostPort + "'");
        port = static_cast<std::uint16_t>(parsedPort);
    }

    if (host.empty())
        throw std::runtime_error("ComputeConnection: expected host or host:port, got '" + hostPort + "'");
    return { host, port };
}

std::string toUtf8StdString(const icu::UnicodeString& value)
{
    std::string utf8;
    value.toUTF8String(utf8);
    return utf8;
}

icu::UnicodeString lastQualifiedSegment(const icu::UnicodeString& value)
{
    int32_t dotIndex = value.lastIndexOf('.');
    return dotIndex >= 0 ? value.tempSubString(dotIndex + 1) : value;
}

Value rewrapLike(const Value& original, const Value& replacement)
{
    return original.isWeak() ? replacement.weakRef() : replacement.strongRef();
}

Value findCanonicalModuleValue(const icu::UnicodeString& fullName,
                               const icu::UnicodeString& moduleName)
{
    auto matchModule = [&](const Value& moduleValue) -> Value {
        if (!isModuleType(moduleValue))
            return Value::nilVal();
        ObjModuleType* module = asModuleType(moduleValue);
        if (!fullName.isEmpty()) {
            if (module->fullName == fullName)
                return moduleValue.strongRef();
            return Value::nilVal();
        }
        if (module->name == moduleName)
            return moduleValue.strongRef();
        return Value::nilVal();
    };

    Value builtin = VM::instance().getBuiltinModuleType(moduleName);
    if (builtin.isNonNil()) {
        Value matched = matchModule(builtin);
        if (matched.isNonNil())
            return matched;
    }

    if (!fullName.isEmpty()) {
        auto globalByFull = VM::instance().loadGlobal(fullName);
        if (globalByFull.has_value()) {
            Value matched = matchModule(globalByFull.value());
            if (matched.isNonNil())
                return matched;
        }
    }

    auto globalByName = VM::instance().loadGlobal(moduleName);
    if (globalByName.has_value()) {
        Value matched = matchModule(globalByName.value());
        if (matched.isNonNil())
            return matched;
    }

    for (const Value& modVal : ObjModuleType::allModules.get()) {
        Value matched = matchModule(modVal);
        if (matched.isNonNil())
            return matched;
    }

    return Value::nilVal();
}

bool isBuiltinModuleValue(const Value& moduleValue)
{
    if (!isModuleType(moduleValue))
        return false;
    Value builtin = VM::instance().getBuiltinModuleType(asModuleType(moduleValue)->name);
    return builtin.isNonNil() && builtin.asObj() == moduleValue.asObj();
}

Value resolveNamedTypeFromModule(const Value& moduleValue,
                                 const icu::UnicodeString& typeName)
{
    auto tryLoadType = [](const Value& container, const icu::UnicodeString& symbol) -> Value {
        if (!isModuleType(container))
            return Value::nilVal();
        auto loaded = asModuleType(container)->vars.load(symbol);
        if (loaded.has_value() && isObjectType(loaded.value()))
            return loaded.value().strongRef();
        return Value::nilVal();
    };

    Value strongModule = moduleValue.strongRef();
    if (isModuleType(strongModule)) {
        Value direct = tryLoadType(strongModule, typeName);
        if (direct.isNonNil())
            return direct;

        int32_t dotIndex = typeName.lastIndexOf('.');
        if (dotIndex >= 0) {
            icu::UnicodeString modulePart = typeName.tempSubString(0, dotIndex);
            icu::UnicodeString symbolPart = typeName.tempSubString(dotIndex + 1);

            Value importedModule = Value::nilVal();
            icu::UnicodeString aliasFull = asModuleType(strongModule)->moduleAliasFullName(modulePart);
            if (!aliasFull.isEmpty())
                importedModule = findCanonicalModuleValue(aliasFull, lastQualifiedSegment(aliasFull));
            if (importedModule.isNil()) {
                auto moduleVar = asModuleType(strongModule)->vars.load(modulePart);
                if (moduleVar.has_value() && isModuleType(moduleVar.value()))
                    importedModule = moduleVar.value().strongRef();
            }
            if (importedModule.isNil())
                importedModule = findCanonicalModuleValue(modulePart, lastQualifiedSegment(modulePart));

            Value qualified = tryLoadType(importedModule, symbolPart);
            if (qualified.isNonNil())
                return qualified;
        }
    }

    auto globalType = VM::instance().loadGlobal(typeName);
    if (globalType.has_value() && isObjectType(globalType.value()))
        return globalType.value().strongRef();

    return Value::nilVal();
}

struct RemoteTypeDependency {
    icu::UnicodeString moduleFullName;
    icu::UnicodeString moduleName;
    icu::UnicodeString symbolName;
    Value typeValue;
};

std::optional<std::pair<icu::UnicodeString, icu::UnicodeString>> findOwningModuleForType(const Value& typeValue)
{
    if (!isObjectType(typeValue))
        return std::nullopt;

    for (const auto& [_, method] : asObjectType(typeValue)->methods) {
        if (!isClosure(method.closure))
            continue;
        Value moduleValue = asFunction(asClosure(method.closure)->function)->moduleType.strongRef();
        if (!isModuleType(moduleValue))
            continue;

        ObjModuleType* module = asModuleType(moduleValue);
        icu::UnicodeString fullName = module->fullName.isEmpty() ? module->name : module->fullName;
        return std::make_pair(fullName, module->name);
    }

    return std::nullopt;
}

std::vector<RemoteTypeDependency> collectRemoteTypeDependencies(const Value& actorTypeVal)
{
    if (!isObjectType(actorTypeVal))
        throw std::runtime_error("collectRemoteTypeDependencies requires an object type");

    ObjObjectType* rootType = asObjectType(actorTypeVal);
    std::vector<RemoteTypeDependency> dependencies;
    std::unordered_set<const Obj*> visitedTypes;
    std::unordered_set<const Obj*> visitedFunctions;

    auto findOwningExport = [&](const Value& typeValue) -> std::optional<RemoteTypeDependency> {
        for (const Value& modVal : ObjModuleType::allModules.get()) {
            if (!isModuleType(modVal))
                continue;
            Value strongModule = modVal.strongRef();
            if (strongModule.isNil() || !isModuleType(strongModule))
                continue;
            if (isBuiltinModuleValue(strongModule))
                continue;

            ObjModuleType* module = asModuleType(strongModule);
            for (const auto& entry : module->vars.snapshot()) {
                Value exported = entry.second.strongRef();
                if (!isObjectType(exported))
                    continue;
                if (exported.asObj() != typeValue.asObj())
                    continue;

                RemoteTypeDependency dep {};
                dep.moduleName = module->name;
                dep.moduleFullName = module->fullName.isEmpty() ? module->name : module->fullName;
                dep.symbolName = entry.first;
                dep.typeValue = exported;
                return dep;
            }
        }
        return std::nullopt;
    };

    std::function<void(const type::Type::FuncType&, const Value&)> collectFromFuncInfo;
    std::function<void(const ptr<type::Type>&, const Value&)> collectFromTypeInfo;
    std::function<void(const Value&)> collectFunctionValue;
    std::function<void(const Value&)> collectTypeValue;

    collectFromFuncInfo = [&](const type::Type::FuncType& funcInfo, const Value& moduleValue) {
        for (const auto& paramOpt : funcInfo.params) {
            if (paramOpt.has_value() && paramOpt->type.has_value())
                collectFromTypeInfo(paramOpt->type.value(), moduleValue);
        }
        for (const auto& returnType : funcInfo.returnTypes)
            collectFromTypeInfo(returnType, moduleValue);
    };

    collectFromTypeInfo = [&](const ptr<type::Type>& typeInfo, const Value& moduleValue) {
        if (typeInfo == nullptr)
            return;

        if (typeInfo->builtin == type::BuiltinType::Func && typeInfo->func.has_value())
            collectFromFuncInfo(typeInfo->func.value(), moduleValue);

        if ((typeInfo->builtin == type::BuiltinType::Object
             || typeInfo->builtin == type::BuiltinType::Actor)
            && typeInfo->obj.has_value()) {
            const auto& objInfo = typeInfo->obj.value();
            if (!objInfo.name.isEmpty()) {
                Value resolved = resolveNamedTypeFromModule(moduleValue, objInfo.name);
                if (resolved.isNil()) {
                    throw std::runtime_error("Remote actor dependency type '" +
                                             toUtf8StdString(objInfo.name) +
                                             "' could not be resolved in module scope");
                }
                collectTypeValue(resolved);
            }
            if (objInfo.extends.has_value())
                collectFromTypeInfo(objInfo.extends.value(), moduleValue);
            for (const auto& implemented : objInfo.implements)
                collectFromTypeInfo(implemented, moduleValue);
            for (const auto& property : objInfo.properties) {
                if (property.type.has_value())
                    collectFromTypeInfo(property.type.value(), moduleValue);
            }
            for (const auto& method : objInfo.methods) {
                if (method.second)
                    collectFromFuncInfo(*method.second, moduleValue);
            }
        }
    };

    collectFunctionValue = [&](const Value& functionValue) {
        Value strongFunction = functionValue.strongRef();
        if (strongFunction.isNil() || !isFunction(strongFunction))
            return;

        ObjFunction* function = asFunction(strongFunction);
        if (!visitedFunctions.insert(function).second)
            return;

        Value moduleValue = function->moduleType.strongRef();
        if (function->funcType.has_value())
            collectFromTypeInfo(function->funcType.value(), moduleValue);

        if (isObjectType(function->ownerType.strongRef()))
            collectTypeValue(function->ownerType.strongRef());

        if (function->chunk) {
            for (const Value& constant : function->chunk->constants) {
                if (isFunction(constant))
                    collectFunctionValue(constant);
                else if (isObjectType(constant))
                    collectTypeValue(constant);
            }
        }

        for (const auto& [_, defaultFunc] : function->paramDefaultFunc)
            collectFunctionValue(defaultFunc);
    };

    collectTypeValue = [&](const Value& typeValue) {
        Value strongType = typeValue.strongRef();
        if (strongType.isNil() || !isObjectType(strongType))
            return;
        if (strongType.asObj() == rootType)
            return;

        ObjObjectType* objectType = asObjectType(strongType);
        if (!visitedTypes.insert(objectType).second)
            return;

        auto depOpt = findOwningExport(strongType);
        if (!depOpt.has_value()) {
            throw std::runtime_error("Remote actor dependency type '" +
                                     toUtf8StdString(objectType->name) +
                                     "' is not exported from a non-builtin module");
        }
        dependencies.push_back(depOpt.value());

        if (isObjectType(objectType->superType.strongRef()))
            collectTypeValue(objectType->superType.strongRef());

        for (const auto& [_, property] : objectType->properties) {
            if (isObjectType(property.type.strongRef()))
                collectTypeValue(property.type.strongRef());
            if (isObjectType(property.ownerType.strongRef()))
                collectTypeValue(property.ownerType.strongRef());
        }

        for (const auto& [_, method] : objectType->methods) {
            if (isObjectType(method.ownerType.strongRef()))
                collectTypeValue(method.ownerType.strongRef());
            if (isClosure(method.closure))
                collectFunctionValue(asClosure(method.closure)->function);
        }
    };

    if (isObjectType(rootType->superType.strongRef()))
        collectTypeValue(rootType->superType.strongRef());
    for (const auto& [_, property] : rootType->properties) {
        if (isObjectType(property.type.strongRef()))
            collectTypeValue(property.type.strongRef());
    }
    for (const auto& [_, method] : rootType->methods) {
        if (isClosure(method.closure))
            collectFunctionValue(asClosure(method.closure)->function);
    }

    std::unordered_set<const Obj*> visitedOwnerModules;
    for (const auto& [_, method] : rootType->methods) {
        if (!isClosure(method.closure))
            continue;
        Value moduleValue = asFunction(asClosure(method.closure)->function)->moduleType.strongRef();
        if (!isModuleType(moduleValue))
            continue;
        if (!visitedOwnerModules.insert(moduleValue.asObj()).second)
            continue;
        if (isBuiltinModuleValue(moduleValue))
            continue;

        for (const auto& entry : asModuleType(moduleValue)->vars.snapshot()) {
            Value exported = entry.second.strongRef();
            if (!isObjectType(exported))
                continue;
            collectTypeValue(exported);
        }
    }

    return dependencies;
}

// ---------------------------------------------------------------------------
// ComputeConnection — construction / destruction
// ---------------------------------------------------------------------------

ComputeConnection::ComputeConnection(int fd, bool startReader) : fd_(fd)
{
    if (startReader)
        readerThread_ = make_ptr<std::thread>([this]() { readerLoop(); });
}

ComputeConnection::~ComputeConnection()
{
    closeSocket();
    if (readerThread_ && readerThread_->joinable())
        readerThread_->join();
}

// ---------------------------------------------------------------------------
// connect() — client-side factory
// ---------------------------------------------------------------------------

ptr<ComputeConnection> ComputeConnection::connect(const std::string& hostPort)
{
    auto [host, port] = parseHostPort(hostPort);

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        throw std::runtime_error("ComputeConnection: socket(): " + std::string(strerror(errno)));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        // Try DNS resolution
        struct addrinfo hints{}, *res;
        hints.ai_family   = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
            addr.sin_addr = ((struct sockaddr_in*)res->ai_addr)->sin_addr;
            freeaddrinfo(res);
        } else {
            ::close(fd);
            throw std::runtime_error("ComputeConnection: cannot resolve host '" + host + "'");
        }
    }

    if (::connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd);
        throw std::runtime_error("ComputeConnection: connect() to " + hostPort
                                 + ": " + strerror(errno));
    }

    // Wrap in connection object (constructor starts reader thread)
    auto conn = make_ptr<ComputeConnection>(fd);

    // Send HELLO
    auto helloPayload = buildHelloPayload();
    conn->sendFrame(ComputeMsg::Hello, helloPayload);

    // Wait for HELLO_OK / HELLO_ERR — block briefly in the reader
    // We do a synchronous mini-read here before the reader thread claims the fd.
    // The reader thread hasn't started processing yet (it's blocked in readExact
    // waiting for the first frame).  We do the handshake synchronously then let
    // the reader take over.
    //
    // In practice we rely on the reader loop to handle HELLO_OK and signal us.
    // For simplicity, we use a promise/future pair.
    {
        std::lock_guard<std::mutex> lk(conn->pendingMu_);
        // Use call_id=0 as the handshake slot (real calls start at 1)
        ComputeConnection::PendingCall pending {};
        pending.promise = make_ptr<std::promise<Value>>();
        pending.printTarget = ComputeConnection::PrintTarget::localStdout();
        conn->pendingCalls_[0] = pending;
    }

    // The reader loop will pick up HELLO_OK (call_id=0 slot) and resolve it.
    // But HELLO_OK has no call_id in the payload — so we handle it specially
    // in readerLoop() by resolving the id=0 slot with nil.
    //
    // Block until the reader delivers the handshake result.
    Value handshakeResult;
    {
        std::future<Value> fut;
        {
            std::lock_guard<std::mutex> lk(conn->pendingMu_);
            fut = conn->pendingCalls_[0].promise->get_future();
        }
        handshakeResult = fut.get(); // blocks; reader loop resolves/rejects this
    }
    // If the reader rejected it (HELLO_ERR) it will have thrown via rejectCall.
    // (The exception propagates through the future.)

    return conn;
}

// ---------------------------------------------------------------------------
// closeSocket / sendBye
// ---------------------------------------------------------------------------

void ComputeConnection::closeSocket()
{
    bool expected = true;
    if (!alive_.compare_exchange_strong(expected, false))
        return; // already closed

    // Wake up any blocked reads/writes
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }

    // Reject all pending calls
    std::lock_guard<std::mutex> lk(pendingMu_);
    for (auto& [id, pending] : pendingCalls_) {
        try { pending.promise->set_exception(std::make_exception_ptr(
                std::runtime_error("ComputeConnection: connection closed"))); }
        catch (...) {}
    }
    pendingCalls_.clear();
}

void ComputeConnection::sendBye()
{
    if (!alive_) return;
    sendFrame(ComputeMsg::Bye, {});
    closeSocket();
}

void ComputeConnection::abort()
{
    closeSocket();
}

// ---------------------------------------------------------------------------
// Low-level I/O
// ---------------------------------------------------------------------------

bool ComputeConnection::readExact(void* buf, std::size_t n)
{
    auto* p = static_cast<char*>(buf);
    std::size_t done = 0;
    while (done < n) {
        ssize_t r = ::recv(fd_, p + done, n - done, 0);
        if (r <= 0) return false;
        done += static_cast<std::size_t>(r);
    }
    return true;
}

void ComputeConnection::sendFrame(ComputeMsg type, const std::vector<std::uint8_t>& payload)
{
    auto frame = buildFrame(type, payload);
    std::lock_guard<std::mutex> lk(writeMu_);
    std::size_t done = 0;
    while (done < frame.size()) {
        ssize_t n = ::send(fd_, frame.data() + done, frame.size() - done, MSG_NOSIGNAL);
        if (n <= 0)
            throw std::runtime_error("ComputeConnection: send failed: " + std::string(strerror(errno)));
        done += static_cast<std::size_t>(n);
    }
}

// ---------------------------------------------------------------------------
// Pending call resolution
// ---------------------------------------------------------------------------

void ComputeConnection::resolveCall(uint64_t callId, Value result)
{
    std::lock_guard<std::mutex> lk(pendingMu_);
    auto it = pendingCalls_.find(callId);
    if (it == pendingCalls_.end()) return;
    it->second.promise->set_value(std::move(result));
    pendingCalls_.erase(it);
}

void ComputeConnection::rejectCall(uint64_t callId, const std::string& errorMsg)
{
    std::lock_guard<std::mutex> lk(pendingMu_);
    auto it = pendingCalls_.find(callId);
    if (it == pendingCalls_.end()) return;
    it->second.promise->set_exception(std::make_exception_ptr(std::runtime_error(errorMsg)));
    pendingCalls_.erase(it);
}

void ComputeConnection::deliverPrintOutput(uint64_t callId, const std::string& text, bool flush)
{
    ComputeConnection::PrintTarget target = ComputeConnection::PrintTarget::localStdout();
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        auto it = pendingCalls_.find(callId);
        if (it == pendingCalls_.end())
            return;
        target = it->second.printTarget;
    }

    if (!target.routesRemotely()) {
        std::cout << text;
        if (flush)
            std::cout << std::flush;
        return;
    }

    auto upstreamConn = target.remoteConn.lock();
    if (!upstreamConn) {
        std::cout << text;
        if (flush)
            std::cout << std::flush;
        return;
    }

    upstreamConn->sendPrintOutput(target.remoteCallId, text, flush);
}

// ---------------------------------------------------------------------------
// Serialization helpers
// ---------------------------------------------------------------------------

std::vector<std::uint8_t> ComputeConnection::serializeValue(
        const Value& v, ptr<SerializationContext> ctx)
{
    std::ostringstream oss(std::ios::binary);
    writeValue(oss, v, ctx);
    auto s = oss.str();
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

Value ComputeConnection::deserializeValue(
        const std::uint8_t*& p, const std::uint8_t* end,
        ptr<SerializationContext> ctx)
{
    // Wrap the remaining buffer in an istream
    std::string buf(reinterpret_cast<const char*>(p), end - p);
    std::istringstream iss(buf, std::ios::binary);
    Value v = readValue(iss, ctx);
    // Advance p by how many bytes were consumed
    p += static_cast<std::size_t>(iss.tellg());
    return v;
}

std::vector<std::uint8_t> ComputeConnection::serializeValueList(
        const std::vector<Value>& vals, ptr<SerializationContext> ctx)
{
    std::vector<std::uint8_t> buf;
    writeU32(buf, static_cast<std::uint32_t>(vals.size()));
    for (const auto& v : vals) {
        auto bytes = serializeValue(v, ctx);
        writeU32(buf, static_cast<std::uint32_t>(bytes.size()));
        buf.insert(buf.end(), bytes.begin(), bytes.end());
    }
    return buf;
}

std::vector<Value> ComputeConnection::deserializeValueList(
        const std::uint8_t*& p, const std::uint8_t* end,
        ptr<SerializationContext> ctx)
{
    std::uint32_t count = readU32(p, end);
    std::vector<Value> vals;
    vals.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t len = readU32(p, end);
        if (p + len > end)
            throw std::runtime_error("ComputeConnection: truncated value in list");
        const std::uint8_t* vend = p + len;
        vals.push_back(deserializeValue(p, vend, ctx));
        p = vend; // ensure cursor is at end of this value's bytes
    }
    return vals;
}

// ---------------------------------------------------------------------------
// spawnActor
// ---------------------------------------------------------------------------

Value ComputeConnection::spawnActor(const Value& actorTypeVal,
                                    const std::vector<Value>& initArgs,
                                    const CallSpec& initCallSpec)
{
    uint64_t callId = nextCallId_.fetch_add(1);

    // Serialize payload: call_id(8) + dependency preamble + main actor type +
    // init CallSpec + init-args list.
    ptr<SerializationContext> ctx = make_ptr<NetworkSerializationContext>(this);
    std::vector<std::uint8_t> payload;
    writeU64(payload, callId);

    std::vector<RemoteTypeDependency> dependencies = collectRemoteTypeDependencies(actorTypeVal);
    writeU32(payload, static_cast<std::uint32_t>(dependencies.size()));
    for (const auto& dependency : dependencies) {
        writeString(payload, toUtf8StdString(dependency.moduleFullName));
        writeString(payload, toUtf8StdString(dependency.moduleName));
        writeString(payload, toUtf8StdString(dependency.symbolName));

        auto dependencyBytes = serializeValue(dependency.typeValue, ctx);
        writeU64(payload, fnv1a64(dependencyBytes));
        writeU32(payload, static_cast<std::uint32_t>(dependencyBytes.size()));
        payload.insert(payload.end(), dependencyBytes.begin(), dependencyBytes.end());
    }

    // Serialize the main actor type after the dependency preamble while reusing
    // the same serialization context so the actor graph can back-reference
    // shipped dependency types.
    auto actorOwner = findOwningModuleForType(actorTypeVal);
    if (!actorOwner.has_value()) {
        throw std::runtime_error("Remote actor type '" +
                                 toUtf8StdString(asObjectType(actorTypeVal)->name) +
                                 "' has no owning module");
    }
    writeString(payload, toUtf8StdString(actorOwner->first));
    writeString(payload, toUtf8StdString(actorOwner->second));
    writeString(payload, toUtf8StdString(asObjectType(actorTypeVal)->name));

    auto typeBytes = serializeValue(actorTypeVal, ctx);
    writeU64(payload, fnv1a64(typeBytes));
    writeU32(payload, static_cast<std::uint32_t>(typeBytes.size()));
    payload.insert(payload.end(), typeBytes.begin(), typeBytes.end());

    auto callSpecBytes = initCallSpec.toBytes();
    writeU32(payload, static_cast<std::uint32_t>(callSpecBytes.size()));
    payload.insert(payload.end(), callSpecBytes.begin(), callSpecBytes.end());

    // Serialize init args
    auto argsBytes = serializeValueList(initArgs, ctx);
    payload.insert(payload.end(), argsBytes.begin(), argsBytes.end());

    // Register promise before sending (avoid race with fast response)
    ptr<std::promise<Value>> promise = make_ptr<std::promise<Value>>();
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        ComputeConnection::PendingCall pending {};
        pending.promise = promise;
        pending.printTarget = VM::currentPrintTarget();
        pendingCalls_[callId] = pending;
    }

    sendFrame(ComputeMsg::SpawnActor, payload);

    // Block until SPAWN_RESULT
    Value result = future.get(); // may throw if server returned an error
    if (!result.isInt())
        throw std::runtime_error("SPAWN_ACTOR returned unexpected payload");

    int64_t remoteId = result.asInt();
    return makeRemoteActor(actorTypeVal, remoteId, ptr<ComputeConnection>(shared_from_this()));
}

// ---------------------------------------------------------------------------
// callRemoteMethod
// ---------------------------------------------------------------------------

Value ComputeConnection::callRemoteMethod(int64_t remoteActorId,
                                          const icu::UnicodeString& methodName,
                                          const std::vector<Value>& args,
                                          const CallSpec& callSpec)
{
    uint64_t callId = nextCallId_.fetch_add(1);

    // Serialize: call_id(8) + actor_id(8) + method_name + args
    ptr<SerializationContext> ctx = make_ptr<NetworkSerializationContext>(this);
    std::vector<std::uint8_t> payload;
    writeU64(payload, callId);
    writeU64(payload, static_cast<std::uint64_t>(remoteActorId));

    std::string methodUtf8;
    methodName.toUTF8String(methodUtf8);
    writeString(payload, methodUtf8);

    auto callSpecBytes = callSpec.toBytes();
    writeU32(payload, static_cast<std::uint32_t>(callSpecBytes.size()));
    payload.insert(payload.end(), callSpecBytes.begin(), callSpecBytes.end());

    auto argsBytes = serializeValueList(args, ctx);
    payload.insert(payload.end(), argsBytes.begin(), argsBytes.end());

    // Register promise before sending
    ptr<std::promise<Value>> promise = make_ptr<std::promise<Value>>();
    auto future = promise->get_future();
    {
        std::lock_guard<std::mutex> lk(pendingMu_);
        ComputeConnection::PendingCall pending {};
        pending.promise = promise;
        pending.printTarget = VM::currentPrintTarget();
        pendingCalls_[callId] = pending;
    }

    sendFrame(ComputeMsg::CallMethod, payload);

    return future.get(); // blocks; may throw on remote error
}

// ---------------------------------------------------------------------------
// Local actor registry (back-channel)
// ---------------------------------------------------------------------------

int64_t ComputeConnection::registerLocalActor(const Value& actorVal)
{
    int64_t id = nextLocalActorId_.fetch_add(1);
    std::lock_guard<std::mutex> lk(localActorsMu_);
    localActors_[id] = actorVal.weakRef();
    return id;
}

void ComputeConnection::unregisterLocalActor(int64_t id)
{
    std::lock_guard<std::mutex> lk(localActorsMu_);
    localActors_.erase(id);
}

Value ComputeConnection::resolveLocalActor(int64_t id)
{
    std::lock_guard<std::mutex> lk(localActorsMu_);
    auto it = localActors_.find(id);
    if (it == localActors_.end())
        return Value::nilVal();
    return it->second.strongRef();
}

void ComputeConnection::sendActorDropped(int64_t actorId)
{
    if (!alive_.load())
        return;

    std::vector<std::uint8_t> payload;
    writeU64(payload, static_cast<std::uint64_t>(actorId));
    sendFrame(ComputeMsg::ActorDropped, payload);
}

void ComputeConnection::sendPrintOutput(uint64_t callId, const std::string& text, bool flush)
{
    if (!alive_.load())
        return;

    std::vector<std::uint8_t> payload;
    writeU64(payload, callId);
    writeU8(payload, flush ? 1 : 0);
    writeString(payload, text);
    sendFrame(ComputeMsg::PrintOutput, payload);
}

// ---------------------------------------------------------------------------
// handleIncomingCall — dispatch a back-channel CALL_METHOD to a local actor
// ---------------------------------------------------------------------------

void ComputeConnection::handleIncomingCall(uint64_t callId, int64_t actorId,
                                           const icu::UnicodeString& method,
                                           const std::vector<Value>& args,
                                           const CallSpec& callSpec)
{
    Value actorVal;
    {
        std::lock_guard<std::mutex> lk(localActorsMu_);
        auto it = localActors_.find(actorId);
        if (it == localActors_.end()) {
            rejectCall(callId, "back-channel: unknown actor id " + std::to_string(actorId));
            return;
        }
        actorVal = it->second.strongRef(); // promote weak ref
    }

    if (actorVal.isNil()) {
        rejectCall(callId, "back-channel: actor has been collected");
        return;
    }

    // Bind the method closure on the local actor, queue the call, and send
    // CALL_RESULT back — same pattern as ComputeServer::handleClient CallMethod.
    // Run in a detached thread so the reader loop is not blocked.
    auto connWeak = weak_from_this();
    std::thread([connWeak, callId, actorVal, method, args, callSpec]() mutable {
        auto sendResult = [&](bool ok, const Value& resultOrNil, const std::string& errMsg) {
            auto conn = connWeak.lock();
            if (!conn) return;
            std::vector<std::uint8_t> response;
            writeU64(response, callId);
            writeU8(response, ok ? 1 : 0);
            if (ok) {
                auto resultBytes = ComputeConnection::serializeValue(resultOrNil);
                writeU32(response, static_cast<std::uint32_t>(resultBytes.size()));
                response.insert(response.end(), resultBytes.begin(), resultBytes.end());
            } else {
                writeString(response, errMsg);
            }
            conn->sendFrame(ComputeMsg::CallResult, response);
        };

        try {
            auto conn = connWeak.lock();
            if (!conn)
                return;

            ObjObjectType* type = asObjectType(asActorInstance(actorVal)->instanceType);
            // Walk supertype chain to find the method
            const ObjObjectType::Method* methodEntry = nullptr;
            for (ObjObjectType* t = type; t != nullptr;
                 t = t->superType.isNil() ? nullptr : asObjectType(t->superType)) {
                auto it = t->methods.find(method.hashCode());
                if (it != t->methods.end()) { methodEntry = &it->second; break; }
            }
            if (!methodEntry)
                throw std::runtime_error("back-channel: undefined actor method '"
                                         + toUTF8StdString(method) + "'");

            Value closureVal = methodEntry->closure;
            if (!isClosure(closureVal))
                throw std::runtime_error("back-channel: method is not a closure");

            ObjClosure* closure = asClosure(closureVal);
            ObjFunction* function = asFunction(closure->function);

            Value callee;
            if (function->builtinInfo) {
                const auto& info = *function->builtinInfo;
                callee = Value::boundNativeVal(actorVal, info.function,
                    function->funcType.has_value() &&
                        function->funcType.value()->func.has_value()
                        ? function->funcType.value()->func->isProc : false,
                    function->funcType.has_value() ? function->funcType.value() : nullptr,
                    info.defaultValues, closure->function);
            } else {
                callee = Value::boundMethodVal(actorVal, closureVal);
            }

            Value* argTop = args.empty() ? nullptr
                                         : const_cast<Value*>(args.data() + args.size());
            VM::ScopedPrintTarget printTargetScope(
                    ActorInstance::MethodCallInfo::PrintTarget::remoteCall(conn, callId));
            Value completion = asActorInstance(actorVal)->queueCall(callee, callSpec, argTop,
                                                                     /*forceCompletionFuture=*/true);
            if (completion.isNil()) {
                sendResult(false, Value::nilVal(), "back-channel: actor is not alive");
                return;
            }

            Value result = asFuture(completion)->future.get();
            sendResult(true, result, "");
        } catch (const std::exception& e) {
            sendResult(false, Value::nilVal(), e.what());
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// readerLoop — runs in its own thread, processes all incoming frames
// ---------------------------------------------------------------------------

void ComputeConnection::readerLoop()
{
    while (alive_) {
        // Read frame header: 4 bytes payload_len + 1 byte msg_type
        std::uint8_t header[5];
        if (!readExact(header, 5)) {
            closeSocket();
            return;
        }

        const std::uint8_t* hp  = header;
        const std::uint8_t* hend = header + 5;
        std::uint32_t payloadLen = readU32(hp, hend);
        ComputeMsg    msgType    = static_cast<ComputeMsg>(readU8(hp, hend));

        std::vector<std::uint8_t> payload(payloadLen);
        if (payloadLen > 0 && !readExact(payload.data(), payloadLen)) {
            closeSocket();
            return;
        }

        const std::uint8_t* p   = payload.data();
        const std::uint8_t* end = p + payloadLen;

        try {
            switch (msgType) {

            case ComputeMsg::HelloOk:
                resolveCall(0, Value::nilVal()); // signal handshake done
                break;

            case ComputeMsg::HelloErr: {
                std::string reason = (payloadLen > 0) ? readString(p, end) : "unknown error";
                rejectCall(0, "HELLO rejected: " + reason);
                closeSocket();
                return;
            }

            case ComputeMsg::SpawnResult: {
                uint64_t callId = readU64(p, end);
                uint8_t  ok     = readU8(p, end);
                if (ok) {
                    int64_t remoteId = static_cast<int64_t>(readU64(p, end));
                    // Phase 4 will construct the remote actor proxy here.
                    // For now, resolve with a placeholder int value so spawnActor
                    // can return; Phase 6 will replace this with makeRemoteActor().
                    resolveCall(callId, Value::intVal(static_cast<int64_t>(remoteId)));
                } else {
                    std::string err = readString(p, end);
                    rejectCall(callId, "SPAWN_ACTOR failed: " + err);
                }
                break;
            }

            case ComputeMsg::CallResult: {
                uint64_t callId = readU64(p, end);
                uint8_t  ok     = readU8(p, end);
                if (ok) {
                    std::uint32_t len = readU32(p, end);
                    if (p + len > end)
                        throw std::runtime_error("truncated CALL_RESULT value");
                    const std::uint8_t* vend = p + len;
                    ptr<SerializationContext> ctx = make_ptr<NetworkSerializationContext>(this);
                    Value result = deserializeValue(p, vend, ctx);
                    resolveCall(callId, std::move(result));
                } else {
                    std::string err = readString(p, end);
                    rejectCall(callId, "remote call failed: " + err);
                }
                break;
            }

            case ComputeMsg::PrintOutput: {
                uint64_t callId = readU64(p, end);
                bool flush = readU8(p, end) != 0;
                std::string text = readString(p, end);
                deliverPrintOutput(callId, text, flush);
                break;
            }

            case ComputeMsg::CallMethod: {
                // Back-channel: remote actor is calling a method on one of our local actors
                uint64_t callId  = readU64(p, end);
                int64_t actorId = static_cast<int64_t>(readU64(p, end));
                std::string methodUtf8 = readString(p, end);
                icu::UnicodeString method = icu::UnicodeString::fromUTF8(methodUtf8);

                std::uint32_t specLen = readU32(p, end);
                if (p + specLen > end)
                    throw std::runtime_error("truncated CALL_METHOD CallSpec");
                Chunk::CodeType specBytes(p, p + specLen);
                auto specIt = specBytes.begin();
                CallSpec cs{specIt};
                p += specLen;

                // Deserialize args
                ptr<SerializationContext> ctx = make_ptr<NetworkSerializationContext>(this);
                auto args = deserializeValueList(p, end, ctx);

                handleIncomingCall(callId, actorId, method, args, cs);
                break;
            }

            case ComputeMsg::ActorDropped: {
                int64_t actorId = static_cast<int64_t>(readU64(p, end));
                unregisterLocalActor(actorId);
                break;
            }

            case ComputeMsg::Bye:
                closeSocket();
                return;

            default:
                // Unknown message type — ignore and continue
                break;
            }
        } catch (const std::exception& e) {
            // Frame parsing error — close connection
            closeSocket();
            return;
        }
    }
}

} // namespace roxal

#endif // ROXAL_COMPUTE_SERVER
