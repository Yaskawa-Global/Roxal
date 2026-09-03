#pragma once

// Preparing a program: compiling it and rooting what the launch will need,
// without starting it.
//
// Preparation is deliberately separate from activation.  Compiling parses,
// reads and writes the bytecode cache, allocates, and updates shared
// registries -- unbounded work that belongs on whatever thread the embedding
// can afford to spend it on.  Activation is what claims a thread, pushes the
// body's frame and runs the launch's preludes.  Keeping them apart is what
// lets a host with a deadline compile off its driver and still execute on it.
//
// A prepared program owns a stable heap record holding ONE consolidated typed
// root.  Roots register by address and must never move while registered, so
// the movable handle owns a pointer and moves only that: accepting a prepared
// program transfers the same record, with the same registration, rather than
// copying Values from one root into another.
//
// The C++ ownership does not keep anything alive for tracing.  Only the
// record's typed root does: an unreachable cycle with a positive strong count
// is exactly what the tracing sweep exists to reclaim.

#include <memory>
#include <string>
#include <vector>

#include <core/ustring.h>

#include "GCRoots.h"
#include "Value.h"

namespace roxal {

// A call the launch performs before the program body, on the program's own
// thread.  Hosts use it to bind or configure state the body then assumes.
struct PreludeCall {
    Value receiver;
    ustring method;
};

// When a launch is considered finished.
enum class CompletionPolicy {
    // The top-level body returned.  Threads, handlers and actors it created
    // may still be alive.
    TopLevelReturned,
    // The body returned AND the launch's own threads have terminated.  This
    // is what a fresh program means by "done".
    ExecutionDomainQuiescent,
};

struct ProgramOptions {
    std::string sourceName;
    // Modules whose variables are pre-populated into the program's own
    // module, so unqualified names resolve against them during compilation.
    std::vector<Value> imports;
    std::vector<PreludeCall> preludes;
    CompletionPolicy completion { CompletionPolicy::ExecutionDomainQuiescent };
    // Debugger launch: arm a step-in on the run's thread right before its
    // body starts (after the preludes), so the first user statement stops
    // as "entry".  Applied at activation, which is the earliest moment a
    // driven run has a thread; the synchronous adapter arms the staged
    // thread itself and leaves this false.
    bool stopOnEntry { false };
};

enum class PrepareStatus {
    Ready,
    CompileError,
    ShuttingDown,
};

struct CompileDiagnostics {
    // Empty when compilation succeeded.  Diagnostics are also emitted through
    // the normal output path as they are produced; this is the summary a
    // caller can attach to its own error reporting.
    std::string message;
};

// Every Value a prepared or active launch retains, in one place.
//
// One consolidated root rather than a scatter of scalar roots: a launch's
// Values move through preparation, activation and finalization together, and
// a single registration cannot half-transfer.  Anything Value-bearing added
// to a launch belongs in here -- never beside it.
struct ExecutionRootValues {
    Value closure;
    // Strong on purpose: a compiled function holds its module through a WEAK
    // moduleType link, so nothing else keeps the module alive between
    // preparation and activation.
    Value module;
    std::vector<Value> imports;
    std::vector<PreludeCall> preludes;
    // The body's return value, from the moment execution produces it until
    // the result is consumed.
    Value terminalValue;
};

// Stable heap storage for one launch's roots and metadata.  Never moved: the
// root registers by address.
class PreparedExecutionRecord {
public:
    // Runs with the world stopped, and several parked mutators may hold
    // ordinary application locks -- so this allocates nothing, copies no
    // Value, takes no lock and calls no host code.
    static void traceValues(ValueVisitor& visitor,
                            const ExecutionRootValues& values)
    {
        if (values.closure.isObj())       visitor.visit(values.closure);
        if (values.module.isObj())        visitor.visit(values.module);
        if (values.terminalValue.isObj()) visitor.visit(values.terminalValue);
        for (const auto& imported : values.imports)
            if (imported.isObj())
                visitor.visit(imported);
        for (const auto& prelude : values.preludes)
            if (prelude.receiver.isObj())
                visitor.visit(prelude.receiver);
    }

    TracedMember<ExecutionRootValues> roots { &traceValues };

    // Non-GC metadata.  A Value must never be added here -- it belongs in
    // ExecutionRootValues, where the tracer can see it.
    std::string sourceName;
    CompletionPolicy completion { CompletionPolicy::ExecutionDomainQuiescent };
    bool stopOnEntry { false };   // see ProgramOptions::stopOnEntry
    // Set for a session fragment: activation reuses the session's existing
    // Roxal thread rather than creating a fresh one, which is what makes
    // handlers and actors registered by an earlier fragment still belong to
    // the thread that services this one.
    class ReplSessionState* session { nullptr };
    // A prepared fragment holds its session's in-flight claim until either
    // execution takes it over (which then releases it at the end) or the
    // record is destroyed unexecuted.  Without this, a fragment dropped
    // before running would leave the session SessionBusy forever.
    bool fragmentClaimHeld { false };

    ~PreparedExecutionRecord();
};

// Move-only owner of a prepared launch.  Moving transfers the record pointer;
// the root registration is untouched, so a move costs nothing and changes no
// address the collector has recorded.
class PreparedProgram {
public:
    PreparedProgram() = default;
    explicit PreparedProgram(std::unique_ptr<PreparedExecutionRecord> record)
        : record_(std::move(record)) {}

    PreparedProgram(PreparedProgram&&) noexcept = default;
    PreparedProgram& operator=(PreparedProgram&& other) noexcept;
    PreparedProgram(const PreparedProgram&) = delete;
    PreparedProgram& operator=(const PreparedProgram&) = delete;
    ~PreparedProgram();

    // Discard the launch, releasing its roots under mutator coverage.
    void reset();

    bool valid() const noexcept { return record_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    const std::string& sourceName() const { return record_->sourceName; }
    CompletionPolicy completion() const { return record_->completion; }

    // Address identity, for proving a handoff moved the record rather than
    // rebuilding it.
    const PreparedExecutionRecord* record() const noexcept
    {
        return record_.get();
    }

private:
    friend class VM;
    friend class EmbeddedRuntime;
    std::unique_ptr<PreparedExecutionRecord> record_;
};

struct PrepareProgramResult {
    PrepareStatus status { PrepareStatus::CompileError };
    PreparedProgram program;      // present only when status == Ready
    CompileDiagnostics diagnostics;
};

} // namespace roxal
