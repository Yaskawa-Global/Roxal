#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleSys : public BuiltinModule {
public:
    ModuleSys();

    // Register builtin sys functions and natives
    void registerBuiltins(VM& vm) override;

    inline ObjModuleType* moduleType() const { return asModuleType(moduleTypeValue); }

    // builtin function implementations
    Value print_builtin(VM& vm, int argCount, Value* args);
    Value len_builtin(VM& vm, int argCount, Value* args);
    Value clone_builtin(VM& vm, int argCount, Value* args);
    Value wait_builtin(VM& vm, int argCount, Value* args);
    Value fork_builtin(VM& vm, int argCount, Value* args);
    Value join_builtin(VM& vm, int argCount, Value* args);
    Value stacktrace_builtin(VM& vm, int argCount, Value* args);
    Value threadid_builtin(VM& vm, int argCount, Value* args);
    Value stackdepth_builtin(VM& vm, int argCount, Value* args);
    Value await_builtin(VM& vm, int argCount, Value* args);
    Value runtests_builtin(VM& vm, int argCount, Value* args);
    Value weakref_builtin(VM& vm, int argCount, Value* args);
    Value weak_alive_builtin(VM& vm, int argCount, Value* args);
    Value strongref_builtin(VM& vm, int argCount, Value* args);
    Value serialize_builtin(VM& vm, int argCount, Value* args);
    Value deserialize_builtin(VM& vm, int argCount, Value* args);
    Value toJson_builtin(VM& vm, int argCount, Value* args);
    Value fromJson_builtin(VM& vm, int argCount, Value* args);

    // native implementations
    Value clock_native(VM& vm, int argCount, Value* args);
    Value clock_signal_native(VM& vm, int argCount, Value* args);
    Value signal_source_native(VM& vm, int argCount, Value* args);
    Value engine_stop_native(VM& vm, int argCount, Value* args);
    Value typeof_native(VM& vm, int argCount, Value* args);
    Value df_graph_native(VM& vm, int argCount, Value* args);
    Value df_graphdot_native(VM& vm, int argCount, Value* args);
    Value loadlib_native(VM& vm, int argCount, Value* args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
