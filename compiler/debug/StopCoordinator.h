#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <functional>

#include "core/memory.h"
#include "core/TimePoint.h"
#include "DebugHandleTable.h"
#include "DebugHostControl.h"
#include "../Thread.h"

namespace roxal {

class DebugWorker;
class ExecutionDomain;
class VM;

// The stop coordinator: forms all-or-nothing stop epochs over one
// execution domain.
//
//   Running -> Forming(epoch) -> Stopped(epoch) -> Releasing -> Running
//                        \-> Aborting(epoch) ---------------/
//
// A `stopped` outcome is reported only after: the epoch's membership set is
// immutable (revalidated snapshots), every member acknowledged, dataflow
// admission is closed and no evaluator remains active.  A failed stop rolls
// everything back (acknowledged members released, no stop event) and names
// the thread that did not reach a safe point.
//
// Threading: requestStop()/resume() are called by ONE controlling thread at
// a time (the debug worker / test controller) -- non-RT.  The cooperative
// stop points are called by every debuggee thread; their fast path is one
// interrupt-word bit test, and the RT variant performs only bounded atomic
// work (store + return), never a lock, wait, allocation, or host callback.
class StopCoordinator {
public:
    StopCoordinator(VM& vm, const ptr<ExecutionDomain>& domain);
    ~StopCoordinator();

    // Point this coordinator at another domain -- the program run or REPL
    // session being activated.  Stop state lives on the domain, so the new
    // one starts clean; session id, host control and hold generation live
    // here and carry over.  Only between stops: never while one is forming
    // or active.
    void rebind(const ptr<ExecutionDomain>& domain);

    // Execution is starting on this coordinator's domain: stops may form.
    void beginExecution();
    // Execution is ending: refuse new stops, then release any active one
    // WITHOUT releasing the host's hold (fail-safe stays with the host, as at
    // session end).  Every join-all calls this before joining.
    void endExecution();

    struct StopOutcome {
        bool stopped { false };
        std::string failure;     // human-readable, names the wedged thread
    };

    // Form a stop epoch and wait (bounded) for full quiescence.
    // On success the domain is Stopped and a DebugStopNotice was delivered.
    // On failure everything is rolled back and DebugStopFailure delivered.
    StopOutcome requestStop(DebugStopReason reason, TimeDuration timeout);

    // Release a completed stop.  releaseHold=true is a normal continue
    // (exactly one DebugContinueNotice); releaseHold=false is the step
    // resume shape -- the hold generation stays held.
    void resume(bool releaseHold = true);

    bool isStopped() const { return stopped_.load(std::memory_order_acquire); }
    uint64_t currentEpoch() const;
    uint64_t holdGeneration() const {
        return holdGeneration_.load(std::memory_order_acquire);
    }
    bool holdActive() const { return holdActive_.load(std::memory_order_acquire); }

    // The embedding-host hook.  May be null (no notifications).  Set before
    // execution starts or between stops; not while a stop is forming.
    // Atomic: the debug worker reads it while controllers (re)set it.
    void setHostControl(DebugHostControl* hc) {
        hostControl_.store(hc, std::memory_order_release);
    }

    // Session end (VM shutdown/terminate): notifies the host WITHOUT
    // releasing an active hold (fail-safe).
    void notifySessionEnd();

    // Controller-presence gate.  A stop is only safe to publish while some
    // controller can release it again: a debuggee parked at a breakpoint
    // answers nothing else, not even a request to exit, so a stop nobody
    // owns freezes the program permanently.  Breakpoint requests outlive the
    // run they were made for (that is what lets them bind before the next
    // program starts), so an embedding whose controller comes and goes --
    // the web IDE's control actor exists only for a program started under an
    // armed session -- installs a predicate here; traps then stay inert
    // while no controller exists, which is what "no debugger attached"
    // should mean.  A plain function pointer, not std::function: the trap
    // path calls it and must not allocate or lock.  Null (the default) means
    // a controller is always present, the case for the DAP adapter; an
    // in-VM controller thread (setExcludedThread) counts as one regardless.
    // Set before execution starts.
    void setControllerPresentPredicate(bool (*p)()) {
        controllerPresent_.store(p, std::memory_order_release);
    }

    // ---- Asynchronous stop path (breakpoints/steps) ----
    // Called by the thread that DISCOVERS a stop condition at a statement
    // boundary: bounded lock-free work only (epoch publication; a vDSO
    // clock read for the backoff window) -- it never invokes the host hold
    // hook itself.  canNotify=false is the RT shape: no worker mutex, no
    // futex wake -- the worker's armed-mode timed poll observes the
    // publication instead.  Returns false when a stop is already
    // forming/active or within the failed-stop backoff window; the caller
    // parks/returns Paused either way via the normal stop point.
    // `description` (Exception stops): the failure text, staged under the
    // info mutex by the WINNING publication only -- after beginStop
    // succeeds and before asyncPending_ is set, so the worker can never
    // consume a loser's text.  RT callers pass nothing (an empty string
    // allocates nothing).
    bool tryBeginAsyncStop(DebugStopReason reason, uint64_t threadId,
                           const void* chunk, uint32_t stmtIndex,
                           bool canNotify, std::string description = {});
    // Worker-side completion: hold notification, wakes, acknowledgement
    // collection, commit-or-rollback.
    void completeAsyncStop();

    // Location/reason of the current committed stop (empty when running).
    struct StopInfo {
        DebugStopReason reason { DebugStopReason::Pause };
        uint64_t threadId { 0 };
        std::string sourceName;
        int line { 0 };
        // Exception stops: the staged failure description (error message +
        // rendered stack), captured BEFORE any unwinding.
        std::string description;
    };
    std::optional<StopInfo> stoppedInfo();

    // ---- Fatal-error stop policy ----
    // While enabled (DAP setExceptionBreakpoints / an embedding host), a
    // fatal runtime error publishes an Exception stop BEFORE frames are
    // reset, so the failure remains inspectable; continue/terminate then
    // performs the existing teardown exactly once.  Off by default: with no
    // debugger the external behavior is unchanged.
    void setStopOnFatal(bool on) { stopOnFatal_.store(on, std::memory_order_release); }
    bool stopOnFatal() const { return stopOnFatal_.load(std::memory_order_acquire); }


    // Arm a step on a STOPPED thread and resume WITHOUT releasing the hold
    // generation.  The next statement boundary satisfying the mode
    // publishes a Step stop through the async path.
    void stepAndResume(Thread& t, Thread::DebugStepMode mode);

    // Slow-path demand accounting: while nonzero, IntrDebugSlowPath routes
    // dispatch through the full loop's statement-boundary check.  Counted
    // by the breakpoint manager (any enabled breakpoints) and by armed
    // steps; removeSlowPathDemand is RT-safe (atomics only).
    void addSlowPathDemand();
    void removeSlowPathDemand();
    bool slowPathArmed() const {
        return slowPathDemand_.load(std::memory_order_acquire) > 0;
    }
    // A trap publication awaiting worker completion (part of the worker's
    // wait predicate: an RT publication never notifies).
    bool hasAsyncPending() const {
        return asyncPending_.load(std::memory_order_acquire);
    }

    // Step-state lifecycle: the armed step and its paired demand must have
    // exactly one owner.  cancelStepIfArmed: disarm + release the demand;
    // called for quiesced/parked threads only.
    void cancelStepIfArmed(Thread& t);

    // Worker lifecycle: created on first async use; joined during VM
    // shutdown BEFORE the debug index drops its roots.
    void ensureWorker();
    void shutdownWorker();

    // Route a command onto the debug worker -- the debugger's single
    // controlling thread (transport adapters call this instead of driving
    // stop/resume/step/inspection from their own threads).  Non-RT.
    void postToWorker(std::function<void()> fn);

    // An in-VM controller thread (a native call driving the debugger -- the
    // self-test harness, later in-process embedder controllers) is wedged in
    // native code and must be excluded from epoch membership even when the
    // stop completes on the debug worker, where VM::thread cannot identify
    // it.  Null clears.
    void setExcludedThread(Thread* t) {
        excludedThread_.store(t, std::memory_order_release);
    }

    // ---- Cooperative stop-point slow paths (called from VM/engine code
    //      after the interrupt-word bit test) ----

    // Non-RT: acknowledge the pending epoch and park (GC-safe-blocked) until
    // released.  Returns when the stop has been resumed or aborted; the
    // caller re-evaluates its normal predicates (sleep deadlines, queues).
    void ackAndPark(Thread& t);

    // RT/deadline path: acknowledge only (atomic stores).  The caller must
    // return ExecutionStatus::Paused (or its layer's equivalent) promptly.
    void ackNoPark(Thread& t);

    // Entry gate for threads about to begin executing user code while a stop
    // is forming/active.  Non-RT: parks until release, returns true to
    // proceed.  RT (canPark=false): acknowledges and returns false -- the
    // caller must return Paused without executing.
    bool admitOrPark(Thread& t, bool canPark);

private:
    // requestStop = beginStop -> openHoldAndWake -> awaitQuiescence; the
    // async path runs beginStop on the discovering thread (atomics only)
    // and the remaining two on the worker.
    bool beginStop(DebugStopReason reason);
    void cancelArmedStepsInEpoch();
    void openHoldAndWake(uint64_t epoch, DebugStopReason reason, uint64_t threadId);
    StopOutcome awaitQuiescence(uint64_t epoch, TimeDuration timeout);
    StopOutcome rollback(uint64_t epoch, const std::string& why);
    void releaseAll();

    VM& vm_;
    ptr<ExecutionDomain> domain_;
    std::atomic<DebugHostControl*> hostControl_ { nullptr };
    std::atomic<bool (*)()> controllerPresent_ { nullptr };

    std::atomic<bool> stopped_ { false };
    std::atomic<bool> forming_ { false };
    // Execution-lifetime gate.  Cleared at end of execution BEFORE the parks
    // are released, so a thread let go to finish its last statements cannot
    // trap on a still-armed breakpoint and form a new stop under the join;
    // set again when a program, fragment or run is activated.
    std::atomic<bool> acceptingStops_ { true };
    // Async (breakpoint/step) trap publication: written by the discovering
    // thread with atomics only, consumed by the worker.
    std::atomic<bool> asyncPending_ { false };
    std::atomic<const void*> trapChunk_ { nullptr };
    std::atomic<uint32_t> trapStmt_ { 0 };
    std::atomic<uint64_t> trapThread_ { 0 };
    std::atomic<uint8_t> trapReason_ { 0 };
    // Failed-stop backoff: a wedged thread must not produce a retrap storm.
    std::atomic<int64_t> lastStopFailureUs_ { 0 };
    std::atomic<int> slowPathDemand_ { 0 };
    std::atomic<Thread*> excludedThread_ { nullptr };
    std::mutex infoMutex_;
    std::optional<StopInfo> stopInfo_;
    std::string stagedFailure_;
    std::atomic<bool> stopOnFatal_ { false };
    std::mutex workerMutex_;
    std::unique_ptr<DebugWorker> worker_;
    // Set by shutdownWorker; ensureWorker/postToWorker no-op afterwards.
    std::atomic<bool> workerRetired_ { false };
    // Raw mirror for the RT trap path (set-once before the slow path arms).
    std::atomic<DebugWorker*> workerRaw_ { nullptr };
    TimeDuration asyncStopTimeout_ { TimeDuration::milliSecs(3000) };
    // Strong membership leases of the COMMITTED epoch: retained until
    // resume/rollback/session-end so no member Thread object can be
    // destroyed during the stopped interval -- the inspection phase reads
    // through these.  Guarded by leaseMutex_: the controller
    // assigns/clears, and the registration admission gate appends threads
    // created while a stop is forming/active.
    std::mutex leaseMutex_;
    std::vector<ptr<Thread>> epochLeases_;

public:
    // Called by ThreadManager's registration admission gate for a thread
    // created during a forming/active stop: it joins the epoch pre-
    // acknowledged and must be leased like every other member.
    void addLateEpochLease(const ptr<Thread>& t);

    // ---- Inspection surface ----
    // Session-local opaque handles; a typed persistent GC root.  Cleared at
    // the top of every release (continue/step/rollback/session end), before
    // any debuggee code runs.
    DebugHandleTable& handleTable() { return handleTable_; }

    // Iterate the COMMITTED epoch's leased threads (the inspection set).
    // The callback must not call back into stop/resume.
    void forEachEpochThread(const std::function<void(const ptr<Thread>&)>& f) {
        std::lock_guard<std::mutex> lock(leaseMutex_);
        for (const auto& t : epochLeases_)
            if (t)
                f(t);
    }

private:
    // Inspection handles.  A typed persistent GC root: constructed with the
    // coordinator (always from a controller/native context, never inside an
    // RT GC-yield section).
    DebugHandleTable handleTable_;

    uint64_t sessionId_ { 1 };
    // The hold state is touched by TWO threads, not one: a synchronous stop
    // opens the hold on the controller, while a breakpoint or step stop
    // completes on the debug worker -- and an embedding host pausing while a
    // trap is being completed puts both in flight at once.  Atomic, with the
    // false->true exchange as the exactly-once gate, so a hold generation can
    // never be opened or released twice.
    std::atomic<uint64_t> holdGeneration_ { 0 };
    std::atomic<bool> holdActive_ { false };
    DebugStopReason reason_ { DebugStopReason::Pause };
};

} // namespace roxal
