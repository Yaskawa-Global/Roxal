#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleMath : public BuiltinModule {
public:
    ModuleMath();
    virtual ~ModuleMath() { moduleTypeValue = Value::nilVal(); }

    void registerBuiltins(VM& vm) override;

    inline ObjModuleType* moduleType() const { return asModuleType(moduleTypeValue); }

    // builtin function implementations
    Value math_identity_builtin(VM& vm, ArgsView args);
    Value math_zeros_builtin(VM& vm, ArgsView args);
    Value math_ones_builtin(VM& vm, ArgsView args);
    Value math_dot_builtin(VM& vm, ArgsView args);
    Value math_cross_builtin(VM& vm, ArgsView args);


    // Example for implementing a builtin type (math.Counter) that wraps a C++ class
    class Counter {
    public:
        Counter(int32_t start) : count(start) {}
        int32_t inc(int32_t inc = 1) {
            count += inc;
            return count;
        }
        int32_t value() const { return count; }
    protected:
        int32_t count;
    };


    // builtin Counter methods
    Value counter_init_builtin(VM& vm, ArgsView args);
    Value counter_inc_builtin(VM& vm, ArgsView args);
    Value counter_value_builtin(VM& vm, ArgsView args);



private:
    Value moduleTypeValue; // ObjModuleType*
};

}
