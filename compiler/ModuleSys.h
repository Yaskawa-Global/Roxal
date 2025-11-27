#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"
#include <string>
#include <cstdint>

namespace roxal {

class ModuleSys : public BuiltinModule {
public:
    ModuleSys();
    virtual ~ModuleSys();

    // Register builtin sys functions and natives
    void registerBuiltins(VM& vm) override;

    inline Value moduleType() const { return moduleTypeValue; }

    // builtin function implementations
    Value print_builtin(VM& vm, ArgsView args);
    Value len_builtin(VM& vm, ArgsView args);
    Value help_builtin(VM& vm, ArgsView args);
    Value clone_builtin(VM& vm, ArgsView args);
    Value wait_builtin(VM& vm, ArgsView args);
    Value is_ready_builtin(VM& vm, ArgsView args);
    Value fork_builtin(VM& vm, ArgsView args);
    Value join_builtin(VM& vm, ArgsView args);
    Value exit_builtin(VM& vm, ArgsView args);
    Value stacktrace_builtin(VM& vm, ArgsView args);
    Value threadid_builtin(VM& vm, ArgsView args);
    Value stackdepth_builtin(VM& vm, ArgsView args);
    Value runtests_builtin(VM& vm, ArgsView args);
    Value weakref_builtin(VM& vm, ArgsView args);
    Value weak_alive_builtin(VM& vm, ArgsView args);
    Value strongref_builtin(VM& vm, ArgsView args);
    Value gc_builtin(VM& vm, ArgsView args);
    Value gc_config_builtin(VM& vm, ArgsView args);
    Value serialize_builtin(VM& vm, ArgsView args);
    Value deserialize_builtin(VM& vm, ArgsView args);
    Value to_json_builtin(VM& vm, ArgsView args);
    Value from_json_builtin(VM& vm, ArgsView args);

    // Time type natives
    Value time_init_native(VM& vm, ArgsView args);
    Value time_kind_native(VM& vm, ArgsView args);
    Value time_is_steady_native(VM& vm, ArgsView args);
    Value time_seconds_native(VM& vm, ArgsView args);
    Value time_micros_native(VM& vm, ArgsView args);
    Value time_diff_native(VM& vm, ArgsView args);
    Value time_since_native(VM& vm, ArgsView args);
    Value time_until_native(VM& vm, ArgsView args);
    Value time_format_native(VM& vm, ArgsView args);
    Value time_components_native(VM& vm, ArgsView args);

    // TimeSpan type natives
    Value timespan_init_native(VM& vm, ArgsView args);
    Value timespan_seconds_native(VM& vm, ArgsView args);
    Value timespan_micros_native(VM& vm, ArgsView args);
    Value timespan_split_native(VM& vm, ArgsView args);
    Value timespan_total_days_native(VM& vm, ArgsView args);
    Value timespan_total_hours_native(VM& vm, ArgsView args);
    Value timespan_total_minutes_native(VM& vm, ArgsView args);
    Value timespan_total_seconds_native(VM& vm, ArgsView args);
    Value timespan_total_millis_native(VM& vm, ArgsView args);
    Value timespan_total_micros_native(VM& vm, ArgsView args);
    Value timespan_human_native(VM& vm, ArgsView args);

    // Type-level helpers
    Value time_type_wall_now(VM& vm, ArgsView args);
    Value time_type_steady_now(VM& vm, ArgsView args);
    Value time_type_parse(VM& vm, ArgsView args);
    Value time_type_from_parts(VM& vm, ArgsView args);
    Value timespan_type_from_fields(VM& vm, ArgsView args);

    // native implementations
    Value clock_native(VM& vm, ArgsView args);
    Value clock_signal_native(VM& vm, ArgsView args);
    Value engine_stop_native(VM& vm, ArgsView args);
    Value typeof_native(VM& vm, ArgsView args);
    Value df_graph_native(VM& vm, ArgsView args);
    Value df_islands_native(VM& vm, ArgsView args);
    Value df_graphdot_native(VM& vm, ArgsView args);
    Value loadlib_native(VM& vm, ArgsView args);


private:
    Value moduleTypeValue; // ObjModuleType*
    Value timeTypeValue;
    Value timeSpanTypeValue;
    ObjObjectType* timeTypeObj { nullptr };
    ObjObjectType* timeSpanTypeObj { nullptr };

    Value typeMethodDecl(const Value& typeValue, const std::string& methodName) const;
};

ObjObjectType* sysTimeType();
ObjObjectType* sysTimeSpanType();
std::string sysTimeDefaultString(ObjectInstance* inst);
std::string sysTimeSpanDefaultString(ObjectInstance* inst);
Value sysNewTimeSpan(int64_t totalMicros);

}
