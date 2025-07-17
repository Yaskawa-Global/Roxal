#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleMath : public BuiltinModule {
public:
    explicit ModuleMath(Value moduleType);

    void registerBuiltins(VM& vm) override;

    inline ObjModuleType* moduleType() const { return asModuleType(moduleTypeValue); }

    // builtin function implementations
    Value math_identity_builtin(VM& vm, int argCount, Value* args);
    Value math_zeros_builtin(VM& vm, int argCount, Value* args);
    Value math_ones_builtin(VM& vm, int argCount, Value* args);
    Value math_dot_builtin(VM& vm, int argCount, Value* args);
    Value math_cross_builtin(VM& vm, int argCount, Value* args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
