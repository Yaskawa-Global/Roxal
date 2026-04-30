#pragma once

#include <vector>
#include <atomic>
#include <unordered_map>
#include <map>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <array>
#include <filesystem>

#include "core/atomic.h"
#include "Chunk.h"
#include "Value.h"
#include "ArgsView.h"
#include "ExecutionStatus.h"
#include "Thread.h"
#include "BuiltinModule.h"
#include "LazyModuleRegistry.h"
#ifdef ROXAL_ENABLE_FILEIO
#include "ModuleFileIO.h"
#endif
#ifdef ROXAL_ENABLE_GRPC
#include "ModuleGrpc.h"
#endif
#ifdef ROXAL_ENABLE_DDS
#include "dds/ModuleDDS.h"
#endif
#ifdef ROXAL_ENABLE_REGEX
#include "ModuleRegex.h"
#endif
#ifdef ROXAL_ENABLE_SOCKET
#include "ModuleSocket.h"
#endif
#ifdef ROXAL_ENABLE_AI_NN
#include "ModuleNN.h"
#endif
#ifdef ROXAL_ENABLE_MEDIA
#include "ModuleMedia.h"
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
    CallFrame() : closure(Value::nilVal()), slots(nullptr), strict(false), callerStrict(false), isEventHandler(false), isContinuationCallback(false) {}
    #else
    CallFrame() : closure(Value::nilVal()), strict(false), callerStrict(false), isEventHandler(false), isContinuationCallback(false) {}
    #endif
    Value closure; // ObjClosure
    Chunk::iterator startIp;
    Chunk::iterator ip;
    Value* slots;

    CallFrames::iterator parent;

    bool strict; // whether current frame executes in strict mode
    bool callerStrict; // caller's lexical strict setting (for parameter conversion context)

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

    bool isEventHandler { false }; // true for event handler frames (pushed by processEventDispatch)
    bool isContinuationCallback { false }; // true for native continuation callback frames (e.g., filter/map/reduce, native default params)
};



// The Virtual Machine (singleton)
class VM
{
public:
    friend class Thread;
    friend class ModuleSys;
    friend class SimpleMarkSweepGC;
#ifdef ROXAL_ENABLE_GRPC
    friend class ModuleGrpc;
#endif
#ifdef ROXAL_ENABLE_DDS
    friend class ModuleDDS;
#endif

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
    void setScriptArguments(const std::vector<std::string>& args);
    const std::vector<std::string>& getScriptArguments() const { return scriptArguments; }
    void setCacheMode(CacheMode mode);
    CacheMode cacheMode() const { return cacheModeSetting; }
    bool cacheReadsEnabled() const;
    bool cacheWritesEnabled() const;
    void enableOpcodeProfiling(std::string filePath = {});
    void writeOpcodeProfile();

    ptr<BuiltinModule> getBuiltinModule(const icu::UnicodeString& name);
    Value getBuiltinModuleType(const icu::UnicodeString& name);
    std::optional<Value> loadGlobal(const icu::UnicodeString& name) { return globals.load(name); }
    void storeGlobal(const icu::UnicodeString& name, const Value& value) { globals.storeGlobal(name, value); }
    void registerBuiltinModule(ptr<BuiltinModule> module);
#ifdef ROXAL_ENABLE_GRPC
    Value importProtoModule(const std::string& path);
#endif
#ifdef ROXAL_ENABLE_DDS
    Value importIdlModule(const std::string& path);
#endif

    // =========================================================================
    // Execution API
    // =========================================================================

    // --- One-shot execution ---
    /// Compile and run source to completion. Suitable for simple scripts.
    ExecutionStatus run(std::istream& source, const std::string& sourceName);

    /// REPL mode: compile and execute a single line/expression.
    ExecutionStatus runLine(std::istream& linestream,
                                  bool replMode=true,
                                  const std::string& sourceNameOverride="");

    // --- Incremental execution ---
    // Use setup() + runFor() when you need control over execution timing,
    // e.g., running Roxal within a host application's main loop.

    /// Compile source and set up initial call frame, but don't execute.
    /// Returns CompileError on failure, OK on success.
    /// After setup(), call runFor() repeatedly to execute incrementally.
    ExecutionStatus setup(std::istream& source, const std::string& sourceName);

    /// Execute for up to the given duration, then yield.
    /// Returns: {OK, returnValue} if completed, {Yielded, nil} if budget exhausted or blocked,
    /// {RuntimeError, nil} on error. Call repeatedly to continue execution.
    std::pair<ExecutionStatus, Value> runFor(TimeDuration duration);

    /// Check if the current thread has more work to do (not completed).
    bool hasMoreWork() const;

    /// Check if the current thread is blocked (sleeping or awaiting future).
    bool isBlocked() const;

    /// Get the earliest time the blocked thread could make progress.
    /// Returns TimePoint::max() if not blocked or if blocked on future.
    TimePoint blockedUntil() const;

    // --- RT REPL integration ---
    // Use setupLine() on a non-RT thread to compile REPL input, then
    // runFor() on an RT thread to execute incrementally with a time budget.
    // setupLine() + runFor() can be used interchangeably with setup() + runFor().

    enum class RTState : int { Idle, Ready, Executing, Yielded };

    /// Compile a REPL line/script and enqueue the closure for execution via runFor().
    /// Blocks if previous work is still executing (waits for Idle state).
    /// Uses persistent REPL state (replThread, replModuleValue, compiler).
    ExecutionStatus setupLine(std::istream& linestream,
                              bool replMode = true,
                              const std::string& sourceNameOverride = "");

    /// Current RT coordination state (for diagnostics/coordination).
    RTState rtState() const { return rtState_.load(std::memory_order_acquire); }

    /// Block until rtState_ becomes Idle (RT thread finished executing).
    void waitForRTCompletion();

    /// Set the RT core index that actor threads should avoid.
    /// Set to -1 (default) to disable actor thread affinity restrictions.
    /// When set (e.g. to 3), spawned actor threads will be pinned to all cores
    /// except this one and will use SCHED_OTHER (non-RT) scheduling.
    void setRTCoreExclusion(int coreIndex) { rtCoreExclusion_ = coreIndex; }
    int rtCoreExclusion() const { return rtCoreExclusion_; }

    /// Control the synchronous-execution guard that prevents runFor() from
    /// entering execute() while run()/runLine() owns the VM. Tests that call
    /// runFor() from within a native builtin can temporarily clear this.
    void setSynchronousExecution(bool sync) { inSynchronousExecution_.store(sync, std::memory_order_release); }

    /// Enable timing instrumentation for native (C++) function calls.
    /// When enabled, calls that exceed the remaining RT budget are logged
    /// with the function name to help identify blocking builtins.
    void setNativeCallTimingEnabled(bool enabled) { nativeCallTimingEnabled_ = enabled; }

    /// After runFor() returns, check if a native call exceeded the RT budget.
    /// Returns the function name and elapsed time, or empty string if no overrun.
    /// Clears the stored overrun on read. Call from the same thread as runFor().
    static std::string consumeNativeCallOverrun();

    // =========================================================================
    // Internal call mechanics (used by the above APIs)
    // =========================================================================

    bool call(ObjClosure* closure, const CallSpec& callSpec);
    bool call(ValueType builtinType, const CallSpec& callSpec);
    bool callValue(const Value& callee, const CallSpec& callSpec);
    bool invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec,
                        const Value& receiver);
    bool invoke(ObjString* name, const CallSpec& callSpec);

    // Operator method name hashes for fast lookup during operator dispatch
    struct OperatorHashes {
        int32_t op;   // "operator<sym>"
        int32_t lop;  // "loperator<sym>"
        int32_t rop;  // "roperator<sym>"
    };

    // Operator overload dispatch helpers
    // Returns the method closure Value, or nil if not found. Walks supertype chain.
    Value findOperatorMethod(ObjObjectType* type, int32_t hash);
    bool tryDispatchBinaryOperator(const OperatorHashes& hashes);
    bool tryDispatchUnaryOperator(int32_t hash);

    // Conversion operator lookup. Returns method closure Value (nil if not found
    // or not allowed in current strict context when implicitCall is true).
    Value findConversionMethod(const Value& instanceType, int32_t hash, bool implicitCall);

    // Check if a value can be converted to the target type (pure predicate, no side effects).
    bool canConvertToType(const Value& val, const Value& targetTypeSpec, bool implicitCall) const;

    // Unified type conversion. Attempts to convert val to targetTypeSpec.
    // Returns outcome indicating whether conversion was sync, async (frame pushed), or failed.
    // For NeedsAsyncFrame: a call frame + PendingConversion have been set up;
    // the caller must break to the dispatch loop. The converted value will be
    // pushed by the PendingConversion completion handler when the frame returns.
    enum class ConversionResult { AlreadyCorrectType, ConvertedSync, NeedsAsyncFrame, Failed };
    struct ConversionOutcome {
        ConversionResult result;
        Value convertedValue;  // valid when result == ConvertedSync
    };
    ConversionOutcome tryConvertValue(
        const Value& val,
        const Value& targetTypeSpec,
        bool strict,
        bool implicitCall,
        Thread::PendingConversion::Kind pendingKind,
        const Value& savedContext = Value::nilVal()
    );

    /// Invoke a closure with arguments. Executes until completion or deadline.
    /// Returns {OK, value} on completion, {Yielded, nil} if deadline exceeded,
    /// {RuntimeError, nil} on error.
    /// Used by REPL, module execution, dataflow func nodes, event handlers.
    std::pair<ExecutionStatus,Value> invokeClosure(ObjClosure* closure,
                                                    const std::vector<Value>& args,
                                                    TimePoint deadline = TimePoint::max());
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
    void defineEventPayload(ObjString* name);
    void extendEventType();
    void defineMethod(ObjString* name);
    void defineEnumLabel(ObjString* name);
    void defineNative(const std::string& name, NativeFn function,
                      ptr<type::Type> funcType = nullptr,
                      std::vector<Value> defaults = {},
                      uint32_t resolveArgMask = 0);

    // Helper used by builtin call marshalling
    size_t marshalArgs(ptr<type::Type> funcType,
                       const std::vector<Value>& defaults,
                       const CallSpec& callSpec,
                       Value* out,
                       bool includeReceiver = false,
                       const Value& receiver = Value::nilVal(),
                       const std::map<int32_t, Value>& paramDefaultFuncs = {});

    // Identifies which params need closure default evaluation (returns param indices)
    std::vector<size_t> getClosureDefaultParamIndices(
        ptr<type::Type> funcType,
        const std::vector<Value>& defaults,
        const CallSpec& callSpec,
        const std::map<int32_t, Value>& paramDefaultFuncs);

    // Marshal args without evaluating closure defaults (stores nil placeholders)
    size_t marshalArgsPartial(ptr<type::Type> funcType,
                              const std::vector<Value>& defaults,
                              const CallSpec& callSpec,
                              Value* out,
                              bool includeReceiver = false,
                              const Value& receiver = Value::nilVal(),
                              const std::map<int32_t, Value>& paramDefaultFuncs = {});

    // Process native default param continuation after a closure default returns
    // Called by the nativeContinuation.onComplete callback
    bool processNativeDefaultParamDispatch(Value defaultValue);

    // Check if a future's promised type is assignable to the target type.
    // If true, the future can pass through without resolution.
    bool isFutureAssignableTo(const Value& futureVal, ValueType targetVT);
    bool isFutureAssignableTo(const Value& futureVal, const Value& targetTypeSpec);

    // Returns true if converting val to the given param type requires executing Roxal code
    // (user-defined conversion operator or constructor auto-conversion).
    bool needsAsyncConversion(const Value& val, ptr<type::Type> paramType, bool strictCtx);

    // Process native param conversion continuation after a conversion frame returns
    bool processNativeParamConversion(Value convertedValue);

    // Process closure param conversion after a conversion frame returns
    bool processClosureParamConversion(Value convertedValue);

    // Push a conversion frame for a single param (operator call or constructor call).
    // strictCtx: the caller's lexical strict setting
    bool pushParamConversionFrame(const Value& val, ptr<type::Type> paramType, bool strictCtx);

    bool callNativeFn(NativeFn fn, ptr<type::Type> funcType,
                      const std::vector<Value>& defaults,
                      const CallSpec& callSpec,
                      bool includeReceiver = false,
                      const Value& receiver = Value::nilVal(),
                      const Value& declFunction = Value::nilVal(),
                      uint32_t resolveArgMask = 0);

    // Expose a simple helper to keep track of active threads.  Actor
    // deserialization needs this to prevent the thread object from being
    // destroyed immediately after creation.
    inline void registerThread(ptr<Thread> t) { threads.store(t->id(), t); }
    inline void unregisterThread(uint64_t id) { threads.erase(id); }

    void wakeAllThreadsForGC();

    // Request termination of the VM with the given exit code
    void requestExit(int code);
    inline bool isExitRequested() const { return exitRequested.load(); }
    inline int exitCode() const { return exitCodeValue.load(); }

    // Join all currently tracked threads, optionally skipping one by id.
    // Returns ExecutionStatus::RuntimeError if any joined thread failed.
    ExecutionStatus joinAllThreads(uint64_t skipId = 0);


    static constexpr size_t DefaultMaxStack = 1024;
    static constexpr size_t DefaultMaxCallFrames = 128;

    static std::string versionString();
    static std::vector<std::string> featureStrings();
    static std::string featureString();
#ifdef ROXAL_COMPUTE_SERVER
    using PrintTarget = ActorInstance::MethodCallInfo::PrintTarget;
    struct ScopedPrintTarget {
        explicit ScopedPrintTarget(const PrintTarget& target);
        ~ScopedPrintTarget();
        PrintTarget previous;
    };
    static const PrintTarget& currentPrintTarget();
    static void emitPrintOutput(const std::string& text, bool flush, bool here = false);
#endif
    static std::filesystem::path executablePath();
    static std::vector<std::string> defaultModuleSearchPaths();
    static void configureModulePaths(const std::vector<std::string>& modulePaths);

    static void configureStackLimits(size_t stackSize, size_t callFrameLimit);
    static void configureCacheMode(CacheMode mode);
    void setStackLimits(size_t stackSize, size_t callFrameLimit);
    size_t maxStackSize() const { return stackLimit; }
    size_t maxCallFrameCount() const { return callFrameLimit; }
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
    friend class LazyModuleRegistry;  // For lazy module loading

    VM();
    ~VM();

    size_t stackLimit { DefaultMaxStack };
    size_t callFrameLimit { DefaultMaxCallFrames };

    void ensureDataflowEngineStopped();

    /// Low-level dispatch loop. Runs until completion, error, or deadline.
    /// Prefer runFor() for incremental execution; this is used internally
    /// by run(), runFor(), and invokeClosure().
    std::pair<ExecutionStatus,Value> execute(TimePoint deadline = TimePoint::max());

    bool outputBytecodeDisassembly;
    bool lineMode;
    std::istream* lineStream;

    std::vector<std::string> modulePaths {};
    std::vector<std::string> scriptArguments {};

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

    // builtin modules (eagerly loaded, e.g. sys)
    std::vector<ptr<BuiltinModule>> builtinModules;
    // lazy-loaded builtin modules (loaded on first import)
    LazyModuleRegistry lazyModuleRegistry;
#ifdef ROXAL_ENABLE_GRPC
    ModuleGrpc* grpcModule { nullptr };
#endif
#ifdef ROXAL_ENABLE_DDS
    ModuleDDS* ddsModule { nullptr };
#endif

    // builtin dataflow engine actor
    ptr<df::DataflowEngine> dataflowEngine;
    Value dataflowEngineActor;
    ptr<Thread> dataflowEngineThread;

    Value conditionalInterruptClosure {}; // ObjClosure
    Value replModuleValue { Value::nilVal() }; // ObjModuleType

    // RT REPL synchronization
    std::atomic<RTState> rtState_ { RTState::Idle };
    std::mutex rtMutex_;
    std::condition_variable rtCondVar_;
    Value pendingRTClosure_ { Value::nilVal() }; // protected by rtMutex_
    int rtCoreExclusion_ { -1 }; // -1 = disabled (desktop), >=0 = exclude this core for actor threads

    // Guard: prevents runFor() from entering execute() while run()/runLine() is executing
    // synchronously. Handles the case where ax.init() (inside a synchronous --setup script)
    // starts the WC RoxalLoop whose callback calls runFor().
    std::atomic<bool> inSynchronousExecution_ { false };

    // Native call timing instrumentation.
    // When enabled, callNativeFn() times each C++ native call and warns if it
    // exceeds the remaining RT budget. Identifies blocking builtins by name.
    // The deadline and call context are thread_local since execute() runs on
    // multiple threads (RT main thread + non-RT actor threads).
    bool nativeCallTimingEnabled_ { false };
    static thread_local TimePoint nativeCallDeadline_;
    static thread_local UnicodeString nativeCallContext_;
    static thread_local std::string nativeCallOverrun_; // set by callNativeFn() on overrun
#ifdef ROXAL_COMPUTE_SERVER
    static thread_local PrintTarget currentPrintTarget_;
#endif

    // Dataflow thread flag: when true, module var reads return const refs
    // and module var writes raise a runtime error.
    static thread_local bool onDataflowThread_;

public:
    static bool onDataflowThread() { return onDataflowThread_; }
    static void setOnDataflowThread(bool v) { onDataflowThread_ = v; }
    Value getConditionalInterruptClosure() const { return conditionalInterruptClosure; } // ObjClosure
    ObjModuleType* replModuleType() const;


    Value initString; // ObjString "init"

    OperatorHashes opHashAdd, opHashSub, opHashMul, opHashDiv, opHashMod;
    OperatorHashes opHashEq, opHashNe, opHashLt, opHashGt, opHashLe, opHashGe;
    int32_t opHashNeg;  // "uoperator-"
    int32_t opHashConvString;  // "operator->string"

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
        uint32_t resolveArgMask {0}; // bit N set → resolve arg N before call
        bool noMutateSelf {false};   // method doesn't mutate receiver state
        uint32_t noMutateArgs {0};   // bitmask: bit N set → arg N not mutated

        BuiltinMethodInfo() : isProc(false), declFunction(Value::nilVal()) {}
        BuiltinMethodInfo(NativeFn fn, bool proc = false,
                          ptr<type::Type> type=nullptr,
                          std::vector<Value> defaults = {},
                          Value declFn = Value::nilVal(),
                          uint32_t resolveMask = 0,
                          bool noMutateSelf_ = false,
                          uint32_t noMutateArgs_ = 0)
            : function(fn), isProc(proc), funcType(type),
              defaultValues(std::move(defaults)), declFunction(declFn),
              resolveArgMask(resolveMask),
              noMutateSelf(noMutateSelf_), noMutateArgs(noMutateArgs_) {}

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

    // Event handler closures are pushed as regular call frames (like func call).
    bool processEventDispatch();
    bool invokeNextEventHandler();

    // Native continuation support - allows native functions to call Roxal closures
    // without re-entering execute() (e.g., list.filter/map/reduce)
    bool processContinuationDispatch();
    bool pushContinuationCall(ObjClosure* closure, const std::vector<Value>& args);
    void clearContinuation();

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
    void reportStackOverflow();




    void defineBuiltinMethods();
    void defineBuiltinMethod(ValueType type, const std::string& name, NativeFn fn,
                             bool isProc = false,
                             ptr<type::Type> funcType = nullptr,
                             std::vector<Value> defaults = {},
                             Value declFunction = Value::nilVal(),
                             bool noMutateSelf = false,
                             uint32_t noMutateArgs = 0);

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
    Value vector_min_builtin(ArgsView args);
    Value vector_max_builtin(ArgsView args);
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
    Value matrix_min_builtin(ArgsView args);
    Value matrix_max_builtin(ArgsView args);
    Value tensor_min_builtin(ArgsView args);
    Value tensor_max_builtin(ArgsView args);
    Value tensor_sum_builtin(ArgsView args);

    // Orient methods
    Value orient_rotate_builtin(ArgsView args);
    Value orient_slerp_builtin(ArgsView args);
    Value orient_angle_to_builtin(ArgsView args);
    Value orient_euler_builtin(ArgsView args);

    // Orient property getters
    Value orient_rpy_getter(Value& receiver);
    Value orient_r_getter(Value& receiver);
    Value orient_p_getter(Value& receiver);
    Value orient_y_getter(Value& receiver);
    Value orient_quat_getter(Value& receiver);
    Value orient_mat_getter(Value& receiver);
    Value orient_axis_getter(Value& receiver);
    Value orient_angle_getter(Value& receiver);
    Value orient_inverse_getter(Value& receiver);

    Value list_append_builtin(ArgsView args);
    Value list_filter_builtin(ArgsView args);
    Value list_map_builtin(ArgsView args);
    Value list_reduce_builtin(ArgsView args);

#ifdef ROXAL_ENABLE_REGEX
    Value string_match_builtin(ArgsView args);
    Value string_search_builtin(ArgsView args);
    Value string_replace_builtin(ArgsView args);
    Value string_split_builtin(ArgsView args);
#endif

    Value signal_run_builtin(ArgsView args);
    Value signal_stop_builtin(ArgsView args);
    Value signal_tick_builtin(ArgsView args);
    Value signal_freq_builtin(ArgsView args);
    Value signal_set_builtin(ArgsView args);
    Value signal_on_changed_builtin(ArgsView args);


    Value event_emit_builtin(ArgsView args);
    Value event_when_builtin(ArgsView args);
    Value event_remove_builtin(ArgsView args);

    // Output stack traces for all running threads
    void dumpStackTraces();

    Value captureStacktrace();

    bool resolveValue(Value& value);
    FutureStatus tryResolveValue(Value& value);

    // Non-blocking await helpers for opcode handlers.
    // On Pending, each sets thread->awaitedFuture and rewinds the IP.
    inline FutureStatus tryAwaitFuture(Value& v);
    inline FutureStatus tryAwaitFutures(Value& a, Value& b);
    inline FutureStatus tryAwaitValue(Value& v);
    inline FutureStatus tryAwaitValues(Value& a, Value& b);



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
    Value signal_running_getter(Value& receiver);
    Value exception_stacktrace_getter(Value& receiver);
    Value exception_stacktrace_string_getter(Value& receiver);
    Value exception_detail_getter(Value& receiver);

    // Range property getters
    Value range_start_getter(Value& receiver);
    Value range_stop_getter(Value& receiver);
    Value range_step_getter(Value& receiver);
    Value range_closed_getter(Value& receiver);
    Value range_first_getter(Value& receiver);
    Value range_last_getter(Value& receiver);

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
void scheduleEventHandlers(Value eventWeak, ObjEventType* ev, Value eventInstance, TimePoint when);
}
