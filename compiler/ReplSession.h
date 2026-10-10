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
#include <cstddef>
#include <istream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "GCRoots.h"
#include "EmbeddedRuntime.h"
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
    InitFailed,        // the VM failed to set itself up (VM::initError())
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
    // across every fragment: a fragment's actors and errors belong to the
    // session, not to whichever program run happens to be next.  An exit()
    // raised in it ends the line in flight, never the session.
    ptr<ExecutionDomain> domain;
    // The same domain as a plain pointer, for the synchronous interrupt --
    // which a signal handler may call, and which therefore must not touch
    // the ptr<> above.  Published once, when the domain is created.
    std::atomic<ExecutionDomain*> signalDomain { nullptr };
    // Serializes producers.  The root tracer must never acquire it.
    std::mutex producerMutex;
    // True from the moment a fragment is prepared until it has finished.
    std::atomic<bool> fragmentInFlight { false };

    // Create the session's domain if it has none yet.  Caller: whoever is
    // about to give the session its thread.
    void ensureDomain();

    // The driven line in flight (the last one submitted), so interrupt() can
    // end it.  Value-free.  Its own small lock: never the producer mutex,
    // which a preparing producer holds for a whole compile.
    void noteFragmentRun(const RunHandle& run);
    void forgetFragmentRun(RunId id);
    RunHandle fragmentRun() const;

private:
    mutable std::mutex runMutex_;
    RunHandle fragmentRun_;
};

// One completion offered for a REPL buffer (ReplSession::complete).
struct CompletionCandidate {
    std::string text;     // what replaces [replaceStart, replaceEnd)
    // "variable", "function", "type", "module", "method", "property",
    // "enum" (an enumeration label) or "keyword".
    std::string kind;
    std::string detail;   // a signature or type, for display; may be empty
};

struct CompletionResult {
    // The byte range of the buffer a candidate replaces: the partial name
    // before the cursor.  An adapter that can only replace a whole line
    // (linenoise) keeps buffer[0, replaceStart), puts the candidate there
    // and appends buffer[replaceEnd, end).
    size_t replaceStart { 0 };
    size_t replaceEnd { 0 };
    std::vector<CompletionCandidate> candidates;   // sorted, unique by text
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
    // a VM with no embedded driver -- a CLI REPL, a test.  A line ended by
    // interrupt() returns OK.  exit() typed in a line keeps its process
    // semantics on this path (the VM exits; a CLI quits).
    ExecutionStatus evaluateFragmentSync(std::istream& source,
                                         FragmentOptions options = {});

    // End the line in flight early -- the session itself, its bindings, and
    // whatever the line already spawned (actors, `when` handlers, threads)
    // all carry on; an interrupt just ends the body where it got to.  The
    // session's thread is put back to idle (frames dropped, waits abandoned,
    // closures over the line's locals closed), and a debugger stop over the
    // line is released.  The next line may follow at once.
    //
    // With an embedded driver: EmbeddedRuntime::requestExit() on the
    // session's submitted line -- Requested, Cancelled if not yet claimed,
    // AlreadyTerminal if no line is in flight.  The run COMPLETES (it is not
    // a failure) with `code` as RunHandle::exitCode().
    //
    // Without one (evaluateFragmentSync): raises the line's abort bit and
    // returns -- atomics only, so a SIGINT handler may call it.  The line
    // ends at its next instruction, or within 50 ms if it is blocked in a
    // wait.  AlreadyTerminal if no line is executing; a native call that
    // never returns is not interrupted.
    //
    // Unknown on an invalid session.
    ExitRequestStatus interrupt(int code = 130);

    // Completions for `buffer` at byte offset `cursor` (npos: its end).
    // Session bindings, builtins and keywords; members of modules, object
    // and actor instances and types, enumeration labels, and builtin
    // methods of lists, strings and the other builtin types -- through a
    // dotted chain where each step resolves without running code (through
    // module variables and DECLARED member types, never an instance's own
    // storage).  Read-only: it runs no Roxal code and takes no session lock,
    // so it answers while a line runs.  Nothing inside a string or comment.
    // Any non-driver thread; empty when the session is invalid.
    CompletionResult complete(std::string_view buffer,
                              size_t cursor = std::string_view::npos,
                              size_t maxCandidates = 200) const;

    // The session's persistent module, once it has one.
    Value module() const;

private:
    friend class VM;
    ReplSession(VM& vm, ReplSessionState& state) : vm_(&vm), state_(&state) {}
    VM* vm_ { nullptr };
    ReplSessionState* state_ { nullptr };
};

} // namespace roxal
