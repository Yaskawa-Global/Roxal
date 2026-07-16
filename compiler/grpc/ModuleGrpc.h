#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include "BuiltinModule.h"
#include "Connector.h"
#include "GCRoots.h"
#include "SimpleMarkSweepGC.h"

#include <optional>
#include <memory>
#include <string>
#include <unordered_map>

namespace roxal {

// Rooting: this module retains roxal Values in C++ containers
// (protoModules, ProtoAdapter decl maps, and -- critically --
// ActiveStreamState signal/arg Values in the connector, which may be the
// ONLY reference once the script drops its stream handles).  Own members
// are typed roots (PersistentRoot/TracedMember, GC v2 phase B); the
// adapter/connector internals keep their visitRoots() methods, reached
// through a delegating root member (their conversion to typed members is
// a later B1 step).
class ModuleGrpc : public BuiltinModule {
public:
    ModuleGrpc();
    ~ModuleGrpc() override;

    virtual bool hasModuleScript() const { return false; }

    void registerBuiltins(VM& vm) override;
    void onModuleLoaded(VM& vm) override;
    // Release Value-holding C++ containers before the VM's shutdown sweep.
    void onModuleUnloading(VM& vm) override;

    inline Value moduleType() const override { return moduleTypeValue; }

    Value importProto(const std::string& protoPath);
    void setTarget(const std::string& addr);
    void addProtoPath(const std::string& path);

private:
    void ensureConnector();
    Value getOrCreateModule(const std::string& name);
    void registerGeneratedTypes(Value moduleVal, const std::vector<Value>& types);
    void registerServices(Value moduleVal, const std::vector<ProtoAdapter::ServiceInfo>& services);
    Value makeServiceType(const std::string& serviceName);
    void addNativeMethod(ObjObjectType* type,
                         const std::string& name,
                         NativeFn fn,
                         size_t paramCount = 0,
                         const std::vector<std::optional<type::Type::FuncType::ParamType>>& params = {},
                         const std::vector<ptr<type::Type>>& returnTypes = {},
                         const std::vector<Value>& defaultValues = {});

    PersistentRoot<Value> moduleTypeValue; // ObjModuleType*
    std::string targetAddress;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ProtoAdapter> adapter;
    std::unique_ptr<ACUCommunicator> connector;
    TracedMember<std::unordered_map<std::string, Value>> protoModules; // name -> ObjModuleType
};

}

#endif // ROXAL_ENABLE_GRPC
