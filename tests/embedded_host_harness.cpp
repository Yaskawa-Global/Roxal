// A standalone embedding host, linked against the public Roxal library.
//
// This is deliberately NOT a _runtests suite: those run from inside an outer
// Roxal script, which forces a harness to clear the synchronous-execution
// guard and mark the outer thread debug-excluded just to make itself work --
// arrangements no real host has to make, and which quietly weaken what the
// test proves.  Here the host owns the process, exactly as an embedding does.
//
// The roles mirror a real periodic host:
//
//   bootstrap/producer   builds a mock imported module and prelude receiver,
//                        attaches the runtime, prepares programs while the
//                        driver is already ticking, submits and waits
//   periodic driver      wakes on a cadence, spends part of its budget on
//                        mock host work, then calls driveFor() with the rest
//   debug controller     stops, inspects, steps, continues
//   host-control sink    records hold/release notices; never performs
//                        blocking machine control on the driver
//
// Correctness is asserted with counters and barriers.  Wall-clock time is
// used only as a generous deadlock guard, never as the thing under test:
// timing-dependent assertions are how a suite starts failing for reasons
// that have nothing to do with the contract.

#include "compiler/BuiltinModule.h"
#include "compiler/EmbeddedRuntime.h"
#include "compiler/Object.h"
#include "compiler/ReplSession.h"
#include "compiler/SimpleMarkSweepGC.h"
#include "compiler/ThreadManager.h"
#include "compiler/VM.h"
#include "compiler/debug/DebugHostControl.h"
#include "compiler/debug/DebugInspector.h"
#include "compiler/debug/StopCoordinator.h"
#include "core/Output.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace roxal;

namespace {

int g_failures = 0;
void expect(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

const char* runStateName(RunState s)
{
    switch (s) {
    case RunState::Queued:              return "Queued";
    case RunState::Running:             return "Running";
    case RunState::DebugPaused:         return "DebugPaused";
    case RunState::MainReturned:        return "MainReturned";
    case RunState::FinalizationPending: return "FinalizationPending";
    case RunState::Finalizing:          return "Finalizing";
    case RunState::Completed:           return "Completed";
    case RunState::Failed:              return "Failed";
    case RunState::Cancelled:           return "Cancelled";
    }
    return "?";
}

// A generous deadlock guard, not a timing assertion.
bool waitUntil(const std::function<bool()>& pred, int seconds = 60)
{
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Bounded, non-blocking output destination: a host with a deadline cannot
// afford one that waits.
class BoundedSink final : public OutputSink {
public:
    static constexpr size_t kCapacity = 32;

    OutputResult emit(const OutputEventView& event) override
    {
        if (event.kind != OutputKind::Print)
            return OutputResult::Accepted;   // diagnostics are not the payload
        std::lock_guard<std::mutex> lock(mutex_);
        if (records_.size() >= kCapacity) {
            ++dropped_;
            return OutputResult::Dropped;
        }
        records_.emplace_back(event.text);
        text_.append(event.text);
        return OutputResult::Accepted;
    }

    std::string take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = std::move(text_);
        text_.clear();
        records_.clear();
        return out;
    }

    size_t dropped() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> records_;
    std::string text_;
    size_t dropped_ { 0 };
};

// Records hold/release notices.  Every method is bounded and non-blocking:
// the real machine control a host would perform belongs on its own thread,
// reached through a queue, never inline here.
struct HostControlSink final : DebugHostControl {
    std::atomic<int> holds { 0 }, stops { 0 }, conts { 0 }, fails { 0 }, ends { 0 };
    std::atomic<uint64_t> lastHoldGen { 0 }, lastContGen { 0 };
    std::atomic<bool> calledOnDriver { false };
    std::atomic<bool> driverKnown { false };
    std::thread::id driverThread {};

    void note() noexcept
    {
        if (driverKnown.load(std::memory_order_acquire)
            && std::this_thread::get_id() == driverThread)
            calledOnDriver.store(true, std::memory_order_release);
    }
    DebugHostResult onDebugHoldRequested(const DebugHoldRequest& r) noexcept override
    {
        note();
        lastHoldGen.store(r.holdGeneration, std::memory_order_release);
        ++holds;
        return DebugHostResult::Queued;
    }
    void onDebugStopped(const DebugStopNotice&) noexcept override { note(); ++stops; }
    void onDebugContinueRequested(const DebugContinueNotice& c) noexcept override
    {
        note();
        lastContGen.store(c.holdGeneration, std::memory_order_release);
        ++conts;
    }
    void onDebugStopFailed(const DebugStopFailure&) noexcept override { note(); ++fails; }
    void onDebugSessionEnded(const DebugSessionEnd&) noexcept override { note(); ++ends; }
};

// Observes module lifecycle hooks so the harness can assert that a fresh
// program fires exactly one start/complete pair and a fragment fires none.
class HookProbe final : public BuiltinModule {
public:
    void registerBuiltins(VM&) override {}
    void onScriptStart(VM&) override
    {
        // Runs during activation, on whichever thread activated the run --
        // which is the only trustworthy way to observe where a body starts.
        startThread = std::this_thread::get_id();
        startThreadKnown.store(true, std::memory_order_release);
        ++starts;
    }
    void onScriptComplete(VM&) override
    {
        completeThread = std::this_thread::get_id();
        ++completes;
    }
    Value moduleType() const override { return Value::nilVal(); }

    std::atomic<int> starts { 0 };
    std::atomic<int> completes { 0 };
    std::atomic<bool> startThreadKnown { false };
    std::thread::id startThread {};
    std::thread::id completeThread {};
};

// The periodic driver: a host loop that spends part of each cycle on its own
// work and gives Roxal the remainder.
class PeriodicDriver {
public:
    PeriodicDriver(EmbeddedRuntime& runtime, TimeDuration budget)
        : runtime_(runtime), budget_(budget) {}

    void start()
    {
        thread_ = std::thread([this] {
            id_ = std::this_thread::get_id();
            idKnown_.store(true, std::memory_order_release);
            while (!stop_.load(std::memory_order_acquire)) {
                // Mock host work first, Roxal gets what is left -- the shape
                // a control host actually uses.
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                const auto sliceStart = std::chrono::steady_clock::now();
                // A real-time host brackets every GC-touching cycle in a
                // yield section: with a collection pending the section is
                // refused and the cycle does no Roxal work at all, rather
                // than entering a slice that would block on the barrier.
                SliceResult r;
                if (SimpleMarkSweepGC::GCYieldScope gcs{}; gcs) {
                    r = runtime_.driveFor(budget_);
                } else {
                    yieldedOut_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                const auto sliceUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - sliceStart).count();
                if (r.state == SliceState::DebugPaused) {
                    long long prev = maxPausedUs_.load(std::memory_order_relaxed);
                    while (sliceUs > prev
                           && !maxPausedUs_.compare_exchange_weak(prev, sliceUs,
                                                                  std::memory_order_relaxed)) {}
                }
                record(r);
                if (r.state == SliceState::ShuttingDown)
                    break;
            }
        });
    }

    void stop()
    {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

    std::thread::id id() const { return id_; }
    bool idKnown() const { return idKnown_.load(std::memory_order_acquire); }

    int count(SliceState s) const
    {
        return counts_[static_cast<size_t>(s)].load(std::memory_order_acquire);
    }
    // Worst slice observed while a debugger stop was active.  The claim is
    // that the debugger never PARKS the driver: a slice taken during a stop
    // comes straight back.  How long a running slice takes is a property of
    // the build, so it is measured and reported, never asserted.
    long long maxPausedSliceUs() const
    {
        return maxPausedUs_.load(std::memory_order_acquire);
    }
    // Cycles that skipped Roxal because a collection was pending.
    int yieldedOut() const { return yieldedOut_.load(std::memory_order_acquire); }
    int ended() const { return count(SliceState::ExecutionEnded); }
    int failed() const { return count(SliceState::ExecutionFailed); }
    int idle() const { return count(SliceState::Idle); }
    int paused() const { return count(SliceState::DebugPaused); }
    // Every thread that ever executed a body, as seen from the slice itself.
    std::thread::id executingThread() const { return executing_; }

private:
    void record(const SliceResult& r)
    {
        counts_[static_cast<size_t>(r.state)].fetch_add(1, std::memory_order_acq_rel);
        if (r.state == SliceState::Yielded || r.state == SliceState::ExecutionEnded)
            executing_ = std::this_thread::get_id();
    }

    EmbeddedRuntime& runtime_;
    TimeDuration budget_;
    std::thread thread_;
    std::atomic<bool> stop_ { false };
    std::atomic<bool> idKnown_ { false };
    std::thread::id id_ {};
    std::thread::id executing_ {};
    std::array<std::atomic<int>, 8> counts_ {};
    std::atomic<long long> maxPausedUs_ { 0 };
    std::atomic<int> yieldedOut_ { 0 };
};

PrepareProgramResult prepareOffDriver(VM& vm, const std::string& source,
                                      const std::string& name,
                                      std::vector<Value> imports = {},
                                      std::vector<PreludeCall> preludes = {})
{
    std::stringstream stream(source);
    ProgramOptions options;
    options.sourceName = name;
    options.imports = std::move(imports);
    options.preludes = std::move(preludes);
    return vm.prepareProgram(stream, std::move(options));
}

} // namespace

int main()
{
    VM& vm = VM::instance();
    BoundedSink sink;
    OutputRouter::setSink(&sink);

    ptr<HookProbe> hooks = make_ptr<HookProbe>();
    vm.registerBuiltinModule(hooks);

    HostControlSink host;
    vm.stopCoordinator().setHostControl(&host);

    AttachDriverResult attached = vm.attachEmbeddedRuntime();
    expect(attached.status == AttachStatus::Attached && attached.runtime,
           "the host attaches an embedded runtime");
    EmbeddedRuntime& runtime = *attached.runtime;

    PeriodicDriver driver(runtime, TimeDuration::microSecs(500));
    driver.start();
    expect(waitUntil([&] { return driver.idKnown(); }), "the driver thread starts");
    host.driverThread = driver.id();
    host.driverKnown.store(true, std::memory_order_release);

    // 1. The driver produces Idle slices before anything is submitted.
    expect(waitUntil([&] { return driver.idle() > 2; }),
           "the driver reports Idle before any submission");

    // 6. An imported module's names are visible without editing the source.
    Value importModule;
    {
        ScopedGCMutatorCover cover;
        importModule = Value::objVal(newModuleTypeObj(toUnicodeString("harness_import")));
        ObjModuleType::allModules.push_back(importModule);
        const ustring name = toUnicodeString("imported_base");
        asModuleType(importModule)->vars.store(name.hashCode(), name,
                                               Value::intVal(40), true);
    }

    // 2 + 3. Compilation happens off the driver, runs no user code, and the
    // driver stays callable throughout.
    const int idleBeforePrepare = driver.idle();
    const int startsBeforePrepare = hooks->starts.load();
    PrepareProgramResult prepared = prepareOffDriver(
        vm, "print('body ' + string(imported_base + 2))\n", "harness_program",
        { importModule });
    expect(prepared.status == PrepareStatus::Ready, "the producer prepares a program");
    expect(sink.take().empty(), "preparation executes no user code");
    expect(hooks->starts.load() == startsBeforePrepare,
           "preparation fires no script-start hook");
    expect(waitUntil([&] { return driver.idle() > idleBeforePrepare; }),
           "the driver keeps cycling while the producer prepares");

    SubmitResult submitted = runtime.submit(std::move(prepared.program));
    expect(submitted.status == SubmitStatus::Accepted, "the program is accepted");

    // 4 + 10. The body runs only on the bound driver, and the run ends once.
    expect(waitUntil([&] { return driver.ended() >= 1; }),
           "the driver runs the program to its end");
    expect(hooks->startThreadKnown.load(std::memory_order_acquire)
               && hooks->startThread == driver.id(),
           "activation happened on the bound driver thread");
    expect(hooks->startThread != std::this_thread::get_id(),
           "activation did NOT happen on the producer thread");
    expect(submitted.run.state() == RunState::FinalizationPending,
           "ExecutionEnded is not completion: the run awaits finalization");
    expect(sink.take() == "body 42\n",
           "imported names resolved without editing the source");

    // 10 (continued). Completed only after non-driver finalization.
    FinalizeResult finalized = submitted.run.wait();
    expect(finalized.status == FinalizeStatus::Finalized,
           "a non-driver waiter finalizes the run");
    expect(submitted.run.state() == RunState::Completed,
           "the run reaches Completed only after finalization");
    expect(hooks->completes.load() == startsBeforePrepare + 1,
           "the completion hook ran exactly once");
    expect(hooks->completeThread != driver.id(),
           "the completion hook ran off the driver, on the finalizing waiter");

    // 5. A prelude runs before the body, on the same Roxal thread, and on
    //    the driver -- not synchronously at submission time.
    {
        // The receiver is an ordinary object with a bytecode method, built in
        // a fragment session the way a host builds its bindings.
        Value receiver = Value::nilVal();
        {
            // With a driver attached, a fragment goes THROUGH the runtime --
            // synchronous evaluation is refused, because it would be a second
            // driver on the same VM.  This is also the path that proves a
            // fragment activates on its session's thread.
            std::stringstream setup(
                "type HarnessHook object:\n"
                "  proc arm():\n"
                "    print('prelude')\n"
                "var harness_hook = HarnessHook()\n");
            ReplSession session = vm.defaultReplSession();
            FragmentOptions fragmentOptions;
            fragmentOptions.sourceName = "harness_session";
            fragmentOptions.replMode = false;
            PrepareFragmentResult fragment =
                session.prepareFragment(setup, fragmentOptions);
            expect(fragment.status == PrepareFragmentStatus::Ready,
                   "the session fragment prepares while a driver owns execution");
            const int endedBeforeFragment = driver.ended();
            SubmitResult fragmentRun = runtime.submit(std::move(fragment.fragment));
            expect(fragmentRun.status == SubmitStatus::Accepted,
                   "a fragment is submitted through the same runtime");
            expect(waitUntil([&] { return driver.ended() > endedBeforeFragment; }),
                   "the driver runs the fragment");
            fragmentRun.run.wait();
            sink.take();
            if (ObjModuleType* mod = vm.replModuleType()) {
                auto found = mod->vars.load(toUnicodeString("harness_hook"));
                if (found.has_value())
                    receiver = found.value();
            }
        }
        expect(isObjectInstance(receiver), "the prelude receiver exists");
        sink.take();

        const int endedBefore = driver.ended();
        std::vector<PreludeCall> preludes;
        preludes.push_back(PreludeCall{ receiver, toUnicodeString("arm") });
        PrepareProgramResult withPrelude = prepareOffDriver(
            vm, "print('body')\n", "harness_prelude", {}, std::move(preludes));
        expect(withPrelude.status == PrepareStatus::Ready,
               "the prelude program prepared");
        expect(sink.take().empty(),
               "preparing a program does not run its prelude");

        SubmitResult run = runtime.submit(std::move(withPrelude.program));
        expect(run.status == SubmitStatus::Accepted, "the prelude program is accepted");
        expect(waitUntil([&] { return driver.ended() > endedBefore; }),
               "the prelude program runs to its end");
        run.run.wait();
        expect(sink.take() == "prelude\nbody\n",
               "the prelude ran before the body");
        expect(hooks->startThread == driver.id(),
               "the prelude and body ran on the driver's Roxal thread");
        {
            ScopedGCMutatorCover cover;
            receiver = Value::nilVal();
        }
    }

    // 7 + 8 + 9. A debug controller stops the program while the driver keeps
    //    cycling, inspects it, steps, and continues -- with the hold retained
    //    across the step and released exactly once by the continue.
    {
        StopCoordinator& coord = vm.stopCoordinator();
        const int endedBefore = driver.ended();
        const int holdsBefore = host.holds.load();
        const int contsBefore = host.conts.load();

        PrepareProgramResult longRun = prepareOffDriver(
            vm,
            "var total = 0\n"
            "for i in range(..<400000):\n"
            "  total = total + i\n"
            "print('debugged')\n",
            "harness_debugged");
        expect(longRun.status == PrepareStatus::Ready, "the debuggee prepared");
        SubmitResult run = runtime.submit(std::move(longRun.program));
        expect(run.status == SubmitStatus::Accepted, "the debuggee is accepted");
        expect(waitUntil([&] { return run.run.state() == RunState::Running; }),
               "the debuggee starts running");

        // The controller is its own thread: a host's debug client never runs
        // on the driver.
        bool stopped = false, inspected = false, steppedHeld = false;
        bool releasedOnce = false, keptCadence = false;
        // Reported on failure: which of the four inspection queries came back
        // empty says whether the stop landed somewhere unexpected or the
        // inspector itself is at fault.
        bool inspHadInfo = false, inspThreadListed = false;
        bool inspVars = false, inspEval = false;
        size_t inspFrameCount = 0;
        std::thread controller([&] {
            const int pausedBefore = driver.paused();
            auto outcome = coord.requestStop(DebugStopReason::Host,
                                             TimeDuration::milliSecs(5000));
            stopped = outcome.stopped && coord.isStopped();
            if (!stopped)
                return;

            // 7. The driver keeps cycling and reports DebugPaused promptly --
            //    asserted, not merely waited for.
            keptCadence = waitUntil([&] { return driver.paused() > pausedBefore; }, 20);

            // 8. Inspection over the stopped epoch.
            auto info = coord.stoppedInfo();
            inspHadInfo = info.has_value();
            if (info.has_value()) {
                DebugInspector insp(coord);
                bool threadListed = false;
                for (auto& t : insp.threads())
                    threadListed |= (t.threadId == info->threadId);
                auto frames = insp.stackTrace(info->threadId, 0, 0);
                inspThreadListed = threadListed;
                inspFrameCount = frames.size();
                bool varsOk = false, evalOk = false;
                if (!frames.empty()) {
                    for (auto& scope : insp.scopes(frames[0].handle)) {
                        for (auto& v : insp.variables(scope.variablesHandle, 0, 0))
                            if (v.name == "total") varsOk = true;
                    }
                    auto ev = insp.evaluate(frames[0].handle, "total");
                    evalOk = ev.ok;
                }
                inspVars = varsOk;
                inspEval = evalOk;
                inspected = threadListed && !frames.empty() && varsOk && evalOk;

                // 9. Step retains the hold: no continue notice, same
                //    generation.
                const int contsAtStep = host.conts.load();
                const uint64_t genAtStep = coord.holdGeneration();
                // The stopped thread lives in ITS RUN'S domain, not the
                // VM's default one: find it by id across every thread.
                ptr<Thread> target;
                for (auto& t : ThreadManager::instance().snapshotThreads())
                    if (t->id() == info->threadId) { target = t; break; }
                if (target) {
                    coord.stepAndResume(*target, Thread::DebugStepMode::Next);
                    waitUntil([&] { return coord.isStopped(); }, 10);
                    steppedHeld = coord.isStopped()
                               && host.conts.load() == contsAtStep
                               && coord.holdGeneration() == genAtStep
                               && coord.holdActive();
                }
            }

            // Ordinary continue releases the hold exactly once.
            const int contsAtResume = host.conts.load();
            if (coord.isStopped())
                coord.resume();
            releasedOnce = (host.conts.load() == contsAtResume + 1);
        });
        controller.join();

        expect(stopped, "the controller stops the driven program");
        expect(keptCadence,
               "the driver keeps its cadence while the program is stopped");
        expect(driver.maxPausedSliceUs() < 250000,
               "a slice taken during a stop returns promptly: the debugger "
               "never parks the driver");
        expect(inspected,
               "the stopped program yields threads, stack, scopes, vars, eval"
               " [stoppedInfo=" + std::to_string(inspHadInfo)
               + " thread=" + std::to_string(inspThreadListed)
               + " frames=" + std::to_string(inspFrameCount)
               + " vars=" + std::to_string(inspVars)
               + " eval=" + std::to_string(inspEval) + "]");
        expect(steppedHeld, "a step retains the hold and emits no continue notice");
        expect(releasedOnce, "an ordinary continue releases the hold exactly once");
        expect(host.holds.load() > holdsBefore, "the host was asked to hold");
        expect(host.conts.load() == contsBefore + 1,
               "exactly one release notice for the whole session");
        expect(!host.calledOnDriver.load(std::memory_order_acquire),
               "no hold notice was delivered on the driver thread");

        const bool finished = waitUntil([&] { return driver.ended() > endedBefore; });
        expect(finished,
               "the program finishes after the debugger releases it [ended="
               + std::to_string(driver.ended()) + " before="
               + std::to_string(endedBefore) + " state="
               + runStateName(run.run.state()) + "]");
        run.run.wait();
        expect(sink.take() == "debugged\n", "the debugged program completed");

        // A session ending while stopped notifies the host and releases
        // NOTHING: the hold is the host's to drop, and a debugger going away
        // must never be what restarts a machine.
        {
            PrepareProgramResult held = prepareOffDriver(
                vm,
                "var n = 0\n"
                "for i in range(..<400000):\n"
                "  n = n + 1\n",
                "harness_session_end");
            SubmitResult sessionRun = runtime.submit(std::move(held.program));
            expect(sessionRun.status == SubmitStatus::Accepted,
                   "the session-end program is accepted");
            expect(waitUntil([&] { return sessionRun.run.state() == RunState::Running; }),
                   "the session-end program runs");

            bool retained = false;
            std::thread ender([&] {
                auto outcome = coord.requestStop(DebugStopReason::Host,
                                                 TimeDuration::milliSecs(5000));
                if (!outcome.stopped)
                    return;
                const int endsBefore = host.ends.load();
                const int contsBefore2 = host.conts.load();
                coord.notifySessionEnd();
                retained = host.ends.load() == endsBefore + 1
                        && host.conts.load() == contsBefore2;
                coord.resume();
            });
            ender.join();
            expect(retained,
                   "a session ending while stopped notifies but releases no hold");
            sessionRun.run.wait();
            sink.take();
        }
    }

    // 17. A second driver is rejected deterministically.
    {
        std::atomic<int> foreignSlices { 0 };
        std::thread foreign([&] {
            for (int i = 0; i < 8; ++i) {
                const SliceResult r = runtime.driveFor(TimeDuration::microSecs(500));
                if (r.state != SliceState::Idle)
                    ++foreignSlices;
            }
        });
        foreign.join();
        expect(foreignSlices.load() == 0,
               "a second driver thread advances nothing");
    }

    // 11. A runtime error surfaces as ExecutionFailed for that run.  Until a
    //     finalizer has run, the handle says FinalizationPending with
    //     failed() set -- NOT Failed, which is a post-finalization state.  A
    //     losing finalizer that saw Failed early would return before the
    //     winner had begun cleanup.
    {
        const int failedBefore = driver.failed();
        PrepareProgramResult bad = prepareOffDriver(
            vm, "var xs = [1]\nprint(xs[5])\n", "harness_failing");
        expect(bad.status == PrepareStatus::Ready, "the failing program compiles");
        SubmitResult badRun = runtime.submit(std::move(bad.program));
        expect(badRun.status == SubmitStatus::Accepted, "the failing program is accepted");
        expect(waitUntil([&] { return driver.failed() > failedBefore; }),
               "a runtime error surfaces as ExecutionFailed");
        expect(waitUntil([&] { return badRun.run.state() == RunState::FinalizationPending; }, 10),
               "a failed run awaits finalization as FinalizationPending, not Failed");
        expect(badRun.run.failed(), "the failure is carried on the handle before finalization");
        expect(!badRun.run.diagnostic().empty(),
               "the handle says WHY it failed, from the handover on [" + badRun.run.diagnostic() + "]");
        FinalizeResult f = badRun.run.wait();
        expect(f.status == FinalizeStatus::Finalized, "this waiter performed the finalization");
        expect(f.state == RunState::Failed, "the failed run finalizes as Failed");
        expect(!badRun.run.diagnostic().empty(),
               "the diagnostic is durable on the handle after finalization");
        sink.take();
    }

    // 12. A rejected submission never executes later.
    {
        PrepareProgramResult first = prepareOffDriver(
            vm, "print('accepted')\n", "harness_accepted");
        PrepareProgramResult second = prepareOffDriver(
            vm, "print('rejected')\n", "harness_rejected");
        expect(first.status == PrepareStatus::Ready && second.status == PrepareStatus::Ready,
               "both programs prepared");
        SubmitResult a = runtime.submit(std::move(first.program));
        SubmitResult b = runtime.submit(std::move(second.program));
        expect(a.status == SubmitStatus::Accepted, "the first is accepted");
        expect(b.status == SubmitStatus::RunActive, "the second is rejected");
        expect(second.program.valid(), "a rejected submission stays with its producer");
        a.run.wait();
        // Drop the rejected program without ever submitting it.
        second.program.reset();
        expect(sink.take() == "accepted\n",
               "only the accepted program ever ran");
    }

    // 13. Concurrent producers: deterministic acceptance, no shared-state
    //     corruption, and every loser keeps its own program.
    {
        constexpr int kProducers = 6;
        std::vector<PrepareProgramResult> programs(kProducers);
        for (int i = 0; i < kProducers; ++i)
            programs[i] = prepareOffDriver(vm, "print('race')\n",
                                           "harness_race_" + std::to_string(i));
        std::vector<SubmitResult> results(kProducers);
        std::vector<std::thread> producers;
        for (int i = 0; i < kProducers; ++i)
            producers.emplace_back([&, i] {
                results[i] = runtime.submit(std::move(programs[i].program));
            });
        for (auto& t : producers) t.join();

        int accepted = 0;
        bool ownershipHeld = true;
        for (int i = 0; i < kProducers; ++i) {
            if (results[i].status == SubmitStatus::Accepted) {
                ++accepted;
                if (programs[i].program.valid()) ownershipHeld = false;
            } else if (!programs[i].program.valid()) {
                ownershipHeld = false;
            }
        }
        expect(accepted == 1, "exactly one concurrent producer is accepted");
        expect(ownershipHeld, "every rejected producer still owns its program");
        for (auto& r : results)
            if (r.status == SubmitStatus::Accepted)
                r.run.wait();
        sink.take();
    }

    // 16. A full output destination drops rather than blocking the driver.
    {
        std::string noisy = "for i in range(..<200):\n  print('n' + string(i))\n";
        PrepareProgramResult loud = prepareOffDriver(vm, noisy, "harness_noisy");
        expect(loud.status == PrepareStatus::Ready, "the noisy program compiles");
        SubmitResult run = runtime.submit(std::move(loud.program));
        expect(run.status == SubmitStatus::Accepted, "the noisy program is accepted");
        run.run.wait();
        expect(sink.dropped() > 0,
               "a full output destination drops instead of blocking");
        sink.take();
    }

    // 25. A collection requested while preparing is latched, not started, and
    //     runs once preparation is done.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        const std::uint64_t epochBefore = gc.currentEpoch();
        {
            SimpleMarkSweepGC::CollectionDeferralScope defer;
            gc.requestCollect();
            expect(!gc.isCollectionRequested(),
                   "a collection requested during preparation is latched");
            SimpleMarkSweepGC::GCYieldScope slice{};
            expect(static_cast<bool>(slice),
                   "a real-time slice still enters while the request is latched");
        }
        expect(waitUntil([&] { return gc.currentEpoch() != epochBefore; }),
               "the latched collection runs once preparation ends");
    }

    // 19 + 21. A prepared program's imported module and prelude receiver are
    //     reachable ONLY through its record, and survive a real collection;
    //     dropping the producer's handle after acceptance does not unroot it.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        const size_t rootsBefore = gc.persistentRootCount();
        PrepareProgramResult held = prepareOffDriver(
            vm, "print('survivor ' + string(imported_base))\n", "harness_survivor",
            { importModule });
        expect(held.status == PrepareStatus::Ready, "the survivor program prepared");
        expect(gc.persistentRootCount() == rootsBefore + 1,
               "a prepared program registers exactly one typed root");
        const PreparedExecutionRecord* address = held.program.record();

        const std::uint64_t epochBefore = gc.currentEpoch();
        gc.requestCollect();
        expect(waitUntil([&] { return gc.currentEpoch() != epochBefore; }),
               "a collection ran while the program was only prepared");

        SubmitResult run = runtime.submit(std::move(held.program));
        expect(run.status == SubmitStatus::Accepted, "the survivor program is accepted");
        expect(!held.program.valid(), "acceptance took the producer's handle");
        expect(gc.persistentRootCount() == rootsBefore + 1,
               "submission moved the record rather than re-registering it");
        (void)address;

        // Drop every external handle to VM-owned work; it must still run.
        {
            RunHandle copy = run.run;
            (void)copy;
        }
        expect(waitUntil([&] { return driver.ended() >= 2; }),
               "work whose producer handle is gone still runs");
        run.run.wait();
        expect(sink.take() == "survivor 40\n",
               "the survivor program produced its output after a collection");
        expect(gc.persistentRootCount() == rootsBefore,
               "finalization released the run's root");
    }

    // 18. Shutdown is bounded and leaves nothing live.
    driver.stop();
    vm.shutdownEmbeddedRuntime();
    expect(!vm.embeddedDriverAttached(), "the runtime detaches at shutdown");

    // 26. A Value-free external handle is safe after the runtime is gone.
    {
        PrepareProgramResult orphan = prepareOffDriver(
            vm, "print('never')\n", "harness_orphan");
        expect(orphan.status == PrepareStatus::Ready, "an orphan program prepared");
        orphan.program.reset();
        expect(sink.take().empty(), "a discarded program never ran");
    }

    vm.stopCoordinator().setHostControl(nullptr);
    {
        ScopedGCMutatorCover cover;
        importModule = Value::nilVal();
    }
    OutputRouter::setSink(nullptr);
    VM::shutdownIfConstructed();

    if (g_failures == 0)
        std::cout << "embedded host harness passed\n";
    return g_failures == 0 ? 0 : 1;
}
