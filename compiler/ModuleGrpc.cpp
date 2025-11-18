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
    : targetAddress("0.0.0.0:50051")
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("grpc")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleGrpc::~ModuleGrpc()
{
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

    // Wire up native implementations for the builtin functions declared in compiler/grpc.rox
    link("import_proto", [this](VM& v, ArgsView a){ return import_proto_builtin(v, a); });
    link("set_target", [this](VM& v, ArgsView a){ return set_target_builtin(v, a); });

    // Legacy convenience alias matching the original branch.
    if (!vm.loadGlobal(toUnicodeString("_import")).has_value()) {
        vm.defineNative("_import", [this](VM& v, ArgsView a){ return import_proto_builtin(v, a); });
    }
    if (!vm.loadGlobal(toUnicodeString("_set_target")).has_value()) {
        vm.defineNative("_set_target", [this](VM& v, ArgsView a){ return set_target_builtin(v, a); });
    }
}

Value ModuleGrpc::set_target_builtin(VM&, ArgsView args)
{
    if (args.size() != 1 || !isString(args[0]))
        throw std::invalid_argument("grpc.set_target expects a single address string");

    targetAddress = toUTF8StdString(asStringObj(args[0])->s);
    channel.reset();
    connector.reset();
    ensureConnector();
    return Value::nilVal();
}

Value ModuleGrpc::import_proto_builtin(VM&, ArgsView args)
{
    if (args.size() < 1 || args.size() > 2 || !isString(args[0]))
        throw std::invalid_argument("grpc.import_proto expects proto path string and optional address string");

    std::string protoFilename = toUTF8StdString(asStringObj(args[0])->s);
    if (!std::filesystem::exists(std::filesystem::path(protoFilename)))
        throw std::invalid_argument("grpc.import_proto - proto file '"+protoFilename+"' not found.");

    if (args.size() == 2) {
        if (!isString(args[1]))
            throw std::invalid_argument("grpc.import_proto address must be a string");
        targetAddress = toUTF8StdString(asStringObj(args[1])->s);
        channel.reset();
        connector.reset();
    }

    ensureConnector();

    auto types = adapter->allocateObjects(protoFilename);
    registerGeneratedTypes(types);

    auto methods = adapter->addServices(protoFilename);
    registerServices(methods);

    return Value::nilVal();
}

void ModuleGrpc::registerGeneratedTypes(const std::vector<Value>& types)
{
    ObjModuleType* mod = asModuleType(moduleTypeValue);
    for (const auto& typeVal : types) {
        if (!isObjectType(typeVal))
            continue;

        ObjObjectType* type = asObjectType(typeVal);
        mod->vars.store(type->name, typeVal, true);

        // Also expose as global so existing scripts can use the bare name.
        vm().globals.storeGlobal(type->name, typeVal);
    }
}

void ModuleGrpc::registerServices(const std::vector<std::string>& methods)
{
    ObjModuleType* mod = asModuleType(moduleTypeValue);
    for (const auto& method : methods) {
        auto nativeFcn = [this, method](VM&, ArgsView a) {
            ensureConnector();
            return connector->call(method, a);
        };
        std::string name = "_" + method;
        icu::UnicodeString uname = toUnicodeString(name);

        Value funcVal = Value::nativeVal(nativeFcn);
        mod->vars.store(uname, funcVal, true);
        if (!vm().loadGlobal(uname).has_value())
            vm().defineNative(name, nativeFcn);
    }
}

#endif // ROXAL_ENABLE_GRPC
