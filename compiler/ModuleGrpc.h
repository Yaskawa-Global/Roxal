#pragma once

#ifdef ROXAL_ENABLE_GRPC

#include "BuiltinModule.h"
#include "Connector.h"

#include <memory>
#include <string>

namespace roxal {

class ModuleGrpc : public BuiltinModule {
public:
    ModuleGrpc();
    ~ModuleGrpc() override;

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const override { return moduleTypeValue; }

    // builtin function implementations
    Value import_proto_builtin(VM& vm, ArgsView args);
    Value set_target_builtin(VM& vm, ArgsView args);

private:
    void ensureConnector();
    void registerGeneratedTypes(const std::vector<Value>& types);
    void registerServices(const std::vector<std::string>& methods);

    Value moduleTypeValue; // ObjModuleType*
    std::string targetAddress;
    std::shared_ptr<grpc::Channel> channel;
    std::unique_ptr<ProtoAdapter> adapter;
    std::unique_ptr<ACUCommunicator> connector;
};

}

#endif // ROXAL_ENABLE_GRPC
