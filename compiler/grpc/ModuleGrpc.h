#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include "BuiltinModule.h"
#include "Connector.h"

#include <optional>
#include <memory>
#include <string>
#include <unordered_map>

namespace roxal {

class ModuleGrpc : public BuiltinModule {
public:
    ModuleGrpc();
    ~ModuleGrpc() override;

    virtual bool hasModuleScript() const { return false; }

    void registerBuiltins(VM& vm) override;
    void onModuleLoaded(VM& vm) override;

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

    Value moduleTypeValue; // ObjModuleType*
    std::string targetAddress;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ProtoAdapter> adapter;
    std::unique_ptr<ACUCommunicator> connector;
    std::unordered_map<std::string, Value> protoModules; // name -> ObjModuleType
};

}

#endif // ROXAL_ENABLE_GRPC
