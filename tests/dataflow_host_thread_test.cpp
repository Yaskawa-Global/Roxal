// Host-driven dataflow evaluated from the program's driver thread.
//
// An embedding host that drives both the dataflow engine (tickFor) and a
// program (driveFor) from ONE OS thread -- the usual shape of a control loop
// -- must see the same results a standalone run gives.  Every Roxal-bodied
// node that tickFor evaluates runs in the engine's own execution context,
// never on the program's Roxal thread, whatever binding the program's slices
// left on the driver (the run's thread while it runs, none after it ends).
//
// One scenario per process, named by argv[1]: the engine is a process
// singleton and the nodes a finished run created stay registered, so
// scenarios sharing a process would evaluate each other's nodes.
//
// As in the embedded host harness, wall-clock time is only a deadlock guard.
// What is asserted is the program's own result, and that the cycles really
// did interleave node evaluation with the program's slices.

#include "compiler/CallFrame.h"
#include "compiler/EmbeddedRuntime.h"
#include "compiler/SimpleMarkSweepGC.h"
#include "compiler/Thread.h"
#include "compiler/ThreadManager.h"
#include "compiler/VM.h"
#include "core/Output.h"
#include "dataflow/DataflowEngine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace roxal;
using TickResult = df::DataflowEngine::TickResult;

namespace {

int g_failures = 0;
void expect(bool condition, const std::string& what)
{
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++g_failures;
    }
}

// A generous deadlock guard, not a timing assertion.
bool waitUntil(const std::function<bool()>& pred, int seconds = 60)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

class CaptureSink final : public OutputSink {
public:
    OutputResult emit(const OutputEventView& event) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event.kind == OutputKind::Print)
            text_.append(event.text);
        else
            diagnostics_.append(event.text);
        return OutputResult::Accepted;
    }
    std::string text()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }
    std::string diagnostics()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return diagnostics_;
    }

private:
    std::mutex mutex_;
    std::string text_;
    std::string diagnostics_;
};

// FC's cycle, reduced: mock host work, then -- inside one GC yield section,
// as a real-time host must -- tick the engine, then give the program the
// rest.  tickFor does not gate on wall-clock time (pacing is the host's
// choice); this host ticks every cycle, so node evaluation interleaves with
// as many program slices as possible.  Like a host that tolerates lateness,
// it treats Overrun as "this tick was late" and carries on.
class Driver {
public:
    Driver(EmbeddedRuntime& runtime, TimeDuration tickBudget, TimeDuration driveBudget,
           bool tickAfterRun)
        : runtime_(runtime), tickBudget_(tickBudget), driveBudget_(driveBudget),
          tickAfterRun_(tickAfterRun) {}

    void start()
    {
        thread_ = std::thread([this] {
            auto engine = df::DataflowEngine::instance();
            while (!stop_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                SimpleMarkSweepGC::GCYieldScope section{};
                if (!section)
                    continue;   // collection pending: skip the whole cycle
                const bool afterRun = runHandedOver_.load(std::memory_order_acquire);
                if (!afterRun || tickAfterRun_)
                    record(engine->tickFor(tickBudget_), afterRun);
                const SliceResult slice = runtime_.driveFor(driveBudget_);
                if (slice.state == SliceState::ExecutionEnded
                    || slice.state == SliceState::ExecutionFailed)
                    runHandedOver_.store(true, std::memory_order_release);
                if (slice.state == SliceState::ShuttingDown)
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

    int during(TickResult r) const { return during_[index(r)].load(std::memory_order_relaxed); }
    int after(TickResult r) const { return after_[index(r)].load(std::memory_order_relaxed); }
    int afterTotal() const
    {
        int total = 0;
        for (const auto& c : after_)
            total += c.load(std::memory_order_relaxed);
        return total;
    }

private:
    static size_t index(TickResult r) { return static_cast<size_t>(r); }
    void record(TickResult r, bool afterRun)
    {
        (afterRun ? after_ : during_)[index(r)].fetch_add(1, std::memory_order_relaxed);
    }

    EmbeddedRuntime& runtime_;
    TimeDuration tickBudget_;
    TimeDuration driveBudget_;
    bool tickAfterRun_;
    std::thread thread_;
    std::atomic<bool> stop_ { false };
    std::atomic<bool> runHandedOver_ { false };
    std::array<std::atomic<int>, 6> during_ {};
    std::array<std::atomic<int>, 6> after_ {};
};

// Node invocations still suspended on engine-owned (Dataflow) threads: every
// invokeClosure marks the frame it pushes, so each marked frame is one body
// that started and has not returned.  Read only once the driver has stopped.
int suspendedNodeBodies()
{
    int bodies = 0;
    for (const auto& t : ThreadManager::instance().snapshotThreads()) {
        if (t->kind != ThreadKind::Dataflow)
            continue;
        for (const auto& frame : t->frames)
            if (frame.unwindOnReturn)
                ++bodies;
    }
    return bodies;
}

struct Scenario {
    const char* name;
    const char* source;
    const char* expectedOutput;
    // Keep ticking after the run is handed over.  Off elsewhere so each
    // scenario reports its own property rather than a post-run failure.
    bool tickAfterRun;
    // The node body must have suspended across cycles at least once, so the
    // resume path is covered, not just the start path.
    bool requireNodeYield;
    // The host must have seen tickFor abandon a suspended tick at least once.
    bool requireOverrun;
    // A node body fails (or exits) on purpose: the run it belongs to must
    // end that way, with this in its diagnostic, and the engine must go on
    // evaluating afterwards.
    RunState expectedState = RunState::Completed;
    const char* diagnosticContains = nullptr;
    int expectedExitCode = 0;
};

// A Roxal-bodied gate feeding back on itself, then a busy loop of
// module-variable writes: the node's result must never reach the program's
// operand stack.
const char* kBusyLoop =
    "import logic\n"
    "var b = signal(10, false)\n"
    "b <- logic.not_gate(b[-1])\n"
    "var k :int = 0\n"
    "while k < 200000:\n"
    "  k = k + 1\n"
    "print(k)\n";

// The same node, with the program blocking in sys.wait between writes: the
// program's own statements must never run under the dataflow-function flag.
const char* kWaitLoop =
    "import sys\n"
    "import logic\n"
    "var b = signal(10, false)\n"
    "b <- logic.not_gate(b[-1])\n"
    "var k :int = 0\n"
    "for i in range(..<20):\n"
    "  sys.wait(2ms)\n"
    "  k = k + 1\n"
    "print(k)\n";

// A node whose body outlasts the tick budget, so it suspends and resumes
// across cycles while the program runs between them.  The program's state
// must be intact afterwards, and the node must have made progress.
const char* kYieldingNode =
    "func slow(x :int) -> int:\n"
    "  var s = 0\n"
    "  for i in range(..<20000):\n"
    "    s = s + i\n"
    "  return x + 1\n"
    "var n = signal(10, 0)\n"
    "n <- slow(n[-1])\n"
    "var k :int = 0\n"
    "while k < 300000:\n"
    "  k = k + 1\n"
    "print(k)\n"
    "print(n.value > 0)\n";

// The same slow body on a 1 kHz schedule: a suspended tick outlives its
// period, so tickFor reports Overrun and abandons it.  An abandoned body
// must be discarded -- neither finished by the program's slices nor left
// under the next body the engine suspends.
const char* kOverrunOrphan =
    "func slow(x :int) -> int:\n"
    "  var s = 0\n"
    "  for i in range(..<20000):\n"
    "    s = s + i\n"
    "  return x + 1\n"
    "var n = signal(1000, 0)\n"
    "n <- slow(n[-1])\n"
    "var k :int = 0\n"
    "while k < 300000:\n"
    "  k = k + 1\n"
    "print(k)\n";

// A run that leaves a Roxal-bodied node registered and ends; the host's loop
// keeps ticking, as a control loop does between programs.
const char* kTickAfterRun =
    "import logic\n"
    "var b = signal(10, false)\n"
    "b <- logic.not_gate(b[-1])\n"
    "print('started')\n";

// A node whose body raises once (on the clock's third tick): its run fails
// with the node's error, as the program's own error would fail it, and the
// engine -- its context discarded with the failure -- keeps evaluating the
// same node on later ticks.
const char* kNodeError =
    "func check(x :int) -> int:\n"
    "  if x == 3:\n"
    "    raise RuntimeException('node boom')\n"
    "  return x\n"
    "var c = clock(10)\n"
    "var r = check(c)\n"
    "c.run()\n"
    "var k :int = 0\n"
    "while k < 3000000:\n"
    "  k = k + 1\n"
    "print(k)\n";

// The same, with the body calling exit(): the run exits with its code.
const char* kNodeExit =
    "import sys\n"
    "func stop(x :int) -> int:\n"
    "  if x == 3:\n"
    "    sys.exit(7)\n"
    "  return x\n"
    "var c = clock(10)\n"
    "var r = stop(c)\n"
    "c.run()\n"
    "var k :int = 0\n"
    "while k < 3000000:\n"
    "  k = k + 1\n"
    "print(k)\n";

const Scenario kScenarios[] = {
    { "busy_loop",      kBusyLoop,      "200000\n",       false, false, false },
    { "wait_loop",      kWaitLoop,      "20\n",           false, false, false },
    { "yielding_node",  kYieldingNode,  "300000\ntrue\n", false, true,  false },
    { "overrun_orphan", kOverrunOrphan, "300000\n",       false, true,  true  },
    { "tick_after_run", kTickAfterRun,  "started\n",      true,  false, false },
    { "node_error",     kNodeError,     "",               true,  false, false,
      RunState::Failed, "node boom", 0 },
    { "node_exit",      kNodeExit,      "",               true,  false, false,
      RunState::Completed, nullptr, 7 },
};

int runScenario(const Scenario& scenario)
{
    const std::string name = scenario.name;
    VM& vm = VM::instance();
    CaptureSink sink;
    OutputRouter::setSink(&sink);

    AttachDriverResult attached = vm.attachEmbeddedRuntime();
    expect(attached.status == AttachStatus::Attached && attached.runtime,
           "the host attaches an embedded runtime");
    if (!attached.runtime)
        return 1;
    EmbeddedRuntime& runtime = *attached.runtime;

    // A tick budget far below the slow node's body; the program's slice
    // gets the harness's usual 500us.
    Driver driver(runtime, TimeDuration::microSecs(50), TimeDuration::microSecs(500),
                  scenario.tickAfterRun);
    driver.start();

    std::stringstream stream(scenario.source);
    ProgramOptions options;
    options.sourceName = "df_host_" + name;
    PrepareProgramResult prepared = vm.prepareProgram(stream, std::move(options));
    expect(prepared.status == PrepareStatus::Ready, name + ": the program prepares");

    RunState finalState = RunState::Failed;
    std::string diagnostic;
    int exitCode = -1;
    if (prepared.status == PrepareStatus::Ready) {
        SubmitResult submitted = runtime.submit(std::move(prepared.program));
        expect(submitted.status == SubmitStatus::Accepted, name + ": the program is accepted");
        if (submitted.status == SubmitStatus::Accepted) {
            // An unbounded wait: a wedged driver shows up as the ctest timeout.
            FinalizeResult finalized = submitted.run.wait();
            finalState = finalized.state;
            diagnostic = submitted.run.diagnostic();
            exitCode = submitted.run.exitCode();
        }
    }

    if (scenario.tickAfterRun)
        expect(waitUntil([&] { return driver.afterTotal() >= 20; }),
               name + ": the host kept ticking after the run ended");

    driver.stop();

    const std::string output = sink.text();
    // One line of context for any failure below.
    std::cout << name << ": ticks during run complete/yielded/overrun/error/busy/paused = "
              << driver.during(TickResult::Complete) << '/'
              << driver.during(TickResult::Yielded) << '/'
              << driver.during(TickResult::Overrun) << '/'
              << driver.during(TickResult::Error) << '/'
              << driver.during(TickResult::Busy) << '/'
              << driver.during(TickResult::Paused)
              << ", after run = " << driver.afterTotal()
              << ", final state " << int(finalState) << '\n';
    if (!sink.diagnostics().empty())
        std::cout << name << ": diagnostics: " << sink.diagnostics() << '\n';
    expect(finalState == scenario.expectedState,
           name + ": the run ends " + std::string(scenario.expectedState == RunState::Completed
                                                 ? "Completed" : "Failed")
               + " (state " + std::to_string(int(finalState)) + ", diagnostic: '"
               + diagnostic + "', stderr: '" + sink.diagnostics() + "')");
    if (scenario.diagnosticContains)
        expect(diagnostic.find(scenario.diagnosticContains) != std::string::npos,
               name + ": the run's diagnostic names the node's failure ('" + diagnostic + "')");
    expect(exitCode == scenario.expectedExitCode,
           name + ": exit code " + std::to_string(exitCode) + ", expected "
               + std::to_string(scenario.expectedExitCode));
    expect(output == scenario.expectedOutput,
           name + ": output '" + output + "', expected '" + scenario.expectedOutput + "'");

    // The interleaving is the thing under test: without ticks sharing the
    // driver with the live run, a pass proves nothing.
    const int worked = driver.during(TickResult::Complete) + driver.during(TickResult::Yielded)
                     + driver.during(TickResult::Overrun);
    expect(worked >= 3,
           name + ": node evaluation interleaved with the run ("
               + std::to_string(driver.during(TickResult::Complete)) + " complete, "
               + std::to_string(driver.during(TickResult::Yielded)) + " yielded, "
               + std::to_string(driver.during(TickResult::Overrun)) + " overrun)");
    const bool nodeFails = scenario.diagnosticContains || scenario.expectedExitCode != 0;
    if (nodeFails)
        expect(driver.during(TickResult::Error) + driver.after(TickResult::Error) >= 1,
               name + ": the failing tick reported Error to the host");
    else
        expect(driver.during(TickResult::Error) == 0 && driver.after(TickResult::Error) == 0,
               name + ": no tick reported an error");
    if (scenario.requireNodeYield)
        expect(driver.during(TickResult::Yielded) > 0,
               name + ": the node body suspended across cycles");
    if (scenario.requireOverrun)
        expect(driver.during(TickResult::Overrun) > 0,
               name + ": tickFor abandoned a suspended tick at least once");
    if (scenario.tickAfterRun)
        expect(driver.after(TickResult::Complete) >= 20,
               name + ": ticks after the run completed ("
                   + std::to_string(driver.after(TickResult::Complete)) + ")");

    // At most the one body the engine's yield state still tracks may remain
    // suspended; an abandoned one left beneath it would be finished by the
    // next resume and deliver its result as that body's.
    const int suspended = suspendedNodeBodies();
    expect(suspended <= 1,
           name + ": no abandoned node body is left suspended ("
               + std::to_string(suspended) + " suspended)");

    vm.shutdownEmbeddedRuntime();
    OutputRouter::setSink(nullptr);
    VM::shutdownIfConstructed();
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "usage: " << argv[0] << " <scenario>\n  scenarios:";
        for (const auto& s : kScenarios)
            std::cerr << ' ' << s.name;
        std::cerr << '\n';
        return 2;
    }
    for (const auto& scenario : kScenarios) {
        if (std::strcmp(argv[1], scenario.name) == 0) {
            const int rc = runScenario(scenario);
            if (rc == 0)
                std::cout << "dataflow host thread '" << scenario.name << "' passed\n";
            return rc;
        }
    }
    std::cerr << "unknown scenario '" << argv[1] << "'\n";
    return 2;
}
