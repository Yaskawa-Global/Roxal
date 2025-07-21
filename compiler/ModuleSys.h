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
    Value print_builtin(VM& vm, ArgsView args);
    Value len_builtin(VM& vm, ArgsView args);
    Value help_builtin(VM& vm, ArgsView args);
    Value clone_builtin(VM& vm, ArgsView args);
    Value wait_builtin(VM& vm, ArgsView args);
    Value fork_builtin(VM& vm, ArgsView args);
    Value join_builtin(VM& vm, ArgsView args);
    Value stacktrace_builtin(VM& vm, ArgsView args);
    Value threadid_builtin(VM& vm, ArgsView args);
    Value stackdepth_builtin(VM& vm, ArgsView args);
    Value await_builtin(VM& vm, ArgsView args);
    Value runtests_builtin(VM& vm, ArgsView args);
    Value weakref_builtin(VM& vm, ArgsView args);
    Value weak_alive_builtin(VM& vm, ArgsView args);
    Value strongref_builtin(VM& vm, ArgsView args);
    Value serialize_builtin(VM& vm, ArgsView args);
    Value deserialize_builtin(VM& vm, ArgsView args);
    Value toJson_builtin(VM& vm, ArgsView args);
    Value fromJson_builtin(VM& vm, ArgsView args);

    // native implementations
    Value clock_native(VM& vm, ArgsView args);
    Value clock_signal_native(VM& vm, ArgsView args);
    Value signal_source_native(VM& vm, ArgsView args);
    Value engine_stop_native(VM& vm, ArgsView args);
    Value typeof_native(VM& vm, ArgsView args);
    Value df_graph_native(VM& vm, ArgsView args);
    Value df_graphdot_native(VM& vm, ArgsView args);
    Value loadlib_native(VM& vm, ArgsView args);

    // builtin Counter methods
    Value counter_init_builtin(VM& vm, ArgsView args);
    Value counter_inc_builtin(VM& vm, ArgsView args);

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
