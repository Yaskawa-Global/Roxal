#ifdef ROXAL_ENABLE_GRPC

#include "ModuleGrpc.h"
#include "VM.h"
#include "Object.h"
#include "core/AST.h"

#include <cctype>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include <grpcpp/channel.h>
#include <grpcpp/support/status_code_enum.h>

#include <filesystem>
#include <stdexcept>
#include <vector>

using namespace roxal;

namespace {
bool isApplicationStatus(grpc::StatusCode code)
{
    switch (code) {
        case grpc::StatusCode::INVALID_ARGUMENT:
        case grpc::StatusCode::FAILED_PRECONDITION:
        case grpc::StatusCode::OUT_OF_RANGE:
        case grpc::StatusCode::PERMISSION_DENIED:
        case grpc::StatusCode::UNAUTHENTICATED:
        case grpc::StatusCode::ALREADY_EXISTS:
        case grpc::StatusCode::NOT_FOUND:
        case grpc::StatusCode::ABORTED:
        case grpc::StatusCode::DATA_LOSS:
        case grpc::StatusCode::RESOURCE_EXHAUSTED:
        case grpc::StatusCode::UNKNOWN:
            return true;
        default:
            return false;
    }
}
}

ModuleGrpc::ModuleGrpc()
    : targetAddress("127.0.0.1:50051")
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("grpc")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleGrpc::~ModuleGrpc()
{
    if (!moduleTypeValue.isNil())
        destroyModuleType(moduleTypeValue);
}

void ModuleGrpc::ensureConnector()
{
    if (!adapter)
        adapter = std::make_unique<ProtoAdapter>("");

    if (!channel)
        channel = grpc::CreateChannel(targetAddress, grpc::InsecureChannelCredentials());

    if (!connector)
        connector = std::make_unique<ACUCommunicator>(channel, adapter.get());
}

void ModuleGrpc::registerBuiltins(VM& vm)
{
    setVM(vm);
}

void ModuleGrpc::setTarget(const std::string& addr)
{
    targetAddress = addr;
    channel.reset();
    connector.reset();
    ensureConnector();
}

void ModuleGrpc::addProtoPath(const std::string& path)
{
    if (path.empty())
        return;

    if (!adapter)
        adapter = std::make_unique<ProtoAdapter>(path);
    else
        adapter->addProtoSearchPath(path);
}

Value ModuleGrpc::importProto(const std::string& protoFilename)
{
    if (!std::filesystem::exists(std::filesystem::path(protoFilename)))
        throw std::invalid_argument("gRPC import - proto file '"+protoFilename+"' not found.");

    ensureConnector();

    std::filesystem::path pp(protoFilename);
    auto sanitizeName = [](std::string s) {
        if (s.empty())
            return s;
        for (auto& ch : s) {
            if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_'))
                ch = '_';
        }
        if (!s.empty() && std::isdigit(static_cast<unsigned char>(s[0])))
            s.insert(s.begin(), '_');
        return s;
    };

    auto types = adapter->allocateObjects(protoFilename);

    std::string moduleName = adapter->packageName();
    if (moduleName.empty())
        moduleName = pp.stem().string();
    moduleName = sanitizeName(moduleName);
    Value moduleVal = getOrCreateModule(moduleName);

    registerGeneratedTypes(moduleVal, types);

    auto services = adapter->addServices(protoFilename);
    registerServices(moduleVal, services);

    return moduleVal;
}

Value ModuleGrpc::getOrCreateModule(const std::string& name)
{
    auto it = protoModules.find(name);
    if (it != protoModules.end())
        return it->second;

    Value moduleVal = Value::moduleTypeVal(toUnicodeString(name));
    ObjModuleType::allModules.push_back(moduleVal);
    protoModules[name] = moduleVal;
    // make available as global
    vm().globals.storeGlobal(toUnicodeString(name), moduleVal);
    return moduleVal;
}

void ModuleGrpc::registerGeneratedTypes(Value moduleVal, const std::vector<Value>& types)
{
    ObjModuleType* mod = asModuleType(moduleVal);
    for (const auto& typeVal : types) {
        if (!isObjectType(typeVal) && !isEnumType(typeVal))
            continue;

        ObjObjectType* type = asObjectType(typeVal);
        mod->vars.store(type->name, typeVal, true);
    }
}

void ModuleGrpc::registerServices(Value moduleVal, const std::vector<ProtoAdapter::ServiceInfo>& services)
{
    ObjModuleType* mod = asModuleType(moduleVal);
    auto makeObjTypeMeta = [](const std::string& fullName) -> ptr<type::Type> {
        ptr<type::Type> t = make_ptr<type::Type>(type::BuiltinType::Object);
        t->obj = type::Type::ObjectType();
        auto pos = fullName.find_last_of('.');
        std::string shortName = pos == std::string::npos ? fullName : fullName.substr(pos + 1);
        t->obj->name = toUnicodeString(shortName);
        return t;
    };

    for (const auto& svc : services) {
        Value serviceTypeVal = makeServiceType(svc.name);
        ObjObjectType* svcType = asObjectType(serviceTypeVal);

        // init(addr:string="127.0.0.1:50051", opts:dict={})
        std::vector<std::optional<type::Type::FuncType::ParamType>> initParams;
        initParams.emplace_back(type::Type::FuncType::ParamType(toUnicodeString("addr")));
        initParams.back()->type = make_ptr<type::Type>(type::BuiltinType::String);
        type::Type::FuncType::ParamType optParam(toUnicodeString("opts"));
        optParam.type = make_ptr<type::Type>(type::BuiltinType::Dict);
        optParam.hasDefault = true;
        initParams.emplace_back(optParam);

        addNativeMethod(svcType, "init", [this](VM&, ArgsView args) -> Value {
            if (args.size() < 1 || !isActorInstance(args[0]))
                throw std::invalid_argument("init expects service instance");
            ActorInstance* self = asActorInstance(args[0]);

            auto setProp = [&](const icu::UnicodeString& name, const Value& v) {
                auto& slot = self->properties[name.hashCode()];
                slot.assign(v);
            };

            Value addrVal = Value::stringVal(toUnicodeString(this->targetAddress));
            Value optsVal = Value::nilVal();
            if (args.size() >= 2 && !args[1].isNil()) {
                if (!isString(args[1]))
                    throw std::invalid_argument("init address must be string");
                addrVal = args[1];
            }
            if (args.size() >= 3 && !args[2].isNil()) {
                if (!isDict(args[2]))
                    throw std::invalid_argument("init opts must be dict");
                optsVal = args[2];
            }
            setProp(toUnicodeString("__addr"), addrVal);

            grpc::ChannelArguments chanArgs;
            std::optional<int64_t> timeoutMs;
            bool hasChanArgs = false;
            auto applyOpts = [&](ObjDict* dict) {
                for (const auto& kv : dict->items()) {
                    if (!isString(kv.first))
                        continue;
                    std::string key = toUTF8StdString(asStringObj(kv.first)->s);
                    const Value& val = kv.second;
                    if (key == "addr" && isString(val)) {
                        addrVal = val;
                    } else if (key == "timeout_ms" && val.isNumber()) {
                        timeoutMs = val.asInt();
                    } else if (key == "keepalive_time_ms" && val.isNumber()) {
                        chanArgs.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, val.asInt());
                        hasChanArgs = true;
                    } else if (key == "keepalive_timeout_ms" && val.isNumber()) {
                        chanArgs.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, val.asInt());
                        hasChanArgs = true;
                    } else if (key == "max_receive_message_length" && val.isNumber()) {
                        chanArgs.SetInt(GRPC_ARG_MAX_RECEIVE_MESSAGE_LENGTH, val.asInt());
                        hasChanArgs = true;
                    } else if (key == "max_send_message_length" && val.isNumber()) {
                        chanArgs.SetInt(GRPC_ARG_MAX_SEND_MESSAGE_LENGTH, val.asInt());
                        hasChanArgs = true;
                    } else if (key == "user_agent" && isString(val)) {
                        chanArgs.SetString("grpc.primary_user_agent", toUTF8StdString(asStringObj(val)->s));
                        hasChanArgs = true;
                    }
                }
            };
            if (!optsVal.isNil())
                applyOpts(asDict(optsVal));

            // create per-instance connector
            std::string addr = toUTF8StdString(asStringObj(addrVal)->s);
            std::shared_ptr<grpc::Channel>* ch = new std::shared_ptr<grpc::Channel>(
                hasChanArgs
                    ? grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), chanArgs)
                    : grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
            ACUCommunicator* comm = new ACUCommunicator(*ch, this->adapter.get());
            Value fpCh = Value::foreignPtrVal(ch);
            Value fpComm = Value::foreignPtrVal(comm);
            asForeignPtr(fpCh)->registerCleanup([](void* p){ delete static_cast<std::shared_ptr<grpc::Channel>*>(p); });
            asForeignPtr(fpComm)->registerCleanup([](void* p){ delete static_cast<ACUCommunicator*>(p); });
            setProp(toUnicodeString("__channel"), fpCh);
            setProp(toUnicodeString("__connector"), fpComm);
            if (timeoutMs.has_value())
                setProp(toUnicodeString("__timeout_ms"), Value::intVal(static_cast<int64_t>(timeoutMs.value())));
            return Value::nilVal();
        }, initParams.size(), initParams);

        // close(): release connector/channel
        addNativeMethod(svcType, "close", [](VM&, ArgsView args) -> Value {
            if (args.size() < 1 || !isActorInstance(args[0]))
                throw std::invalid_argument("close expects service instance");
            ActorInstance* self = asActorInstance(args[0]);
            auto clearProp = [&](const icu::UnicodeString& name) {
                auto hit = self->properties.find(name.hashCode());
                if (hit != self->properties.end())
                    hit->second.assign(Value::nilVal());
            };
            clearProp(toUnicodeString("__connector"));
            clearProp(toUnicodeString("__channel"));
            return Value::nilVal();
        }, 0);

        for (const auto& method : svc.methods) {
            Value requestTypeVal = adapter->declForFullName(method.inputTypeFullName);
            if (requestTypeVal.isNil() || !isObjectType(requestTypeVal))
                throw std::runtime_error("Unknown request type for gRPC method " + method.name);
            ObjObjectType* requestType = asObjectType(requestTypeVal);
            Value requestTypeWeak = requestTypeVal.weakRef();

            std::vector<int32_t> fieldHashes;
            fieldHashes.reserve(requestType->propertyOrder.size());

            std::vector<std::optional<type::Type::FuncType::ParamType>> mparams;
            std::vector<Value> paramDefaults;

            type::Type::FuncType::ParamType requestParam(toUnicodeString("request"));
            requestParam.type = makeObjTypeMeta(method.inputTypeFullName);
            requestParam.hasDefault = true;
            mparams.emplace_back(requestParam);
            paramDefaults.push_back(Value::nilVal());

            for (int32_t hash : requestType->propertyOrder) {
                auto pit = requestType->properties.find(hash);
                if (pit == requestType->properties.end())
                    continue;
                const auto& prop = pit->second;
                type::Type::FuncType::ParamType fieldParam(prop.name);
                fieldParam.hasDefault = true;
                mparams.emplace_back(fieldParam);
                paramDefaults.push_back(Value::nilVal());
                fieldHashes.push_back(hash);
            }

            std::vector<ptr<type::Type>> returns;
            returns.push_back(makeObjTypeMeta(method.outputTypeFullName));

            addNativeMethod(svcType, method.name,
                [this, method, requestTypeWeak, fieldHashes](VM& vm, ArgsView args) -> Value {
                    Value requestTypeVal = requestTypeWeak.strongRef();
                    if (requestTypeVal.isNil())
                        throw std::runtime_error("gRPC request type unavailable for method " + method.name);
                    ObjObjectType* requestType = asObjectType(requestTypeVal);
                    size_t expectedArgs = fieldHashes.size() + 2;
                    if (args.size() < 2 || args.size() > expectedArgs || !isActorInstance(args[0]))
                        throw std::invalid_argument(method.name + " expects receiver plus parameters");
                    ActorInstance* self = asActorInstance(args[0]);
                    Value requestArg = args[1];
                    Value requestValue;
                    if (!requestArg.isNil()) {
                        if (!isObjectInstance(requestArg))
                            throw std::invalid_argument(method.name + " expects request object instance");
                        ObjectInstance* inst = asObjectInstance(requestArg);
                        if (!inst->instanceType.is(requestTypeVal))
                            throw std::invalid_argument(method.name + " expects request of type " +
                                                        toUTF8StdString(asObjectType(requestTypeVal)->name));
                        requestValue = requestArg;
                    } else {
                        requestValue = Value::objectInstanceVal(requestTypeVal);
                        ObjectInstance* inst = asObjectInstance(requestValue);
                        size_t providedFields = args.size() - 2;
                        for (size_t i = 0; i < fieldHashes.size(); ++i) {
                            Value argVal = (i < providedFields) ? args[2 + i] : Value::nilVal();
                            if (argVal.isNil())
                                continue;
                            auto propIt = requestType->properties.find(fieldHashes[i]);
                            Value coerced = argVal;
                            if (propIt != requestType->properties.end()) {
                                const auto& prop = propIt->second;
                                if (!prop.type.isNil())
                                    coerced = toType(prop.type, argVal, false);
                            }
                            inst->properties[fieldHashes[i]].assign(coerced);
                        }
                    }

                    // reuse connector if available, else create and store
                    Value connVal = self->properties[toUnicodeString("__connector").hashCode()].value;
                    ACUCommunicator* comm = nullptr;
                    if (isForeignPtr(connVal))
                        comm = static_cast<ACUCommunicator*>(asForeignPtr(connVal)->ptr);

                    if (!comm) {
                        Value addrVal = self->properties[toUnicodeString("__addr").hashCode()].value;
                        std::string addr = targetAddress;
                        if (isString(addrVal))
                            addr = toUTF8StdString(asStringObj(addrVal)->s);
                        std::shared_ptr<grpc::Channel>* ch = new std::shared_ptr<grpc::Channel>(
                            grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
                        comm = new ACUCommunicator(*ch, this->adapter.get());
                        Value fpCh = Value::foreignPtrVal(ch);
                        Value fpComm = Value::foreignPtrVal(comm);
                        asForeignPtr(fpCh)->registerCleanup([](void* p){ delete static_cast<std::shared_ptr<grpc::Channel>*>(p); });
                        asForeignPtr(fpComm)->registerCleanup([](void* p){ delete static_cast<ACUCommunicator*>(p); });
                        self->properties[toUnicodeString("__channel").hashCode()].assign(fpCh);
                        self->properties[toUnicodeString("__connector").hashCode()].assign(fpComm);
                    }

                    Value timeoutVal = self->properties[toUnicodeString("__timeout_ms").hashCode()].value;
                    std::optional<std::chrono::milliseconds> timeout;
                    if (timeoutVal.isNumber()) {
                        int64_t ms = timeoutVal.asInt();
                        if (ms > 0)
                            timeout = std::chrono::milliseconds(ms);
                    }
                    Value reqStorage[1] = { requestValue };
                    ArgsView reqArgs(reqStorage, 1);
                    try {
                        return comm->call(method.name, reqArgs, timeout);
                    } catch (const GrpcStatusError& ge) {
                        const bool isAppStatus = isApplicationStatus(ge.code());
                        const char* typeName = isAppStatus ? "ProgramException" : "RuntimeException";
                        auto exTypeOpt = vm.loadGlobal(toUnicodeString(typeName));
                        Value exType = exTypeOpt.has_value() ? exTypeOpt.value() : Value::nilVal();
                        Value msg = Value::stringVal(toUnicodeString(std::string(ge.what())));
                        Value detail = Value::dictVal();
                        ObjDict* dict = asDict(detail);
                        dict->store(Value::stringVal(toUnicodeString("grpc_status_code")),
                                    Value::intVal(static_cast<int64_t>(ge.code())));
                        dict->store(Value::stringVal(toUnicodeString("grpc_status_name")),
                                    Value::stringVal(toUnicodeString(grpcStatusCodeName(ge.code()))));
                        dict->store(Value::stringVal(toUnicodeString("grpc_method")),
                                    Value::stringVal(toUnicodeString(ge.method())));
                        dict->store(Value::stringVal(toUnicodeString("grpc_status_message")),
                                    Value::stringVal(toUnicodeString(ge.grpcMessage())));
                        dict->store(Value::stringVal(toUnicodeString("grpc_application_error")),
                                    Value::boolVal(isAppStatus));
                        Value exc = Value::exceptionVal(msg, exType, Value::nilVal(), detail);
                        return exc;
                    } catch (const std::exception& e) {
                        auto exTypeOpt = vm.loadGlobal(toUnicodeString("RuntimeException"));
                        Value exType = exTypeOpt.has_value() ? exTypeOpt.value() : Value::nilVal();
                        Value msg = Value::stringVal(toUnicodeString(std::string(e.what())));
                        Value exc = Value::exceptionVal(msg, exType);
                        return exc;
                    }
                }, mparams.size(), mparams, returns, paramDefaults);
        }

        mod->vars.store(svcType->name, serviceTypeVal, true);
    }
}

#ifdef ROXAL_ENABLE_GRPC
void ModuleGrpc::addNativeMethod(ObjObjectType* type,
                                 const std::string& name,
                                 NativeFn fn,
                                 size_t paramCount,
                                 const std::vector<std::optional<type::Type::FuncType::ParamType>>& params,
                                 const std::vector<ptr<type::Type>>& returnTypes,
                                 const std::vector<Value>& defaultValues)
{
    Value fnVal = Value::objVal(newFunctionObj(toUnicodeString(name),
                                               toUnicodeString(""),
                                               toUnicodeString("grpc"),
                                               toUnicodeString("grpc")));
    ObjFunction* of = asFunction(fnVal);
    of->nativeImpl = fn;
    of->ownerType = Value::objRef(type);
    of->fnType = FunctionType::Method;
    of->access = ast::Access::Public;
    of->arity = static_cast<int>(paramCount);
    ptr<type::Type> t = make_ptr<type::Type>(type::BuiltinType::Func);
    t->func = type::Type::FuncType();
    t->func->isProc = false;
    if (!params.empty())
        t->func->params = params;
    else
        t->func->params.resize(paramCount);
    if (!returnTypes.empty())
        t->func->returnTypes = returnTypes;
    of->funcType = t;
    of->nativeDefaults = defaultValues;

    Value closure = Value::closureVal(fnVal);
    ObjObjectType::Method m;
    m.name = toUnicodeString(name);
    m.closure = closure;
    m.access = ast::Access::Public;
    m.ownerType = Value::objRef(type);
    type->methods[toUnicodeString(name).hashCode()] = m;
}

Value ModuleGrpc::makeServiceType(const std::string& serviceName)
{
    Value typeVal = Value::objectTypeVal(toUnicodeString(serviceName), true);
    ObjObjectType* type = asObjectType(typeVal);

    ObjObjectType::Property prop;
    prop.name = toUnicodeString("__addr");
    prop.type = Value::typeSpecVal(ValueType::String);
    prop.initialValue = Value::stringVal(toUnicodeString(targetAddress));
    prop.ownerType = typeVal;
    int32_t hash = prop.name.hashCode();
    type->properties.emplace(hash, prop);
    type->propertyOrder.push_back(hash);

    // connector slot
    ObjObjectType::Property propConn;
    propConn.name = toUnicodeString("__connector");
    propConn.type = Value::typeSpecVal(ValueType::Nil);
    propConn.initialValue = Value::nilVal();
    propConn.ownerType = typeVal;
    int32_t hconn = propConn.name.hashCode();
    type->properties.emplace(hconn, propConn);
    type->propertyOrder.push_back(hconn);

    ObjObjectType::Property propTimeout;
    propTimeout.name = toUnicodeString("__timeout_ms");
    propTimeout.type = Value::typeSpecVal(ValueType::Int);
    propTimeout.initialValue = Value::intVal(-1);
    propTimeout.ownerType = typeVal;
    int32_t htimeout = propTimeout.name.hashCode();
    type->properties.emplace(htimeout, propTimeout);
    type->propertyOrder.push_back(htimeout);

    ObjObjectType::Property propCh;
    propCh.name = toUnicodeString("__channel");
    propCh.type = Value::typeSpecVal(ValueType::Nil);
    propCh.initialValue = Value::nilVal();
    propCh.ownerType = typeVal;
    int32_t hch = propCh.name.hashCode();
    type->properties.emplace(hch, propCh);
    type->propertyOrder.push_back(hch);

    return typeVal;
}
#endif

#endif // ROXAL_ENABLE_GRPC
