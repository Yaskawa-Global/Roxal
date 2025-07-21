#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleMath : public BuiltinModule {
public:
    ModuleMath();

    void registerBuiltins(VM& vm) override;

    inline ObjModuleType* moduleType() const { return asModuleType(moduleTypeValue); }

    // builtin function implementations
    Value math_identity_builtin(VM& vm, ArgsView args);
    Value math_zeros_builtin(VM& vm, ArgsView args);
    Value math_ones_builtin(VM& vm, ArgsView args);
    Value math_dot_builtin(VM& vm, ArgsView args);
    Value math_cross_builtin(VM& vm, ArgsView args);

    // builtin Counter methods
    Value counter_init_builtin(VM& vm, ArgsView args);
    Value counter_inc_builtin(VM& vm, ArgsView args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
