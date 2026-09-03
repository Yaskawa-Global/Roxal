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
#include "core/Output.h"
#include "Chunk.h"
#include "Value.h"
#include "CallFrame.h"
#include "ArgsView.h"
#include "ExecutionStatus.h"
#include "OutputRoute.h"
#include "Thread.h"
#include "ExecutionDomain.h"
#include "BuiltinModule.h"
#include "PreparedProgram.h"
#include "EmbeddedRuntime.h"
#include "ReplSession.h"
#include "LazyModuleRegistry.h"
// The optional module headers are deliberately NOT included here.  VM.h needs
// none of them: ModuleFileIO/Regex/Socket/NN/Media are unreferenced in this
// header, and the only gRPC/DDS uses are the forward declarations, friend
// declarations and the `ModuleGrpc*`/`ModuleDDS*` members below -- all of which
// a forward declaration satisfies.  Including them here would force every
// consumer of VM.h (including out-of-tree hosts linking libroxal.a, which must
// define the same ROXAL_ENABLE_* macros to get matching class layouts) onto the
// include paths of gRPC, CycloneDDS, pugixml and friends purely to compile a
// pointer member.  TUs that touch a module include its header directly.
// Same rationale as the qt note below, which the core has always followed.
// NOTE: the qt module (ModuleQt) is a dlopen'd plugin, not part of the core build, so
// its header is deliberately NOT included here — the core only loads it via a C entry
// point (roxal_qt_create_module) resolved at runtime. See the qt factory in VM.cpp.

namespace roxal { struct ObjObjectType; }
using roxal::ObjObjectType;

namespace df { class DataflowEngine; class FuncNode; }


namespace roxal {

struct ActorInstance;
class RoxalCompiler;
// Forward-declared UNCONDITIONALLY so the grpcModule/ddsModule members below
// exist in every translation unit regardless of ROXAL_ENABLE_GRPC/DDS -- see
// the member comment. The full types arrive via the guarded #includes above
// when the features are on; a forward declaration is all a pointer member needs.
class ModuleGrpc;
class ModuleDDS;
class StopCoordinator;   // compiler/debug/StopCoordinator.h


// GC coverage for host-thread code that touches GC state OUTSIDE execute():
// compilation / cache deserialization, embedder init that builds Values or
// stores module vars (e.g. a robot host's init walk), pre-run global installs,
// post-execute teardown.  Such threads are invisible to the collector's
// stop-the-world set, so a concurrent collection could sweep objects that
// exist only on their C++ stacks.  Constructing this makes the thread a GC
// ExternalParticipant for the scope (the collector waits for scope exit;
// bounded by the covered phase).  No-op when the thread is already covered
// (participant / RT yield-section / inside execute()).  Used internally by
// the VM's own host-entry APIs and exported for embedders.
class ScopedGCMutatorCover {
public:
    ScopedGCMutatorCover();
    ~ScopedGCMutatorCover();
    ScopedGCMutatorCover(const ScopedGCMutatorCover&) = delete;
    ScopedGCMutatorCover& operator=(const ScopedGCMutatorCover&) = delete;
private:
    void* participant_ { nullptr };  // SimpleMarkSweepGC::ExternalParticipant* (opaque: keep GC header out of VM.h)
};


// Generic integration point for a host UI event loop (e.g. Qt's QGuiApplication).
// A native module installs an implementation via VM::setHostEventLoop(); the VM
// dispatch loop then services it cooperatively — blocking on waitForEvents() when
// idle (so host events wake the VM with ~zero latency) and calling pump() at a
// throttled cadence while busy. Invoked on the main thread only, and never with a
// VM lock held, so a host callback may safely re-enter the VM. Dependency-free:
// no host-toolkit types leak into the VM, and the hook stays null in the default
// build (no behavior change).
struct HostEventLoop {
    virtual ~HostEventLoop() = default;
    // Block until a host event arrives or `maxWait` elapses, servicing host events.
    virtual void waitForEvents(TimeDuration maxWait) = 0;
    // Non-blocking: service any pending host events and return immediately.
    virtual void pump() = 0;
};


// The Virtual Machine (singleton)
class VM
{
public:
    friend class Thread;
    friend class ModuleSys;
    // The VM half of the two execution collaborators: they exist as separate
    // classes precisely so a host does not see the surface below.
    friend class EmbeddedRuntime;
    friend class ReplSession;
    // Resumes a FuncNode body that yielded, by re-entering execute() on the
    // thread it left suspended.  Part of the runtime, not an embedding: a
    // host never continues someone else's half-finished execution.
    friend class df::FuncNode;
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

    /// Deterministic full teardown: stop and join all VM-owned threads,
    /// unload modules, release the host event loop, and run the final GC.
    /// Idempotent — the destructor calls it as a fallback. Hosts embedding
    /// libroxal should call this (or shutdownIfConstructed()) before
    /// returning from main: left to the singleton's destructor, teardown
    /// runs inside __run_exit_handlers, where cross-library static
    /// destruction order is undefined and host threads may still be
    /// running — historically an exit-time segfault.
    void shutdown();

    /// shutdown() if the singleton was ever created; never materializes it.
    /// Safe to call unconditionally at any return from main.
    static void shutdownIfConstructed();

    /// True once the singleton's constructor has completed. Lets low-level
    /// services (e.g. the GC auto-trigger) avoid re-entering VM::instance()
    /// while the function-local static is still initializing.
    static bool constructed();
    /// True once shutdown() has run.  Retained handles (a ReplSession, a
    /// RunHandle's runtime link) consult this rather than trusting a pointer.
    bool isShutdown() const noexcept {
        return shutdownComplete_.load(std::memory_order_acquire);
    }

    VM(VM const&) = delete;
    void operator=(VM const&) = delete;

    void setDisassemblyOutput(bool outputBytecodeDisassembly);
    void appendModulePaths(const std::vector<std::string>& modulePaths);
    const std::vector<std::string>& getModulePaths() const { return modulePaths; }
    void setScriptArguments(const std::vector<std::string>& args);
    const std::vector<std::string>& getScriptArguments() const { return scriptArguments; }
    void setCacheMode(CacheMode mode);
    CacheMode cacheMode() const { return cacheModeSetting; }
    bool cacheReadsEnabled() const;
    bool cacheWritesEnabled() const;
    void enableOpcodeProfiling(std::string filePath = {});
    void writeOpcodeProfile();

    ptr<BuiltinModule> getBuiltinModule(const ustring& name);
    Value getBuiltinModuleType(const ustring& name);
    std::optional<Value> loadGlobal(const ustring& name) { return globals.load(name); }
    void storeGlobal(const ustring& name, const Value& value) { globals.storeGlobal(name, value); }
    void registerBuiltinModule(ptr<BuiltinModule> module);

    // Cross-compiler user-module canonicalisation.  Each RoxalCompiler
    // instance has its own per-compilation `importedModules` map; without
    // a process-wide registry, two top-level compilations (e.g. a
    // builtin-module's companion .rox followed by a user script that
    // imports the same transitive user module) produce distinct
    // ObjModuleType pointers for the same module.  That breaks
    // `linkMethod`: the native binding lands on one ObjObjectType, but
    // instances constructed later use the other.
    //
    // `lookupUserModule` returns the canonical ObjModuleType Value for
    // a user module if it has already been registered in this VM,
    // otherwise nullopt.  `registerUserModule` records the canonical
    // Value for future lookups; registering happens BEFORE the module
    // body runs, so a circular import (A's body imports B, B's body
    // re-imports A) sees A's already-registered (partially-populated)
    // module rather than infinitely recursing.
    std::optional<Value> lookupUserModule(const ustring& qualifiedName);
    void registerUserModule(const ustring& qualifiedName, const Value& moduleType);

    // REPL-only: drop all cached user-module entries so the next `import X.*`
    // re-runs each module's body, picking up source edits.  Does NOT reset
    // existing bindings in the REPL module's vars — paired with the REPL's
    // overwrite-on-re-import semantics so a subsequent `run` of a script
    // that re-imports the same modules will rebind to the freshly-loaded
    // versions.  Old user-created instances retain their old instanceType
    // and old method tables (Python `reload` semantics — see future task
    // for IPython %autoreload-2-style in-place class mutation).
    void clearUserModuleRegistry();
#ifdef ROXAL_ENABLE_GRPC
    Value importProtoModule(const std::string& path);
#endif
#ifdef ROXAL_ENABLE_DDS
    // annotations: names of annotations attached to the import statement,
    // passed through verbatim (interpreted by the dds module, not the VM).
    Value importIdlModule(const std::string& path,
                          const std::vector<std::string>& annotations = {},
                          std::vector<std::string>* outGlobals = nullptr);
#endif

    // =========================================================================
    // Execution API
    //
    // Three ways to run a program, chosen by what the HOST owns:
    //
    //   no loop of its own    executeProgramSync()
    //   a prompt or console   defaultReplSession(), then fragments
    //   a periodic loop       attachEmbeddedRuntime() + prepareProgram(),
    //                         then EmbeddedRuntime::submit()/driveFor()
    //
    // Exactly one owns execution at a time: once a driver is attached, the
    // synchronous entry points refuse with Busy.  Note that what ADVANCES a
    // run in recipes 2 and 3 is not on VM at all -- it is
    // EmbeddedRuntime::driveFor().  embedding.md writes each recipe out.
    // =========================================================================

    // --- 1. Run a whole program on the calling thread ---
    // start:   executeProgramSync()
    // advance: nothing -- it returns when the program is done

    /// Compile and run a whole program to completion on the CALLING thread.
    /// The synchronous shape: a command line, a test, any host that has no
    /// loop of its own to give the VM.  A host that owns a periodic loop
    /// prepares off its driver and submits to an embedded runtime instead.
    ExecutionStatus executeProgramSync(std::istream& source,
                                       ProgramOptions options);

    // Split form, for a debugger launch that must configure breakpoints
    // between compilation and the first statement.
    // start:   stageProgramSync()
    // advance: executeStagedSync()

    /// Compile a program and stage it on the CALLING thread.  STAGED means:
    /// compiled, thread bound, preludes complete, body not started.  Preludes
    /// are the host's own pre-setup and run here, before a debugger can be
    /// configured and before onScriptStart -- the same order the driver path
    /// uses -- so a breakpoint or stopOnEntry never lands in host plumbing.
    /// As opposed to a PreparedProgram handle, nothing is handed back to own.
    ExecutionStatus stageProgramSync(std::istream& source,
                                     ProgramOptions options);

    /// Run the program staged on this thread to completion.  Must be called
    /// on the thread that staged it.
    ExecutionStatus executeStagedSync();

    // --- 2. Persistent fragment sessions ---
    // start:   defaultReplSession(), then ReplSession::prepareFragment()
    // advance: no driver -> ReplSession::evaluateFragmentSync(), which runs
    //                       the fragment to completion on the CALLING thread
    //                       using the session's own Thread
    //          driver    -> EmbeddedRuntime::submit() + driveFor(); the slice
    //                       binds the SESSION's thread and fires no script
    //                       start/complete hooks
    //
    // A session owns the compiler, module and thread that fragments share --
    // which is what lets a fragment see what earlier fragments declared,
    // where a fresh program never does.  Version 1 has one per VM (the
    // REPL's), created on first use; the shape is already per-session so
    // more can follow without changing callers.
    ReplSession createReplSession(ReplOptions options = {});
    ReplSession defaultReplSession();

    // --- 3. Programs driven by a host loop ---
    // start:   attachEmbeddedRuntime() once, then prepareProgram() off the
    //          driver, then EmbeddedRuntime::submit()
    // advance: EmbeddedRuntime::driveFor(budget), on the driver thread
    // finish:  RunHandle::wait(), on a NON-driver thread

    // Compiling and starting are separate operations.  prepareProgram()
    // compiles the source and roots everything the launch will need --
    // closure, module, imports, prelude receivers -- WITHOUT assigning this
    // thread's execution state, pushing a frame, running a prelude, or
    // touching a live run's error state.  The result is a movable handle over
    // stable heap storage: it survives its producer returning, and survives
    // collection, because the record carries the launch's only mark root.
    //
    // Preparation is a non-driver operation.  It parses, reads and writes the
    // bytecode cache, allocates, and updates shared registries, so it belongs
    // on a thread the embedding can afford to spend unbounded time on -- never
    // inside a control slice.  While it runs, tracing collection is latched
    // rather than published, so a host driving real-time slices keeps working.
    PrepareProgramResult prepareProgram(std::istream& source,
                                        ProgramOptions options);

    // Attaching hands execution ownership to a host that owns a periodic
    // loop.  It is explicit and unique per VM, and it is a one-way statement
    // about who runs programs: once attached, the synchronous entry points
    // (executeProgramSync/stageProgramSync and session evaluation) refuse
    // with Busy rather than competing with the driver for the same VM.
    AttachDriverResult attachEmbeddedRuntime(DriverOptions options = {});

    /// Give ownership back.  Refused while a claimed run is still in flight:
    /// tearing a record out from under a live execution is not a detach.
    DetachStatus detachEmbeddedRuntime();

    /// Unconditional teardown for host shutdown: cancels whatever the runtime
    /// still owns -- pending or claimed -- and releases those roots while the
    /// collector is alive.  A host uses this when it is going away and the
    /// run's outcome no longer matters.
    void shutdownEmbeddedRuntime();

    /// The attached runtime, or null when none is.  A NON-OWNING observation:
    /// the VM owns the runtime, and detach/shutdown destroy it regardless of
    /// what a host is holding -- otherwise a detached runtime could stay
    /// alive as a zombie, still holding a driver identity and a pending slot
    /// while the VM believed nothing was attached.
    ///
    /// Valid only while attached.  A host that caches this across its own
    /// detachEmbeddedRuntime()/shutdownEmbeddedRuntime() has a dangling
    /// pointer; re-read it, or check embeddedDriverAttached() first.  (A
    /// RunHandle, by contrast, may safely outlive the runtime: it holds a
    /// weak lease that keeps the object alive only for the duration of a
    /// call, because a host is expected to keep run handles around.)
    EmbeddedRuntime* embeddedRuntime() const { return embeddedRuntime_.get(); }
    bool embeddedDriverAttached() const {
        return embeddedRuntimeAttached_.load(std::memory_order_acquire);
    }

    // --- Execution state of the calling thread ---

    /// Check if the current thread has more work to do (not completed).
    bool hasMoreWork() const;

    /// Check if the current thread is blocked (sleeping or awaiting future).
    bool isBlocked() const;

    /// Get the earliest time the blocked thread could make progress.
    /// Returns TimePoint::max() if not blocked or if blocked on future.
    TimePoint blockedUntil() const;

    // --- Host UI event-loop integration ---
    /// Install (nullptr clears) a host UI event-loop integration. A native module
    /// (e.g. qt) sets this on the main thread before loading any host UI; the
    /// dispatch loop then services the host loop cooperatively. See HostEventLoop.
    /// Held by ptr<> (shared) so the module and VM share ownership, matching the
    /// VM's convention of managing instances via ptr<>/Value rather than raw C ptrs.
    void setHostEventLoop(ptr<HostEventLoop> loop) { hostEventLoop_ = std::move(loop); }
    const ptr<HostEventLoop>& hostEventLoop() const { return hostEventLoop_; }



    /// Set the RT core index that actor threads should avoid.
    /// Set to -1 (default) to disable actor thread affinity restrictions.
    /// When set (e.g. to 3), spawned actor threads will be pinned to all cores
    /// except this one and will use SCHED_OTHER (non-RT) scheduling.
    void setRTCoreExclusion(int coreIndex) { rtCoreExclusion_ = coreIndex; }
    int rtCoreExclusion() const { return rtCoreExclusion_; }

    /// ABI guard: `sizeof(VM)` as libroxal itself was compiled (with the
    /// library's ROXAL_ENABLE_* feature flags).  Deliberately OUT-OF-LINE so a
    /// consumer that includes VM.h with a different flag set gets the library's
    /// real size, not its own view.  A host can compare this to its own
    /// `sizeof(roxal::VM)` at startup and fail loudly on mismatch -- the exact
    /// hazard that silently corrupted memory before the grpcModule/ddsModule
    /// members were made flag-independent.  See ModuleRobot's static check.
    static std::size_t abiInstanceSize();

    /// Enable timing instrumentation for native (C++) function calls.
    /// When enabled, calls that exceed the remaining RT budget are logged
    /// with the function name to help identify blocking builtins.
    void setNativeCallTimingEnabled(bool enabled) { nativeCallTimingEnabled_ = enabled; }

    /// After a slice returns, check if a native call exceeded the RT budget.
    /// Returns the function name and elapsed time, or empty string if no overrun.
    /// Clears the stored overrun on read. Call from the thread that drove it.
    static std::string consumeNativeCallOverrun();

    /// Main-thread identity: the host's own thread that runs scripts/REPL
    /// (as opposed to spawned actor/worker threads).  Consulted by the host
    /// UI event-loop pump, which may only be driven from that thread.
    /// markMainThread() latches the first calling thread; later calls no-op.
    static void markMainThread();
    static bool onMainThread();
    // Web liveness: pump the host UI loop briefly while parked in a debug
    // stop -- see StopCoordinator::ackAndPark's pump-park.
    void debugStopHostPump();
    // Spawn an actor thread for a host-constructed actor instance (the
    // script-instantiation shape: create + register + act).  The web
    // module builds the debugger control actor this way.
    // debugExcluded marks the thread BEFORE it becomes runnable -- a
    // service actor must never be trappable by a prebound breakpoint in
    // the window before its first method could self-mark.
    ptr<Thread> spawnActorThread(const Value& actorInstance,
                                 bool debugExcluded = false);
    // Re-queue the dataflow engine's run loop if an exit/interrupt stopped
    // it (the loop is otherwise queued once at construction).  Embeddings
    // that run scripts sequentially call this between scripts.
    void restartDataflowEngineIfStopped();
    bool hasHostEventLoop() const { return hostEventLoop_ != nullptr; }

    // =========================================================================
    // Internal call mechanics (used by the above APIs)
    // =========================================================================

    bool call(ObjClosure* closure, const CallSpec& callSpec);
    bool call(ValueType builtinType, const CallSpec& callSpec);
    bool callValue(const Value& callee, const CallSpec& callSpec);
    bool invokeFromType(ObjObjectType* type, ObjString* name, const CallSpec& callSpec,
                        const Value& receiver);
    bool invoke(ObjString* name, const CallSpec& callSpec);
    // Compile-time-resolved method dispatch: like invoke() but skips the
    // OverloadResolver and goes directly to the overload at the given index
    // in the named method's overload set on the receiver's type chain.
    bool invokeOverloadAt(ObjString* name, uint16_t overloadIndex, const CallSpec& callSpec);

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

    /// As invokeClosure(), but each argument may carry a parameter name
    /// (argNames parallel to args; an empty name means positional).  Named
    /// arguments are matched to parameters exactly as a compiled call site
    /// would, so unsupplied parameters take their declared defaults.  Used by
    /// inspect.call() to build a call whose shape is only known at runtime.
    std::pair<ExecutionStatus,Value> invokeClosure(ObjClosure* closure,
                                                    const std::vector<Value>& args,
                                                    const std::vector<ustring>& argNames,
                                                    TimePoint deadline = TimePoint::max());

    /// Call a Roxal method by name on an object instance, with `receiver` bound as
    /// `this`, run to completion → {OK, returnValue}. Resolves a single
    /// (non-overloaded) user method walking the inheritance chain; returns
    /// {RuntimeError, nil} if the receiver isn't an object instance, or the method
    /// isn't found / is overloaded / is native. Args (0 for a `__get_` getter, 1 for
    /// a `__set_` setter, etc.) are passed positionally. For C++ callers (e.g. module
    /// code) that need to invoke a Roxal method without a bytecode call site.
    ///
    /// Like invokeClosure(), this **re-enters the VM dispatch loop** (a nested
    /// execute()), so callers must be re-entrancy-safe (e.g. not hold VM-internal
    /// references across the call).
    std::pair<ExecutionStatus,Value> invokeMethod(const Value& receiver,
                                                  const ustring& methodName,
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
    // Verify `impl` (and its `extends` chain) supplies a non-abstract method
    // for every abstract method declared on `iface` (and its own extends
    // chain). Property-accessor satisfaction (`__get_X`/`__set_X`) accepts a
    // plain property `X` on the implementer chain; setters reject `const`
    // properties. Returns "" on success, or a multi-line error otherwise.
    std::string checkInterfaceConformance(ObjObjectType* impl, ObjObjectType* iface);
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
    // The interrupt word of the CURRENT thread's domain (the service domain's
    // -- which aliases interrupts_ -- when there is no VM thread, i.e. on a
    // host thread).  Every flag accessor below goes through this, so a
    // runtime error or exit raised in a program run's domain is observed by
    // that run and not by the REPL session or the services.
    std::atomic<uint32_t>& currentInterrupts() {
        return thread ? thread->domain->interrupts() : interrupts_;
    }
    const std::atomic<uint32_t>& currentInterrupts() const {
        return thread ? thread->domain->interrupts() : interrupts_;
    }
    inline bool isExitRequested() const { return (currentInterrupts().load() & ExecutionDomain::IntrExit) != 0; }
    inline int exitCode() const { return exitCodeValue.load(); }

    // The default execution domain every Thread currently joins.  Its
    // interrupt word ALIASES VM::interrupts_ (see the member comment), so
    // these accessors read the VM member directly -- single-load on the hot
    // paths -- while domain-addressed access (thread->domain->interrupts())
    // reaches the same word.  Bit-targeted fetch_or/fetch_and so concurrent
    // setters of OTHER bits are never lost.
    const ptr<ExecutionDomain>& defaultDomain() const { return defaultDomain_; }
    // The current VM thread's execution domain, or the default domain when no
    // VM thread is installed (foreign/host threads).  Work spawned on behalf
    // of the current context (actor construction, deserialization) inherits
    // its domain through this, so a non-default domain's children can never
    // escape into the default domain.
    static const ptr<ExecutionDomain>& currentOrDefaultDomain() {
        return thread ? thread->domain : instance().defaultDomain();
    }

    // ---- Debugger stop machinery ----
    // Coordinator for the default domain (constructed eagerly with the VM).
    StopCoordinator& stopCoordinator() { return *stopCoordinator_; }
    // Fast cooperative-stop-point test: one bit in the interrupt word.
    inline bool debugStopRequested() const {
        return (interrupts_.load() & ExecutionDomain::IntrDebugStop) != 0;
    }
    // Non-RT acknowledge-and-park for Roxal-owned native loops (the dataflow
    // engine's run() drain, actor queue idle).  No-op when no stop is
    // pending, when no VM thread is installed, or inside a gate-counted
    // dataflow section.
    void debugStopParkIfRequested();
    // Statement-boundary check for breakpoints/stepping; returns true when
    // the thread must trap at the current boundary (a stop has been
    // published).  Called from the dispatch loop only while
    // IntrDebugSlowPath is armed.
    bool debugStatementBoundary(Thread& t, CallFrames::iterator frame, bool canNotify);
    // Engine access for the coordinator's dataflow admission gate.
    df::DataflowEngine* dataflowEngineForDebug();
    inline bool hasRuntimeError() const { return (currentInterrupts().load() & ExecutionDomain::IntrRuntimeError) != 0; }
    inline void setRuntimeErrorFlag()   { currentInterrupts().fetch_or(ExecutionDomain::IntrRuntimeError); }
    inline void clearRuntimeErrorFlag() { currentInterrupts().fetch_and(~uint32_t(ExecutionDomain::IntrRuntimeError)); }
    /// The text of the most recent runtime error, taken (and cleared).  The
    /// error itself is a flag; this is what an embedding shows a user when a
    /// run fails.  VM-wide, like the flag: one run at a time is the contract
    /// that makes that unambiguous until execution domains scope it.
    std::string takeRuntimeErrorMessage();
    inline void setExitFlag()           { currentInterrupts().fetch_or(ExecutionDomain::IntrExit); }
    inline void clearExitFlag()         { currentInterrupts().fetch_and(~uint32_t(ExecutionDomain::IntrExit)); }

    /// Join only the threads of one execution domain: a program run's
    /// finalizer must not join the REPL session's actors or the service
    /// threads, whose lifetimes are their own.
    ExecutionStatus joinDomainThreads(uint64_t domainId);

    // Join all currently tracked threads, optionally skipping one by id.
    // Returns ExecutionStatus::RuntimeError if any joined thread failed.
    ExecutionStatus joinAllThreads(uint64_t skipId = 0);


    // 16K Values (128KB) per thread: 1024 proved too small for real event
    // traffic -- a camera-frame backlog pumped recursively through change
    // handlers nests ~2 slots per pending event, so a half-second stall
    // (e.g. one large GC pause) overflowed the old limit.  Thread::push
    // bounds-checks in all builds; this is headroom, not a safety net.
    static constexpr size_t DefaultMaxStack = 16384;
    static constexpr size_t DefaultMaxCallFrames = 128;

    // Await `future` INSIDE the dispatcher: resolve immediately if ready,
    // else park the calling Roxal thread (pendingWaitFor + WaitSuspension --
    // sys.wait(for=)'s machinery) and let finalizeWaitSuspension() write the
    // resolved value into the native call's result slot. The OS thread never
    // blocks: under a driven slice the thread reports not-runnable, and a host UI
    // loop keeps pumping. For use by builtins that want synchronous-LOOKING
    // semantics over async work (fileio's async=false, ai.nn's Model.init).
    // Returns the resolved value when already ready, else nil (the dispatch
    // loop delivers the real value).
    Value awaitFutureInVM(Value future);

    static std::string versionString();
    static std::vector<std::string> featureStrings();
    static std::string featureString();

    // Whether the embedding host runs the VM under a real-time scheduler
    // (a periodic driver taking bounded slices).
    // The VM cannot detect this -- it is a property of the host, so the host
    // declares it, before scripts run.  Surfaced to scripts as sys.realtime;
    // defaults to false.
    static void setRealtimeHost(bool rt) { realtimeHost_ = rt; }
    static bool isRealtimeHost() { return realtimeHost_; }
private:
    inline static bool realtimeHost_ = false;   // set once by the host, then read-only
public:

    // Source location of the currently executing instruction (the innermost
    // call frame's chunk line table).  Best effort: all-zero when no frame is
    // active (e.g. called off the VM thread).  Used to stamp creation
    // provenance on dataflow signals/nodes for introspection.
    struct SourceLocation { std::string name; size_t line = 0; size_t col = 0; };
    SourceLocation currentSourceLocation() const;
    struct ScopedOutputRoute {
        explicit ScopedOutputRoute(const OutputRoute& route);
        ~ScopedOutputRoute();
        OutputRoute previous;
    };
    static const OutputRoute& currentOutputRoute();
    static void emitOutput(
        const OutputEventView& event,
        OutputDelivery delivery = OutputDelivery::FollowCallRoute);
    static void emitDiagnostic(std::string_view text,
                               OutputSeverity severity,
                               std::string_view category,
                               bool flush = true);
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
    // --- Runtime collaborators: not host API ---
    // These take types a host never holds (RunRecord, ReplSessionState) or
    // leave a half-started execution behind for someone else to advance.
    // EmbeddedRuntime and ReplSession call them; a host calls those.

    /// Claim a prepared program on the CALLING thread: create its Roxal
    /// thread, run its preludes, and push the body's frame ready to execute.
    /// Consumes the handle.  stageProgramSync() is this plus compilation.
    ExecutionStatus activatePrepared(PreparedProgram program);

    /// Advance one claimed run by at most `budget`, on the calling (driver)
    /// thread.  Binds the run's Roxal thread on first entry, runs module
    /// start hooks, then drives the launch's preludes and body incrementally
    /// -- each under the slice deadline, so no step can overrun the host's
    /// cycle.  Reached by a host through EmbeddedRuntime::driveFor().
    SliceResult driveRunSlice(RunRecord& run, TimeDuration budget);

    /// The unbounded half of a run's lifecycle, performed by a NON-driver
    /// thread once the driver has stopped touching the run: quit and join
    /// what the launch started, run module completion hooks, and leave no
    /// state that could contaminate the next submission.  Object destruction
    /// stays with the collector -- this releases references, it does not
    /// sweep.  Reached by a host through RunHandle::wait().
    /// Returns whether the run is to be reported as failed: the execution's
    /// own failure, or a completion hook that threw during finalization.
    bool finalizeRunRecord(RunRecord& run, bool failed);

    /// Fragment compilation and evaluation, for ReplSession.
    void configureFragmentCompiler(RoxalCompiler& compiler, bool replMode);
    ExecutionStatus evaluateFragmentOnThisThread(ReplSessionState& session,
                                                 PreparedProgram fragment);

    friend class LazyModuleRegistry;  // For lazy module loading

    VM();
    ~VM();

    size_t stackLimit { DefaultMaxStack };
    size_t callFrameLimit { DefaultMaxCallFrames };

    void ensureDataflowEngineStopped();

    /// Low-level dispatch loop. Runs until completion, error, or deadline.
    /// Used by the synchronous entry points, the embedded driver's slice,
    /// and invokeClosure().
    /// baseFrameDepth: the frame count this execution is considered to have
    /// started at -- it terminates when the frame stack drops BELOW it.
    /// Defaults to the current depth, which is right when execute() is entered
    /// before any call is set up.  A caller that pushes the callee frame first
    /// must pass the depth of THAT frame + 1: call() stacks default-value
    /// frames on top of the callee, and their returns must not end the run.
    std::pair<ExecutionStatus,Value> execute(TimePoint deadline = TimePoint::max(),
                                             size_t baseFrameDepth = SIZE_MAX);

    bool outputBytecodeDisassembly;

    std::vector<std::string> modulePaths {};
    std::vector<std::string> scriptArguments {};

    static constexpr size_t OpcodeCount = static_cast<size_t>(OpCode::_Last);
    std::atomic_bool opcodeProfilingEnabled {false};
    std::filesystem::path opcodeProfilePath {"opcode_profile.json"};
    std::array<std::atomic<uint64_t>, OpcodeCount> opcodeProfileCounts {};

    CacheMode cacheModeSetting;

    atomic_unordered_map<uint64_t, ptr<Thread>> threads;

    // Consolidated interrupt/control word (bit layout in
    // ExecutionDomain::InterruptBit).  Declared BEFORE defaultDomain_, whose
    // initializer aliases it: the hot dispatch checks load this member
    // directly (one load), while the domain exposes the same word for
    // domain-addressed access.  Per-session domains (each owning its own
    // word) are still to come -- notably one for debugger evaluation, so
    // its failure/exit state stays isolated from the debuggee's.
    std::atomic<uint32_t> interrupts_ { 0 };
    // The default execution domain every Thread currently joins via
    // Thread::create.
    ptr<ExecutionDomain> defaultDomain_ { make_ptr<ExecutionDomain>(interrupts_) };
    // Stop coordinator for the default domain; constructed in the VM
    // constructor (eager -- no lazy-init races with stop points).
    std::unique_ptr<StopCoordinator> stopCoordinator_;
    // Set once shutdown() has run; makes teardown idempotent so the static
    // destructor is a no-op after an explicit host-driven shutdown.
    std::atomic_bool shutdownComplete_ {false};
    std::atomic_int exitCodeValue {0};

    // The attached embedded runtime, if a host has claimed execution
    // ownership.  Owned by the VM: pending and active runs stay rooted
    // through it even when the host drops every external handle.
    // shared_ptr so a RunHandle's weak lease can hold the object alive for
    // the duration of one call after the VM has dropped it (see
    // RunControl::runtime_).  The VM is still the owner: detach and
    // shutdown drop it, and nothing else holds it strongly at rest.
    std::shared_ptr<EmbeddedRuntime> embeddedRuntime_;
    std::atomic<bool> embeddedRuntimeAttached_ { false };

    // The VM's default fragment session: persistent compiler, module and
    // thread, all reached through ReplSession rather than as ambient state.
    std::unique_ptr<ReplSessionState> replSession_;
    ReplSessionState& ensureReplSession();

    // Persistent thread used for REPL execution so that state such as event
    // handlers persists across entered lines.  Kept as the session's thread;
    // this alias remains because the collector's thread gathering names it.
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
        // reference, not copy: this runs on every module-variable access, and
        // copying the CallFrame (with its Values) dominated interpreter profiles
        auto& currentFrame { thread->frames.back() };

        return asModuleType(asFunction(asClosure(currentFrame.closure)->function)->moduleType);
    }
    inline VariablesMap& moduleVars() { return moduleType()->vars; }

    // global vars cannot be created in the language, but represent builtin symbols available in all modules
    VariablesMap globals;

    // builtin modules (eagerly loaded, e.g. sys)
    std::vector<ptr<BuiltinModule>> builtinModules;
    // lazy-loaded builtin modules (loaded on first import)
    LazyModuleRegistry lazyModuleRegistry;

    // Cross-compiler user-module registry — see lookupUserModule /
    // registerUserModule.  Holds strong Value refs for the VM lifetime
    // (user modules are already pinned via ObjModuleType::allModules,
    // so this is not an additional retention path in practice).  Mutex
    // serialises concurrent registrations from multi-threaded
    // compilation paths and from compile-vs-reconcile races.
    std::unordered_map<ustring, Value> userModuleRegistry;
    std::mutex userModuleRegistryMutex;
    // Serializes every compilation -- full programs and session fragments
    // alike -- because preparation updates module registries, lazy builtin
    // state, debug indexes and caches that are not safe to update from two
    // compilers at once.  Producers are off-driver by contract, so blocking
    // here is acceptable; the driver never takes it.
    std::mutex preparationMutex_;
    // gRPC / DDS module back-pointers.  Declared UNCONDITIONALLY so the size
    // and field offsets of `class VM` are identical whether or not a TU is
    // compiled with ROXAL_ENABLE_GRPC / ROXAL_ENABLE_DDS.  These pointers gate
    // members were the source of a silent ABI mismatch: a consumer (e.g. FC)
    // that included VM.h without the flags got a `class VM` 16 bytes smaller
    // than libroxal's, so its inline VM methods wrote to wrong member offsets
    // and corrupted adjacent state (crash at shutdown).  Only the module
    // *implementation* -- the #includes above and the methods that touch these
    // -- stays feature-gated; the storage is always present (a null pointer
    // when the feature is off costs 8 bytes and removes the layout hazard).
    ModuleGrpc* grpcModule { nullptr };
    ModuleDDS* ddsModule { nullptr };

    // builtin dataflow engine actor
    ptr<df::DataflowEngine> dataflowEngine;
    Value dataflowEngineActor;
    ptr<Thread> dataflowEngineThread;

    Value conditionalInterruptClosure {}; // ObjClosure
    // Sentinel function for sys.allof/anyof slot wakeups. Each slot
    // registration creates a fresh ObjClosure wrapping this function so the
    // closure's handlerThread is per-registration (avoids cross-thread
    // mutation of a shared closure). Dispatch recognises the sentinel by
    // identity of the underlying ObjFunction.
    Value combinatorRelayFunction {}; // ObjFunction

    // Real-time host configuration
    int rtCoreExclusion_ { -1 }; // -1 = disabled (desktop), >=0 = exclude this core for actor threads


    // Host UI event-loop integration (e.g. Qt). When set (serviced on the main
    // thread only), the dispatch loop pumps the host loop while busy and blocks
    // on it while idle, instead of the bare sleep condvar. Null in the default
    // build, so behavior is unchanged. Shared ownership with the installing module.
    ptr<HostEventLoop> hostEventLoop_;
    int64_t lastHostPumpUs_ { 0 }; // throttle timestamp for the busy-pump (main thread)

    // Block the current thread until a host event or `maxWait`. Uses the installed
    // host event loop when on the main thread; otherwise the thread's sleep condvar
    // (the original behavior). Defined in VM.cpp.
    void hostOrCondVarWait(Thread* thread, TimeDuration maxWait);

    // Native call timing instrumentation.
    // When enabled, callNativeFn() times each C++ native call and warns if it
    // exceeds the remaining RT budget. Identifies blocking builtins by name.
    // The deadline and call context are thread_local since execute() runs on
    // multiple threads (RT main thread + non-RT actor threads).
    bool nativeCallTimingEnabled_ { false };
    static thread_local TimePoint nativeCallDeadline_;
    static thread_local ustring nativeCallContext_;
    static thread_local std::string nativeCallOverrun_; // set by callNativeFn() on overrun
    static thread_local OutputRoute currentOutputRoute_;

    // Dataflow thread flag: when true, module var reads return const refs
    // and module var writes raise a runtime error.
    static thread_local bool onDataflowThread_;

public:
    static bool onDataflowThread() { return onDataflowThread_; }
    static void setOnDataflowThread(bool v) { onDataflowThread_ = v; }
    Value getConditionalInterruptClosure() const { return conditionalInterruptClosure; } // ObjClosure
    Value getCombinatorRelayFunction() const { return combinatorRelayFunction; } // ObjFunction
    ObjModuleType* replModuleType() const;

    /// Like `replModuleType()`, but lazily creates the REPL module if
    /// none exists yet (rather than returning nullptr).  Lets callers
    /// pre-populate the REPL's globals before the user types anything.
    /// Safe to call multiple times; returns the same module each time.
    ObjModuleType* ensureReplModule();

    /// Copy all exported vars from each `source` ObjModuleType into `target`.
    /// Equivalent to executing `import S0.*; import S1.*; ...` against
    /// `target`, but pure C++ -- no opcode dispatch, no source-code
    /// generation, no RT-loop involvement.  Source modules must have
    /// been fully evaluated (their top-level statements run) before
    /// calling.  Replicates the wildcard branch of OpCode::ImportModuleVars
    /// including OverloadSet cloning + REPL-reimport overwrite semantics.
    /// Throws if `target` or any element of `sources` is not a module type.
    void importModuleVarsInto(ObjModuleType* target,
                              const std::vector<Value>& sources);


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

    /// As above, but each argument may carry a parameter name (empty = positional),
    /// and a non-nil receiver takes the callee slot so the callee sees it as
    /// `this` -- the layout a compiled method call uses.  Lets a native invoke a
    /// callable whose shape is only known at runtime WITHOUT re-entering
    /// execute(): the dispatch loop runs the call and hands the result to the
    /// continuation's onComplete.
    bool pushContinuationCall(ObjClosure* closure, const std::vector<Value>& args,
                              const std::vector<ustring>& argNames,
                              const Value& receiver = Value::nilVal());
    void clearContinuation();

    void resetStack();
    void freeObjects();
    void cleanupWeakRegistries();
    void unwindFrame();
    void raiseException(Value exc);
    bool unwindToExceptionHandler(Value& exc); // shared unwinder; true = handler entered
    void debugFatalStopIfArmed(const std::string& description, Chunk* chunk, size_t instruction); // exception-stop policy
    // Raise a catchable Roxal ZeroDivisionError carrying `msg`. Used by the
    // arithmetic opcodes to convert a native roxal::ZeroDivisionError into an
    // exception user code can try/except, rather than a fatal runtimeError().
    void raiseZeroDivisionError(const char* msg);
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
    Value tensor_to_bytes_builtin(ArgsView args);
    Value tensor_astype_builtin(ArgsView args);
    Value tensor_take_builtin(ArgsView args);
    Value tensor_fill_builtin(ArgsView args);
    Value tensor_sample_col_builtin(ArgsView args);
    Value tensor_sample_span_builtin(ArgsView args);
    Value tensor_remap_builtin(ArgsView args);
    Value tensor_shape_builtin(ArgsView args);
    Value tensor_dtype_builtin(ArgsView args);
    Value tensor_dims_builtin(ArgsView args);

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
    Value list_sampled_builtin(ArgsView args);
    Value list_extend_builtin(ArgsView args);
    Value list_insert_builtin(ArgsView args);
    Value list_remove_builtin(ArgsView args);
    Value list_pop_builtin(ArgsView args);
    Value list_reserve_builtin(ArgsView args);

    Value string_upper_builtin(ArgsView args);
    Value string_lower_builtin(ArgsView args);
    Value string_capitalize_builtin(ArgsView args);
    Value string_title_builtin(ArgsView args);

#ifdef ROXAL_ENABLE_REGEX
    Value string_match_builtin(ArgsView args);
    Value string_search_builtin(ArgsView args);
    Value string_replace_builtin(ArgsView args);
    Value string_split_builtin(ArgsView args);
#else
    // Literal-text stand-ins, so that split() and search() are part of the
    // string type in every build. Only match() and replace() genuinely need a
    // regex engine; splitting on "," and finding a substring do not, and making
    // them disappear meant a script could work natively and fail in a build with
    // regex off -- the wasm build, for one.
    Value string_search_plain_builtin(ArgsView args);
    Value string_split_plain_builtin(ArgsView args);
#endif

    Value signal_run_builtin(ArgsView args);
    Value signal_stop_builtin(ArgsView args);
    Value signal_tick_builtin(ArgsView args);
    Value signal_freq_builtin(ArgsView args);
    Value signal_domain_builtin(ArgsView args);
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
    // Engine-thread bootstrap only (queued once at VM construction); not
    // registered as a script-callable method.
    Value dataflow_run_native(ArgsView args);

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

#ifdef ROXAL_ENABLE_FFI
    Value loadlib_native(ArgsView args);
    Value ffi_native(ArgsView args);
#endif

private:
    // Serializes reclamation-role handoffs (dedicated collector thread,
    // inline-electing thread's tail, shutdown path).  Contention is ~zero.
    std::mutex freeObjectsMutex_;
};


}

namespace roxal {
void scheduleEventHandlers(Value eventWeak, ObjEventType* ev, Value eventInstance, TimePoint when);
}
