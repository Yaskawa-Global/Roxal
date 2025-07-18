#include "ModuleSys.h"
#include "VM.h"
#include "Object.h"

using namespace roxal;

ModuleSys::ModuleSys()
{
    moduleTypeValue = objVal(moduleTypeVal(toUnicodeString("sys")));
}


void ModuleSys::registerBuiltins(VM& vm)
{
    auto addSys = [&](const std::string& name, NativeFn fn,
                      ptr<type::Type> funcType = nullptr,
                      std::vector<Value> defaults = {}){
        vm.defineNative(name, fn, funcType, defaults);
        moduleType()->vars.store(toUnicodeString(name),
            objVal(nativeVal(fn, nullptr, funcType, defaults)));
    };

    if (!vm.loadGlobal(toUnicodeString("print")).has_value()) {
        addSys("print", std::mem_fn(&VM::print_builtin));
        addSys("len", std::mem_fn(&VM::len_builtin));
        addSys("clone", std::mem_fn(&VM::clone_builtin));
        {
            auto t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            t->func->isProc = true;
            std::vector<Value> defaults { intVal(0), intVal(0), intVal(0), intVal(0) };
            auto params = BuiltinModule::constructParams({ {"s", type::BuiltinType::Int},
                                           {"ms", type::BuiltinType::Int},
                                           {"us", type::BuiltinType::Int},
                                           {"ns", type::BuiltinType::Int} },
                                         defaults);
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("wait", std::mem_fn(&VM::wait_builtin), t, defaults);
        }
        addSys("fork", std::mem_fn(&VM::fork_builtin));
        addSys("join", std::mem_fn(&VM::join_builtin));
        addSys("stacktrace", std::mem_fn(&VM::stacktrace_builtin));
        addSys("_threadid", std::mem_fn(&VM::threadid_builtin));
        addSys("_stackdepth", std::mem_fn(&VM::stackdepth_builtin));
        addSys("_wait", std::mem_fn(&VM::await_builtin));
        addSys("_runtests", std::mem_fn(&VM::runtests_builtin));
        addSys("_weakref", std::mem_fn(&VM::weakref_builtin));
        addSys("_weak_alive", std::mem_fn(&VM::weak_alive_builtin));
        addSys("_strongref", std::mem_fn(&VM::strongref_builtin));
        addSys("serialize", std::mem_fn(&VM::serialize_builtin));
        addSys("deserialize", std::mem_fn(&VM::deserialize_builtin));
        addSys("toJson", std::mem_fn(&VM::toJson_builtin));
        addSys("fromJson", std::mem_fn(&VM::fromJson_builtin));
    }

    if (!vm.loadGlobal(toUnicodeString("_clock")).has_value()) {
        addSys("_clock", std::mem_fn(&VM::clock_native));
        {
            auto t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            auto params = BuiltinModule::constructParams({{"freq", type::BuiltinType::Int}}, {});
            t->func->params.resize(params.size());
            for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
            addSys("clock", std::mem_fn(&VM::clock_signal_native), t, {});
        }
        {
            auto t = make_ptr<type::Type>(type::BuiltinType::Func);
            t->func = type::Type::FuncType();
            type::Type::FuncType::ParamType p1(toUnicodeString("freq"));
            p1.type = make_ptr<type::Type>(type::BuiltinType::Int);
            type::Type::FuncType::ParamType p2(toUnicodeString("initial"));
            t->func->params.resize(2);
            t->func->params[0] = p1;
            t->func->params[1] = p2;
            addSys("signal", std::mem_fn(&VM::signal_source_native), t, {});
        }
        addSys("_engine_stop", std::mem_fn(&VM::engine_stop_native));
        addSys("typeof", std::mem_fn(&VM::typeof_native));
        addSys("_df_graph", std::mem_fn(&VM::df_graph_native));
        addSys("_df_graphdot", std::mem_fn(&VM::df_graphdot_native));
        addSys("loadlib", std::mem_fn(&VM::loadlib_native));
    }
}
