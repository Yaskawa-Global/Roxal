#pragma once

#include <vector>
#include <atomic>
#include <unordered_map>

#include "core/atomic.h"
#include "Chunk.h"
#include "Value.h"
#include "InterpretResult.h"
#include "Thread.h"
#include <ffi.h>
#include <vector>

namespace roxal { struct ObjObjectType; }
using roxal::ObjObjectType;

namespace df { class DataflowEngine; }

struct FFIWrapper {
    ffi_cif cif;
    void* fn;
    std::vector<ffi_type*> argTypes;
    ffi_type* retType;
    ObjObjectType* retObjType { nullptr };
    std::vector<ffi_type*> retStructElems;
    ffi_type retStructType;
};

namespace roxal {

struct CallFrame; // forward


typedef std::vector<CallFrame> CallFrames;

struct CallFrame {
    #ifdef DEBUG_BUILD
    CallFrame() : closure(nullptr), slots(nullptr), strict(false) {}
    #else
    CallFrame() : closure(nullptr), strict(false) {}
    #endif
    ObjClosure* closure;
    Chunk::iterator startIp;
    Chunk::iterator ip;
    Value* slots;

    CallFrames::iterator parent;

    bool strict; // whether current frame executes in strict mode

    // on frame start, move argument Value (second) to end of the frame's
    //  argument list (in existing stack arg placeholder slots)
    std::vector<Value> tailArgValues;

    // if not empty, used to reorder call arguments on the stack
    std::vector<int8_t> reorderArgs; // reordering
};



// The Virtual Machine (singleton)
class VM
{
public:
    friend class Thread;

    static VM& instance()
    {
        static VM instance; // Guaranteed to be destroyed.
                            // Instantiated on first use.
        return instance;
    }

    VM(VM const&) = delete;
    void operator=(VM const&) = delete;

    void setDisassemblyOutput(bool outputBytecodeDisassembly);
    void appendModulePaths(const std::vector<std::string>& modulePaths);

    ObjModuleType* getBuiltinModule(const icu::UnicodeString& name);

    InterpretResult interpret(std::istream& source, const std::string& name);
    InterpretResult interpretLine(std::istream& linestream);


    bool call(ObjClosure* closure, const CallSpec& callSpec);
    bool call(ValueType builtinType, const CallSpec& callSpec);
    bool callValue(const Value& callee, const CallSpec& callSpec);
    bool invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec);
    bool invoke(ObjString* name, const CallSpec& callSpec);

    // Convenience for C++ callers: execute closure with arguments
    std::pair<InterpretResult,Value> callAndExec(ObjClosure* closure, const std::vector<Value>& args);
    bool indexValue(const Value& indexable, int subscriptCount);
    bool setIndexValue(const Value& indexable, int subscriptCount, Value& value);
    enum class BindResult {
        Bound,
        NotFound,
        Private
    };
    BindResult bindMethod(ObjObjectType* instanceType, ObjString* name);
    ObjUpvalue* captureUpvalue(Value& local);
    void closeUpvalues(Value* last);
    Value opReturn();
    bool isAccessAllowed(ObjObjectType* ownerType, ast::Access access);

    void defineProperty(ObjString* name);
    void defineMethod(ObjString* name);
    void defineEnumLabel(ObjString* name);
    void defineNative(const std::string& name, NativeFn function);


    const int MaxStack = 1024;
    typedef std::vector<Value> ValueStack;

    inline void push(const Value& value) { thread->push(value); }
    inline Value pop() { return thread->pop(); }
    inline void popN(size_t n) { thread->popN(n); } // call pop() n times
    inline Value& peek(int distance) { return thread->peek(distance); }



    // the current thread
    static thread_local ptr<Thread> thread;

protected:
    VM();
    ~VM();

    std::pair<InterpretResult,Value> execute();

    bool outputBytecodeDisassembly;
    bool lineMode;
    std::istream* lineStream;

    std::vector<std::string> modulePaths {};

    atomic_unordered_map<uint64_t, ptr<Thread>> threads;

    // Persistent thread used for REPL execution so that state such as event
    // handlers persists across entered lines.
    ptr<Thread> replThread;

    ObjModuleType* moduleType()
    {
        #ifdef DEBUG_BUILD
        assert(thread != nullptr);
        assert(!thread->frames.empty());
        assert(thread->frames.back().closure != nullptr);
        assert(thread->frames.back().closure->function != nullptr);
        assert(isModuleType(thread->frames.back().closure->function->moduleType));
        #endif
        auto currentFrame { thread->frames.back() };

        return asModuleType(currentFrame.closure->function->moduleType);
    }
    inline VariablesMap& moduleVars() { return moduleType()->vars; }

    // global vars cannot be created in the language, but represent builtin symbols available in all modules
    VariablesMap globals;

    // builtin modules
    ObjModuleType* sysModule;
    ObjModuleType* mathModule;

    // builtin dataflow engine actor
    std::shared_ptr<df::DataflowEngine> dataflowEngine;
    Value dataflowEngineActor;
    std::shared_ptr<Thread> dataflowEngineThread;


    ObjString* initString;

    // TODO: perhaps implement inheritance first, then pre-define
    //  object type as root of class heirarchy and add clone() and other
    //  builtins to that
    // Builtin method info structure
    struct BuiltinMethodInfo {
        NativeFn function;
        bool isProc;  // true for proc methods, false for func methods

        BuiltinMethodInfo() : function(nullptr), isProc(false) {}
        BuiltinMethodInfo(NativeFn fn, bool proc = false) : function(fn), isProc(proc) {}
    };

    // Builtin methods: builtin value type -> method name hash -> BuiltinMethodInfo
    std::unordered_map<ValueType, std::unordered_map<int32_t, BuiltinMethodInfo>> builtinMethods;

    bool processPendingEvents();

    void resetStack();
    void freeObjects();
    void outputAllocatedObjs();

    void concatenate();

    void runtimeError(const std::string& format, ...);


    // Builtin functions
    void defineBuiltinFunctions();

    void defineBuiltinMethods();
    void defineBuiltinMethod(ValueType type, const std::string& name, NativeFn fn, bool isProc = false);

    // Native property support
    typedef Value (VM::*NativePropertyGetter)(Value&);
    typedef void (VM::*NativePropertySetter)(Value&, Value);

    struct BuiltinPropertyInfo {
        NativePropertyGetter getter;
        NativePropertySetter setter;  // nullptr for read-only properties
        bool readOnly;

        BuiltinPropertyInfo() : getter(nullptr), setter(nullptr), readOnly(true) {}
        BuiltinPropertyInfo(NativePropertyGetter get, NativePropertySetter set = nullptr)
            : getter(get), setter(set), readOnly(set == nullptr) {}
    };

    // Builtin properties: builtin value type -> property name hash -> BuiltinPropertyInfo
    std::unordered_map<ValueType, std::unordered_map<int32_t, BuiltinPropertyInfo>> builtinProperties;

    void defineBuiltinProperties();
    void defineBuiltinProperty(ValueType type, const std::string& name, NativePropertyGetter getter, NativePropertySetter setter = nullptr);

    Value vector_norm_builtin(int argCount, Value* args);
    Value vector_sum_builtin(int argCount, Value* args);
    Value vector_normalized_builtin(int argCount, Value* args);
    Value vector_dot_builtin(int argCount, Value* args);
    Value matrix_rows_builtin(int argCount, Value* args);
    Value matrix_cols_builtin(int argCount, Value* args);
    Value matrix_transpose_builtin(int argCount, Value* args);
    Value matrix_determinant_builtin(int argCount, Value* args);
    Value matrix_inverse_builtin(int argCount, Value* args);
    Value matrix_trace_builtin(int argCount, Value* args);
    Value matrix_norm_builtin(int argCount, Value* args);
    Value matrix_sum_builtin(int argCount, Value* args);

    Value list_append_builtin(int argCount, Value* args);

    Value signal_run_builtin(int argCount, Value* args);
    Value signal_stop_builtin(int argCount, Value* args);
    Value signal_tick_builtin(int argCount, Value* args);

    Value math_identity_builtin(int argCount, Value* args);
    Value math_zeros_builtin(int argCount, Value* args);
    Value math_ones_builtin(int argCount, Value* args);
    Value math_dot_builtin(int argCount, Value* args);
    Value math_cross_builtin(int argCount, Value* args);

    Value print_builtin(int argCount, Value* args);
    Value len_builtin(int argCount, Value* args);
    Value clone_builtin(int argCount, Value* args);
    Value sleep_builtin(int argCount, Value* args);
    Value fork_builtin(int argCount, Value* args);
    Value join_builtin(int argCount, Value* args);
    Value threadid_builtin(int argCount, Value* args);
    Value wait_builtin(int argCount, Value* args);
    Value runtests_builtin(int argCount, Value* args);
    Value event_emit_builtin(int argCount, Value* args);
    Value weakref_builtin(int argCount, Value* args);
    Value weak_alive_builtin(int argCount, Value* args);



    // Native functions
    void defineNativeFunctions();

    Value clock_native(int argCount, Value* args);
    Value usSleep_native(int argCount, Value* args);
    Value msSleep_native(int argCount, Value* args);
    Value sleep_native(int argCount, Value* args);
    Value clock_signal_native(int argCount, Value* args);
    Value engine_stop_native(int argCount, Value* args);
    Value typeof_native(int argCount, Value* args);

    // DataflowEngine actor native methods
    Value dataflow_tick_native(int argCount, Value* args);
    Value dataflow_run_native(int argCount, Value* args);
    Value dataflow_run_for_native(int argCount, Value* args);

    // Builtin property getters
    Value signal_value_getter(Value& receiver);

    Value loadlib_native(int argCount, Value* args);
    Value ffi_native(int argCount, Value* args);

    void* createFFIWrapper(void* fn, ffi_type* retType,
                           const std::vector<ffi_type*>& argTypes);

    Value callCFunc(ObjClosure* closure, const CallSpec& callSpec);

    std::vector<uint8_t> objectToCStruct(ObjectInstance* instance);
    ObjectInstance* objectFromCStruct(ObjObjectType* type, const void* data, size_t len);

};


}
