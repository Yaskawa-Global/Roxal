#pragma once

#ifdef ROXAL_ENABLE_DDS

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "BuiltinModule.h"

namespace roxal {

class DdsAdapter;

class ModuleDDS : public BuiltinModule {
public:
    ModuleDDS();
    ~ModuleDDS() override;

    void registerBuiltins(VM& vm) override;
    void initialize() override {};
    Value moduleType() const override { return moduleTypeValue; }

    Value importIdl(const std::string& idlFilename);

private:
    Value moduleTypeValue;
    std::unique_ptr<DdsAdapter> adapter;
    std::unordered_map<std::string, Value> idlModules;

    Value getOrCreateModule(const std::string& name);
    void registerGeneratedTypes(Value moduleVal, const std::vector<Value>& types);

    bool functionsLinked{false};
    void linkNativeFunctions();

    // native implementations
    static Value dds_create_participant(VM&, ArgsView args);
    static Value dds_create_topic(VM&, ArgsView args);
    static Value dds_create_writer(VM&, ArgsView args);
    static Value dds_create_reader(VM&, ArgsView args);
};

} // namespace roxal

#endif // ROXAL_ENABLE_DDS
