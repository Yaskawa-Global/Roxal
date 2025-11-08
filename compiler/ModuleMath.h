#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleMath : public BuiltinModule {
public:
    ModuleMath();
    virtual ~ModuleMath();

    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const { return moduleTypeValue; }

    // builtin function implementations
    Value math_identity_builtin(ArgsView args);
    Value math_zeros_builtin(ArgsView args);
    Value math_ones_builtin(ArgsView args);
    Value math_dot_builtin(ArgsView args);
    Value math_cross_builtin(ArgsView args);
    Value math_setVecSignal_builtin(VM& vm, ArgsView args);


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
    Value counter_init_builtin(ArgsView args);
    Value counter_inc_builtin(ArgsView args);
    Value counter_value_builtin(ArgsView args);



private:
    Value moduleTypeValue; // ObjModuleType*
};

}
