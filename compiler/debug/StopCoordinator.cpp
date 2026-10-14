#include "StopCoordinator.h"

#include <chrono>

#include "../ExecutionDomain.h"
#include "../Thread.h"
#include "../ThreadManager.h"
#include "../VM.h"
#include "../SimpleMarkSweepGC.h"
#include "../../dataflow/DataflowEngine.h"
#include "DebugWorker.h"

#include <cassert>

namespace roxal {

StopCoordinator::StopCoordinator(VM& vm, const ptr<ExecutionDomain>& domain)
    : vm_(vm), domain_(domain)
{
    domain_->coordinator.store(this, std::memory_order_release);
}

StopCoordinator::~StopCoordinator()
{
    domain_->coordinator.store(nullptr, std::memory_order_release);
}

void StopCoordinator::beginExecution()
{
    acceptingStops_.store(true, std::memory_order_release);
    // A host hold placed before this execution started: it begins paused.
    // Atomics only (this can run on the RT driver); the worker opens the
    // hold and collects the acknowledgements (advancePendingStop).
    if ((owners_.load(std::memory_order_acquire) & ownerBit(StopOwner::Host)) != 0
        && beginStop(DebugStopReason::Host))
        pending_.store(true, std::memory_order_release);
}

void StopCoordinator::endExecution()
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    // Order matters: close the gate first, so nothing that the release
    // lets run can open a new stop; then release.
    acceptingStops_.store(false, std::memory_order_release);
    // The debugger's interest ends with the execution; the host's does not
    // (while it holds, the next execution starts paused).
    owners_.fetch_and(uint8_t(~ownerBit(StopOwner::Debugger)), std::memory_order_acq_rel);
    pending_.store(false, std::memory_order_release);
    if (stopped_.load(std::memory_order_acquire)) {
        resumeNow(/*releaseHold=*/false);
        return;
    }
    // A stop still FORMING -- its controller gave up, or its worker never
    // got its acks -- has the interrupt bit set and members parked on it
    // just as an active one does, but stopped_ is false.  Roll it back the
    // way a timed-out stop is: everything it parked is released, the host
    // hears it failed, and the gate above keeps it from re-forming.
    if (forming_.load(std::memory_order_acquire)
        || (domain_->interrupts().load() & ExecutionDomain::IntrDebugStop) != 0)
        rollback(domain_->stopEpoch.load(std::memory_order_acquire),
                 "execution ended");
}

void StopCoordinator::rebind(const ptr<ExecutionDomain>& domain)
{
    if (!domain || domain == domain_)
        return;
    assert(!stopped_.load(std::memory_order_acquire)
           && !forming_.load(std::memory_order_acquire)
           && "rebind only between stops");
    domain_->coordinator.store(nullptr, std::memory_order_release);
    // The slow-path demand is the coordinator's, not one domain's: breakpoints
    // and armed steps that exist BEFORE this execution had a domain (a driven
    // launch sets its breakpoints between preparation and activation) must
    // trap in the domain being activated.  Move the dispatch-loop bit along.
    if (slowPathDemand_.load(std::memory_order_acquire) > 0) {
        domain_->interrupts().fetch_and(~uint32_t(ExecutionDomain::IntrDebugSlowPath));
        domain->interrupts().fetch_or(ExecutionDomain::IntrDebugSlowPath);
    }
    domain_ = domain;
    domain_->coordinator.store(this, std::memory_order_release);
}

void StopCoordinator::addLateEpochLease(const ptr<Thread>& t)
{
    // Registration admission gate: a thread created during a forming/active
    // stop joins the epoch pre-acknowledged; lease it like every other
    // member.  Duplicates with the commit snapshot are harmless (strong
    // refs).
    if (t->debugExcluded.load(std::memory_order_acquire))
        return;   // service thread: runs through the stop, never a lease
    std::lock_guard<std::mutex> lk(leaseMutex_);
    if (stopped_.load(std::memory_order_acquire)
        || forming_.load(std::memory_order_acquire))
        epochLeases_.push_back(t);
}

uint64_t StopCoordinator::currentEpoch() const
{
    return domain_->stopEpoch.load(std::memory_order_acquire);
}

static std::string describeThread(const ptr<Thread>& t)
{
    const char* kind = "thread";
    switch (t->kind) {
        case ThreadKind::Main:     kind = "main";     break;
        case ThreadKind::Init:     kind = "init";     break;
        case ThreadKind::Repl:     kind = "repl";     break;
        case ThreadKind::Actor:    kind = "actor";    break;
        case ThreadKind::Dataflow: kind = "dataflow"; break;
    }
    return std::string(kind) + " thread #" + std::to_string(t->id());
}

bool StopCoordinator::beginStop(DebugStopReason reason)
{
    // Atomic-only stop publication (RT-safe): epoch, admission gate,
    // interrupt bit, dataflow gate.  Wakes and the host hold notification
    // are NOT here -- they involve syscalls/callbacks and belong to the
    // (non-RT) controller or the debug worker (openHoldAndWake).
    if (!acceptingStops_.load(std::memory_order_acquire))
        return false;   // execution is ending; a stop now could never be released
    if (stopped_.load(std::memory_order_acquire) || forming_.exchange(true))
        return false;
    reason_ = reason;
    domain_->stopEpoch.fetch_add(1, std::memory_order_acq_rel);
    domain_->admissionClosed.store(true, std::memory_order_release);
    domain_->interrupts().fetch_or(ExecutionDomain::IntrDebugStop);
    if (df::DataflowEngine* eng = vm_.dataflowEngineForDebug())
        eng->debugGate.close();
    return true;
}

void StopCoordinator::openHoldAndWake(uint64_t epoch, DebugStopReason reason,
                                      uint64_t threadId)
{
    // Under controlMutex_ (most callers already hold it): holdDropped()
    // decides "no stop is forming" and clears the generation under it, so a
    // stop's hold opening cannot interleave with that.
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    // Host hold notification: exactly once per running-to-held generation.
    // Invoked by a non-RT thread (sync controller or the debug worker),
    // never by the RT/interpreter callback that discovered the stop
    // condition.  Queued != physically held.
    // Exactly-once per running-to-held generation: whoever wins the
    // false->true exchange owns the notification, so a controller and the
    // worker cannot both open the same hold.
    bool wasHeld = false;
    if (holdActive_.compare_exchange_strong(wasHeld, true,
                                            std::memory_order_acq_rel)) {
        const uint64_t generation =
            holdGeneration_.fetch_add(1, std::memory_order_acq_rel) + 1;
        DebugHostControl* hc = hostControl_.load(std::memory_order_acquire);
        if (hc) {
            DebugHoldRequest req;
            req.sessionId = sessionId_;
            req.holdGeneration = generation;
            req.stopEpoch = epoch;
            req.reason = reason;
            req.threadId = threadId;
            if (hc->onDebugHoldRequested(req) == DebugHostResult::Rejected) {
                // Recorded and surfaced; Roxal still quiesces.  A
                // safety-critical host integration should treat this as a
                // safety fault on its side.
                VM::emitDiagnostic("debugger: host hold notification rejected",
                                   OutputSeverity::Warning, "debug");
            }
        }
    } else if (DebugHostControl* hc = hostControl_.load(std::memory_order_acquire)) {
        // A stop forming while the generation is already held: a step
        // landing, or a breakpoint / pause / host hold / exception
        // preempting it, or a generation still held from an earlier
        // execution.  Not a new generation -- re-assert the current one, so
        // a host that released anything during a step holds again.  Each
        // stop reaches here once: every opener records pendingWokenEpoch_
        // (or, for completeAsyncStop, consumes asyncPending_) before calling.
        DebugHoldRequest req;
        req.sessionId = sessionId_;
        req.holdGeneration = holdGeneration_.load(std::memory_order_acquire);
        req.stopEpoch = epoch;
        req.reason = reason;
        req.threadId = threadId;
        hc->onDebugHoldReasserted(req);
    }

    // Wake everything that might be blocked so it reaches a stop point:
    // sleepers/awaiters (loop top), idle actors (queue condvar), the
    // dataflow drain sleep.  The RT host is never waited on synchronously.
    ThreadManager::instance().wakeAll();
    if (df::DataflowEngine* eng = vm_.dataflowEngineForDebug())
        eng->wakeDrain();
}

StopCoordinator::StopOutcome StopCoordinator::requestStop(DebugStopReason reason,
                                                          TimeDuration timeout)
{
    // A blocking request is the debugger's (whatever its reason label); the
    // host pauses through requestHostPause.
    if (!beginStop(reason)) {
        if (forming_.load(std::memory_order_acquire)
            && pending_.load(std::memory_order_acquire)) {
            // A pending stop: join it, and help it along -- this may be the
            // worker that would otherwise advance it.  If it does not
            // complete in our budget it stays pending as its owners' (the
            // Debugger share is dropped only if this request added it).
            const bool hadDebugger =
                (owners_.fetch_or(ownerBit(StopOwner::Debugger), std::memory_order_acq_rel)
                 & ownerBit(StopOwner::Debugger)) != 0;
            const TimePoint deadline = TimePoint::currentTime() + timeout;
            for (;;) {
                advancePendingStop();
                if (stopped_.load(std::memory_order_acquire))
                    return { true, {} };
                if (!forming_.load(std::memory_order_acquire)
                    || TimePoint::currentTime() >= deadline)
                    break;
                SimpleMarkSweepGC::GCSafeBlockScope blockScope;
                std::unique_lock<std::mutex> lk(domain_->stopMutex);
                domain_->ackCv.wait_for(lk, std::chrono::milliseconds(1));
            }
            if (!hadDebugger)
                owners_.fetch_and(uint8_t(~ownerBit(StopOwner::Debugger)),
                                  std::memory_order_acq_rel);
            std::string waiting;
            {
                std::lock_guard<std::mutex> lk(infoMutex_);
                waiting = waitingFor_;
            }
            return { false, waiting.empty() ? std::string("stop timed out")
                                            : "stop timed out waiting for " + waiting };
        }
        return { false, "a stop is already forming or active" };
    }
    owners_.fetch_or(ownerBit(StopOwner::Debugger), std::memory_order_acq_rel);
    const uint64_t epoch = currentEpoch();
    {
        // Before quiescence: the commit-time onDebugStopped hook may read
        // stoppedInfo() (see completeAsyncStop).  The reported thread is
        // chosen at commit (tryCommitPass).
        std::lock_guard<std::mutex> lk(infoMutex_);
        StopInfo info;
        info.reason = reason;
        stopInfo_ = info;
    }
    // Recorded as the other openers do: if this stop later stays pending as
    // the host's (giveUp), the worker's advancePendingStop must not open --
    // and notify -- its hold a second time.
    pendingWokenEpoch_.store(epoch, std::memory_order_release);
    openHoldAndWake(epoch, reason, VM::thread ? VM::thread->id() : 0);
    // (tryCommitPass cancels preempted steps before publishing the stop.)
    return awaitQuiescence(epoch, timeout);
}

StopCoordinator::StopOutcome StopCoordinator::awaitQuiescence(uint64_t epoch,
                                                              TimeDuration timeout)
{
    // Collect acknowledgements until the epoch commits -- or the timeout
    // expires, when the stop is rolled back (or left pending as the host's).
    const TimePoint deadline = TimePoint::currentTime() + timeout;
    std::string laggard;
    for (;;) {
        if (tryCommitPass(epoch, laggard))
            return { true, {} };
        if (!forming_.load(std::memory_order_acquire))
            return { false, "the stop was withdrawn" };
        if (TimePoint::currentTime() >= deadline)
            return giveUp(epoch, laggard.empty()
                                     ? std::string("stop timed out")
                                     : "stop timed out waiting for " + laggard,
                          StopOwner::Debugger);
        // Timed ack wait: non-RT ackers notify ackCv; RT ackers only store
        // atomics, which this poll interval observes.
        SimpleMarkSweepGC::GCSafeBlockScope blockScope;
        std::unique_lock<std::mutex> lk(domain_->stopMutex);
        domain_->ackCv.wait_for(lk, std::chrono::milliseconds(1));
    }
}

bool StopCoordinator::tryCommitPass(uint64_t epoch, std::string& laggard)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    laggard.clear();
    if (!forming_.load(std::memory_order_acquire) || currentEpoch() != epoch)
        return false;   // withdrawn (cancelled, rolled back) under us

    // The controller thread itself (debug worker, or an in-VM test
    // controller inside a native call) is not a member of the epoch: it is
    // the one driving it and must not run debuggee code while stopped.
    // (On the worker, VM::thread is null; an in-VM controller registers
    // itself via setExcludedThread so the worker path can skip it too.)
    Thread* controller = VM::thread ? VM::thread.get() : nullptr;
    Thread* excluded = excludedThread_.load(std::memory_order_acquire);
    auto isController = [&](const Thread* t) {
        return t == controller || (excluded != nullptr && t == excluded)
               || t->debugExcluded.load(std::memory_order_acquire);
    };
    df::DataflowEngine* eng = vm_.dataflowEngineForDebug();

    // Acknowledge idle threads externally; the epoch commits once the
    // membership snapshot is stable and fully acknowledged and the dataflow
    // gate is quiesced.
    bool allAcked = true;
    auto leases = ThreadManager::instance().snapshotLeases(domain_->id());
    for (auto& t : leases) {
        if (isController(t.get()))
            continue;
        if (t->debugAckEpoch.load(std::memory_order_acquire) >= epoch)
            continue;
        // Idle thread: acknowledge it externally.  The CAS proves nobody
        // is inside execute(); the entry gate keeps it out until release.
        auto expect = Thread::DebugOwnership::Idle;
        if (t->debugOwnership.compare_exchange_strong(
                expect, Thread::DebugOwnership::StoppedExternal,
                std::memory_order_acq_rel)) {
            t->debugAckEpoch.store(epoch, std::memory_order_release);
            continue;
        }
        allAcked = false;
        laggard = describeThread(t);
    }
    const bool gateQuiet = !eng || eng->debugGate.quiesced();
    if (allAcked && !gateQuiet && laggard.empty())
        laggard = "dataflow evaluator still active";
    if (!allAcked || !gateQuiet)
        return false;
    // Membership revalidation: threads registered while forming are
    // pre-acknowledged by the registration gate, so one more stable scan
    // under the same predicate confirms the set is closed.
    auto again = ThreadManager::instance().snapshotLeases(domain_->id());
    for (auto& t : again) {
        if (isController(t.get()))
            continue;
        if (t->debugAckEpoch.load(std::memory_order_acquire) < epoch) {
            laggard = describeThread(t);
            return false;
        }
    }
    {
        // Commit: retain the revalidated membership as strong leases for
        // the whole stopped interval -- no member Thread can be destroyed
        // while stopped.  Merge (not replace): the registration gate may
        // already have appended late-registered threads.  MEMBERS ONLY:
        // controllers and debug-excluded service threads keep RUNNING
        // through the stop -- leasing them would hand the inspector
        // actively-changing frames as if frozen.
        std::lock_guard<std::mutex> lk(leaseMutex_);
        for (auto& t : again) {
            if (isController(t.get()))
                continue;
            epochLeases_.push_back(std::move(t));
        }
    }
    {
        // A pause (debugger or host) reports a deterministic FROZEN member,
        // never the controller -- which for a web pause is the Debug service
        // actor, not something the user can inspect.  Prefer the
        // smallest-id member WITH frames (the engine actor idles frameless
        // and makes a useless selection); frames of quiesced members are
        // safe to size.  Before the stopped notice, which reads it.
        uint64_t chosen = 0;
        uint64_t chosenAny = 0;
        {
            std::lock_guard<std::mutex> lk(leaseMutex_);
            for (const auto& t : epochLeases_) {
                if (!t)
                    continue;
                if (chosenAny == 0 || t->id() < chosenAny)
                    chosenAny = t->id();
                if (!t->frames.empty() && (chosen == 0 || t->id() < chosen))
                    chosen = t->id();
            }
        }
        std::lock_guard<std::mutex> lk(infoMutex_);
        waitingFor_.clear();
        if (stopInfo_ && (stopInfo_->reason == DebugStopReason::Pause
                          || stopInfo_->reason == DebugStopReason::Host))
            stopInfo_->threadId = chosen != 0 ? chosen : chosenAny;
    }

    // Cancel steps preempted by this stop BEFORE publishing stopped_: every
    // member is parked, and no controller can observe the stop yet -- a
    // sweep after publication would race an external controller's immediate
    // stepAndResume (TSan-caught).
    cancelArmedStepsInEpoch();
    stopped_.store(true, std::memory_order_release);
    forming_.store(false, std::memory_order_release);
    pending_.store(false, std::memory_order_release);
    if (DebugHostControl* hc = hostControl_.load(std::memory_order_acquire))
        hc->onDebugStopped({ sessionId_,
                             holdGeneration_.load(std::memory_order_acquire),
                             epoch });
    return true;
}

StopCoordinator::StopOutcome StopCoordinator::giveUp(uint64_t epoch, const std::string& why,
                                                     StopOwner owner)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    if (!forming_.load(std::memory_order_acquire) || currentEpoch() != epoch)
        return { false, why };
    owners_.fetch_and(uint8_t(~ownerBit(owner)), std::memory_order_acq_rel);
    if ((owners_.load(std::memory_order_acquire) & ownerBit(StopOwner::Host)) != 0) {
        // The host joined while it formed: a host pause never fails, so the
        // stop stays pending as the host's and the worker carries it on.
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            const std::string prefix = "stop timed out waiting for ";
            waitingFor_ = why.rfind(prefix, 0) == 0 ? why.substr(prefix.size()) : std::string();
        }
        pending_.store(true, std::memory_order_release);
        if (DebugWorker* w = workerRaw_.load(std::memory_order_acquire))
            w->notifyRecheck();
        return { false, why };
    }
    return rollback(epoch, why);
}

StopCoordinator::StopOutcome StopCoordinator::rollback(uint64_t epoch, const std::string& why)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    // All-or-nothing: acknowledged participants are released, no stopped
    // event is emitted, and the host motion hold is NOT silently released
    // -- the host is told the stop failed and applies its own fail-safe
    // policy.
    releaseAll();
    if (DebugHostControl* hc = hostControl_.load(std::memory_order_acquire))
        hc->onDebugStopFailed({ sessionId_,
                                holdGeneration_.load(std::memory_order_acquire),
                                epoch, why });
    forming_.store(false, std::memory_order_release);
    pending_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        waitingFor_.clear();
    }
    // Failed-stop backoff for the async path: a wedged thread must not
    // produce a breakpoint retrap storm.
    lastStopFailureUs_.store(TimePoint::currentTime().microSecs(),
                             std::memory_order_relaxed);
    return { false, why };
}

void StopCoordinator::cancelPending(bool releaseHold)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    if (!forming_.load(std::memory_order_acquire))
        return;
    // Nobody wants it any more: release what parked, close the hold it
    // opened (unless the execution is ending -- see the header), report
    // nothing as failed (it did not fail; it was withdrawn).
    releaseAll();
    forming_.store(false, std::memory_order_release);
    pending_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        stopInfo_.reset();
        waitingFor_.clear();
    }
    if (releaseHold && holdActive_.exchange(false, std::memory_order_acq_rel)) {
        if (DebugHostControl* hc = hostControl_.load(std::memory_order_acquire))
            hc->onDebugContinueRequested(
                { sessionId_, holdGeneration_.load(std::memory_order_acquire) });
    }
}

StopCoordinator::HostPauseState StopCoordinator::requestHostPause()
{
    ensureWorker();
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    owners_.fetch_or(ownerBit(StopOwner::Host), std::memory_order_acq_rel);
    if (stopped_.load(std::memory_order_acquire))
        return HostPauseState::Stopped;
    if (forming_.load(std::memory_order_acquire))
        return HostPauseState::Pending;   // joined: if that stop gives up, it stays ours (giveUp)
    if (!beginStop(DebugStopReason::Host))
        return HostPauseState::Idle;      // no execution: the next one starts paused
    const uint64_t epoch = currentEpoch();
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        StopInfo info;
        info.reason = DebugStopReason::Host;
        stopInfo_ = info;
        waitingFor_.clear();
    }
    pendingWokenEpoch_.store(epoch, std::memory_order_release);
    pending_.store(true, std::memory_order_release);
    openHoldAndWake(epoch, DebugStopReason::Host, 0);
    if (DebugWorker* w = workerRaw_.load(std::memory_order_acquire))
        w->notifyRecheck();
    return HostPauseState::Pending;
}

StopCoordinator::PauseState StopCoordinator::requestDebuggerPause()
{
    ensureWorker();
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    if (stopped_.load(std::memory_order_acquire) || forming_.load(std::memory_order_acquire)) {
        // Join what is already stopped or forming (a host hold, a trap).
        owners_.fetch_or(ownerBit(StopOwner::Debugger), std::memory_order_acq_rel);
        return stopped_.load(std::memory_order_acquire) ? PauseState::Stopped
                                                         : PauseState::Pending;
    }
    if (!beginStop(DebugStopReason::Pause))
        return PauseState::Idle;   // nothing executes
    owners_.fetch_or(ownerBit(StopOwner::Debugger), std::memory_order_acq_rel);
    const uint64_t epoch = currentEpoch();
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        StopInfo info;
        info.reason = DebugStopReason::Pause;
        stopInfo_ = info;
        waitingFor_.clear();
    }
    pendingWokenEpoch_.store(epoch, std::memory_order_release);
    pending_.store(true, std::memory_order_release);
    openHoldAndWake(epoch, DebugStopReason::Pause, 0);
    if (DebugWorker* w = workerRaw_.load(std::memory_order_acquire))
        w->notifyRecheck();
    return PauseState::Pending;
}

void StopCoordinator::releaseHostPause()
{
    const uint8_t host = ownerBit(StopOwner::Host);
    const uint8_t before = owners_.fetch_and(uint8_t(~host), std::memory_order_acq_rel);
    if ((before & host) == 0 || (before & uint8_t(~host)) != 0)
        return;   // not held by the host, or the debugger still holds it
    // Physical release on the worker: it serializes with the debugger's
    // inspection, which reads the stopped threads.
    postToWorker([this] {
        std::lock_guard<std::recursive_mutex> control(controlMutex_);
        if (owners_.load(std::memory_order_acquire) != 0)
            return;   // taken again meanwhile
        if (stopped_.load(std::memory_order_acquire))
            resumeNow(/*releaseHold=*/true);
        else if (pending_.load(std::memory_order_acquire))
            cancelPending(/*releaseHold=*/true);
    });
}

void StopCoordinator::releaseForExit()
{
    // No new stop in this execution: it is ending.  The next one re-opens
    // the gate (beginExecution) -- and starts paused if the host still holds.
    acceptingStops_.store(false, std::memory_order_release);
    owners_.fetch_and(uint8_t(~ownerBit(StopOwner::Debugger)), std::memory_order_acq_rel);
    postToWorker([this] {
        std::lock_guard<std::recursive_mutex> control(controlMutex_);
        // releaseHold=false: an exit must never be what restarts motion; the
        // hold stays the host's to drop (as at session end) -- for a stop
        // still pending just as for a committed one.
        if (stopped_.load(std::memory_order_acquire))
            resumeNow(/*releaseHold=*/false);
        else if (pending_.load(std::memory_order_acquire))
            cancelPending(/*releaseHold=*/false);
    });
}

bool StopCoordinator::holdDropped()
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    // A live stop owns the generation: its release (or failure) is what
    // ends it.  beginStop sets forming_ before any openHoldAndWake, and that
    // runs under this mutex -- so either the stop is visible here, or its
    // hold opens after this store (a fresh generation).
    if (stopped_.load(std::memory_order_acquire)
        || forming_.load(std::memory_order_acquire))
        return false;
    holdActive_.store(false, std::memory_order_release);
    return true;
}

void StopCoordinator::resumeForShutdown()
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    owners_.store(0, std::memory_order_release);
    pending_.store(false, std::memory_order_release);
    if (stopped_.load(std::memory_order_acquire))
        resumeNow(/*releaseHold=*/false);
    else if (forming_.load(std::memory_order_acquire))
        rollback(currentEpoch(), "VM shutting down");
}

StopCoordinator::PauseStatus StopCoordinator::pauseStatus()
{
    PauseStatus s;
    const uint8_t o = owners_.load(std::memory_order_acquire);
    s.host = (o & ownerBit(StopOwner::Host)) != 0;
    s.debugger = (o & ownerBit(StopOwner::Debugger)) != 0;
    s.stopped = stopped_.load(std::memory_order_acquire);
    s.pending = !s.stopped && forming_.load(std::memory_order_acquire)
                && pending_.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(infoMutex_);
    if (s.pending)
        s.waitingFor = waitingFor_;
    return s;
}

void StopCoordinator::advancePendingStop()
{
    if (!pending_.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    if (!pending_.load(std::memory_order_acquire))
        return;
    if (!forming_.load(std::memory_order_acquire)) {
        pending_.store(false, std::memory_order_release);
        return;
    }
    if (owners_.load(std::memory_order_acquire) == 0) {
        // Released before it committed.  If the execution is ending, this is
        // releaseForExit's withdrawal reached first by the worker (it drops
        // the Debugger owner before posting): keep the hold, as it does.
        cancelPending(/*releaseHold=*/acceptingStops_.load(std::memory_order_acquire));
        return;
    }
    const uint64_t epoch = currentEpoch();
    if (pendingWokenEpoch_.load(std::memory_order_acquire) != epoch) {
        // Started on the driver (beginExecution under a host hold), which
        // could do atomics only: open the hold and wake everything here.
        {
            std::lock_guard<std::mutex> lk(infoMutex_);
            StopInfo info;
            info.reason = DebugStopReason::Host;
            stopInfo_ = info;
        }
        pendingWokenEpoch_.store(epoch, std::memory_order_release);
        openHoldAndWake(epoch, DebugStopReason::Host, 0);
    }
    std::string laggard;
    if (!tryCommitPass(epoch, laggard)) {
        std::lock_guard<std::mutex> lk(infoMutex_);
        waitingFor_ = laggard;
    }
}

void StopCoordinator::releaseAll()
{
    // Release ordering: FIRST invalidate every inspection handle -- stale
    // ids must be dead before any debuggee code can run -- then reopen
    // dataflow admission, clear the publication, release
    // externally-acknowledged threads, and wake parked members.
    handleTable_.clearForResume();
    if (df::DataflowEngine* eng = vm_.dataflowEngineForDebug())
        eng->debugGate.open();
    domain_->interrupts().fetch_and(~uint32_t(ExecutionDomain::IntrDebugStop));
    domain_->admissionClosed.store(false, std::memory_order_release);
    for (auto& t : ThreadManager::instance().snapshotLeases(domain_->id())) {
        auto expect = Thread::DebugOwnership::StoppedExternal;
        t->debugOwnership.compare_exchange_strong(expect, Thread::DebugOwnership::Idle,
                                                  std::memory_order_acq_rel);
    }
    {
        std::lock_guard<std::mutex> lk(domain_->stopMutex);
        domain_->releaseGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    domain_->stopCv.notify_all();
    // Membership leases live exactly as long as the stop: released last.
    std::lock_guard<std::mutex> lk(leaseMutex_);
    epochLeases_.clear();
}

void StopCoordinator::resume(bool releaseHold)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    owners_.fetch_and(uint8_t(~ownerBit(StopOwner::Debugger)), std::memory_order_acq_rel);
    if ((owners_.load(std::memory_order_acquire) & ownerBit(StopOwner::Host)) != 0)
        return;   // the host still holds it: the domain stays stopped
    if (!stopped_.load(std::memory_order_acquire) && pending_.load(std::memory_order_acquire)) {
        cancelPending(releaseHold);   // still forming, and nobody wants it now
        return;
    }
    resumeNow(releaseHold);
}

bool StopCoordinator::joinStop(StopOwner owner)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    if (!stopped_.load(std::memory_order_acquire))
        return false;
    owners_.fetch_or(ownerBit(owner), std::memory_order_acq_rel);
    return true;
}

void StopCoordinator::resumeNow(bool releaseHold)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    if (!stopped_.exchange(false))
        return;
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        stopInfo_.reset();
    }
    releaseAll();
    // Exactly-once release, by the same exchange rule as the open: only the
    // thread that observed the hold as held sends the continue notice.
    if (releaseHold && holdActive_.exchange(false, std::memory_order_acq_rel)) {
        if (DebugHostControl* hc = hostControl_.load(std::memory_order_acquire))
            hc->onDebugContinueRequested(
                { sessionId_, holdGeneration_.load(std::memory_order_acquire) });
    }
    // releaseHold=false is the step-resume shape: the hold generation stays
    // held, so stepping can never implicitly restart robot motion.
}

void StopCoordinator::notifySessionEnd()
{
    if (DebugHostControl* hc = hostControl_.load(std::memory_order_acquire))
        hc->onDebugSessionEnded({ sessionId_, stopped_.load() });
    // Deliberately no hold release: fail-safe stays with the host.
}

void StopCoordinator::ackAndPark(Thread& t)
{
    if (t.debugExcluded.load(std::memory_order_acquire))
        return;   // service thread: never a member, never parked
    // Read the release generation FIRST: if release lands between our ack
    // and the wait, the generation already differs and the wait falls
    // through -- level-triggered, no lost wakeup.
    const uint64_t gen = domain_->releaseGeneration.load(std::memory_order_acquire);
    if ((domain_->interrupts().load() & ExecutionDomain::IntrDebugStop) == 0)
        return;   // already released
    t.debugAckEpoch.store(domain_->stopEpoch.load(std::memory_order_acquire),
                          std::memory_order_release);
    domain_->ackCv.notify_all();
#ifdef __EMSCRIPTEN__
    // Web liveness: the main thread is the ONLY host-loop pump --
    // a condvar park here would leave store calls (including the debug
    // store's continue!) and the whole IDE dead while stopped.  Pump-park
    // instead.  Deliberately NOT GC-safe-blocked: the pump allocates and
    // can re-enter the VM, so this thread stays a visible mutator and takes
    // the collection safepoint each turn.  The stop-aware inbound drain
    // (JsBridge) defers user callbacks and non-actor store work meanwhile.
    if (VM::onMainThread() && vm_.hasHostEventLoop()) {
        while (domain_->releaseGeneration.load(std::memory_order_acquire) == gen) {
            vm_.debugStopHostPump();
            SimpleMarkSweepGC::instance().safepoint(t);
        }
        return;
    }
#endif
    // Parked = GC-safe-blocked: invisible to the collection barrier (the
    // thread's frames stay rooted via the ThreadManager registry trace).
    SimpleMarkSweepGC::GCSafeBlockScope blockScope;
    std::unique_lock<std::mutex> lk(domain_->stopMutex);
    domain_->stopCv.wait(lk, [&] {
        return domain_->releaseGeneration.load(std::memory_order_acquire) != gen;
    });
}

void StopCoordinator::ackNoPark(Thread& t)
{
    // RT path: bounded atomic work only -- no lock, no notify (syscall), no
    // allocation.  The coordinator's timed ack poll observes the store.
    t.debugAckEpoch.store(domain_->stopEpoch.load(std::memory_order_acquire),
                          std::memory_order_release);
}

bool StopCoordinator::tryBeginAsyncStop(DebugStopReason reason, uint64_t threadId,
                                        const void* chunk, uint32_t stmtIndex,
                                        bool canNotify, std::string description)
{
    // Bounded, lock-free work only -- callable from RT/interpreter paths.
    // (The clock read is vDSO-backed and lock-free; the RT dispatch path
    // already reads the clock for deadline checks.)
    // Do not take a stop nobody could release: a parked debuggee answers
    // nothing, not even a request to exit.  An in-VM controller thread is a
    // controller in its own right; otherwise ask the embedding.
    if (auto* present = controllerPresent_.load(std::memory_order_acquire))
        if (!excludedThread_.load(std::memory_order_acquire) && !present())
            return false;
    const int64_t nowUs = TimePoint::currentTime().microSecs();
    if (nowUs - lastStopFailureUs_.load(std::memory_order_relaxed) < 200000)
        return false;   // failed-stop backoff window
    if (!beginStop(reason))
        return false;
    owners_.fetch_or(ownerBit(StopOwner::Debugger), std::memory_order_acq_rel);
    if (!description.empty()) {
        // Winner-only staging, strictly before the asyncPending_ publication
        // the worker consumes it behind.
        std::lock_guard<std::mutex> lk(infoMutex_);
        stagedFailure_ = std::move(description);
    }
    trapChunk_.store(chunk, std::memory_order_relaxed);
    trapStmt_.store(stmtIndex, std::memory_order_relaxed);
    trapThread_.store(threadId, std::memory_order_relaxed);
    trapReason_.store(uint8_t(reason), std::memory_order_relaxed);
    asyncPending_.store(true, std::memory_order_release);
    // The worker exists whenever the slow path is armed (addSlowPathDemand
    // creates it from a non-RT context before the bit is set).  RT slices
    // (canNotify=false) must not take the worker's mutex or issue a futex
    // wake: they rely on the worker's armed-mode timed poll, which observes
    // asyncPending_ within one poll interval.
    DebugWorker* w = workerRaw_.load(std::memory_order_acquire);
    assert(w && "async stop published with no debug worker armed");
    if (w && canNotify)
        w->notifyStopRequested();
    return true;
}

void StopCoordinator::completeAsyncStop()
{
    if (!asyncPending_.exchange(false, std::memory_order_acq_rel))
        return;
    const uint64_t epoch = currentEpoch();
    const auto reason = DebugStopReason(trapReason_.load(std::memory_order_relaxed));
    // A fired step's paired slow-path demand is released HERE, not on the
    // trap path: releasing it there could drop the demand to zero right
    // after an unnotified RT publication, letting the worker enter its idle
    // wait with a stop pending.  Exactly once per publication (the
    // asyncPending_ exchange above), commit or rollback.
    if (reason == DebugStopReason::Step)
        removeSlowPathDemand();
    // Resolve the trap location BEFORE quiescence: the onDebugStopped notice
    // fires inside awaitQuiescence at commit, and a transport host reads
    // stoppedInfo() from that hook -- the info must already be there.  (The
    // trapping thread published then parked, so its closure -- and
    // therefore the chunk -- stays alive.)  stoppedInfo() itself gates on
    // stopped_, so a rollback leaves the stale record unreachable.
    {
        StopInfo info;
        info.reason = reason;
        info.threadId = trapThread_.load(std::memory_order_relaxed);
        if (const auto* ch = static_cast<const Chunk*>(trapChunk_.load(std::memory_order_relaxed))) {
            info.sourceName = toUTF8StdString(ch->sourceName);
            const uint32_t idx = trapStmt_.load(std::memory_order_relaxed);
            if (ch->debugInfo && idx < ch->debugInfo->stmts.size())
                info.line = ch->debugInfo->stmts[idx].line;
        }
        std::lock_guard<std::mutex> lk(infoMutex_);
        if (reason == DebugStopReason::Exception)
            info.description = std::move(stagedFailure_);
        stopInfo_ = std::move(info);
    }
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    pendingWokenEpoch_.store(epoch, std::memory_order_release);
    openHoldAndWake(epoch, reason, trapThread_.load(std::memory_order_relaxed));
    // One pass now; a stop that cannot commit yet -- a thread still inside a
    // native call -- stays PENDING and the worker re-checks it every 2 ms
    // (advancePendingStop) until the last thread parks.  Never rolled back
    // for being slow: a skipped breakpoint is worse than a late one.
    // (tryCommitPass cancels preempted steps before publishing the stop.)
    std::string laggard;
    if (tryCommitPass(epoch, laggard))
        return;
    if (!forming_.load(std::memory_order_acquire) || currentEpoch() != epoch)
        return;   // withdrawn meanwhile
    {
        std::lock_guard<std::mutex> lk(infoMutex_);
        waitingFor_ = laggard;
    }
    pending_.store(true, std::memory_order_release);
}

std::optional<StopCoordinator::StopInfo> StopCoordinator::stoppedInfo()
{
    std::lock_guard<std::mutex> lk(infoMutex_);
    if (!stopped_.load(std::memory_order_acquire))
        return std::nullopt;
    return stopInfo_;
}

bool StopCoordinator::stepAndResume(Thread& t, Thread::DebugStepMode mode)
{
    std::lock_guard<std::recursive_mutex> control(controlMutex_);
    if (!stopped_.load(std::memory_order_acquire)
        || mode == Thread::DebugStepMode::None || t.frames.empty())
        return false;
    if ((owners_.load(std::memory_order_acquire) & ownerBit(StopOwner::Host)) != 0)
        return false;   // held by the host: no step may run until it releases
    // The target is parked/externally-stopped: its frames are safe to read.
    cancelStepIfArmed(t);   // re-arming replaces (and rebalances the demand)
    auto& fr = t.frames.back();
    if (fr.activationId == 0)
        fr.activationId = t.nextActivationId++;
    t.debugStepActivation = fr.activationId;
    t.debugStepOriginIndex = uint32_t(t.frames.size() - 1);
    t.debugStepMode = mode;
    // If the thread is parked exactly AT a statement boundary (a pause stop
    // can park anywhere; async stops already set this at publication), arm
    // the one-shot suppression so the step advances to the NEXT statement
    // instead of refiring at the one the user is looking at.
    Chunk* ch = asFunction(asClosure(fr.closure)->function)->chunk.get();
    if (ch && ch->debugInfo && !ch->debugInfo->stmts.empty()) {
        const auto off = uint32_t(fr.ip - ch->code.begin());
        const int32_t idx = debugLocateStatement(*ch->debugInfo, off);
        if (idx >= 0 && ch->debugInfo->stmts[idx].offset == off) {
            t.debugSuppressChunk = ch;
            t.debugSuppressStmt = uint32_t(idx);
            t.debugSuppressActivation = fr.activationId;
        }
    }
    addSlowPathDemand();   // released when the step fires or is cancelled
    resume(/*releaseHold=*/false);   // stepping never releases the motion hold
    // Tell the host the domain is running for the step (the generation stays
    // held).  Still under controlMutex_: the step's own stop -- or anything
    // preempting it -- re-asserts the hold in openHoldAndWake, which takes
    // the same mutex, so this notice always arrives first.
    if (!stopped_.load(std::memory_order_acquire)) {
        if (DebugHostControl* hc = hostControl_.load(std::memory_order_acquire))
            hc->onDebugStepResumed(
                { sessionId_, holdGeneration_.load(std::memory_order_acquire), t.id() });
    }
    return true;
}

void StopCoordinator::cancelStepIfArmed(Thread& t)
{
    if (t.debugStepMode == Thread::DebugStepMode::None)
        return;
    t.debugStepMode = Thread::DebugStepMode::None;
    t.debugStepActivation = 0;
    t.debugStepOriginIndex = 0;
    removeSlowPathDemand();   // the arm's paired demand
}

void StopCoordinator::cancelArmedStepsInEpoch()
{
    // Any stop that COMMITS consumes pending steps (DAP semantics: a
    // breakpoint or pause preempts an in-flight step; the step's own stop
    // already disarmed at publication, so this sweep finds nothing then).
    // Members are quiesced, so their step fields are safe to touch.
    forEachEpochThread([&](const ptr<Thread>& t) { cancelStepIfArmed(*t); });
}

void StopCoordinator::addSlowPathDemand()
{
    ensureWorker();   // non-RT callers only (breakpoint manager, step arm)
    if (slowPathDemand_.fetch_add(1, std::memory_order_acq_rel) == 0) {
        domain_->interrupts().fetch_or(ExecutionDomain::IntrDebugSlowPath);
        // Kick the worker into its armed-mode timed poll (it may be in an
        // indefinite idle wait): RT trap publications rely on that poll.
        if (DebugWorker* w = workerRaw_.load(std::memory_order_acquire))
            w->notifyRecheck();
    }
}

void StopCoordinator::removeSlowPathDemand()
{
    // RT-safe (atomics only): the boundary check calls this when a step
    // consumes itself.
    if (slowPathDemand_.fetch_sub(1, std::memory_order_acq_rel) == 1)
        domain_->interrupts().fetch_and(~uint32_t(ExecutionDomain::IntrDebugSlowPath));
}

void StopCoordinator::postToWorker(std::function<void()> fn)
{
    ensureWorker();
    if (DebugWorker* w = workerRaw_.load(std::memory_order_acquire))
        w->post(std::move(fn));
}

void StopCoordinator::ensureWorker()
{
    std::lock_guard<std::mutex> lk(workerMutex_);
    if (workerRetired_.load(std::memory_order_acquire))
        return;
    if (!worker_) {
        worker_ = std::make_unique<DebugWorker>(*this);
        workerRaw_.store(worker_.get(), std::memory_order_release);
    }
}

void StopCoordinator::shutdownWorker()
{
    std::lock_guard<std::mutex> lk(workerMutex_);
    // Permanent retirement latch: a transport command racing teardown must
    // not be able to construct a NEW worker against a torn-down VM.
    workerRetired_.store(true, std::memory_order_release);
    if (worker_) {
        workerRaw_.store(nullptr, std::memory_order_release);
        worker_->shutdownAndJoin();
        worker_.reset();
    }
}

bool StopCoordinator::admitOrPark(Thread& t, bool canPark)
{
    if (t.debugExcluded.load(std::memory_order_acquire)) {
        // Service thread: admitted straight through an active stop; it
        // manages its own ownership like the fast path.
        t.debugOwnership.store(Thread::DebugOwnership::Executing,
                               std::memory_order_release);
        return true;
    }
    for (;;) {
        // A thread inside a gate-counted dataflow section must finish its
        // bounded node work before parking -- it may hold the engine's eval
        // mutex, and parking with it held would wedge the engine thread's
        // own stop point.  The gate's active count keeps the epoch from
        // committing until the section exits.
        if (t.dataflowEvalDepth > 0)
            return true;
        if (t.debugOwnership.load(std::memory_order_acquire)
                == Thread::DebugOwnership::StoppedExternal) {
            if (!canPark) return false;
            ackAndPark(t);
            continue;   // released; retry admission
        }
        if (domain_->admissionClosed.load(std::memory_order_acquire)) {
            if (!canPark) { ackNoPark(t); return false; }
            ackAndPark(t);
            continue;
        }
        auto expect = Thread::DebugOwnership::Idle;
        if (t.debugOwnership.compare_exchange_strong(
                expect, Thread::DebugOwnership::Executing,
                std::memory_order_acq_rel))
            return true;
        if (expect == Thread::DebugOwnership::Executing)
            return true;   // already executing (re-entry before nesting count)
        // else StoppedExternal: loop
    }
}

} // namespace roxal
