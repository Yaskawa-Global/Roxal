#include "compiler/BuiltinModule.h"
#include "compiler/Object.h"
#include "compiler/ThreadManager.h"
#include "compiler/EmbeddedRuntime.h"
#include "compiler/SimpleMarkSweepGC.h"
#include "compiler/VM.h"
#include "core/Output.h"

#include <iostream>
#include <chrono>
#include <functional>
#include <cstdint>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Captures PROGRAM OUTPUT only.  The sink is process-wide, and background
// components emit diagnostics into it at moments this test does not control
// -- under a sanitizer the dataflow engine reports tick-period overruns, for
// instance.  These assertions are about what the program printed, so folding
// unrelated diagnostics into the same buffer would make them fail for reasons
// that have nothing to do with the contract under test.
class CaptureSink final : public roxal::OutputSink {
public:
    roxal::OutputResult emit(const roxal::OutputEventView& event) override
    {
        if (event.kind != roxal::OutputKind::Print)
            return roxal::OutputResult::Accepted;
        std::lock_guard<std::mutex> lock(mutex_);
        text_.append(event.text);
        return roxal::OutputResult::Accepted;
    }

    std::string take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string result = std::move(text_);
        text_.clear();
        return result;
    }

    // What has been printed so far, left in place.
    std::string peek()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }

private:
    std::mutex mutex_;
    std::string text_;
};

class LifecycleProbeModule final : public roxal::BuiltinModule {
public:
    bool hasModuleScript() const override { return false; }
    void registerBuiltins(roxal::VM&) override {}
    roxal::Value moduleType() const override { return roxal::Value::nilVal(); }

    void onScriptStart(roxal::VM&) override
    {
        starts.push_back(std::this_thread::get_id());
        // What the program had printed when its start hook fired: the test
        // for "preludes run before the hooks" reads this.
        if (sink)
            outputAtStart.push_back(sink->peek());
        if (throwOnStart)
            throw std::runtime_error("probe: start hook failed");
    }

    void onScriptComplete(roxal::VM&) override
    {
        completes.push_back(std::this_thread::get_id());
        if (throwOnComplete)
            throw std::runtime_error("probe: completion hook failed");
        int actors = 0;
        for (const auto& thread : roxal::ThreadManager::instance().snapshotThreads()) {
            if (thread->kind == roxal::ThreadKind::Actor)
                ++actors;
        }
        actorThreadsAtComplete.push_back(actors);
    }

    bool throwOnStart { false };
    bool throwOnComplete { false };
    CaptureSink* sink { nullptr };
    std::vector<std::string> outputAtStart;
    std::vector<std::thread::id> starts;
    std::vector<std::thread::id> completes;
    std::vector<int> actorThreadsAtComplete;
};

roxal::ExecutionStatus runProgram(roxal::VM& vm, const std::string& source,
                                  const std::string& name)
{
    std::stringstream stream(source);
    roxal::ProgramOptions options;
    options.sourceName = name;
    return vm.executeProgramSync(stream, std::move(options));
}

roxal::ExecutionStatus runFragment(roxal::VM& vm, const std::string& source)
{
    std::stringstream stream(source);
    return vm.defaultReplSession().evaluateFragmentSync(stream);
}

} // namespace

int main()
{
    using namespace roxal;

    int failures = 0;
    auto expect = [&](bool condition, const std::string& description) {
        if (!condition) {
            std::cerr << "FAIL: " << description << '\n';
            ++failures;
        }
    };

    VM& vm = VM::instance();
    CaptureSink sink;
    OutputRouter::setSink(&sink);

    ptr<LifecycleProbeModule> hooks = make_ptr<LifecycleProbeModule>();
    hooks->sink = &sink;
    vm.registerBuiltinModule(hooks);

    // A prelude is a Roxal object-instance bytecode method (the FC-shaped
    // sim.bind() case), is consumed once, and runs before the body. Build the
    // receiver in persistent fragment state so it can be inspected/reused by
    // this host-side characterization.
    expect(runFragment(vm,
                       "type PhaseAHook object:\n"
                       "  proc arm():\n"
                       "    print('prelude')\n"
                       "var phase_a_hook = PhaseAHook()\n") ==
               ExecutionStatus::OK,
           "prelude receiver bootstrap fragment completes");
    std::optional<Value> preludeValue;
    if (ObjModuleType* repl = vm.replModuleType())
        preludeValue = repl->vars.load(toUnicodeString("phase_a_hook"));
    expect(preludeValue.has_value() && isObjectInstance(*preludeValue),
           "prelude receiver is retained in fragment state");
    Value preludeReceiver = preludeValue.value_or(Value::nilVal());
    ExecutionStatus preludeStatus = ExecutionStatus::OK;
    {
        // Preludes are part of the launch, not ambient VM state waiting to be
        // picked up by whatever runs next.
        std::stringstream body("print('body')\n");
        ProgramOptions options;
        options.sourceName = "phase_a_prelude";
        options.preludes.push_back(PreludeCall{ preludeReceiver,
                                                toUnicodeString("arm") });
        preludeStatus = vm.executeProgramSync(body, std::move(options));
    }
    const std::string preludeOutput = sink.take();
    if (preludeStatus != ExecutionStatus::OK)
        std::cerr << "prelude status=" << static_cast<int>(preludeStatus)
                  << " output=" << preludeOutput << '\n';
    expect(preludeStatus == ExecutionStatus::OK,
           "synchronous prelude program completes");
    expect(preludeOutput == "prelude\nbody\n",
           "prelude executes before the program body");
    expect(runProgram(vm, "print('body2')\n", "phase_a_prelude_consumed") ==
               ExecutionStatus::OK,
           "second synchronous program completes");
    expect(sink.take() == "body2\n",
           "a launch's preludes belong to that launch alone");

    // A launch with explicit imports is still a fresh-program launch and must
    // take the same module start/complete path as one without.
    Value importModule;
    {
        ScopedGCMutatorCover cover;
        importModule = Value::objVal(newModuleTypeObj(
            toUnicodeString("phase_a_import")));
        ObjModuleType::allModules.push_back(importModule);
        const ustring injected = toUnicodeString("injected");
        asModuleType(importModule)->vars.store(
            injected.hashCode(), injected, Value::intVal(41), true);
    }
    {
        std::stringstream source("print(injected + 1)\n");
        ProgramOptions options;
        options.sourceName = "phase_a_import_program";
        options.imports = { importModule };
        expect(vm.executeProgramSync(source, std::move(options)) == ExecutionStatus::OK,
               "fresh program sees explicitly supplied imports");
    }
    expect(sink.take() == "42\n", "imported value is visible to compilation and execution");

    // A fresh program's completion hook runs only after its actor workers have
    // been joined.  Waiting for one result proves the actor started; it then
    // remains alive and idle until full-program teardown joins it.
    expect(runProgram(vm,
                      "type PhaseAWorker actor:\n"
                      "  func ping(x):\n"
                      "    return x + 1\n"
                      "var phase_a_worker = PhaseAWorker()\n"
                      "print(int(phase_a_worker.ping(41)))\n",
                      "phase_a_actor_lifetime") == ExecutionStatus::OK,
           "fresh program executes actor work");
    expect(sink.take() == "42\n", "actor result is delivered before completion");

    // Fresh programs do not share their module declarations.
    expect(runProgram(vm, "var phase_a_fresh = 7\n", "phase_a_fresh_one") ==
               ExecutionStatus::OK,
           "first fresh program completes");
    expect(runProgram(vm, "@strict\nprint(phase_a_fresh)\n",
                      "phase_a_fresh_two") ==
               ExecutionStatus::CompileError,
           "second fresh program cannot see the first program's declarations");
    sink.take(); // discard the expected compile diagnostic

    // In contrast, session fragments share compiler, module and session
    // Thread state, and they are not complete script launches for hooks.
    const size_t startsBeforeFragments = hooks->starts.size();
    const size_t completesBeforeFragments = hooks->completes.size();
    expect(runFragment(vm, "var phase_a_repl = 40\n") == ExecutionStatus::OK,
           "first persistent fragment completes");
    expect(runFragment(vm,
                       "phase_a_repl = phase_a_repl + 2\n"
                       "print(phase_a_repl)\n") == ExecutionStatus::OK,
           "second persistent fragment sees prior declarations");
    const std::string fragmentOutput = sink.take();
    // Interactive mode echoes the assignment result and the print call's nil
    // result in addition to print()'s own record.
    expect(fragmentOutput == "42\n42\nnil\n",
           "persistent fragment state and interactive echo are observable");
    expect(hooks->starts.size() == startsBeforeFragments &&
               hooks->completes.size() == completesBeforeFragments,
           "fragment evaluation does not fire full-program lifecycle hooks");

    expect(hooks->starts.size() == 5 && hooks->completes.size() == 5,
           "every full-program launch fires one start/complete hook pair");
    expect(hooks->starts == hooks->completes,
           "start and complete hooks retain executing-thread affinity");
    expect(hooks->actorThreadsAtComplete.size() == hooks->completes.size(),
           "completion hook records post-join thread state");
    for (int actors : hooks->actorThreadsAtComplete)
        expect(actors == 0, "actor threads are joined before onScriptComplete");

    // ---- Preparation is separate from activation -------------------------
    // A prepared program compiles and roots the launch without claiming a
    // thread, pushing a frame, or running a prelude.  These characterize the
    // split the embedded API is built on.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();

        // A failed compile yields diagnostics and no program, and does not
        // leave a launch half-built.
        {
            std::stringstream broken("var = = =\n");
            ProgramOptions options;
            options.sourceName = "phase_b_broken";
            PrepareProgramResult failed =
                vm.prepareProgram(broken, std::move(options));
            expect(failed.status == PrepareStatus::CompileError,
                   "a failed compile reports CompileError");
            expect(!failed.program.valid(),
                   "a failed compile yields no prepared program");
            sink.take(); // discard the expected compile diagnostic
        }

        // Moving a prepared program moves a POINTER.  Roots register by
        // address, so a move must not re-register: same record, same count.
        const size_t rootsBefore = gc.persistentRootCount();
        PreparedProgram moved;
        const PreparedExecutionRecord* recordAddress = nullptr;
        {
            std::stringstream source("print('prepared')\n");
            ProgramOptions options;
            options.sourceName = "phase_b_prepared";
            PrepareProgramResult prepared =
                vm.prepareProgram(source, std::move(options));
            expect(prepared.status == PrepareStatus::Ready,
                   "preparation succeeds without activating anything");
            expect(prepared.program.valid(), "a ready result carries a program");
            expect(sink.take().empty(),
                   "preparation runs no user code");
            recordAddress = prepared.program.record();
            expect(gc.persistentRootCount() == rootsBefore + 1,
                   "a prepared program registers exactly one typed root");
            moved = std::move(prepared.program);
        }
        expect(moved.record() == recordAddress,
               "moving a prepared program keeps the same record address");
        expect(gc.persistentRootCount() == rootsBefore + 1,
               "moving a prepared program does not re-register its root");

        // It survives its producer returning AND a real collection: the
        // record's typed root is the launch's only mark root, and a strong
        // refcount would not save an unreachable cycle.
        const std::uint64_t epochBefore = gc.currentEpoch();
        gc.requestCollect();
        for (int i = 0; i < 400 && gc.currentEpoch() == epochBefore; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        expect(gc.currentEpoch() != epochBefore,
               "a collection ran while a program was merely prepared");

        {
            AttachDriverResult driver = vm.attachEmbeddedRuntime();
            expect(driver.status == AttachStatus::Attached,
                   "a runtime attaches to run the survived program");
            SubmitResult submitted = driver.runtime->submit(std::move(moved));
            expect(submitted.status == SubmitStatus::Accepted,
                   "a program that survived a collection is still submittable");
            SliceState last = SliceState::Idle;
            const auto giveUp = std::chrono::steady_clock::now()
                                + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < giveUp) {
                last = driver.runtime->driveFor(TimeDuration::milliSecs(5)).state;
                if (last == SliceState::ExecutionEnded ||
                    last == SliceState::ExecutionFailed)
                    break;
            }
            expect(last == SliceState::ExecutionEnded,
                   "the program that survived a collection runs to completion");
            vm.shutdownEmbeddedRuntime();
        }
        expect(sink.take() == "prepared\n",
               "the program that ran is the one that was prepared");

        // Discarding a prepared program is a supported outcome: its root
        // unregisters and the compile garbage becomes collectable.
        {
            std::stringstream source("print('discarded')\n");
            ProgramOptions options;
            options.sourceName = "phase_b_discarded";
            PrepareProgramResult discarded =
                vm.prepareProgram(source, std::move(options));
            expect(discarded.status == PrepareStatus::Ready,
                   "the discarded program prepared successfully");
        }
        expect(gc.persistentRootCount() == rootsBefore,
               "discarding a prepared program unregisters its root");
        expect(sink.take().empty(), "a discarded program never runs");
    }

    // ---- gc() means collected, even while another thread is compiling ----
    // A collection requested while a preparation scope is open is latched,
    // not published.  gc() promises "collected" on return, so it must wait
    // for the scope to lift and the latched request to run -- returning at
    // once would hand a script a "collected" that never happened.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        const std::uint64_t epochBefore = gc.currentEpoch();
        std::atomic<bool> scopeHeld { false };
        std::thread compiler([&] {
            SimpleMarkSweepGC::CollectionDeferralScope defer;
            scopeHeld.store(true, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });
        while (!scopeHeld.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        expect(runProgram(vm, "gc()\n", "phase_j_gc_waits") == ExecutionStatus::OK,
               "a program calling gc() completes while another thread compiles");
        compiler.join();
        expect(gc.currentEpoch() != epochBefore,
               "gc() did not return until the deferred collection had run");
        sink.take();
    }

    // ---- Collection is deferred, not blocked, during preparation ---------
    // A published request makes every real-time GCYieldScope decline entry,
    // so a compiling host would stall its control slices.  Deferral latches
    // the request instead: slices keep entering, and the collection runs when
    // preparation ends.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        const std::uint64_t epochBefore = gc.currentEpoch();
        {
            SimpleMarkSweepGC::CollectionDeferralScope defer;
            gc.requestCollect();
            expect(!gc.isCollectionRequested(),
                   "a collection request is latched, not published, while deferred");
            expect(gc.collectionDeferred(),
                   "the latched request is observable");
            {
                SimpleMarkSweepGC::GCYieldScope slice{};
                expect(static_cast<bool>(slice),
                       "a real-time slice still enters while a request is latched");
            }
        }
        for (int i = 0; i < 400 && gc.currentEpoch() == epochBefore; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        expect(gc.currentEpoch() != epochBefore,
               "the latched collection runs once deferral ends");
        expect(!gc.collectionDeferred(),
               "the latch is cleared by publication");
    }

    // ---- Embedded driver ownership ---------------------------------------
    // Attaching hands execution ownership to a host loop.  From then on the
    // synchronous entry points refuse rather than racing the driver, exactly
    // one submission can be outstanding, rejection keeps the caller's
    // program, and only the bound driver can claim accepted work.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();

        AttachDriverResult attached = vm.attachEmbeddedRuntime();
        expect(attached.status == AttachStatus::Attached && attached.runtime,
               "an embedded runtime attaches");
        expect(vm.attachEmbeddedRuntime().status == AttachStatus::AlreadyAttached,
               "attachment is unique per VM");
        EmbeddedRuntime& runtime = *attached.runtime;

        // Synchronous execution is refused while a driver owns the VM.
        expect(runProgram(vm, "print('should not run')\n", "phase_c_refused") ==
                   ExecutionStatus::Busy,
               "run() refuses once a driver owns execution");
        expect(runFragment(vm, "1 + 1\n") == ExecutionStatus::Busy,
               "synchronous fragment evaluation refuses once a driver owns execution");
        expect(sink.take().empty(), "a refused synchronous launch runs nothing");

        auto prepare = [&](const char* text, const char* name) {
            std::stringstream source(text);
            ProgramOptions options;
            options.sourceName = name;
            return vm.prepareProgram(source, std::move(options));
        };

        // One run at a time: the second submission is rejected and its
        // program is STILL OWNED by the caller -- nothing the VM holds can
        // execute it later.
        PrepareProgramResult first = prepare("print('c1')\n", "phase_c_first");
        PrepareProgramResult second = prepare("print('c2')\n", "phase_c_second");
        expect(first.status == PrepareStatus::Ready &&
                   second.status == PrepareStatus::Ready,
               "both programs prepared");
        const PreparedExecutionRecord* secondRecord = second.program.record();

        SubmitResult accepted = runtime.submit(std::move(first.program));
        expect(accepted.status == SubmitStatus::Accepted,
               "the first submission is accepted");
        expect(accepted.run.valid() && accepted.run.id() != 0,
               "an accepted submission yields an identified run");
        expect(accepted.run.state() == RunState::Queued,
               "an accepted run is queued until the driver claims it");
        expect(!first.program.valid(),
               "acceptance takes ownership of the prepared program");

        SubmitResult rejected = runtime.submit(std::move(second.program));
        expect(rejected.status == SubmitStatus::RunActive,
               "a second submission is rejected while one is outstanding");
        expect(!rejected.run.valid(), "a rejected submission yields no run");
        expect(second.program.valid() &&
                   second.program.record() == secondRecord,
               "a rejected submission leaves the caller owning its program");

        // Only the bound driver may claim.  A non-driver thread asking must
        // not take the run.
        expect(runtime.hasPendingRun(), "the accepted run is pending");
        RunId claimedElsewhere = 1;
        std::thread other([&] { claimedElsewhere = runtime.claimPendingRun(); });
        other.join();
        expect(claimedElsewhere == accepted.run.id(),
               "the first claimer latches the driver role");
        expect(runtime.driverBound(), "the driver identity is latched");
        expect(!runtime.isDriverThread(),
               "this thread is not the driver after another latched it");
        expect(accepted.run.state() == RunState::Running,
               "a claimed run is running");
        expect(runtime.claimPendingRun() == 0,
               "a non-driver thread cannot claim work");

        // Cancellation is only meaningful before the claim.
        expect(runtime.cancelBeforeStart(accepted.run) ==
                   CancelStatus::AlreadyRunning,
               "a claimed run can no longer be cancelled before start");

        // Detaching is refused while a claimed run is in flight.
        expect(vm.detachEmbeddedRuntime() == DetachStatus::RunActive,
               "detach is refused while a run is active");

        // Dropping every external handle must not unroot accepted work: the
        // VM owns the record through the run, not the handle.
        const size_t rootsWithActive = gc.persistentRootCount();
        {
            RunHandle copy = accepted.run;
            expect(copy.id() == accepted.run.id(), "run handles are copyable");
        }
        expect(gc.persistentRootCount() == rootsWithActive,
               "dropping a run handle does not unroot the run");

        // Shutdown of the runtime cancels what it still owns and clears the
        // roots while the collector is alive.
        vm.shutdownEmbeddedRuntime();
        expect(accepted.run.state() == RunState::Cancelled,
               "shutdown cancels the run the runtime still owned");
        expect(!vm.embeddedDriverAttached(),
               "the VM reports no driver after detach");
        expect(gc.persistentRootCount() == rootsWithActive - 1,
               "the cancelled run's root is released");

        // Ownership is back: synchronous execution works again.
        expect(runProgram(vm, "print('c3')\n", "phase_c_after_detach") ==
                   ExecutionStatus::OK,
               "synchronous execution resumes after detach");
        expect(sink.take() == "c3\n", "the post-detach program ran");
        expect(sink.take().empty(), "no rejected or cancelled program ever ran");
    }

    // ---- Concurrent submitters -------------------------------------------
    // Exactly one submission may win; every loser keeps its own program.
    {
        AttachDriverResult attached = vm.attachEmbeddedRuntime();
        expect(attached.status == AttachStatus::Attached, "runtime re-attaches");
        EmbeddedRuntime& runtime = *attached.runtime;

        constexpr int kSubmitters = 8;
        std::vector<PrepareProgramResult> prepared;
        for (int i = 0; i < kSubmitters; ++i) {
            std::stringstream source("print('race')\n");
            ProgramOptions options;
            options.sourceName = "phase_c_race_" + std::to_string(i);
            prepared.push_back(vm.prepareProgram(source, std::move(options)));
        }

        std::vector<SubmitResult> results(kSubmitters);
        std::vector<std::thread> submitters;
        for (int i = 0; i < kSubmitters; ++i)
            submitters.emplace_back([&, i] {
                results[i] = runtime.submit(std::move(prepared[i].program));
            });
        for (auto& t : submitters)
            t.join();

        int acceptedCount = 0;
        bool losersKeptOwnership = true;
        for (int i = 0; i < kSubmitters; ++i) {
            if (results[i].status == SubmitStatus::Accepted) {
                ++acceptedCount;
                if (prepared[i].program.valid())
                    losersKeptOwnership = false;   // acceptance must take it
            } else if (!prepared[i].program.valid()) {
                losersKeptOwnership = false;       // rejection must leave it
            }
        }
        expect(acceptedCount == 1,
               "exactly one concurrent submission is accepted");
        expect(losersKeptOwnership,
               "every rejected submitter still owns its prepared program");

        vm.shutdownEmbeddedRuntime();
        expect(sink.take().empty(), "no raced submission executed");
    }

    // ---- The driver runs the program, in slices --------------------------
    // Phase D's whole point: a program prepared on one OS thread executes
    // only on a different, periodic driver thread -- including its prelude
    // and its first statement -- with every step bounded by the slice budget.
    {
        AttachDriverResult attached = vm.attachEmbeddedRuntime();
        expect(attached.status == AttachStatus::Attached, "runtime attaches for driving");
        EmbeddedRuntime& runtime = *attached.runtime;

        PrepareProgramResult prepared;
        {
            std::stringstream source(
                "var total = 0\n"
                "for i in range(..<200):\n"
                "  total = total + i\n"
                "print('driven ' + string(total))\n");
            ProgramOptions options;
            options.sourceName = "phase_d_driven";
            prepared = vm.prepareProgram(source, std::move(options));
        }
        expect(prepared.status == PrepareStatus::Ready, "the driven program prepared");
        // Prepared HERE; nothing of it has run on this thread.
        expect(sink.take().empty(), "preparation executed nothing");

        SubmitResult submitted = runtime.submit(std::move(prepared.program));
        expect(submitted.status == SubmitStatus::Accepted, "the program was accepted");

        // A periodic driver on its own thread, exactly as a host loop would:
        // a bounded slice, then the rest of the host's cycle.
        std::atomic<bool> ended { false };
        std::atomic<int> slices { 0 };
        std::atomic<int> pausedSlices { 0 };
        std::thread driver([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            for (;;) {
                SliceResult r = runtime.driveFor(TimeDuration::microSecs(500));
                ++slices;
                if (r.state == SliceState::DebugPaused) ++pausedSlices;
                if (r.state == SliceState::ExecutionEnded ||
                    r.state == SliceState::ExecutionFailed) {
                    ended.store(r.state == SliceState::ExecutionEnded,
                                std::memory_order_release);
                    break;
                }
                if (r.state == SliceState::ShuttingDown ||
                    std::chrono::steady_clock::now() > deadline)
                    break;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        driver.join();

        expect(ended.load(std::memory_order_acquire),
               "the driver ran the program to completion");
        expect(slices.load() > 1,
               "the program was advanced across multiple slices, not one call");
        expect(sink.take() == "driven 19900\n",
               "the program produced its output while driven");
        expect(submitted.run.state() == RunState::FinalizationPending,
               "execution end hands the run to the finalizer");

        // The handed-over run still owns its slot until a finalizer is done
        // with it: a second run reaching its own terminal transition first
        // would otherwise overwrite this record, and this run would report
        // Completed without its joins or completion hooks.  Detaching would
        // tear the record out from under the waiter the same way.
        {
            std::stringstream second("print('displacer')\n");
            ProgramOptions options;
            options.sourceName = "phase_d_displacer";
            PrepareProgramResult next = vm.prepareProgram(second, std::move(options));
            expect(next.status == PrepareStatus::Ready, "a second program prepares");
            SubmitResult refused = runtime.submit(std::move(next.program));
            expect(refused.status == SubmitStatus::RunActive,
                   "a run awaiting finalization still blocks the next submission");
            expect(next.program.valid(),
                   "the refused program is still the caller's");
            expect(vm.detachEmbeddedRuntime() == DetachStatus::RunActive,
                   "a run awaiting finalization cannot be detached out from under its waiter");
        }

        // Published exactly once: further slices find nothing to advance.
        SliceResult after = runtime.driveFor(TimeDuration::microSecs(500));
        expect(after.state == SliceState::Idle,
               "the terminal transition is published only once");

        // An idle driver is the steady state of every embedding that keeps a
        // VM around between programs, and it is called forever at the host's
        // control rate.  It must cost nothing that accumulates: an allocation
        // here buys the host a collection it never asked for, at a rate set
        // by its control period rather than by its workload.
        {
            SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
            const std::uint64_t bytesBefore = gc.bytesAllocatedSinceCollect();
            const auto started = std::chrono::steady_clock::now();
            constexpr int kIdleSlices = 2000;
            for (int i = 0; i < kIdleSlices; ++i) {
                SliceResult idle = runtime.driveFor(TimeDuration::microSecs(500));
                if (idle.state != SliceState::Idle) {
                    expect(false, "an idle driver reports Idle");
                    break;
                }
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;
            expect(gc.bytesAllocatedSinceCollect() == bytesBefore,
                   "an idle slice allocates nothing");
            // Generous by three orders of magnitude: this catches a slice that
            // started blocking or allocating, not microarchitectural drift.
            const auto perSlice =
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()
                / kIdleSlices;
            expect(perSlice < 50000,
                   "an idle slice returns without blocking");
        }

        vm.shutdownEmbeddedRuntime();
    }

    // Preludes run on the driver too, before the body, and under the same
    // slice budget -- not synchronously to completion at activation.
    {
        AttachDriverResult attached = vm.attachEmbeddedRuntime();
        EmbeddedRuntime& runtime = *attached.runtime;
        expect(attached.status == AttachStatus::Attached, "runtime attaches for preludes");

        // The prelude receiver lives in persistent fragment state, as the
        // synchronous prelude characterization above builds it.
        std::optional<Value> hook;
        if (ObjModuleType* repl = vm.replModuleType())
            hook = repl->vars.load(toUnicodeString("phase_a_hook"));
        expect(hook.has_value() && isObjectInstance(*hook),
               "the prelude receiver is still available");

        PrepareProgramResult prepared;
        {
            std::stringstream source("print('driven body')\n");
            ProgramOptions options;
            options.sourceName = "phase_d_prelude";
            options.preludes.push_back(PreludeCall{ hook.value_or(Value::nilVal()),
                                                    toUnicodeString("arm") });
            prepared = vm.prepareProgram(source, std::move(options));
        }
        expect(prepared.status == PrepareStatus::Ready, "the prelude program prepared");

        SubmitResult submitted = runtime.submit(std::move(prepared.program));
        expect(submitted.status == SubmitStatus::Accepted, "the prelude program was accepted");

        bool ok = false;
        std::thread driver([&] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            for (;;) {
                SliceResult r = runtime.driveFor(TimeDuration::microSecs(500));
                if (r.state == SliceState::ExecutionEnded) { ok = true; break; }
                if (r.state == SliceState::ExecutionFailed ||
                    r.state == SliceState::ShuttingDown ||
                    std::chrono::steady_clock::now() > deadline)
                    break;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        driver.join();

        expect(ok, "the driver ran a program with a prelude to completion");
        expect(sink.take() == "prelude\ndriven body\n",
               "the prelude ran on the driver, before the body");
        // Preludes are the host's pre-setup, run BEFORE the module start
        // hooks on every launch path: a hook sees the world the body will
        // see, and a debugger a hook arms never sees the plumbing.
        expect(!hooks->outputAtStart.empty()
                   && hooks->outputAtStart.back().find("prelude\n") != std::string::npos,
               "on the driver path the prelude has run when onScriptStart fires");

        vm.shutdownEmbeddedRuntime();
    }

    // ---- Completion and non-driver finalization --------------------------
    // Under the domain-quiescent policy the body returning is NOT completion:
    // the launch's own threads have to end too.  They do not end on their own
    // -- an actor waits for work until asked to quit -- so the quit-and-join
    // is unbounded and belongs to a non-driver finalizer, claimed exactly
    // once, never in a driver slice.
    {
        AttachDriverResult attached = vm.attachEmbeddedRuntime();
        expect(attached.status == AttachStatus::Attached, "runtime attaches for finalization");
        EmbeddedRuntime& runtime = *attached.runtime;

        const size_t hooksBefore = hooks->completes.size();

        PrepareProgramResult prepared;
        {
            std::stringstream source(
                "type PhaseEWorker actor:\n"
                "  func ping(x):\n"
                "    return x + 1\n"
                "var w = PhaseEWorker()\n"
                "print(int(w.ping(41)))\n");
            ProgramOptions options;
            options.sourceName = "phase_e_actor";
            prepared = vm.prepareProgram(source, std::move(options));
        }
        expect(prepared.status == PrepareStatus::Ready, "the actor program prepared");
        SubmitResult submitted = runtime.submit(std::move(prepared.program));
        expect(submitted.status == SubmitStatus::Accepted, "the actor program was accepted");

        // ONE driver for the runtime's life: the driver identity latches on
        // the first claim, so a second thread could never advance a later
        // run.  This mirrors how a host actually works -- one loop, many
        // programs over its lifetime.
        std::atomic<bool> sawMainReturned { false };
        std::atomic<int> handovers { 0 };
        std::atomic<bool> stopDriver { false };
        std::thread driver([&] {
            while (!stopDriver.load(std::memory_order_acquire)) {
                SliceResult r = runtime.driveFor(TimeDuration::microSecs(500));
                if (r.state == SliceState::MainReturned)
                    sawMainReturned.store(true, std::memory_order_release);
                if (r.state == SliceState::ExecutionEnded)
                    handovers.fetch_add(1, std::memory_order_acq_rel);
                if (r.state == SliceState::ShuttingDown)
                    break;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });

        const auto waitFor = [](const std::function<bool()>& pred) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (!pred() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return pred();
        };
        const bool handedOverOk = waitFor([&] {
            return handovers.load(std::memory_order_acquire) >= 1;
        });
        expect(handedOverOk, "the driver handed the run over");

        expect(sawMainReturned.load(std::memory_order_acquire),
               "the body returning is published as a non-terminal event");

        expect(submitted.run.state() == RunState::FinalizationPending,
               "the run awaits finalization, and is not yet complete");
        expect(sink.take() == "42\n", "the actor's result was delivered");
        // The driver stopped at the handover: completion hooks are the
        // finalizer's, not the slice's.
        expect(hooks->completes.size() == hooksBefore,
               "no completion hook ran in a driver slice");

        // Finalizing is exactly once, even with several waiters.
        constexpr int kWaiters = 4;
        std::vector<FinalizeResult> results(kWaiters);
        std::vector<std::thread> waiters;
        for (int i = 0; i < kWaiters; ++i)
            waiters.emplace_back([&, i] { results[i] = submitted.run.wait(); });
        for (auto& t : waiters) t.join();

        int performed = 0, observed = 0;
        for (const auto& r : results) {
            if (r.status == FinalizeStatus::Finalized) ++performed;
            else if (r.status == FinalizeStatus::AlreadyFinalized) ++observed;
        }
        expect(performed == 1, "exactly one waiter performs the finalization");
        expect(observed == kWaiters - 1, "the other waiters observe its result");
        expect(submitted.run.state() == RunState::Completed, "the run completes");
        expect(hooks->completes.size() == hooksBefore + 1,
               "the completion hook ran exactly once, in the finalizer");
        expect(hooks->actorThreadsAtComplete.back() == 0,
               "the launch's actor threads were joined before the hook");

        // Ready for another submission: nothing of the finished run is left
        // behind to contaminate it.
        PrepareProgramResult second;
        {
            std::stringstream source("print('second run')\n");
            ProgramOptions options;
            options.sourceName = "phase_e_second";
            second = vm.prepareProgram(source, std::move(options));
        }
        expect(second.status == PrepareStatus::Ready, "a second program prepares");
        SubmitResult again = runtime.submit(std::move(second.program));
        expect(again.status == SubmitStatus::Accepted,
               "the runtime accepts another run after finalization");

        // The SAME driver picks the new run up.
        const bool secondOk = waitFor([&] {
            return handovers.load(std::memory_order_acquire) >= 2;
        });
        expect(secondOk, "the same driver executes the next run");
        expect(again.run.wait().status == FinalizeStatus::Finalized,
               "the second run finalizes");
        expect(sink.take() == "second run\n", "the second run produced its output");

        stopDriver.store(true, std::memory_order_release);
        driver.join();
        vm.shutdownEmbeddedRuntime();
    }

    // ---- Persistent fragment sessions ------------------------------------
    // A fragment and a fresh program are different source lifetimes.  The
    // session owns the compiler, module and thread that fragments share; a
    // program gets none of it.
    {
        ReplSession session = vm.defaultReplSession();
        expect(session.valid(), "the VM has a default fragment session");

        {
            std::stringstream a("var phase_f_x = 5\n");
            expect(session.evaluateFragmentSync(a) == ExecutionStatus::OK,
                   "a fragment evaluates in its session");
        }
        {
            std::stringstream b("print(phase_f_x + 2)\n");
            expect(session.evaluateFragmentSync(b) == ExecutionStatus::OK,
                   "a later fragment sees what an earlier one declared");
        }
        // print() emits its line; interactive mode then echoes the call's own
        // nil result.
        expect(sink.take() == "7\nnil\n",
               "fragment state persists across fragments");

        expect(session.module().isNonNil(),
               "the session holds its persistent module as a rooted Value");

        // The exit criterion: a fresh program does NOT inherit session
        // lifetime, however much the session has accumulated.
        expect(runProgram(vm, "@strict\nprint(phase_f_x)\n", "phase_f_isolation") ==
                   ExecutionStatus::CompileError,
               "a fresh program cannot see the session's declarations");
        sink.take();   // discard the expected compile diagnostic

        // Preparation refuses rather than compiling against state a live
        // fragment is still mutating.
        std::stringstream busySource("var phase_f_busy = 1\n");
        PrepareFragmentResult held = session.prepareFragment(busySource);
        expect(held.status == PrepareFragmentStatus::Ready,
               "a fragment prepares while the session is idle");
        {
            std::stringstream second("var phase_f_other = 2\n");
            PrepareFragmentResult refused = session.prepareFragment(second);
            expect(refused.status == PrepareFragmentStatus::SessionBusy,
                   "a second fragment is refused while one is in flight");
            expect(!refused.fragment.valid(),
                   "a refused fragment yields nothing to execute");
        }
        // Discarding the held fragment must free the session again -- by
        // itself: the claim belongs to the record, and dies with it.
        held.fragment.reset();
        {
            std::stringstream third("var phase_f_after = 3\n");
            PrepareFragmentResult ok = session.prepareFragment(third);
            expect(ok.status == PrepareFragmentStatus::Ready,
                   "the session accepts a fragment again once idle");
        }
    }

    // --- Preparation leaves no GC bookkeeping behind ---------------------
    //
    // The root accounting is asserted above.  What is left is the DEFERRAL
    // preparation takes while it compiles: it is invisible in every status a
    // host gets back, and leaking one silently disables tracing collection
    // for the life of the process.  Both outcomes must release it.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        expect(!gc.collectionDeferralActive(),
               "no collection deferral is active at the baseline");
        const size_t baselineRoots = gc.persistentRootCount();

        {
            std::stringstream src("print('phase_i_ok')\n");
            ProgramOptions options;
            options.sourceName = "phase_i_ok";
            expect(vm.stageProgramSync(src, std::move(options)) == ExecutionStatus::OK,
                   "a valid program stages");
            expect(vm.executeStagedSync() == ExecutionStatus::OK,
                   "the staged program runs to completion");
            expect(sink.take() == "phase_i_ok\n",
                   "the prepared program produced its output");
        }
        expect(!gc.collectionDeferralActive(),
               "a successful preparation releases its collection deferral");
        expect(gc.persistentRootCount() == baselineRoots,
               "a completed program releases its root");

        {
            std::stringstream src("var = = =\n");
            ProgramOptions options;
            options.sourceName = "phase_i_bad";
            PrepareProgramResult bad = vm.prepareProgram(src, std::move(options));
            expect(bad.status == PrepareStatus::CompileError,
                   "an invalid program fails to prepare");
            sink.take();   // discard the expected compile diagnostic
        }
        expect(!gc.collectionDeferralActive(),
               "a failed preparation releases its collection deferral");
        expect(gc.persistentRootCount() == baselineRoots,
               "a failed preparation leaves no root behind");
    }

    // --- Failures in the launch machinery leave nothing behind ---------
    //
    // A prelude that raises, a start hook that throws, a completion hook that
    // throws: each is a failure of the launch rather than of the program, and
    // each must leave the VM able to run the next one.  The failure modes
    // guarded against are a leaked thread, roots dropped without cover, a
    // start hook re-run on every slice, and a run parked in Finalizing
    // forever.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        const size_t rootsBefore = gc.persistentRootCount();

        // A raising prelude, on the synchronous path.
        expect(runFragment(vm,
                           "type PhaseJHook object:\n"
                           "  proc boom():\n"
                           "    undefined_fn_phase_j()\n"
                           "var phase_j_hook = PhaseJHook()\n") == ExecutionStatus::OK,
               "the raising prelude receiver is defined");
        std::optional<Value> boomHook;
        if (ObjModuleType* repl = vm.replModuleType())
            boomHook = repl->vars.load(toUnicodeString("phase_j_hook"));
        expect(boomHook.has_value(), "the raising prelude receiver is retained");
        {
            std::stringstream src("print('body must not run')\n");
            ProgramOptions options;
            options.sourceName = "phase_j_bad_prelude";
            options.preludes.push_back(PreludeCall{ boomHook.value_or(Value::nilVal()),
                                                    toUnicodeString("boom") });
            expect(vm.stageProgramSync(src, std::move(options)) == ExecutionStatus::RuntimeError,
                   "a raising prelude fails the launch");
            sink.take();   // discard the runtime diagnostic
        }
        expect(gc.persistentRootCount() == rootsBefore,
               "a failed prelude releases the launch's root");
        expect(runProgram(vm, "print('after prelude failure')\n", "phase_j_after") ==
                   ExecutionStatus::OK,
               "the VM runs the next program after a prelude failure");
        expect(sink.take() == "after prelude failure\n",
               "the next program's output is intact");

        // Hooks that throw, on the driver path.
        AttachDriverResult attached = vm.attachEmbeddedRuntime();
        expect(attached.status == AttachStatus::Attached, "runtime attaches for the hook tests");
        EmbeddedRuntime& runtime = *attached.runtime;
        std::atomic<bool> stopDriver { false };
        std::thread driver([&] {
            while (!stopDriver.load(std::memory_order_acquire)) {
                const SliceResult r = runtime.driveFor(TimeDuration::microSecs(500));
                if (r.state == SliceState::Idle)
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        auto submitAndHandOver = [&](const char* text, const char* name) -> RunHandle {
            std::stringstream src(text);
            ProgramOptions options;
            options.sourceName = name;
            PrepareProgramResult prepared = vm.prepareProgram(src, std::move(options));
            expect(prepared.status == PrepareStatus::Ready, "the hook-test program prepares");
            SubmitResult submitted = runtime.submit(std::move(prepared.program));
            expect(submitted.status == SubmitStatus::Accepted, "the hook-test program is accepted");
            const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (submitted.run.state() != RunState::FinalizationPending
                   && std::chrono::steady_clock::now() < giveUp)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return submitted.run;
        };

        hooks->throwOnStart = true;
        {
            RunHandle run = submitAndHandOver("print('never')\n", "phase_j_start_throws");
            expect(run.state() == RunState::FinalizationPending && run.failed(),
                   "a throwing start hook fails the run and hands it over");
            const FinalizeResult f = run.wait();
            expect(f.status == FinalizeStatus::Finalized && f.state == RunState::Failed,
                   "a run failed by its start hook finalizes as Failed");
            expect(sink.take().empty(), "the body never ran after the start hook failed");
        }
        hooks->throwOnStart = false;
        {
            RunHandle run = submitAndHandOver("print('recovered')\n", "phase_j_recovered");
            expect(run.state() == RunState::FinalizationPending && !run.failed(),
                   "the runtime accepts and runs the next program");
            expect(run.wait().state == RunState::Completed, "it completes normally");
            expect(sink.take() == "recovered\n", "and its output is intact");
        }
        hooks->throwOnComplete = true;
        {
            RunHandle run = submitAndHandOver("print('completes')\n", "phase_j_complete_throws");
            expect(run.state() == RunState::FinalizationPending && !run.failed(),
                   "execution itself succeeded");
            const FinalizeResult f = run.wait();
            expect(f.status == FinalizeStatus::Finalized,
                   "a throwing completion hook does not leave the run in Finalizing");
            expect(f.state == RunState::Failed,
                   "a throwing completion hook is reported as a failed finalization");
            sink.take();
        }
        hooks->throwOnComplete = false;

        // The outcome outlives the run as text on the handle: the terminal
        // value rendered, and for a failure the reason.  Neither holds a
        // Value, so both stay readable after the VM is gone.
        {
            RunHandle run = submitAndHandOver("var x = 40\nreturn x + 2\n", "phase_j_result");
            expect(run.wait().state == RunState::Completed, "the returning program completes");
            expect(run.result() == "42",
                   "the handle carries the terminal value, rendered [" + run.result() + "]");
        }
        {
            RunHandle run = submitAndHandOver("var xs = [1]\nprint(xs[5])\n", "phase_j_why");
            expect(run.failed() && !run.diagnostic().empty(),
                   "a failed run's handle says why before finalization [" + run.diagnostic() + "]");
            expect(run.wait().state == RunState::Failed, "and finalizes as Failed");
            expect(!run.diagnostic().empty(), "the reason is durable after finalization");
            sink.take();
        }
        stopDriver.store(true, std::memory_order_release);
        driver.join();
        vm.shutdownEmbeddedRuntime();
        expect(gc.persistentRootCount() == rootsBefore,
               "every hook-failure path released its roots");
    }

    // --- Shutdown cannot free what someone is still inside ---------------
    //
    // Two races the lifecycle must survive.  A host holding a RunHandle may
    // call wait() at the same moment the VM shuts the runtime down; and a
    // driver may be inside a slice at that moment.  Neither may dereference
    // a freed runtime or a freed record, and neither may hang: a waiter that
    // can never be satisfied would hold its lease on the runtime forever.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();
        const size_t rootsBefore = gc.persistentRootCount();

        // 1. wait() racing shutdown on a handed-over run.
        {
            AttachDriverResult attached = vm.attachEmbeddedRuntime();
            expect(attached.status == AttachStatus::Attached, "runtime attaches for the wait race");
            EmbeddedRuntime& runtime = *attached.runtime;
            std::stringstream src("print('handed over')\n");
            ProgramOptions options;
            options.sourceName = "phase_j_wait_race";
            PrepareProgramResult prepared = vm.prepareProgram(src, std::move(options));
            SubmitResult submitted = runtime.submit(std::move(prepared.program));
            expect(submitted.status == SubmitStatus::Accepted, "the race program is accepted");
            std::thread driver([&] {
                const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (std::chrono::steady_clock::now() < giveUp) {
                    const SliceState st = runtime.driveFor(TimeDuration::milliSecs(5)).state;
                    if (st == SliceState::ExecutionEnded || st == SliceState::ExecutionFailed
                        || st == SliceState::ShuttingDown)
                        break;
                }
            });
            driver.join();
            expect(submitted.run.state() == RunState::FinalizationPending,
                   "the race program is handed over");
            // The waiter and the shutdown start as close together as a
            // thread launch allows; whichever wins, the waiter must return.
            std::atomic<bool> waiterReturned { false };
            FinalizeResult outcome;
            std::thread waiter([&] {
                outcome = submitted.run.wait();
                waiterReturned.store(true, std::memory_order_release);
            });
            vm.shutdownEmbeddedRuntime();
            waiter.join();
            expect(waiterReturned.load(std::memory_order_acquire),
                   "a waiter racing shutdown returns rather than hanging on its lease");
            const RunState finalState = submitted.run.state();
            expect(finalState == RunState::Completed || finalState == RunState::Cancelled,
                   "the raced run ends Completed or Cancelled, never stuck [state="
                   + std::to_string(static_cast<int>(finalState)) + "]");
            expect(outcome.status == FinalizeStatus::Finalized
                       || outcome.status == FinalizeStatus::AlreadyFinalized
                       || outcome.status == FinalizeStatus::RuntimeGone,
                   "the waiter's outcome is one of the three the contract allows [status="
                   + std::to_string(static_cast<int>(outcome.status)) + "]");
            sink.take();
        }

        // 2. Shutdown while the driver is inside a slice.
        {
            AttachDriverResult attached = vm.attachEmbeddedRuntime();
            expect(attached.status == AttachStatus::Attached, "runtime attaches for the slice race");
            EmbeddedRuntime& runtime = *attached.runtime;
            std::stringstream src(
                "var n = 0\n"
                "while n < 100000000:\n"
                "  n = n + 1\n"
                "print(n)\n");
            ProgramOptions options;
            options.sourceName = "phase_j_slice_race";
            PrepareProgramResult prepared = vm.prepareProgram(src, std::move(options));
            SubmitResult submitted = runtime.submit(std::move(prepared.program));
            expect(submitted.status == SubmitStatus::Accepted, "the long program is accepted");
            std::atomic<bool> sawShuttingDown { false };
            std::atomic<int> slices { 0 };
            std::thread driver([&] {
                const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                while (std::chrono::steady_clock::now() < giveUp) {
                    const SliceState st = runtime.driveFor(TimeDuration::milliSecs(2)).state;
                    ++slices;
                    if (st == SliceState::ShuttingDown) { sawShuttingDown.store(true); break; }
                    if (st == SliceState::ExecutionEnded || st == SliceState::ExecutionFailed)
                        break;
                }
            });
            // Let it get properly under way, then pull the runtime out while
            // slices are in flight.  The destructor must wait out the slice
            // it may be inside before cancelling the record that slice holds.
            while (slices.load() < 20)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            vm.shutdownEmbeddedRuntime();
            driver.join();
            expect(sawShuttingDown.load(),
                   "the driver observes ShuttingDown once the runtime is gone");
            expect(submitted.run.state() == RunState::Cancelled,
                   "a run interrupted by shutdown is Cancelled");
            sink.take();
        }
        expect(gc.persistentRootCount() == rootsBefore,
               "both shutdown races released their roots");
    }

    // --- Execution domains ------------------------------------------------
    //
    // A program run owns its threads, its error state and its exit; the REPL
    // session owns its own; the services own theirs.  Finalizing a run joins
    // the run's domain and nothing else.
    {
        SimpleMarkSweepGC& gc = SimpleMarkSweepGC::instance();

        // 1. The motivating case: a session actor survives a program run.
        expect(runFragment(vm,
                           "type PhaseJKeeper actor:\n"
                           "  func alive():\n"
                           "    return 42\n"
                           "var phase_j_keeper = PhaseJKeeper()\n"
                           "print(int(phase_j_keeper.alive()))\n") == ExecutionStatus::OK,
               "a session actor is created by a fragment");
        expect(sink.take() == "42\nnil\n", "the session actor answers before the run");

        AttachDriverResult attached = vm.attachEmbeddedRuntime();
        expect(attached.status == AttachStatus::Attached, "runtime attaches for the domain tests");
        EmbeddedRuntime& runtime = *attached.runtime;
        std::atomic<bool> stopDriver { false };
        std::thread driver([&] {
            while (!stopDriver.load(std::memory_order_acquire)) {
                const SliceResult r = runtime.driveFor(TimeDuration::microSecs(500));
                if (r.state == SliceState::Idle)
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        auto runToHandover = [&](const char* text, const char* name) -> RunHandle {
            std::stringstream src(text);
            ProgramOptions options;
            options.sourceName = name;
            PrepareProgramResult prepared = vm.prepareProgram(src, std::move(options));
            expect(prepared.status == PrepareStatus::Ready, std::string("prepares: ") + name);
            SubmitResult submitted = runtime.submit(std::move(prepared.program));
            expect(submitted.status == SubmitStatus::Accepted, std::string("accepted: ") + name);
            const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (submitted.run.state() != RunState::FinalizationPending
                   && std::chrono::steady_clock::now() < giveUp)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return submitted.run;
        };

        const size_t modulesBefore = ObjModuleType::allModules.size();
        {
            RunHandle run = runToHandover("print('between')\n", "phase_j_between");
            expect(run.wait().state == RunState::Completed, "a program runs and is finalized");
            expect(sink.take() == "between\n", "its output is intact");
        }
        // 4. Retirement: the finalized run's module is no longer pinned.
        expect(ObjModuleType::allModules.size() == modulesBefore,
               "finalizing a run retires its module from the global pin list ["
               + std::to_string(modulesBefore) + " -> "
               + std::to_string(ObjModuleType::allModules.size()) + "]");

        // 2. Error isolation: a failing run marks nothing else failed.
        {
            RunHandle bad = runToHandover("var xs = [1]\nprint(xs[9])\n", "phase_j_failing");
            expect(bad.failed(), "the failing run failed");
            expect(bad.wait().state == RunState::Failed, "and finalizes as Failed");
            sink.take();
            RunHandle good = runToHandover("print('unaffected')\n", "phase_j_unaffected");
            expect(!good.failed() && good.wait().state == RunState::Completed,
                   "the next run is not marked failed by the previous one");
            expect(sink.take() == "unaffected\n", "and runs normally");
        }

        // 3. exit() in a driven program ends that run, nothing else.
        {
            RunHandle run = runToHandover("print('before')\nexit(7)\nprint('after')\n", "phase_j_exit");
            expect(!run.failed(), "exit() is not a failure");
            const FinalizeResult f = run.wait();
            expect(f.state == RunState::Completed, "an exited run finalizes as Completed");
            expect(run.exitCode() == 7, "the exit code is on the handle [" + std::to_string(run.exitCode()) + "]");
            expect(sink.take() == "before\n", "nothing after exit() ran");
            RunHandle next = runToHandover("print('still here')\n", "phase_j_after_exit");
            expect(next.wait().state == RunState::Completed,
                   "the runtime, the driver and the engine survive a program's exit()");
            expect(sink.take() == "still here\n", "the next program runs after an exit()");
        }

        stopDriver.store(true, std::memory_order_release);
        driver.join();
        vm.shutdownEmbeddedRuntime();

        // 1 (continued): the session actor is still alive and answering.
        expect(runFragment(vm, "print(int(phase_j_keeper.alive()))\n") == ExecutionStatus::OK,
               "the session actor is reachable after program runs were finalized");
        expect(sink.take() == "42\nnil\n",
               "the session actor survived every run's finalization");
        (void)gc;
    }

    {
        ScopedGCMutatorCover cover;
        preludeValue.reset();
        preludeReceiver = Value::nilVal();
        importModule = Value::nilVal();
    }
    OutputRouter::setSink(nullptr);

    const size_t rootsBeforeShutdown =
        SimpleMarkSweepGC::instance().persistentRootCount();
    VM::shutdownIfConstructed();

    // Preparation is the long, off-driver step, so it is the one most likely
    // to be entered while the VM is going away.  It must refuse before taking
    // a deferral or allocating a record.
    {
        std::stringstream src("print('phase_i_after_shutdown')\n");
        ProgramOptions options;
        options.sourceName = "phase_i_after_shutdown";
        PrepareProgramResult late = vm.prepareProgram(src, std::move(options));
        expect(late.status == PrepareStatus::ShuttingDown,
               "preparation refuses once the VM has shut down");
        expect(!late.program.valid(),
               "a refused preparation yields nothing to execute");
        expect(!SimpleMarkSweepGC::instance().collectionDeferralActive(),
               "a refused preparation takes no collection deferral");
        expect(SimpleMarkSweepGC::instance().persistentRootCount() <= rootsBeforeShutdown,
               "a refused preparation registers no root");
    }

    if (failures == 0)
        std::cout << "embedded execution contract tests passed\n";
    return failures == 0 ? 0 : 1;
}
