#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"
#include <memory>

struct ModelWrapper;  // opaque native state for Model instances

namespace roxal {

class ModuleNN : public BuiltinModule {
public:
    ModuleNN();
    virtual ~ModuleNN();

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const override { return moduleTypeValue; }

    // Module-level functions
    Value nn_load_builtin(ArgsView args);

    // Model object methods
    Value nn_model_run_builtin(ArgsView args);
    Value nn_model_inputs_builtin(ArgsView args);
    Value nn_model_outputs_builtin(ArgsView args);
    Value nn_model_device_builtin(ArgsView args);
    Value nn_model_close_builtin(ArgsView args);

private:
    Value moduleTypeValue; // ObjModuleType*

    // Helper to create a Model object instance with a predict closure
    Value createModelObject(const std::shared_ptr<ModelWrapper>& wrapper);
};

} // namespace roxal
