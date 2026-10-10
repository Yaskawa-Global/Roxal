// A REPL session hosted the way a console embeds it: lines submitted to an
// embedded runtime ("driven", a periodic driver thread slices them) or
// evaluated on the calling thread ("sync", the CLI's shape).
//
// What it proves: a line can be ENDED -- by the host's interrupt, by exit()
// typed in it, while it is blocked in a wait, while a debugger holds it --
// and the session carries on.  The line's run completes (it did not fail)
// with its own exit code, the runtime slot is free at once, the next line
// runs promptly with exit code 0, and everything the session holds survives:
// its variables, its `when` handlers, its actors, and the values closures
// captured from the interrupted line.  Then completion: replace ranges,
// session names, module/instance/type/builtin members.
//
// One process per mode: a driver, once attached, owns the VM's execution.
// Wall-clock time appears only as a generous deadlock guard.

#include "compiler/EmbeddedRuntime.h"
#include "compiler/ReplSession.h"
#include "compiler/SimpleMarkSweepGC.h"
#include "compiler/VM.h"
#include "compiler/debug/StopCoordinator.h"
#include "core/Output.h"

#include <atomic>
#include <chrono>
#include <cstring>
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

bool waitUntil(const std::function<bool()>& pred, int seconds = 30)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

const char* stateName(RunState s)
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

class CaptureSink final : public OutputSink {
public:
    OutputResult emit(const OutputEventView& event) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event.kind == OutputKind::Print)
            text_.append(event.text);
        else
            diagnostics_.append(event.text).append("\n");
        return OutputResult::Accepted;
    }
    std::string take()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = std::move(text_);
        text_.clear();
        return out;
    }
    bool contains(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_.find(s) != std::string::npos;
    }
    std::string diagnostics()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string out = std::move(diagnostics_);
        diagnostics_.clear();
        return out;
    }

private:
    std::mutex mutex_;
    std::string text_;
    std::string diagnostics_;
};

// The session state every scenario starts from, and the probe that proves it
// survived: the variable, the `when` handler, the actor.
const char* kSessionSetup =
    "import math\n"
    "type Ping event\n"
    "var keep = 41\n"
    "var fired = 0\n"
    "when Ping occurs as e:\n"
    "  fired = fired + 1\n"
    "type Echo actor:\n"
    "  func bump(x :int) -> int:\n"
    "    return x + 1\n"
    "var echo = Echo()\n"
    "type Point object:\n"
    "  var x :int = 3\n"
    "  var y :int = 4\n"
    "  func norm2() -> int:\n"
    "    return x*x + y*y\n"
    "type Holder object:\n"
    "  var p :Point = Point()\n"
    "var pt = Point()\n"
    "var holder = Holder()\n"
    "type Color enum:\n"
    "  red\n"
    "  green\n"
    "var xs = [1, 2]\n"
    "var getters = []\n"
    "var before = 0\n"
    "var answer = nil\n";

const char* kSurvivalProbe =
    "before = fired\n"
    "emit Ping()\n"
    "wait(ms=30)\n"
    "answer = echo.bump(keep)\n"
    "wait(for=answer)\n"
    "print('keep=' + string(keep) + ' fired=' + string(fired - before) + ' echo=' + string(answer))\n";
const char* kSurvivalExpected = "keep=41 fired=1 echo=42\n";

// A line whose closure captures a LOCAL of a frame that is still live when
// the line is interrupted.
const char* kClosureLine =
    "func makeIt():\n"
    "  var local = 99\n"
    "  getters.append(func() -> int:\n"
    "    return local\n"
    "  )\n"
    "  local = 100\n"
    "  print('spinning')\n"
    "  while true:\n"
    "    local = 100\n"
    "makeIt()\n";
// Scribble over the slots the interrupted frame used, then read the capture.
const char* kClosureProbe =
    "func scribble(a, b, c) -> int:\n"
    "  var d = a + b\n"
    "  var e = c * 7\n"
    "  return d + e\n"
    "var s = scribble(1, 2, 3)\n"
    "print('captured=' + string(getters[0]()))\n";

void completionChecks(ReplSession& session, bool lineBusy)
{
    const std::string tag = lineBusy ? " (while a line runs)" : "";
    auto has = [](const CompletionResult& r, const std::string& text,
                  const std::string& kind = "") {
        for (const auto& c : r.candidates)
            if (c.text == text && (kind.empty() || c.kind == kind))
                return true;
        return false;
    };
    {
        const std::string buf = "print(kee";
        CompletionResult r = session.complete(buf);
        expect(has(r, "keep", "variable"), "completes a session variable" + tag);
        expect(r.replaceStart == 6 && r.replaceEnd == buf.size(),
               "the replace range is the partial name only [" + std::to_string(r.replaceStart)
               + "," + std::to_string(r.replaceEnd) + ")" + tag);
        // The CLI adapter's line: the head is kept.
        expect(buf.substr(0, r.replaceStart) + "keep" + buf.substr(r.replaceEnd) == "print(keep",
               "head + candidate rebuilds the line");
    }
    {
        CompletionResult r = session.complete("pri");
        expect(has(r, "print", "function"), "completes a builtin global" + tag);
    }
    {
        CompletionResult r = session.complete("whi");
        expect(has(r, "while", "keyword"), "completes a keyword" + tag);
    }
    {
        CompletionResult r = session.complete("x = math.sq");
        expect(has(r, "sqrt"), "completes a module member" + tag);
        expect(r.replaceStart == 9, "a member's replace range starts after the dot" + tag);
    }
    {
        CompletionResult r = session.complete("pt.");
        expect(has(r, "x", "property") && has(r, "y", "property") && has(r, "norm2", "method"),
               "lists an object instance's properties and methods" + tag);
        expect(r.replaceStart == 3 && r.replaceEnd == 3, "an empty prefix replaces nothing" + tag);
    }
    {
        CompletionResult r = session.complete("echo.bu");
        expect(has(r, "bump", "method"), "completes an actor instance's method" + tag);
    }
    {
        CompletionResult r = session.complete("holder.p.no");
        expect(has(r, "norm2", "method"), "follows a chain through a declared property type" + tag);
    }
    {
        CompletionResult r = session.complete("Color.gr");
        expect(has(r, "green", "enum"), "completes an enumeration label" + tag);
    }
    {
        CompletionResult r = session.complete("xs.app");
        expect(has(r, "append", "method"), "completes a list method" + tag);
    }
    {
        CompletionResult r = session.complete("'abc'.up");
        expect(has(r, "upper", "method"), "completes a string literal's method" + tag);
    }
    {
        CompletionResult r = session.complete("print('kee");
        expect(r.candidates.empty(), "nothing is completed inside a string" + tag);
    }
    {
        CompletionResult r = session.complete("print(kee) # trailing", 9);
        expect(has(r, "keep") && r.replaceStart == 6 && r.replaceEnd == 9,
               "a cursor inside the buffer completes at the cursor" + tag);
    }
    {
        CompletionResult r = session.complete("", std::string_view::npos, 5);
        expect(r.candidates.size() == 5, "maxCandidates bounds the answer" + tag);
    }
}

// ---------------------------------------------------------------------------
// Driven: a periodic driver slices every line, as a controller console does.

class Driver {
public:
    explicit Driver(EmbeddedRuntime& runtime) : runtime_(runtime) {}
    void start()
    {
        thread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                SimpleMarkSweepGC::GCYieldScope gcs{};
                if (!gcs)
                    continue;
                runtime_.driveFor(TimeDuration::microSecs(500));
            }
        });
    }
    void stop()
    {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

private:
    EmbeddedRuntime& runtime_;
    std::thread thread_;
    std::atomic<bool> stop_ { false };
};

RunHandle submitLine(EmbeddedRuntime& runtime, ReplSession& session, const std::string& src,
                     const std::string& what)
{
    std::stringstream stream(src);
    FragmentOptions options;
    options.sourceName = "console";
    options.replMode = false;
    PrepareFragmentResult prepared = session.prepareFragment(stream, options);
    expect(prepared.status == PrepareFragmentStatus::Ready,
           what + ": the line prepares (the session is idle)");
    if (prepared.status != PrepareFragmentStatus::Ready)
        return RunHandle();
    SubmitResult submitted = runtime.submit(std::move(prepared.fragment));
    expect(submitted.status == SubmitStatus::Accepted, what + ": the runtime accepts the line");
    return submitted.run;
}

// Run a line to its end; expect it to complete with exit code 0.
void runLine(EmbeddedRuntime& runtime, ReplSession& session, const std::string& src,
             const std::string& what)
{
    RunHandle run = submitLine(runtime, session, src, what);
    if (!run.valid())
        return;
    const auto t0 = std::chrono::steady_clock::now();
    FinalizeResult fin = run.wait();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    expect(fin.state == RunState::Completed,
           what + ": completes [" + stateName(fin.state) + " " + run.diagnostic() + "]");
    expect(run.exitCode() == 0, what + ": exit code 0 [" + std::to_string(run.exitCode()) + "]");
    // A generous guard, not a timing assertion: a line left resuming its
    // predecessor's frames never completes at all.
    expect(ms < 10000, what + ": completes promptly [" + std::to_string(ms) + " ms]");
}

// Interrupt a running line with `code`, expect it to COMPLETE with that code,
// then prove the session survived.
void interruptAndCheck(EmbeddedRuntime& runtime, ReplSession& session, CaptureSink& sink,
                       RunHandle run, int code, const std::string& what)
{
    const ExitRequestStatus asked = session.interrupt(code);
    expect(asked == ExitRequestStatus::Requested,
           what + ": the interrupt is requested [" + std::to_string(int(asked)) + "]");
    FinalizeResult fin = run.wait();
    expect(fin.state == RunState::Completed,
           what + ": the interrupted line completes, not fails [" + stateName(fin.state)
           + " " + run.diagnostic() + "]");
    expect(!run.failed(), what + ": an interrupt is not a failure");
    expect(run.exitCode() == code,
           what + ": the line reports its own code [" + std::to_string(run.exitCode()) + "]");
    expect(!runtime.hasUnfinalizedRun(), what + ": the runtime slot is free");
    expect(session.interrupt(code) == ExitRequestStatus::AlreadyTerminal,
           what + ": nothing is left to interrupt");
    sink.take();

    runLine(runtime, session, kSurvivalProbe, what + ": the next line");
    expect(sink.take() == kSurvivalExpected,
           what + ": the variable, the handler and the actor all survive");
}

int runDriven()
{
    VM& vm = VM::instance();
    CaptureSink sink;
    OutputRouter::setSink(&sink);

    AttachDriverResult attached = vm.attachEmbeddedRuntime();
    expect(attached.status == AttachStatus::Attached && attached.runtime,
           "the host attaches an embedded runtime");
    EmbeddedRuntime& runtime = *attached.runtime;
    Driver driver(runtime);
    driver.start();

    ReplSession session = vm.defaultReplSession();
    expect(session.interrupt() == ExitRequestStatus::AlreadyTerminal,
           "an idle session has nothing to interrupt");

    runLine(runtime, session, kSessionSetup, "setup");
    runLine(runtime, session, kSurvivalProbe, "the probe before any interrupt");
    expect(sink.take() == kSurvivalExpected, "the probe holds before any interrupt");

    // 1. A busy loop, interrupted with 130 (Ctrl-C's code).
    {
        RunHandle run = submitLine(runtime, session,
                                   "var spin = 0\nprint('spinning')\nwhile true:\n  spin = spin + 1\n",
                                   "busy loop");
        expect(waitUntil([&] { return sink.contains("spinning"); }), "the busy loop starts");
        completionChecks(session, /*lineBusy=*/true);
        interruptAndCheck(runtime, session, sink, run, 130, "busy loop");
    }

    // 2. exit() typed in a line ends that line, with its code, and nothing else.
    {
        RunHandle run = submitLine(runtime, session,
                                   "print('before')\nexit(7)\nprint('after')\n", "exit(7)");
        FinalizeResult fin = run.wait();
        expect(fin.state == RunState::Completed,
               std::string("exit() in a line completes it [") + stateName(fin.state) + "]");
        expect(run.exitCode() == 7, "exit(7) reports 7 [" + std::to_string(run.exitCode()) + "]");
        expect(sink.take() == "before\n", "exit() ends the line where it is");
        runLine(runtime, session, kSurvivalProbe, "after exit(7)");
        expect(sink.take() == kSurvivalExpected, "exit() in a line leaves the session intact");
    }
    {
        RunHandle run = submitLine(runtime, session,
                                   "print('zero')\nexit(0)\nprint('after')\n", "exit(0)");
        FinalizeResult fin = run.wait();
        expect(fin.state == RunState::Completed && run.exitCode() == 0,
               "exit(0) in a line completes it with 0");
        expect(sink.take() == "zero\n", "exit(0) ends the line where it is");
        runLine(runtime, session, kSurvivalProbe, "after exit(0)");
        expect(sink.take() == kSurvivalExpected, "exit(0) in a line leaves the session intact");
    }

    // 3. A line blocked in a long wait.
    {
        RunHandle run = submitLine(runtime, session,
                                   "print('waiting')\nwait(ms=100000)\nprint('woke')\n",
                                   "long wait");
        expect(waitUntil([&] { return sink.contains("waiting"); }), "the waiting line starts");
        interruptAndCheck(runtime, session, sink, run, 130, "long wait");
    }

    // 4. A line held by a debugger stop.
    {
        StopCoordinator& coord = vm.stopCoordinator();
        RunHandle run = submitLine(runtime, session,
                                   "var k = 0\nprint('debuggee')\nwhile true:\n  k = k + 1\n",
                                   "debugger stop");
        expect(waitUntil([&] { return sink.contains("debuggee"); }), "the debuggee line starts");
        bool stopped = false;
        std::thread controller([&] {
            auto outcome = coord.requestStop(DebugStopReason::Pause, TimeDuration::milliSecs(5000));
            stopped = outcome.stopped && coord.isStopped();
        });
        controller.join();
        expect(stopped, "the debugger stops the line");
        expect(waitUntil([&] { return run.state() == RunState::DebugPaused; }, 10),
               "the line reports DebugPaused");
        interruptAndCheck(runtime, session, sink, run, 130, "debugger stop");
        expect(!coord.isStopped(), "the stop over the interrupted line is released");
    }

    // 5. A closure the interrupted line made over a live local keeps its value.
    {
        RunHandle run = submitLine(runtime, session, kClosureLine, "closure line");
        expect(waitUntil([&] { return sink.contains("spinning"); }), "the closure line spins");
        expect(session.interrupt(130) == ExitRequestStatus::Requested,
               "the closure line accepts the interrupt");
        run.wait();
        sink.take();
        runLine(runtime, session, kClosureProbe, "closure probe");
        expect(sink.take() == "captured=100\n",
               "a closure over the interrupted frame's local still sees it");
    }

    // 6. Completion while idle.
    completionChecks(session, /*lineBusy=*/false);

    // 7. A fresh program runs afterwards (the slot is free, nothing leaked).
    {
        std::stringstream src("print('fresh')\n");
        ProgramOptions options;
        options.sourceName = "fresh_program";
        PrepareProgramResult prepared = vm.prepareProgram(src, std::move(options));
        expect(prepared.status == PrepareStatus::Ready, "a fresh program prepares");
        SubmitResult submitted = runtime.submit(std::move(prepared.program));
        expect(submitted.status == SubmitStatus::Accepted, "a fresh program is accepted");
        FinalizeResult fin = submitted.run.wait();
        expect(fin.state == RunState::Completed && submitted.run.exitCode() == 0,
               std::string("the fresh program completes [") + stateName(fin.state) + "]");
        expect(sink.take() == "fresh\n", "the fresh program ran");
    }
    // ...and the session still answers after it.
    runLine(runtime, session, kSurvivalProbe, "the probe after a program");
    expect(sink.take() == kSurvivalExpected, "the session survives a program run");

    driver.stop();
    vm.shutdownEmbeddedRuntime();
    OutputRouter::setSink(nullptr);
    VM::shutdownIfConstructed();
    if (g_failures == 0)
        std::cout << "repl session harness (driven) passed\n";
    return g_failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Sync: the calling thread evaluates; another thread (standing in for a
// SIGINT handler) interrupts.

ExecutionStatus evalSync(ReplSession& session, const std::string& src)
{
    std::stringstream stream(src);
    FragmentOptions options;
    options.sourceName = "cli";
    options.replMode = false;
    return session.evaluateFragmentSync(stream, options);
}

void syncInterrupt(ReplSession& session, CaptureSink& sink, const std::string& src,
                   const std::string& marker, const std::string& what)
{
    std::atomic<bool> asked { false };
    std::thread interrupter([&] {
        if (!waitUntil([&] { return sink.contains(marker); }))
            return;
        asked = session.interrupt(130) == ExitRequestStatus::Requested;
    });
    const auto t0 = std::chrono::steady_clock::now();
    const ExecutionStatus status = evalSync(session, src);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    interrupter.join();
    expect(asked, what + ": the interrupt is requested");
    expect(status == ExecutionStatus::OK, what + ": the interrupted line returns OK");
    expect(ms < 20000, what + ": it ends promptly [" + std::to_string(ms) + " ms]");
    sink.take();
    expect(evalSync(session, kSurvivalProbe) == ExecutionStatus::OK, what + ": the next line runs");
    expect(sink.take() == kSurvivalExpected,
           what + ": the variable, the handler and the actor all survive");
}

int runSync()
{
    VM& vm = VM::instance();
    CaptureSink sink;
    OutputRouter::setSink(&sink);
    ReplSession session = vm.defaultReplSession();

    expect(session.interrupt() == ExitRequestStatus::AlreadyTerminal,
           "an idle session has nothing to interrupt");
    expect(evalSync(session, kSessionSetup) == ExecutionStatus::OK, "setup");
    expect(evalSync(session, kSurvivalProbe) == ExecutionStatus::OK, "the probe");
    expect(sink.take() == kSurvivalExpected, "the probe holds before any interrupt");

    syncInterrupt(session, sink, "print('spinning')\nwhile true:\n  keep = keep\n",
                  "spinning", "busy loop");
    syncInterrupt(session, sink, "print('waiting')\nwait(ms=100000)\n", "waiting", "long wait");

    // A closure over the interrupted frame's local.
    {
        std::thread interrupter([&] {
            if (waitUntil([&] { return sink.contains("spinning"); }))
                session.interrupt(130);
        });
        evalSync(session, kClosureLine);
        interrupter.join();
        sink.take();
        expect(evalSync(session, kClosureProbe) == ExecutionStatus::OK, "the closure probe runs");
        expect(sink.take() == "captured=100\n",
               "a closure over the interrupted frame's local still sees it");
    }

    completionChecks(session, /*lineBusy=*/false);

    OutputRouter::setSink(nullptr);
    VM::shutdownIfConstructed();
    if (g_failures == 0)
        std::cout << "repl session harness (sync) passed\n";
    return g_failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "driven";
    if (mode == "driven")
        return runDriven();
    if (mode == "sync")
        return runSync();
    std::cerr << "usage: " << argv[0] << " driven|sync\n";
    return 2;
}
