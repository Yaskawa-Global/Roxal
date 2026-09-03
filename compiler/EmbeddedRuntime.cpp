#include "EmbeddedRuntime.h"

#include <cassert>

#include "SimpleMarkSweepGC.h"
#include "VM.h"

namespace roxal {

namespace {
// The driver's identity lives in the driver thread itself.  A thread-local
// dies with its thread, so a recycled std::thread::id cannot inherit it; the
// generation makes a token from an earlier runtime worthless against a later
// one that happens to reuse the address.
thread_local const EmbeddedRuntime* t_driverOf = nullptr;
thread_local std::uint64_t t_driverGeneration = 0;
std::atomic<std::uint64_t> g_runtimeGeneration { 0 };
} // namespace

EmbeddedRuntime::EmbeddedRuntime(VM& vm, DriverOptions options)
    : vm_(vm), options_(options),
      generation_(g_runtimeGeneration.fetch_add(1, std::memory_order_relaxed) + 1)
{
}

EmbeddedRuntime::~EmbeddedRuntime()
{
    // Nobody can reach this object through a handle any more: the VM has
    // dropped its shared_ptr, so every weak lease fails to lock from here on,
    // and a call that was already inside holds a strong reference that kept
    // us from getting here.  What remains is the driver, which holds a raw
    // reference from attach -- refuse it, then wait out the slice it may be
    // inside (bounded by the slice budget) before cancelling the run that
    // slice is advancing.
    shuttingDown_.store(true, std::memory_order_release);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        // Destroying the runtime from INSIDE its own driver's slice -- a host
        // shutting down from within driven script code -- would wait here
        // for a slice that is this very call chain.  That is a host contract
        // violation (embedding.md), not a case to accommodate.
        assert(!(sliceInProgress_ && isDriverThread())
               && "the runtime must not be shut down from inside its own slice");
        sliceDone_.wait(lock, [&] { return !sliceInProgress_; });
    }
    cancelPendingForShutdown();
}

void EmbeddedRuntime::destroyRecordCovered(
    std::unique_ptr<PreparedExecutionRecord> record)
{
    if (!record)
        return;
    // Roots may not be created or destroyed inside an RT yield section: the
    // registry is not reachable from there without risking the collector's
    // view of it.  Every path that discards a record is a non-driver path by
    // contract, so this is an invariant check rather than a fallback.
    assert(!SimpleMarkSweepGC::inGCYieldSectionOnThisThread()
           && "a run record must not be destroyed inside a GC yield section");
    ScopedGCMutatorCover gcCover;
    record.reset();
}

SubmitResult EmbeddedRuntime::submit(PreparedProgram&& program)
{
    SubmitResult result;
    if (!program.valid()) {
        result.status = SubmitStatus::DriverUnavailable;
        return result;
    }
    if (shuttingDown_.load(std::memory_order_acquire)) {
        result.status = SubmitStatus::ShuttingDown;
        return result;   // caller keeps its program
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (shuttingDown_.load(std::memory_order_acquire)) {
        result.status = SubmitStatus::ShuttingDown;
        return result;
    }
    if (pending_ || active_ || finalizing_) {
        // One run at a time, and "a time" lasts until finalization is DONE:
        // a handed-over run still owns the finalizing_ slot, and a second run
        // reaching its terminal transition first would overwrite that record
        // -- the first run would then report Completed with none of its
        // joins or completion hooks having run.  Rejection is final and the
        // caller still owns its prepared program.
        result.status = SubmitStatus::RunActive;
        return result;
    }

    auto record = std::make_unique<RunRecord>();
    record->id = nextRunId_++;
    record->control = std::make_shared<RunControl>(record->id);
    record->control->runtime_ = weak_from_this();
    // The only move: the record pointer, so the launch's typed root keeps its
    // address and its registration.
    record->prepared = std::move(program.record_);

    result.run = RunHandle(record->control);
    pending_ = std::move(record);
    result.status = SubmitStatus::Accepted;
    return result;
}

RunId EmbeddedRuntime::claimPendingRun()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (shuttingDown_.load(std::memory_order_acquire))
        return 0;

    if (!driverBound_.load(std::memory_order_acquire)) {
        // First claim latches the driver identity -- on the thread, not in
        // the runtime.
        t_driverOf = this;
        t_driverGeneration = generation_;
        driverBound_.store(true, std::memory_order_release);
    } else if (!isDriverThread()) {
        // Only the bound driver may claim accepted work.  Another thread
        // asking is not an error it can recover from by retrying, but it also
        // must not silently steal the run.
        return 0;
    }

    if (!pending_)
        return 0;

    active_ = std::move(pending_);
    active_->control->setState(RunState::Running);
    return active_->id;
}

SliceResult EmbeddedRuntime::driveFor(TimeDuration budget)
{
    SliceResult slice;
    if (shuttingDown_.load(std::memory_order_acquire)) {
        slice.state = SliceState::ShuttingDown;
        return slice;
    }

    // Claiming latches (or validates) the driver identity, so a slice from
    // any other thread advances nothing.
    if (!hasActiveRun() && claimPendingRun() == 0) {
        if (!isDriverThread() && driverBound()) {
            slice.state = SliceState::Idle;
            return slice;
        }
    }

    RunRecord* active = nullptr;
    // A strong reference for the whole slice: publishing a terminal state can
    // wake a waiter that finalizes the run and drops every other reference,
    // and the driver must not be left holding a destroyed control.
    std::shared_ptr<RunControl> control;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || (driverBound_.load(std::memory_order_acquire)
                         && !isDriverThread())) {
            slice.state = SliceState::Idle;
            return slice;
        }
        // The record is VM-owned and only the driver advances it; the
        // handover below is what keeps a finalizer from destroying it while
        // this slice is still inside, and sliceInProgress_ is what keeps
        // shutdown from doing so.
        active = active_.get();
        control = active_->control;
        sliceInProgress_ = true;
    }

    slice = vm_.driveRunSlice(*active, budget);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sliceInProgress_ = false;
    }
    sliceDone_.notify_all();

    switch (slice.state) {
    case SliceState::DebugPaused:
        control->setState(RunState::DebugPaused);
        break;
    case SliceState::MainReturned:
        control->setState(RunState::MainReturned);
        break;
    case SliceState::ExecutionEnded:
    case SliceState::ExecutionFailed: {
        // The ownership transfer, published once.  Move the record out of the
        // driver's slot FIRST, then make the run finalizable: a finalizer
        // that saw the state before the move could destroy the record while
        // this thread was still inside its slice.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (active_ && active_.get() == active)
                finalizing_ = std::move(active_);
        }
        // Failure travels on the control, not in the state: Failed is a
        // POST-finalization state.  Publishing it here would let a losing
        // finalizer's wait see "Failed" and return before the winner had
        // even begun cleanup.
        if (slice.state == SliceState::ExecutionFailed)
            control->failed_.store(true, std::memory_order_release);
        control->setState(RunState::FinalizationPending);
        break;
    }
    case SliceState::Yielded:
    case SliceState::Blocked:
        if (control->state() == RunState::DebugPaused)
            control->setState(RunState::Running);
        break;
    default:
        break;
    }
    return slice;
}

CancelStatus EmbeddedRuntime::cancelBeforeStart(const RunHandle& run)
{
    if (!run.valid())
        return CancelStatus::Unknown;

    std::unique_ptr<PreparedExecutionRecord> discarded;
    CancelStatus status = CancelStatus::Unknown;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_ && pending_->id == run.id()) {
            discarded = std::move(pending_->prepared);
            pending_->control->setState(RunState::Cancelled);
            pending_.reset();
            status = CancelStatus::Cancelled;
        } else if (active_ && active_->id == run.id()) {
            status = CancelStatus::AlreadyRunning;
        } else {
            status = run.state() == RunState::Cancelled
                         ? CancelStatus::AlreadyTerminal
                         : CancelStatus::Unknown;
        }
    }
    // Outside the lock: destroying roots is a mutator operation, and holding
    // a run lock across it would put a collector barrier behind this mutex.
    destroyRecordCovered(std::move(discarded));
    return status;
}

FinalizeResult EmbeddedRuntime::finalizeRun(const std::shared_ptr<RunControl>& control)
{
    FinalizeResult result;
    if (!control) {
        result.status = FinalizeStatus::RuntimeGone;
        return result;
    }
    // Finalization joins threads and runs module completion hooks.  On the
    // driver that would stall the host's cycle for an unbounded time, which
    // is the whole reason this work is not in driveFor().
    if (isDriverThread()) {
        result.status = FinalizeStatus::OnDriverThread;
        result.state = control->state();
        return result;
    }

    const RunState observed = control->state();
    if (observed == RunState::Cancelled) {
        // Shutdown got here first: there is nothing to finalize and no
        // record to release.
        result.status = FinalizeStatus::AlreadyFinalized;
        result.state = observed;
        return result;
    }
    const bool ready = observed == RunState::FinalizationPending;
    // Finalizing/Completed/Failed means another waiter got here first.
    // Treating those as "not ready" would tell a loser its run had not been
    // handed over, when in fact it is being (or has been) finalized.
    const bool inProgress = observed == RunState::Finalizing
                         || observed == RunState::Completed
                         || observed == RunState::Failed;
    if (!ready && !inProgress) {
        result.status = FinalizeStatus::NotReady;
        result.state = observed;
        return result;
    }

    // Exactly once: the winner finalizes, everyone else waits for its result.
    // Cancelled is a terminal state too -- shutdown publishes it -- and a
    // loser that did not wake on it would hold its lease on this runtime
    // forever.
    if (control->finalizeClaimed_.exchange(true, std::memory_order_acq_rel)) {
        control->waitForState([](RunState s) {
            return s == RunState::Completed || s == RunState::Failed
                || s == RunState::Cancelled;
        });
        result.status = FinalizeStatus::AlreadyFinalized;
        result.state = control->state();
        return result;
    }

    control->setState(RunState::Finalizing);

    std::unique_ptr<RunRecord> record;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Only ever from the handed-over slot: the driver put it there and is
        // provably done with it.
        if (finalizing_ && finalizing_->id == control->id())
            record = std::move(finalizing_);
    }

    bool failed = control->failed();
    if (record) {
        // A hook that throws must not leave the control parked in Finalizing
        // forever with the record alive: the run is finalized either way, and
        // it is reported as failed.
        try {
            failed = vm_.finalizeRunRecord(*record, failed);
        } catch (const std::exception& e) {
            vm_.emitDiagnostic(std::string("finalization failed: ") + e.what(),
                               OutputSeverity::Error, "embed");
            failed = true;
        } catch (...) {
            vm_.emitDiagnostic("finalization failed: unknown exception",
                               OutputSeverity::Error, "embed");
            failed = true;
        }
    }

    // The record (and with it the launch's roots) is released here, on a
    // non-driver thread, with the collector still alive.
    record.reset();

    control->setState(failed ? RunState::Failed : RunState::Completed);
    result.status = FinalizeStatus::Finalized;
    result.state = control->state();
    return result;
}

bool EmbeddedRuntime::hasPendingRun() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_ != nullptr;
}

bool EmbeddedRuntime::hasActiveRun() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ != nullptr;
}

bool EmbeddedRuntime::hasUnfinalizedRun() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    // Claimed and being driven, or handed over and not yet finalized: both
    // are records a detach would tear out from under someone.
    return active_ != nullptr || finalizing_ != nullptr;
}

RunId EmbeddedRuntime::activeRunId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ ? active_->id : 0;
}

bool EmbeddedRuntime::driverBound() const
{
    return driverBound_.load(std::memory_order_acquire);
}

bool EmbeddedRuntime::isDriverThread() const
{
    // Lock-free: the token is this thread's own, and the generation check
    // costs nothing a real-time slice would notice.
    return driverBound_.load(std::memory_order_acquire)
        && t_driverOf == this && t_driverGeneration == generation_;
}

void EmbeddedRuntime::cancelPendingForShutdown()
{
    shuttingDown_.store(true, std::memory_order_release);
    std::unique_ptr<PreparedExecutionRecord> pendingRoots;
    std::unique_ptr<PreparedExecutionRecord> activeRoots;
    std::unique_ptr<PreparedExecutionRecord> finalizingRoots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_) {
            pendingRoots = std::move(pending_->prepared);
            pending_->control->setState(RunState::Cancelled);
            pending_.reset();
        }
        if (active_) {
            activeRoots = std::move(active_->prepared);
            active_->control->setState(RunState::Cancelled);
            active_.reset();
        }
        if (finalizing_) {
            // Handed over but never finalized: release its roots here rather
            // than leaving them registered past collector teardown.
            finalizingRoots = std::move(finalizing_->prepared);
            finalizing_->control->setState(RunState::Cancelled);
            finalizing_.reset();
        }
    }
    // Clear the roots while the collector and root registry are still alive;
    // Values reaching destruction after the shutdown sweep would decRef freed
    // controls.
    destroyRecordCovered(std::move(pendingRoots));
    destroyRecordCovered(std::move(activeRoots));
    destroyRecordCovered(std::move(finalizingRoots));
}

FinalizeResult RunHandle::finalize()
{
    FinalizeResult result;
    if (!control_) {
        result.status = FinalizeStatus::RuntimeGone;
        return result;
    }
    // The lease: a strong reference for the duration of the call, so the
    // runtime cannot be destroyed underneath it.
    std::shared_ptr<EmbeddedRuntime> runtime = control_->runtime_.lock();
    if (!runtime) {
        // The VM went away.  The handle still reports the last state it saw,
        // which is copied, non-GC data and safe to read forever.
        result.status = FinalizeStatus::RuntimeGone;
        result.state = control_->state();
        return result;
    }
    return runtime->finalizeRun(control_);
}

FinalizeResult RunHandle::wait()
{
    FinalizeResult result;
    if (!control_) {
        result.status = FinalizeStatus::RuntimeGone;
        return result;
    }
    control_->waitForState([](RunState s) {
        return s == RunState::FinalizationPending || s == RunState::Failed
            || s == RunState::Completed || s == RunState::Cancelled;
    });
    if (control_->state() == RunState::Cancelled) {
        result.status = FinalizeStatus::AlreadyFinalized;
        result.state = RunState::Cancelled;
        return result;
    }
    return finalize();
}

} // namespace roxal
