#include "ComputeServer.h"

#ifdef ROXAL_COMPUTE_SERVER

#include "ComputeConnection.h"
#include "SimpleMarkSweepGC.h"
#include "VM.h"
#include "Thread.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace roxal {

namespace {

void visitComputeValue(ValueVisitor& visitor, const Value& value)
{
    if (value.isObj() && !value.isWeak()) {
        visitor.visit(value);
    }
}

void visitComputeValues(ValueVisitor& visitor, const std::vector<Value>& values)
{
    for (const auto& value : values) {
        visitComputeValue(visitor, value);
    }
}

template <typename FutureT>
Value waitForComputeFuture(FutureT& future, SimpleMarkSweepGC::ExternalParticipant& participant)
{
    using namespace std::chrono_literals;

    while (future.wait_for(5ms) != std::future_status::ready) {
        participant.pollSafepointIfRequested();
    }

    participant.pollSafepointIfRequested();
    return future.get();
}

class ServerActorRegistry final : public SimpleMarkSweepGC::ExternalRootProvider {
public:
    explicit ServerActorRegistry(SimpleMarkSweepGC& gc)
        : gc_(gc)
    {
        gc_.registerExternalRootProvider(this);
    }

    ~ServerActorRegistry() override
    {
        gc_.unregisterExternalRootProvider(this);
    }

    void visitRoots(ValueVisitor& visitor) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& entry : actors_) {
            visitComputeValue(visitor, entry.second);
        }
    }

    int64_t addActor(const Value& actorVal)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        int64_t actorId = nextActorId_++;
        actors_[actorId] = actorVal;
        return actorId;
    }

    Value lookupActor(int64_t actorId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = actors_.find(actorId);
        if (it == actors_.end()) {
            return Value::nilVal();
        }
        return it->second;
    }

    Value takeActor(int64_t actorId)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = actors_.find(actorId);
        if (it == actors_.end()) {
            return Value::nilVal();
        }
        Value actorVal = it->second;
        actors_.erase(it);
        return actorVal;
    }

    std::vector<Value> drainActors()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Value> drained;
        drained.reserve(actors_.size());
        for (auto& entry : actors_) {
            drained.push_back(entry.second);
        }
        actors_.clear();
        return drained;
    }

private:
    SimpleMarkSweepGC& gc_;
    std::mutex mutex_;
    std::unordered_map<int64_t, Value> actors_;
    int64_t nextActorId_ { 1 };
};

bool readExactFd(int fd, void* buf, std::size_t n)
{
    auto* p = static_cast<char*>(buf);
    std::size_t done = 0;
    while (done < n) {
        ssize_t r = ::recv(fd, p + done, n - done, 0);
        if (r <= 0)
            return false;
        done += static_cast<std::size_t>(r);
    }
    return true;
}

bool readFrameFd(int fd, ComputeMsg& msgType, std::vector<std::uint8_t>& payload)
{
    std::uint8_t header[5];
    if (!readExactFd(fd, header, sizeof(header)))
        return false;

    const std::uint8_t* hp = header;
    const std::uint8_t* hend = header + sizeof(header);
    std::uint32_t payloadLen = readU32(hp, hend);
    msgType = static_cast<ComputeMsg>(readU8(hp, hend));

    payload.resize(payloadLen);
    return payloadLen == 0 || readExactFd(fd, payload.data(), payloadLen);
}

std::vector<std::uint8_t> serializeValueForServer(const Value& v,
                                                  ptr<SerializationContext> ctx = nullptr)
{
    std::ostringstream oss(std::ios::binary);
    writeValue(oss, v, ctx);
    auto s = oss.str();
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

Value deserializeValueForServer(const std::uint8_t*& p, const std::uint8_t* end,
                                ptr<SerializationContext> ctx = nullptr)
{
    std::string buf(reinterpret_cast<const char*>(p), end - p);
    std::istringstream iss(buf, std::ios::binary);
    Value v = readValue(iss, ctx);
    p += static_cast<std::size_t>(iss.tellg());
    return v;
}

std::vector<std::uint8_t> serializeValueListForServer(const std::vector<Value>& vals,
                                                      ptr<SerializationContext> ctx = nullptr)
{
    std::vector<std::uint8_t> buf;
    writeU32(buf, static_cast<std::uint32_t>(vals.size()));
    for (const auto& v : vals) {
        auto bytes = serializeValueForServer(v, ctx);
        writeU32(buf, static_cast<std::uint32_t>(bytes.size()));
        buf.insert(buf.end(), bytes.begin(), bytes.end());
    }
    return buf;
}

std::vector<Value> deserializeValueListForServer(const std::uint8_t*& p, const std::uint8_t* end,
                                                 ptr<SerializationContext> ctx = nullptr)
{
    std::uint32_t count = readU32(p, end);
    std::vector<Value> vals;
    vals.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t len = readU32(p, end);
        if (p + len > end)
            throw std::runtime_error("ComputeServer: truncated value in list");
        const std::uint8_t* vend = p + len;
        vals.push_back(deserializeValueForServer(p, vend, ctx));
        p = vend;
    }
    return vals;
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

Value findOrCreateCanonicalModule(const icu::UnicodeString& fullName,
                                  const icu::UnicodeString& moduleName)
{
    Value existing = findCanonicalModuleValue(fullName, moduleName);
    if (existing.isNonNil())
        return existing.strongRef();

    icu::UnicodeString localName = moduleName;
    if (localName.isEmpty())
        localName = lastQualifiedSegment(fullName);
    Value created = Value::moduleTypeVal(localName);
    ObjModuleType* module = asModuleType(created);
    module->fullName = fullName.isEmpty() ? localName : fullName;
    ObjModuleType::allModules.push_back(created);
    return created.strongRef();
}

struct RemoteSpawnDependency {
    icu::UnicodeString moduleFullName;
    icu::UnicodeString moduleName;
    icu::UnicodeString symbolName;
    std::uint64_t fingerprint { 0 };
    Value deserializedValue;
    Value canonicalValue;
};

std::uint64_t computeTypeFingerprint(const Value& typeValue)
{
    ptr<SerializationContext> ctx = make_ptr<SerializationContext>();
    auto bytes = serializeValueForServer(typeValue, ctx);
    return fnv1a64(bytes);
}

struct RemoteTypeCanonicalizer {
    std::unordered_map<const Obj*, Value> typeReplacements;
    std::unordered_map<const Obj*, Value> moduleReplacements;
    std::unordered_set<const Obj*> visitedTypes;
    std::unordered_set<const Obj*> visitedFunctions;

    Value canonicalizeModuleValue(const Value& moduleValue)
    {
        Value strong = moduleValue.strongRef();
        if (!isModuleType(strong))
            return strong;

        auto replacementIt = moduleReplacements.find(strong.asObj());
        if (replacementIt != moduleReplacements.end())
            return replacementIt->second.strongRef();

        ObjModuleType* source = asModuleType(strong);
        Value canonical = findOrCreateCanonicalModule(source->fullName, source->name);
        moduleReplacements.emplace(strong.asObj(), canonical.strongRef());

        ObjModuleType* target = asModuleType(canonical);
        if (target != source) {
            if (target->fullName.isEmpty() && !source->fullName.isEmpty())
                target->fullName = source->fullName;
            if (target->sourcePath.isEmpty() && !source->sourcePath.isEmpty())
                target->sourcePath = source->sourcePath;

            for (const auto& alias : source->moduleAliasSnapshot())
                target->registerModuleAlias(alias.first, alias.second);

            for (const auto& entry : source->vars.snapshot()) {
                Value sourceValue = entry.second.strongRef();
                if (!isModuleType(sourceValue) && !isObjectType(sourceValue))
                    continue;

                Value canonicalValue = sourceValue;
                if (isModuleType(sourceValue))
                    canonicalValue = canonicalizeModuleValue(sourceValue);
                else if (isObjectType(sourceValue)) {
                    auto typeIt = typeReplacements.find(sourceValue.asObj());
                    if (typeIt != typeReplacements.end())
                        canonicalValue = typeIt->second.strongRef();
                }

                auto existing = target->vars.load(entry.first);
                if (!existing.has_value())
                    target->vars.store(entry.first, canonicalValue.strongRef(), true);

                if (source->constVars.find(entry.first.hashCode()) != source->constVars.end())
                    target->constVars.insert(entry.first.hashCode());
            }
        }

        return canonical.strongRef();
    }

    Value canonicalizeValue(const Value& value)
    {
        Value strong = value.strongRef();
        if (strong.isNil() || !strong.isObj())
            return strong;

        if (isObjectType(strong)) {
            auto it = typeReplacements.find(strong.asObj());
            if (it != typeReplacements.end())
                return it->second.strongRef();
            return strong;
        }

        if (isModuleType(strong))
            return canonicalizeModuleValue(strong);

        return strong;
    }

    void canonicalizeValueInPlace(Value& value)
    {
        Value canonical = canonicalizeValue(value);
        if (canonical.isNonNil())
            value = rewrapLike(value, canonical);
    }

    void canonicalizeFunction(ObjFunction* function)
    {
        if (function == nullptr || !visitedFunctions.insert(function).second)
            return;

        canonicalizeValueInPlace(function->moduleType);
        canonicalizeValueInPlace(function->ownerType);

        if (function->chunk) {
            for (Value& constant : function->chunk->constants) {
                if (isFunction(constant)) {
                    canonicalizeFunction(asFunction(constant));
                } else if (isObjectType(constant)) {
                    canonicalizeValueInPlace(constant);
                    canonicalizeObjectType(asObjectType(constant));
                } else if (isModuleType(constant)) {
                    canonicalizeValueInPlace(constant);
                }
            }
        }

        for (auto& [_, defaultFunc] : function->paramDefaultFunc)
            if (isFunction(defaultFunc))
                canonicalizeFunction(asFunction(defaultFunc));
    }

    void canonicalizeObjectType(ObjObjectType* objectType)
    {
        if (objectType == nullptr || !visitedTypes.insert(objectType).second)
            return;

        canonicalizeValueInPlace(objectType->superType);

        for (auto& [_, property] : objectType->properties) {
            canonicalizeValueInPlace(property.type);
            canonicalizeValueInPlace(property.ownerType);
        }

        for (auto& [_, method] : objectType->methods) {
            canonicalizeValueInPlace(method.ownerType);
            if (isClosure(method.closure))
                canonicalizeFunction(asFunction(asClosure(method.closure)->function));
        }
    }
};

const ObjObjectType::Method* findMethodRecursive(ObjObjectType* type, int32_t nameHash)
{
    for (ObjObjectType* t = type; t != nullptr;
         t = t->superType.isNil() ? nullptr : asObjectType(t->superType)) {
        auto it = t->methods.find(nameHash);
        if (it != t->methods.end())
            return &it->second;
    }
    return nullptr;
}

Value bindActorMethodForServer(const Value& actorVal, const icu::UnicodeString& methodName)
{
    ObjObjectType* type = asObjectType(asActorInstance(actorVal)->instanceType);
    const auto* method = findMethodRecursive(type, methodName.hashCode());
    if (method == nullptr)
        throw std::runtime_error("undefined actor method '" + toUTF8StdString(methodName) + "'");

    Value closureVal = method->closure;
    if (!isClosure(closureVal))
        throw std::runtime_error("actor method '" + toUTF8StdString(methodName) + "' is not a closure");

    ObjClosure* closure = asClosure(closureVal);
    ObjFunction* function = asFunction(closure->function);
    if (function->builtinInfo) {
        const auto& info = *function->builtinInfo;
        return Value::boundNativeVal(actorVal, info.function,
                                     function->funcType.has_value() &&
                                         function->funcType.value()->func.has_value()
                                         ? function->funcType.value()->func->isProc
                                         : false,
                                     function->funcType.has_value() ? function->funcType.value() : nullptr,
                                     info.defaultValues,
                                     closure->function);
    }

    return Value::boundMethodVal(actorVal, closureVal);
}

Value spawnActorForServer(const Value& actorTypeVal, const std::vector<Value>& initArgs,
                          const CallSpec& initCallSpec)
{
    if (!isObjectType(actorTypeVal) || !asObjectType(actorTypeVal)->isActor)
        throw std::runtime_error("SPAWN_ACTOR requires an actor type");

    Value actorVal = Value::actorInstanceVal(actorTypeVal);
    ptr<Thread> newThread = make_ptr<Thread>();
    VM::instance().registerThread(newThread);
    asActorInstance(actorVal)->thread = newThread;
    newThread->act(actorVal);

    const icu::UnicodeString initName = toUnicodeString("init");
    const auto* initMethod = findMethodRecursive(asObjectType(actorTypeVal), initName.hashCode());
    if (initMethod != nullptr) {
        Value boundInit = bindActorMethodForServer(actorVal, initName);
        Value* argTop = nullptr;
        std::vector<Value> initArgBuffer = initArgs;
        if (!initArgBuffer.empty())
            argTop = initArgBuffer.data() + initArgBuffer.size();
        asActorInstance(actorVal)->queueCall(boundInit, initCallSpec, argTop);
    }

    return actorVal;
}

CallSpec decodeCallSpecBytes(const std::uint8_t*& p, const std::uint8_t* end)
{
    std::uint32_t specLen = readU32(p, end);
    if (p + specLen > end)
        throw std::runtime_error("truncated CallSpec payload");
    Chunk::CodeType specBytes(p, p + specLen);
    auto it = specBytes.begin();
    CallSpec spec{it};
    p += specLen;
    return spec;
}

void shutdownActorForServer(const Value& actorVal)
{
    if (!isActorInstance(actorVal))
        return;

    auto* actor = asActorInstance(actorVal);
    if (auto thread = actor->thread.lock()) {
        uint64_t tid = thread->id();
        thread->join(actor);
        VM::instance().unregisterThread(tid);
    }
}

void sendSpawnError(const ptr<ComputeConnection>& conn, uint64_t callId, const std::string& message)
{
    std::vector<std::uint8_t> payload;
    writeU64(payload, callId);
    writeU8(payload, 0);
    writeString(payload, message);
    conn->sendMessage(ComputeMsg::SpawnResult, payload);
}

void sendCallError(const ptr<ComputeConnection>& conn, uint64_t callId, const std::string& message)
{
    std::vector<std::uint8_t> payload;
    writeU64(payload, callId);
    writeU8(payload, 0);
    writeString(payload, message);
    conn->sendMessage(ComputeMsg::CallResult, payload);
}

} // namespace

void ComputeServer::listen(std::uint16_t port)
{
    int serverFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0)
        throw std::runtime_error("ComputeServer: socket(): " + std::string(std::strerror(errno)));

    int enable = 1;
    ::setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (::bind(serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(serverFd);
        throw std::runtime_error("ComputeServer: bind(): " + std::string(std::strerror(errno)));
    }

    sockaddr_in boundAddr {};
    socklen_t boundAddrLen = sizeof(boundAddr);
    if (::getsockname(serverFd, reinterpret_cast<sockaddr*>(&boundAddr), &boundAddrLen) < 0) {
        ::close(serverFd);
        throw std::runtime_error("ComputeServer: getsockname(): " + std::string(std::strerror(errno)));
    }
    std::uint16_t boundPort = ntohs(boundAddr.sin_port);

    if (::listen(serverFd, 16) < 0) {
        ::close(serverFd);
        throw std::runtime_error("ComputeServer: listen(): " + std::string(std::strerror(errno)));
    }

    std::cout << "Roxal compute server listening on port " << boundPort << std::endl;

    while (true) {
        sockaddr_in clientAddr {};
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = ::accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientLen);
        if (clientFd < 0) {
            if (errno == EINTR)
                continue;
            std::cerr << "ComputeServer: accept failed: " << std::strerror(errno) << std::endl;
            continue;
        }

        std::thread([this, clientFd]() {
            try {
                handleClient(clientFd);
            } catch (const std::exception& e) {
                std::cerr << "ComputeServer client error: " << e.what() << std::endl;
                ::close(clientFd);
            }
        }).detach();
    }
}

void ComputeServer::handleClient(int clientFd)
{
    ptr<ComputeConnection> conn = make_ptr<ComputeConnection>(clientFd, false);
    ServerActorRegistry actors(SimpleMarkSweepGC::instance());

    auto cleanupActors = [&]() {
        SimpleMarkSweepGC::ExternalParticipant gcParticipant(SimpleMarkSweepGC::instance());
        std::vector<Value> actorValues = actors.drainActors();
        gcParticipant.setRootVisitor([&](ValueVisitor& visitor) {
            visitComputeValues(visitor, actorValues);
        });
        for (auto& actorVal : actorValues) {
            shutdownActorForServer(actorVal);
        }
    };

    ComputeMsg msgType {};
    std::vector<std::uint8_t> payload;
    if (!readFrameFd(clientFd, msgType, payload)) {
        cleanupActors();
        return;
    }

    if (msgType != ComputeMsg::Hello) {
        std::vector<std::uint8_t> errPayload;
        writeString(errPayload, "expected HELLO");
        conn->sendMessage(ComputeMsg::HelloErr, errPayload);
        cleanupActors();
        return;
    }

    std::string helloError = validateHello(payload.data(), payload.size());
    if (!helloError.empty()) {
        std::vector<std::uint8_t> errPayload;
        writeString(errPayload, helloError);
        conn->sendMessage(ComputeMsg::HelloErr, errPayload);
        cleanupActors();
        return;
    }

    conn->sendMessage(ComputeMsg::HelloOk, {});

    while (readFrameFd(clientFd, msgType, payload)) {
        const std::uint8_t* p = payload.data();
        const std::uint8_t* end = p + payload.size();

        try {
            switch (msgType) {
            case ComputeMsg::SpawnActor: {
                SimpleMarkSweepGC::ExternalParticipant gcParticipant(SimpleMarkSweepGC::instance());
                uint64_t callId = readU64(p, end);
                std::uint32_t dependencyCount = readU32(p, end);
                ptr<SerializationContext> ctx = make_ptr<NetworkSerializationContext>(conn.get());
                std::vector<RemoteSpawnDependency> dependencies;
                Value actorOwnerModule = Value::nilVal();
                Value actorTypeVal = Value::nilVal();
                Value actorVal = Value::nilVal();
                std::vector<Value> initArgs;
                gcParticipant.setRootVisitor([&](ValueVisitor& visitor) {
                    for (const auto& dependency : dependencies) {
                        visitComputeValue(visitor, dependency.deserializedValue);
                        visitComputeValue(visitor, dependency.canonicalValue);
                    }
                    visitComputeValue(visitor, actorOwnerModule);
                    visitComputeValue(visitor, actorTypeVal);
                    visitComputeValue(visitor, actorVal);
                    visitComputeValues(visitor, initArgs);
                });
                dependencies.reserve(dependencyCount);

                RemoteTypeCanonicalizer canonicalizer;
                for (std::uint32_t i = 0; i < dependencyCount; ++i) {
                    RemoteSpawnDependency dependency {};
                    dependency.moduleFullName = icu::UnicodeString::fromUTF8(readString(p, end));
                    dependency.moduleName = icu::UnicodeString::fromUTF8(readString(p, end));
                    dependency.symbolName = icu::UnicodeString::fromUTF8(readString(p, end));
                    dependency.fingerprint = readU64(p, end);

                    Value canonicalModule = findOrCreateCanonicalModule(
                            dependency.moduleFullName, dependency.moduleName);
                    ObjModuleType* module = asModuleType(canonicalModule);
                    auto existing = module->vars.load(dependency.symbolName);
                    bool reuseExisting = existing.has_value() && isObjectType(existing.value())
                                         && computeTypeFingerprint(existing.value().strongRef()) == dependency.fingerprint;
                    if (!reuseExisting && existing.has_value() && isObjectType(existing.value()))
                        module->vars.store(dependency.symbolName, Value::nilVal(), true);

                    std::uint32_t dependencyLen = readU32(p, end);
                    if (p + dependencyLen > end)
                        throw std::runtime_error("truncated dependency type payload");

                    const std::uint8_t* dependencyEnd = p + dependencyLen;
                    dependency.deserializedValue = deserializeValueForServer(p, dependencyEnd, ctx).strongRef();
                    p = dependencyEnd;

                    if (!isObjectType(dependency.deserializedValue))
                        throw std::runtime_error("SPAWN_ACTOR dependency payload is not an object type");

                    if (reuseExisting) {
                        dependency.canonicalValue = existing.value().strongRef();
                    } else {
                        dependency.canonicalValue = dependency.deserializedValue.strongRef();
                        module->vars.store(dependency.symbolName, dependency.canonicalValue, true);
                    }

                    canonicalizer.typeReplacements[dependency.deserializedValue.asObj()] =
                            dependency.canonicalValue.strongRef();
                    dependencies.push_back(std::move(dependency));
                }

                icu::UnicodeString actorModuleFullName = icu::UnicodeString::fromUTF8(readString(p, end));
                icu::UnicodeString actorModuleName = icu::UnicodeString::fromUTF8(readString(p, end));
                icu::UnicodeString actorSymbolName = icu::UnicodeString::fromUTF8(readString(p, end));
                std::uint64_t actorFingerprint = readU64(p, end);

                actorOwnerModule = findOrCreateCanonicalModule(actorModuleFullName, actorModuleName);
                ObjModuleType* ownerModule = asModuleType(actorOwnerModule);
                auto existingActor = ownerModule->vars.load(actorSymbolName);
                bool reuseExistingActor = existingActor.has_value() && isObjectType(existingActor.value())
                                          && computeTypeFingerprint(existingActor.value().strongRef()) == actorFingerprint;
                if (!reuseExistingActor && existingActor.has_value() && isObjectType(existingActor.value()))
                    ownerModule->vars.store(actorSymbolName, Value::nilVal(), true);

                std::uint32_t typeLen = readU32(p, end);
                if (p + typeLen > end)
                    throw std::runtime_error("truncated actor type payload");

                const std::uint8_t* typeEnd = p + typeLen;
                actorTypeVal = deserializeValueForServer(p, typeEnd, ctx).strongRef();
                p = typeEnd;

                if (!isObjectType(actorTypeVal) || !asObjectType(actorTypeVal)->isActor)
                    throw std::runtime_error("SPAWN_ACTOR requires a serialized actor type");

                for (const auto& dependency : dependencies) {
                    Value canonicalModule = findOrCreateCanonicalModule(
                            dependency.moduleFullName, dependency.moduleName);
                    ObjModuleType* module = asModuleType(canonicalModule);
                    module->vars.store(dependency.symbolName, dependency.canonicalValue.strongRef(), true);
                }

                for (const auto& dependency : dependencies)
                    canonicalizer.canonicalizeObjectType(asObjectType(dependency.canonicalValue));
                canonicalizer.canonicalizeObjectType(asObjectType(actorTypeVal));

                if (reuseExistingActor) {
                    canonicalizer.typeReplacements[actorTypeVal.asObj()] = existingActor.value().strongRef();
                    actorTypeVal = existingActor.value().strongRef();
                } else {
                    ownerModule->vars.store(actorSymbolName, actorTypeVal.strongRef(), true);
                }
                CallSpec initCallSpec = decodeCallSpecBytes(p, end);
                initArgs = deserializeValueListForServer(p, end, ctx);
                gcParticipant.pollSafepointIfRequested();
                VM::ScopedPrintTarget printTargetScope(
                        ActorInstance::MethodCallInfo::PrintTarget::remoteCall(conn, callId));
                actorVal = spawnActorForServer(actorTypeVal, initArgs, initCallSpec);
                int64_t actorId = actors.addActor(actorVal);
                gcParticipant.pollSafepointIfRequested();

                std::vector<std::uint8_t> response;
                writeU64(response, callId);
                writeU8(response, 1);
                writeU64(response, static_cast<std::uint64_t>(actorId));
                conn->sendMessage(ComputeMsg::SpawnResult, response);
                break;
            }

            case ComputeMsg::CallMethod: {
                SimpleMarkSweepGC::ExternalParticipant gcParticipant(SimpleMarkSweepGC::instance());
                uint64_t callId = readU64(p, end);
                int64_t actorId = static_cast<int64_t>(readU64(p, end));
                std::string methodUtf8 = readString(p, end);
                icu::UnicodeString methodName = icu::UnicodeString::fromUTF8(methodUtf8);
                Value actorVal = actors.lookupActor(actorId);
                if (actorVal.isNil()) {
                    sendCallError(conn, callId, "unknown actor id " + std::to_string(actorId));
                    break;
                }

                ptr<SerializationContext> ctx = make_ptr<NetworkSerializationContext>(conn.get());
                CallSpec callSpec = decodeCallSpecBytes(p, end);
                std::vector<Value> args = deserializeValueListForServer(p, end, ctx);
                Value callee = bindActorMethodForServer(actorVal, methodName);
                gcParticipant.setRootVisitor([&](ValueVisitor& visitor) {
                    visitComputeValue(visitor, actorVal);
                    visitComputeValue(visitor, callee);
                    visitComputeValues(visitor, args);
                });

                // Dispatch in a background thread so the read loop remains free to
                // process CALL_RESULT frames from the client (back-channel replies).
                std::promise<void> workerReadyPromise;
                auto workerReady = workerReadyPromise.get_future();
                std::thread([conn,
                             callId,
                             actorVal,
                             callee,
                             args = std::move(args),
                             callSpec,
                             workerReadyPromise = std::move(workerReadyPromise)]() mutable {
                    SimpleMarkSweepGC::ExternalParticipant workerParticipant(SimpleMarkSweepGC::instance());
                    Value completion = Value::nilVal();
                    Value result = Value::nilVal();
                    workerParticipant.setRootVisitor([&](ValueVisitor& visitor) {
                        visitComputeValue(visitor, actorVal);
                        visitComputeValue(visitor, callee);
                        visitComputeValues(visitor, args);
                        visitComputeValue(visitor, completion);
                        visitComputeValue(visitor, result);
                    });
                    workerReadyPromise.set_value();
                    try {
                        Value* argTop = args.empty() ? nullptr : args.data() + args.size();
                        VM::ScopedPrintTarget printTargetScope(
                                ActorInstance::MethodCallInfo::PrintTarget::remoteCall(conn, callId));
                        workerParticipant.pollSafepointIfRequested();
                        completion = asActorInstance(actorVal)->queueCall(
                                callee, callSpec, argTop, /*forceCompletionFuture=*/true);
                        workerParticipant.pollSafepointIfRequested();
                        if (completion.isNil()) {
                            sendCallError(conn, callId, "actor is not alive");
                            return;
                        }
                        result = waitForComputeFuture(asFuture(completion)->future, workerParticipant);
                        ptr<SerializationContext> rctx = make_ptr<NetworkSerializationContext>(conn.get());
                        auto resultBytes = serializeValueForServer(result, rctx);
                        std::vector<std::uint8_t> response;
                        writeU64(response, callId);
                        writeU8(response, 1);
                        writeU32(response, static_cast<std::uint32_t>(resultBytes.size()));
                        response.insert(response.end(), resultBytes.begin(), resultBytes.end());
                        conn->sendMessage(ComputeMsg::CallResult, response);
                    } catch (const std::exception& e) {
                        sendCallError(conn, callId, e.what());
                    }
                }).detach();
                using namespace std::chrono_literals;
                while (workerReady.wait_for(5ms) != std::future_status::ready) {
                    gcParticipant.pollSafepointIfRequested();
                }
                gcParticipant.pollSafepointIfRequested();
                break;
            }

            case ComputeMsg::CallResult: {
                SimpleMarkSweepGC::ExternalParticipant gcParticipant(SimpleMarkSweepGC::instance());
                // Client is responding to a back-channel CALL_METHOD we sent earlier.
                uint64_t callId = readU64(p, end);
                uint8_t  ok     = readU8(p, end);
                if (ok) {
                    std::uint32_t len = readU32(p, end);
                    if (p + len > end)
                        throw std::runtime_error("truncated back-channel CALL_RESULT value");
                    const std::uint8_t* vend = p + len;
                    ptr<SerializationContext> ctx = make_ptr<NetworkSerializationContext>(conn.get());
                    Value result = Value::nilVal();
                    gcParticipant.setRootVisitor([&](ValueVisitor& visitor) {
                        visitComputeValue(visitor, result);
                    });
                    result = deserializeValueForServer(p, vend, ctx);
                    gcParticipant.pollSafepointIfRequested();
                    conn->resolveCall(callId, std::move(result));
                    gcParticipant.pollSafepointIfRequested();
                } else {
                    std::string err = readString(p, end);
                    conn->rejectCall(callId, "back-channel call failed: " + err);
                }
                break;
            }

            case ComputeMsg::PrintOutput: {
                uint64_t callId = readU64(p, end);
                bool flush = readU8(p, end) != 0;
                std::string text = readString(p, end);
                conn->deliverPrintOutput(callId, text, flush);
                break;
            }

            case ComputeMsg::ActorDropped: {
                SimpleMarkSweepGC::ExternalParticipant gcParticipant(SimpleMarkSweepGC::instance());
                int64_t actorId = static_cast<int64_t>(readU64(p, end));
                Value actorVal = actors.takeActor(actorId);
                gcParticipant.setRootVisitor([&](ValueVisitor& visitor) {
                    visitComputeValue(visitor, actorVal);
                });
                if (actorVal.isNonNil()) {
                    shutdownActorForServer(actorVal);
                }
                break;
            }

            case ComputeMsg::Bye:
                cleanupActors();
                return;

            default:
                break;
            }
        } catch (const std::exception& e) {
            if (msgType == ComputeMsg::SpawnActor) {
                const std::uint8_t* rp = payload.data();
                const std::uint8_t* rend = rp + payload.size();
                uint64_t callId = readU64(rp, rend);
                sendSpawnError(conn, callId, e.what());
                continue;
            }
            if (msgType == ComputeMsg::CallMethod) {
                const std::uint8_t* rp = payload.data();
                const std::uint8_t* rend = rp + payload.size();
                uint64_t callId = readU64(rp, rend);
                sendCallError(conn, callId, e.what());
                continue;
            }
            break;
        }
    }

    cleanupActors();
}

} // namespace roxal

#endif // ROXAL_COMPUTE_SERVER
