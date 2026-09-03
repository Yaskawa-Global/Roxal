#pragma once

// Persistent fragment sessions.
//
// A REPL line and a fresh program are different source lifetimes, and the VM
// used to blur them: one global compiler, one global module and one global
// thread, reached through entry points named after "line".  A fragment
// deliberately sees what earlier fragments declared; a program deliberately
// does not.  Making that a SESSION rather than ambient VM state is what stops
// a fresh program from silently inheriting REPL lifetime.
//
// The session's persistent Values live in one consolidated typed root at a
// stable VM-owned address, like a prepared launch's.  Its tracer takes no
// session lock: the world-stop supplies GC-side stability, and a tracer that
// waited on a producer's mutex would let a parked mutator hold up the
// collector.
//
// Only one producer may change session compiler/module state, and the session
// must be IDLE before compilation starts -- not merely before the compiled
// closure is handed off.  Compiling against a module that a live fragment is
// still mutating is the failure this ordering exists to prevent.

#include <atomic>
#include <istream>
#include <memory>
#include <mutex>
#include <string>

#include "GCRoots.h"
#include "ExecutionDomain.h"
#include "PreparedProgram.h"
#include "Thread.h"
#include "Value.h"

namespace roxal {

class RoxalCompiler;
class VM;

struct ReplOptions {
    std::vector<Value> imports;
};

struct FragmentOptions {
    // Display name for diagnostics.  Fragments are not files and have no
    // bytecode cache: a fragment's identity is its session, not a path.
    std::string sourceName;
    // REPL semantics (expression statements echo their value).  A host
    // evaluating a whole script into a session turns this off.
    bool replMode { true };
    // A fragment's threads, handlers and actors may outlive it on purpose,
    // so the body returning IS completion.
    CompletionPolicy completion { CompletionPolicy::TopLevelReturned };
};

enum class PrepareFragmentStatus {
    Ready,
    CompileError,
    SessionBusy,       // a fragment is still live; its state cannot be touched
    ShuttingDown,
};

struct PrepareFragmentResult {
    PrepareFragmentStatus status { PrepareFragmentStatus::CompileError };
    PreparedProgram fragment;      // present only when Ready
    CompileDiagnostics diagnostics;
};

// Every Value the session retains between fragments.
struct ReplSessionValues {
    Value module;          // ObjModuleType: the session's persistent bindings
    Value lastResult;
};

// Stable, VM-owned session state.  Never moved: its root registers by address.
class ReplSessionState {
public:
    // World stopped, no allocation, no lock -- see the note above about why
    // this must not reach for the producer mutex.
    static void traceValues(ValueVisitor& visitor, const ReplSessionValues& values)
    {
        if (values.module.isObj())     visitor.visit(values.module);
        if (values.lastResult.isObj()) visitor.visit(values.lastResult);
    }

    TracedMember<ReplSessionValues> roots { &traceValues };

    // Non-GC state.  A Value must never be added here; it belongs above.
    std::unique_ptr<RoxalCompiler> compiler;
    ptr<Thread> thread;
    // The session's execution domain, created with its thread and kept
    // across every fragment: a fragment's actors, errors and exits belong
    // to the session, not to whichever program run happens to be next.
    ptr<ExecutionDomain> domain;
    // Serializes producers.  The root tracer must never acquire it.
    std::mutex producerMutex;
    // True from the moment a fragment is prepared until it has finished.
    std::atomic<bool> fragmentInFlight { false };
};

// Handle to one session.  Copyable and cheap; the state is owned by the VM.
class ReplSession {
public:
    ReplSession() = default;

    bool valid() const noexcept;

    // Compile a fragment against this session's persistent module.  Returns
    // SessionBusy rather than compiling against state a live fragment is
    // still mutating.
    PrepareFragmentResult prepareFragment(std::istream& source,
                                          FragmentOptions options = {});

    // Compile and run a fragment on the calling thread.  The convenience for
    // a VM with no embedded driver -- a CLI REPL, a test.
    ExecutionStatus evaluateFragmentSync(std::istream& source,
                                         FragmentOptions options = {});

    // The session's persistent module, once it has one.
    Value module() const;

private:
    friend class VM;
    ReplSession(VM& vm, ReplSessionState& state) : vm_(&vm), state_(&state) {}
    VM* vm_ { nullptr };
    ReplSessionState* state_ { nullptr };
};

} // namespace roxal
