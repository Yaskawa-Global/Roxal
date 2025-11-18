#ifdef ROXAL_ENABLE_GRPC

#include "ModuleGrpc.h"
#include "VM.h"
#include "Object.h"
#include "core/AST.h"

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
    std::string moduleName = pp.stem().string();
    Value moduleVal = getOrCreateModule(moduleName);

    auto types = adapter->allocateObjects(protoFilename);
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
    for (const auto& svc : services) {
        Value serviceTypeVal = makeServiceType(svc.name);
        ObjObjectType* svcType = asObjectType(serviceTypeVal);

        // init(addr:string="127.0.0.1:50051")
        addNativeMethod(svcType, "init", [this](VM&, ArgsView args) -> Value {
            if (args.size() < 1 || !isObjectInstance(args[0]))
                throw std::invalid_argument("init expects service instance");
            ObjectInstance* self = asObjectInstance(args[0]);
            if (args.size() >= 2) {
                if (!isString(args[1]))
                    throw std::invalid_argument("init address must be string");
                self->setProperty("__addr", args[1]);
            } else {
                self->setProperty("__addr", Value::stringVal(toUnicodeString(this->targetAddress)));
            }
            return Value::nilVal();
        });

        for (const auto& method : svc.methods) {
            addNativeMethod(svcType, method, [this, method](VM&, ArgsView args) -> Value {
                if (args.size() != 2 || !isObjectInstance(args[0]))
                    throw std::invalid_argument(method + " expects receiver and request");
                ObjectInstance* self = asObjectInstance(args[0]);
                if (!isObjectInstance(args[1]))
                    throw std::invalid_argument(method + " expects request object instance");

                Value addrVal = self->getProperty("__addr");
                std::string addr = targetAddress;
                if (isString(addrVal))
                    addr = toUTF8StdString(asStringObj(addrVal)->s);

                auto localAdapter = adapter.get();
                std::shared_ptr<grpc::Channel> ch = grpc::CreateChannel(addr, grpc::InsecureChannelCredentials());
                ACUCommunicator comm(ch, localAdapter);

                ArgsView reqArgs(args.data + 1, args.size() - 1);
                return comm.call(method, reqArgs);
            });
        }

        mod->vars.store(svcType->name, serviceTypeVal, true);
    }
}

#ifdef ROXAL_ENABLE_GRPC
void ModuleGrpc::addNativeMethod(ObjObjectType* type, const std::string& name, NativeFn fn)
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
    Value typeVal = Value::objectTypeVal(toUnicodeString(serviceName), false);
    ObjObjectType* type = asObjectType(typeVal);

    ObjObjectType::Property prop;
    prop.name = toUnicodeString("__addr");
    prop.type = Value::typeSpecVal(ValueType::String);
    prop.initialValue = Value::stringVal(toUnicodeString(targetAddress));
    prop.ownerType = typeVal;
    int32_t hash = prop.name.hashCode();
    type->properties.emplace(hash, prop);
    type->propertyOrder.push_back(hash);

    return typeVal;
}
#endif

#endif // ROXAL_ENABLE_GRPC
