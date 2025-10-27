#pragma once

#include <vector>
#include <atomic>
#include <unordered_map>
#include <map>
#include <deque>
#include <mutex>
#include <array>
#include <filesystem>

#include "core/atomic.h"
#include "Chunk.h"
#include "Value.h"
#include "ArgsView.h"
#include "InterpretResult.h"
#include "Thread.h"
#include "BuiltinModule.h"
#ifdef ROXAL_ENABLE_FILEIO
#include "ModuleFileIO.h"
#endif
#include <ffi.h>
#include <vector>

namespace roxal { struct ObjObjectType; }
using roxal::ObjObjectType;

namespace df { class DataflowEngine; }


namespace roxal {

struct CallFrame; // forward
struct ActorInstance;


typedef std::vector<CallFrame> CallFrames;

struct CallFrame {
    #ifdef DEBUG_BUILD
    CallFrame() : closure(Value::nilVal()), slots(nullptr), strict(false) {}
    #else
    CallFrame() : closure(Value::nilVal()), strict(false) {}
    #endif
    Value closure; // ObjClosure
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

    struct ExceptionHandler {
        Chunk::iterator handlerIp;
        size_t stackDepth;
        size_t frameDepth;
    };
    std::vector<ExceptionHandler> exceptionHandlers;
};



// The Virtual Machine (singleton)
class VM
{
public:
    friend class Thread;
    friend class ModuleSys;
    friend class SimpleMarkSweepGC;

    enum class CacheMode {
        Normal,
        NoCache,
        Recompile
    };

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
    void setCacheMode(CacheMode mode);
    CacheMode cacheMode() const { return cacheModeSetting; }
    bool cacheReadsEnabled() const;
    bool cacheWritesEnabled() const;
    void enableOpcodeProfiling(std::string filePath = {});
    void writeOpcodeProfile();

    Value getBuiltinModule(const icu::UnicodeString& name);
    std::optional<Value> loadGlobal(const icu::UnicodeString& name) { return globals.load(name); }
    void registerBuiltinModule(ptr<BuiltinModule> module);

    InterpretResult interpret(std::istream& source, const std::string& sourceName);
    InterpretResult interpretLine(std::istream& linestream, bool replMode=true);


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
    Value captureUpvalue(Value& local); // returns ObjUpvalue
    void closeUpvalues(Value* last);
    Value opReturn();
    bool isAccessAllowed(const Value& ownerType, ast::Access access);

    void defineProperty(ObjString* name);
    void defineMethod(ObjString* name);
    void defineEnumLabel(ObjString* name);
    void defineNative(const std::string& name, NativeFn function,
                      ptr<type::Type> funcType = nullptr,
                      std::vector<Value> defaults = {});

    // Helper used by builtin call marshalling
    size_t marshalArgs(ptr<type::Type> funcType,
                       const std::vector<Value>& defaults,
                       const CallSpec& callSpec,
                       Value* out,
                       bool includeReceiver = false,
                       const Value& receiver = Value::nilVal(),
                       const std::map<int32_t, Value>& paramDefaultFuncs = {});

    bool callNativeFn(NativeFn fn, ptr<type::Type> funcType,
                      const std::vector<Value>& defaults,
                      const CallSpec& callSpec,
                      bool includeReceiver = false,
                      const Value& receiver = Value::nilVal(),
                      const Value& declFunction = Value::nilVal());

    // Expose a simple helper to keep track of active threads.  Actor
    // deserialization needs this to prevent the thread object from being
    // destroyed immediately after creation.
    inline void registerThread(ptr<Thread> t) { threads.store(t->id(), t); }

    void wakeAllThreadsForGC();

    // Request termination of the VM with the given exit code
    void requestExit(int code);
    inline bool isExitRequested() const { return exitRequested.load(); }
    inline int exitCode() const { return exitCodeValue.load(); }

    // Join all currently tracked threads, optionally skipping one by id.
    // Returns InterpretResult::RuntimeError if any joined thread failed.
    InterpretResult joinAllThreads(uint64_t skipId = 0);


    const int MaxStack = 1024;
    typedef std::vector<Value> ValueStack;

    inline void push(const Value& value) { thread->push(value); }
    inline Value pop() { return thread->pop(); }
    inline void popN(size_t n) { thread->popN(n); } // call pop() n times
    inline Value& peek(int distance) { return thread->peek(distance); }



    // the current thread
    static thread_local ptr<Thread> thread;

    void executeBuiltinModuleScript(const std::string& path, Value moduleType/*ObjModuleType */);

    // Builtin functions (moved from private)
    void defineBuiltinFunctions();

protected:
    VM();
    ~VM();

    void ensureDataflowEngineStopped();

    std::pair<InterpretResult,Value> execute();

    bool outputBytecodeDisassembly;
    bool lineMode;
    std::istream* lineStream;

    std::vector<std::string> modulePaths {};

    static constexpr size_t OpcodeCount = static_cast<size_t>(OpCode::_Last);
    std::atomic_bool opcodeProfilingEnabled {false};
    std::filesystem::path opcodeProfilePath {"opcode_profile.json"};
    std::array<std::atomic<uint64_t>, OpcodeCount> opcodeProfileCounts {};

    CacheMode cacheModeSetting;

    atomic_unordered_map<uint64_t, ptr<Thread>> threads;

    // Set when any thread encounters a runtime error so that
    // all threads can terminate early.
    std::atomic_bool runtimeErrorFlag {false};

    // Set when exit() builtin is called to terminate the VM.
    std::atomic_bool exitRequested {false};
    std::atomic_int exitCodeValue {0};

    std::atomic_bool objectCleanupPending {false};

    // Persistent thread used for REPL execution so that state such as event
    // handlers persists across entered lines.
    ptr<Thread> replThread;

    ObjModuleType* moduleType()
    {
        #ifdef DEBUG_BUILD
        assert(thread != nullptr);
        assert(!thread->frames.empty());
        assert(isClosure(thread->frames.back().closure));
        assert(asClosure(thread->frames.back().closure)->function.isNonNil());
        assert(isFunction(asClosure(thread->frames.back().closure)->function));
        assert(isModuleType(asFunction(asClosure(thread->frames.back().closure)->function)->moduleType));
        #endif
        auto currentFrame { thread->frames.back() };

        return asModuleType(asFunction(asClosure(currentFrame.closure)->function)->moduleType);
    }
    inline VariablesMap& moduleVars() { return moduleType()->vars; }

    // global vars cannot be created in the language, but represent builtin symbols available in all modules
    VariablesMap globals;

    // builtin modules
    std::vector<ptr<BuiltinModule>> builtinModules;

    // builtin dataflow engine actor
    ptr<df::DataflowEngine> dataflowEngine;
    Value dataflowEngineActor;
    ptr<Thread> dataflowEngineThread;

    Value conditionalInterruptClosure {}; // ObjClosure



public:
    Value getConditionalInterruptClosure() const { return conditionalInterruptClosure; } // ObjClosure


    Value initString; // ObjString "init"

    // TODO: perhaps implement inheritance first, then pre-define
    //  object type as root of class heirarchy and add clone() and other
    //  builtins to that
    // Builtin method info structure
    struct BuiltinMethodInfo {
        NativeFn function;
        bool isProc;  // true for proc methods, false for func methods
        ptr<type::Type> funcType;
        std::vector<Value> defaultValues;
        Value declFunction;

        BuiltinMethodInfo() : isProc(false), declFunction(Value::nilVal()) {}
        BuiltinMethodInfo(NativeFn fn, bool proc = false,
                          ptr<type::Type> type=nullptr,
                          std::vector<Value> defaults = {},
                          Value declFn = Value::nilVal())
            : function(fn), isProc(proc), funcType(type),
              defaultValues(std::move(defaults)), declFunction(declFn) {}

        void trace(ValueVisitor& visitor) const
        {
            for (const auto& value : defaultValues) {
                visitor.visit(value);
            }
            visitor.visit(declFunction);
        }
    };

    // Builtin methods: builtin value type -> method name hash -> BuiltinMethodInfo
    std::unordered_map<ValueType, std::unordered_map<int32_t, BuiltinMethodInfo>> builtinMethods;

    bool processPendingEvents();

    void resetStack();
    void freeObjects();
    void requestObjectCleanup();
    bool consumePendingObjectCleanup();
    bool isObjectCleanupPending() const;
    void cleanupWeakRegistries();
    void unwindFrame();
    void raiseException(Value exc);
    void outputAllocatedObjs();

    void concatenate();

    void runtimeError(const std::string& format, ...);




    void defineBuiltinMethods();
    void defineBuiltinMethod(ValueType type, const std::string& name, NativeFn fn,
                             bool isProc = false,
                             ptr<type::Type> funcType = nullptr,
                             std::vector<Value> defaults = {},
                             Value declFunction = Value::nilVal());

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

    Value vector_norm_builtin(ArgsView args);
    Value vector_sum_builtin(ArgsView args);
    Value vector_normalized_builtin(ArgsView args);
    Value vector_dot_builtin(ArgsView args);
    Value matrix_rows_builtin(ArgsView args);
    Value matrix_cols_builtin(ArgsView args);
    Value matrix_transpose_builtin(ArgsView args);
    Value matrix_determinant_builtin(ArgsView args);
    Value matrix_inverse_builtin(ArgsView args);
    Value matrix_trace_builtin(ArgsView args);
    Value matrix_norm_builtin(ArgsView args);
    Value matrix_sum_builtin(ArgsView args);

    Value list_append_builtin(ArgsView args);

    Value signal_run_builtin(ArgsView args);
    Value signal_stop_builtin(ArgsView args);
    Value signal_tick_builtin(ArgsView args);
    Value signal_freq_builtin(ArgsView args);
    Value signal_set_builtin(ArgsView args);


    Value event_emit_builtin(ArgsView args);
    Value event_on_builtin(ArgsView args);
    Value event_off_builtin(ArgsView args);

    // Output stack traces for all running threads
    void dumpStackTraces();

    Value captureStacktrace();



    // Native functions
    void defineNativeFunctions();

    Value clock_native(ArgsView args);
    Value clock_signal_native(ArgsView args);
    Value engine_stop_native(ArgsView args);
    Value typeof_native(ArgsView args);
    Value df_graph_native(ArgsView args);
    Value df_graphdot_native(ArgsView args);

    // DataflowEngine actor native methods
    Value dataflow_tick_native(ArgsView args);
    Value dataflow_run_native(ArgsView args);
    Value dataflow_run_for_native(ArgsView args);

    // Builtin property getters
    Value signal_value_getter(Value& receiver);
    Value signal_name_getter(Value& receiver);
    void  signal_name_setter(Value& receiver, Value value);
    Value exception_stacktrace_getter(Value& receiver);
    Value exception_stacktrace_string_getter(Value& receiver);

    Value loadlib_native(ArgsView args);
    Value ffi_native(ArgsView args);

private:
    bool isCurrentThreadActorWorker() const;
    // Actor workers are not allowed to synchronously join other actor threads
    // during GC teardown because they might be the very threads being joined.
    // We therefore collect actor instances that need their worker shut down in
    // a queue and let whichever non-actor thread next runs the GC drain it.
    void enqueueActorFinalizer(ActorInstance* actorInst);
    void drainActorFinalizerQueue(std::vector<ActorInstance*>& out);
    void finalizeActorInstances(std::vector<ActorInstance*>& actors);

    std::mutex actorFinalizerMutex;
    // Stores actor instances whose workers must be joined from a safe
    // (non-actor) context.  Entries are populated by actor worker threads and
    // drained by regular VM threads before the objects are finally deleted.
    std::deque<ActorInstance*> pendingActorFinalizers;
};


}

namespace roxal {
void scheduleEventHandlers(Value eventWeak, ObjEventType* ev, TimePoint when);
}
