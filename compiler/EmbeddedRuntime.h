#pragma once

// Embedded-driver ownership: who is allowed to run a program, and when.
//
// A host that owns a periodic loop attaches an embedded runtime once, then
// submits prepared programs to it from wherever it likes.  Attachment is what
// transfers ownership of execution from the VM's synchronous entry points to
// that host: after it, the synchronous entry points refuse rather than
// fighting the driver for the same VM.
//
// Two rules shape everything here.
//
// Submission transfers ownership ONLY on acceptance.  There is no interval in
// which a caller has been told its program was rejected while the VM still
// holds a closure that could execute later -- a rejected PreparedProgram is
// still owned, still valid, and can be retried or dropped.
//
// The externally visible objects hold no Values.  A RunHandle observes a
// RunControl of ids, states and copied strings; the launch's Values live in
// the VM-owned record, which stays rooted through pending, active and
// terminal phases even when every external handle is gone.  Dropping a handle
// is not a way to unroot work the VM has accepted.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <optional>
#include <string>

#include <core/TimeDuration.h>
#include <core/TimePoint.h>

#include "PreparedProgram.h"
#include "Thread.h"

namespace roxal {

class VM;
class EmbeddedRuntime;

using RunId = std::uint64_t;   // 0 is never a valid run

enum class RunState {
    Queued,                // accepted, not yet claimed by the driver
    Running,
    DebugPaused,
    MainReturned,
    FinalizationPending,   // handed over; failed() says whether it failed
    Finalizing,
    Completed,             // finalized, succeeded
    Failed,                // finalized, failed -- never seen BEFORE finalization
    Cancelled,             // cancelled before the driver claimed it
};

enum class FinalizeStatus {
    Finalized,          // this call performed the finalization
    AlreadyFinalized,   // another waiter did; its outcome is reported
    NotReady,           // the run has not reached its ownership transfer
    OnDriverThread,     // waiting/finalizing must not happen on the driver
    RuntimeGone,        // the VM (or its runtime) went away first
};

struct FinalizeResult {
    FinalizeStatus status { FinalizeStatus::NotReady };
    RunState state { RunState::Queued };
};

// Externally shared, deliberately Value-free: a host may hold one of these
// across VM shutdown without keeping any Roxal object alive.
class RunControl {
public:
    explicit RunControl(RunId id) : id_(id) {}

    RunId id() const noexcept { return id_; }
    RunState state() const noexcept { return state_.load(std::memory_order_acquire); }
    /// True once the run's execution failed.  Set before FinalizationPending
    /// is published, so a waiter never has to infer failure from a state that
    /// finalization has not yet reached.
    bool failed() const noexcept { return failed_.load(std::memory_order_acquire); }

    // Copied, non-GC diagnostic text; safe to read after the VM is gone.
    // Why the run failed, from the terminal transition on; empty otherwise.
    std::string diagnostic() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return diagnostic_;
    }
    // The program's terminal value, rendered (bounded, no user code run),
    // from Completed/Failed on.  A body that returns nothing renders as
    // "nil".  Copied text: readable after the VM is gone.
    std::string result() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return result_;
    }
    // The code the program passed to exit(), if it exited that way; 0
    // otherwise.  A driven program's exit() ends ITS RUN, nothing else.
    int exitCode() const noexcept { return exitCode_.load(std::memory_order_acquire); }

private:
    friend class EmbeddedRuntime;
    friend class VM;
    friend class RunHandle;

    void setState(RunState next)
    {
        // The notify stays UNDER the lock.  A waiter woken by a terminal
        // state finalizes the run and can drop the last reference to this
        // control -- if the notify ran after the unlock, it would be touching
        // a destroyed mutex and condition variable.  Holding the lock forces
        // the waiter to reacquire it before it can return from wait(), so it
        // cannot run ahead of us.
        std::lock_guard<std::mutex> lock(mutex_);
        state_.store(next, std::memory_order_release);
        cv_.notify_all();
    }

    // Wait until the run reaches a state the predicate accepts.  Deliberately
    // a plain condition-variable wait with no mutator participation: a waiter
    // that held one across this would keep the collection barrier waiting on
    // a thread that is doing nothing.
    template <typename Pred>
    void waitForState(Pred pred)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return pred(state_.load(std::memory_order_acquire)); });
    }

    RunId id_ { 0 };
    std::atomic<RunState> state_ { RunState::Queued };
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string diagnostic_;
    std::string result_;
    std::atomic<int> exitCode_ { 0 };
    // Exactly-once finalizer claim: the winner runs cleanup, everyone else
    // waits for its result.
    std::atomic<bool> finalizeClaimed_ { false };
    // Outcome of execution, independent of finalization progress.  Failed as
    // a STATE means "finalized, and it had failed"; this flag is what carries
    // the failure from the handover to the finalizer.
    std::atomic<bool> failed_ { false };
    // A LEASE on the runtime, not a pointer to it: a handle that reaches for
    // its runtime locks this and holds the object alive for the duration of
    // the call, so shutdown can never free it mid-call.  The VM owns the
    // runtime by shared_ptr and drops it at detach/shutdown; the object then
    // lives exactly as long as one in-flight call, refusing everything.
    std::weak_ptr<EmbeddedRuntime> runtime_;
};

// Copyable observation of one run.  Holds no Value and no raw Obj*.
class RunHandle {
public:
    RunHandle() = default;

    bool valid() const noexcept { return control_ != nullptr; }
    RunId id() const noexcept { return control_ ? control_->id() : 0; }
    RunState state() const { return control_->state(); }
    /// Whether execution failed; meaningful from FinalizationPending on.
    bool failed() const { return control_->failed(); }
    std::string diagnostic() const { return control_->diagnostic(); }
    /// The terminal value, rendered; see RunControl::result().
    std::string result() const { return control_->result(); }
    int exitCode() const { return control_->exitCode(); }

    // Claim and perform this run's finalization -- module completion hooks,
    // joining what the launch started, releasing its roots.  Unbounded by
    // nature, so it runs HERE rather than in a driver slice, and never on the
    // driver thread.  Exactly one caller performs it; others block for its
    // result.  NotReady if the run has not yet been handed over.
    FinalizeResult finalize();

    // Block until the run has been handed over, then finalize it.
    FinalizeResult wait();

private:
    friend class EmbeddedRuntime;
    explicit RunHandle(std::shared_ptr<RunControl> control)
        : control_(std::move(control)) {}
    std::shared_ptr<RunControl> control_;
};

enum class SubmitStatus {
    Accepted,
    RunActive,           // one run at a time in version 1
    DriverUnavailable,
    ShuttingDown,
};

struct SubmitResult {
    SubmitStatus status { SubmitStatus::DriverUnavailable };
    RunHandle run;       // valid only when status == Accepted
};

enum class CancelStatus {
    Cancelled,           // the run never started and never will
    AlreadyRunning,      // the driver claimed it first
    AlreadyTerminal,
    Unknown,
};

enum class AttachStatus {
    Attached,
    AlreadyAttached,
    ShuttingDown,
};

struct DriverOptions {
    // Reserved for queue capacity, diagnostics, and host policy.
};

class EmbeddedRuntime;

struct AttachDriverResult {
    AttachStatus status { AttachStatus::ShuttingDown };
    EmbeddedRuntime* runtime { nullptr };   // valid only when Attached
};

enum class DetachStatus {
    Detached,
    RunActive,           // refuse while a claimed run is still in flight
    NotAttached,
};

// What a slice of driver time did.
enum class SliceState {
    Idle,              // nothing to advance
    Yielded,           // budget spent; call again on the next cycle
    Blocked,           // cannot progress until a deadline or event
    DebugPaused,       // a debugger stop is active; keep calling on cadence
    MainReturned,      // the body returned; not terminal under the
                       // domain-quiescent policy, and published once
    ExecutionEnded,    // the run finished; published exactly once
    ExecutionFailed,   // the run failed; published exactly once
    ShuttingDown,
};

struct SliceResult {
    SliceState state { SliceState::Idle };
    RunId runId { 0 };                       // 0 for Idle/ShuttingDown
    std::optional<TimePoint> retryAt;        // set when Blocked
};

// How far a claimed run has got.  The driver may only spend bounded time per
// slice, so activation, each prelude, and the body are separate steps rather
// than one call that runs to completion.
enum class RunPhase {
    Activate,          // claimed, nothing bound yet
    Preludes,          // running the launch's preludes, one frame at a time
    Body,              // running the program body
    MainReturned,      // body done; the ownership transfer is next
    Finished,          // terminal state published
};

// The VM-owned side of one accepted run: the launch's roots plus the link to
// the control block the host observes.  Never moved -- the prepared record
// registered its root by address, and accepting a submission transfers that
// same record rather than copying Values into a new root.
struct RunRecord {
    RunId id { 0 };
    std::unique_ptr<PreparedExecutionRecord> prepared;
    std::shared_ptr<RunControl> control;

    // Driver-owned progress.  Touched only by the bound driver thread.
    RunPhase phase { RunPhase::Activate };
    size_t nextPrelude { 0 };
    // The run's Roxal thread.  Held here so it stays registered until the
    // finalizer is done with its interpreter roots -- the run record's root
    // and this one deliberately overlap.
    ptr<Thread> thread;
};

class EmbeddedRuntime : public std::enable_shared_from_this<EmbeddedRuntime> {
public:
    explicit EmbeddedRuntime(VM& vm, DriverOptions options);
    ~EmbeddedRuntime();

    EmbeddedRuntime(const EmbeddedRuntime&) = delete;
    EmbeddedRuntime& operator=(const EmbeddedRuntime&) = delete;

    // Thread-safe and non-blocking.  Moves from `program` only on acceptance;
    // on every rejection the caller still owns its prepared program.
    SubmitResult submit(PreparedProgram&& program);

    // Claim the pending run for execution.  The FIRST caller latches this
    // thread as the driver; afterwards only that thread may claim.  Returns 0
    // when there is nothing to claim or the caller is not the driver.
    //
    // Phase C establishes ownership only: claiming marks the run Running and
    // hands the record to the VM.  Activating and slicing it on the driver is
    // the next phase.
    RunId claimPendingRun();

    // Advance the claimed run by at most `budget`.  Must be called from the
    // driver thread (the first call to this or claimPendingRun() latches
    // which thread that is).  Claims a pending run if none is active.
    //
    // The driver keeps calling through Yielded, Blocked and DebugPaused.
    // ExecutionEnded/ExecutionFailed is published exactly once, as the
    // ownership transfer to the non-driver finalizer; later calls report
    // Idle.
    SliceResult driveFor(TimeDuration budget);

    // Cancel before the driver claims the run.  Once claimed, version 1
    // reports AlreadyRunning: cooperative cancellation of a live execution
    // domain is a separate design.
    CancelStatus cancelBeforeStart(const RunHandle& run);

    bool hasPendingRun() const;
    bool hasActiveRun() const;
    /// A claimed run, or one handed over and not yet finalized.
    bool hasUnfinalizedRun() const;
    RunId activeRunId() const;

    // Whether a driver has latched, and whether the calling thread is it.
    bool driverBound() const;
    bool isDriverThread() const;

private:
    friend class VM;

    // Destroys a record's roots under mutator coverage.  Root destruction is
    // a mutator operation and must never happen inside a driver's GC yield
    // section, so this refuses to run there rather than corrupting the
    // registry (the caller is a non-driver path by contract).
    static void destroyRecordCovered(std::unique_ptr<PreparedExecutionRecord> record);

    void cancelPendingForShutdown();

public:
    // Performs one run's finalization.  Called through RunHandle; refuses on
    // the driver thread, where blocking would stall the host's cycle.
    FinalizeResult finalizeRun(const std::shared_ptr<RunControl>& control);

private:

    VM& vm_;
    DriverOptions options_;

    mutable std::mutex mutex_;
    // Version 1: a single pending slot.  Not a queue -- rejection is final
    // and visible, rather than hidden behind unbounded queued work.
    std::unique_ptr<RunRecord> pending_;
    std::unique_ptr<RunRecord> active_;
    // Handed over.  The DRIVER moves the record here when it publishes the
    // end of execution, before making the run visible as finalizable -- so a
    // finalizer can never reach a record the driver might still be inside.
    std::unique_ptr<RunRecord> finalizing_;
    RunId nextRunId_ { 1 };

    // Set while the driver is inside driveRunSlice().  Shutdown waits for it
    // to clear -- bounded by the slice budget -- before cancelling the run
    // the slice may be inside.
    bool sliceInProgress_ { false };
    std::condition_variable sliceDone_;
    std::atomic<bool> driverBound_ { false };
    // The driver is identified by a THREAD-LOCAL token (see the .cpp), not
    // by std::thread::id: ids are recycled once a thread is joined, so an
    // unrelated later thread could otherwise inherit the dead driver's
    // identity and be refused as "the driver".  The generation guards the
    // symmetric case -- a re-attached runtime reusing the old object's
    // address -- so a stale token never matches a new runtime.
    const std::uint64_t generation_;
    std::atomic<bool> shuttingDown_ { false };
};

} // namespace roxal
