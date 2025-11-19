#ifdef ROXAL_ENABLE_GRPC

#include "ModuleGrpc.h"
#include "VM.h"
#include "Object.h"
#include "core/AST.h"

#include <cctype>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <filesystem>
#include <stdexcept>
#include <vector>

using namespace roxal;

ModuleGrpc::ModuleGrpc()
    : targetAddress("127.0.0.1:50051")
{
    moduleTypeValue = Value::nilVal(); // not exposed as builtin module
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
        if (!isObjectType(typeVal))
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

        // init(addr:string="127.0.0.1:50051")
        std::vector<std::optional<type::Type::FuncType::ParamType>> initParams;
        initParams.emplace_back(type::Type::FuncType::ParamType(toUnicodeString("addr")));
        initParams.back()->type = make_ptr<type::Type>(type::BuiltinType::String);

        addNativeMethod(svcType, "init", [this](VM&, ArgsView args) -> Value {
            if (args.size() < 1 || !isActorInstance(args[0]))
                throw std::invalid_argument("init expects service instance");
            ActorInstance* self = asActorInstance(args[0]);
            Value addrVal = Value::stringVal(toUnicodeString(this->targetAddress));
            if (args.size() >= 2) {
                if (!isString(args[1]))
                    throw std::invalid_argument("init address must be string");
                addrVal = args[1];
            }
            auto setProp = [&](const icu::UnicodeString& name, const Value& v) {
                auto& slot = self->properties[name.hashCode()];
                slot.assign(v);
            };
            setProp(toUnicodeString("__addr"), addrVal);
            // create per-instance connector
            std::string addr = toUTF8StdString(asStringObj(addrVal)->s);
            std::shared_ptr<grpc::Channel>* ch = new std::shared_ptr<grpc::Channel>(
                grpc::CreateChannel(addr, grpc::InsecureChannelCredentials()));
            ACUCommunicator* comm = new ACUCommunicator(*ch, this->adapter.get());
            Value fpCh = Value::foreignPtrVal(ch);
            Value fpComm = Value::foreignPtrVal(comm);
            asForeignPtr(fpCh)->registerCleanup([](void* p){ delete static_cast<std::shared_ptr<grpc::Channel>*>(p); });
            asForeignPtr(fpComm)->registerCleanup([](void* p){ delete static_cast<ACUCommunicator*>(p); });
            setProp(toUnicodeString("__channel"), fpCh);
            setProp(toUnicodeString("__connector"), fpComm);
            return Value::nilVal();
        }, 1, initParams);

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
            std::vector<std::optional<type::Type::FuncType::ParamType>> mparams;
            type::Type::FuncType::ParamType p(toUnicodeString("req"));
            p.type = makeObjTypeMeta(method.inputTypeFullName);
            mparams.emplace_back(p);

            std::vector<ptr<type::Type>> returns;
            returns.push_back(makeObjTypeMeta(method.outputTypeFullName));

            addNativeMethod(svcType, method.name, [this, method](VM&, ArgsView args) -> Value {
            if (args.size() != 2 || !isActorInstance(args[0]))
                throw std::invalid_argument(method.name + " expects receiver and request");
            ActorInstance* self = asActorInstance(args[0]);
            if (!isObjectInstance(args[1]))
                throw std::invalid_argument(method.name + " expects request object instance");

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

            ArgsView reqArgs(args.data + 1, args.size() - 1);
            return comm->call(method.name, reqArgs);
        }, 1, mparams, returns);
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
                                 const std::vector<ptr<type::Type>>& returnTypes)
{
    Value fnVal = Value::objVal(newFunctionObj(toUnicodeString(name),
                                               toUnicodeString(""),
                                               toUnicodeString(""),
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
