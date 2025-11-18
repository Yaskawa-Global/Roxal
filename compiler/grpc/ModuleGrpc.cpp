#ifdef ROXAL_ENABLE_GRPC

#include "ModuleGrpc.h"
#include "VM.h"
#include "Object.h"

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

    auto methods = adapter->addServices(protoFilename);
    registerServices(moduleVal, methods);

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

void ModuleGrpc::registerServices(Value moduleVal, const std::vector<std::string>& methods)
{
    ObjModuleType* mod = asModuleType(moduleVal);
    for (const auto& method : methods) {
        auto nativeFcn = [this, method](VM&, ArgsView a) {
            ensureConnector();
            return connector->call(method, a);
        };
        std::string name = method;
        icu::UnicodeString uname = toUnicodeString(name);

        Value funcVal = Value::nativeVal(nativeFcn);
        mod->vars.store(uname, funcVal, true);
    }
}

#endif // ROXAL_ENABLE_GRPC
