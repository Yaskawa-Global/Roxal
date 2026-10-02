// A sliced program's event handlers and native continuation callbacks.
//
// A host that drives a program in short driveFor() slices can have a slice's
// deadline land on ANY instruction boundary -- including the one right after
// an event handler (or a list.map/filter/reduce callback) returns.  That
// return leaves work for the instruction epilogue: discard the handler's
// result and restore the sleep state of the wait() it interrupted, or hand
// the callback's result to its native.  The next slice must finish that work
// before the program runs another instruction; otherwise the interrupted
// frame resumes with a stray value on its stack, its wait() completes early
// and pops it, and the stack drifts down a slot per occurrence until it pops
// below its buffer -- the heap corruption a camera-rate dataflow diagram in
// fc.wasm hit, where every frame fired an on_changed handler.
//
// A budget of a few microseconds puts a deadline after almost every
// instruction, so each handler and callback return is followed by a yield.
// The program's own result is what is asserted: it must equal the unsliced
// run's.

#include "compiler/EmbeddedRuntime.h"
#include "compiler/SimpleMarkSweepGC.h"
#include "compiler/VM.h"
#include "core/Output.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
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

class CaptureSink final : public OutputSink {
public:
    OutputResult emit(const OutputEventView& event) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        (event.kind == OutputKind::Print ? text_ : diagnostics_).append(event.text);
        return OutputResult::Accepted;
    }
    std::string takeText()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(text_, {});
    }
    std::string takeDiagnostics()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::exchange(diagnostics_, {});
    }

private:
    std::mutex mutex_;
    std::string text_;
    std::string diagnostics_;
};

struct Program {
    const char* name;
    const char* source;
    const char* expected;   // the unsliced run's output (roxal on the same source)
};

// Continuation callbacks alone: list.map's callback returns into the native's
// continuation, not into bytecode.
const char* kContinuations =
    "var n = 0\n"
    "var mapped = 0\n"
    "while n < 200:\n"
    "  var xs = [n, n + 1].map(func(x):\n"
    "    return x * 2\n"
    "  )\n"
    "  mapped = mapped + xs[0] + xs[1]\n"
    "  n = n + 1\n"
    "print(mapped)\n";

// Two handlers per event (so a handler's return is followed by the next
// handler's dispatch), a handler that itself runs a continuation callback,
// and continuation callbacks in the program body -- all while the body
// sleeps in wait() between events.
const char* kEvents =
    "var s = signal(0, 0, 'ticks')\n"
    "var seen = 0\n"
    "var total = 0\n"
    "var second = 0\n"
    "func h(e):\n"
    "  seen = seen + 1\n"
    "  total = total + e.value\n"
    "func h2(e):\n"
    "  second = second + [e.value].map(func(x):\n"
    "    return 1\n"
    "  )[0]\n"
    "s.on_changed(h)\n"
    "s.on_changed(h2)\n"
    "var n = 0\n"
    "var acc = 0\n"
    "var mapped = 0\n"
    "while n < 40:\n"
    "  s.set(n + 1)\n"
    "  wait(ms=1)\n"
    "  var xs = [n, n + 1].map(func(x):\n"
    "    return x * 2\n"
    "  )\n"
    "  mapped = mapped + [xs[0], xs[1], 1].filter(func(x):\n"
    "    return x > 0\n"
    "  ).reduce(func(a, b):\n"
    "    return a + b\n"
    "  , 0)\n"
    "  acc = acc + n\n"
    "  n = n + 1\n"
    "wait(ms=20)\n"
    "print(string(n) + ' ' + string(acc) + ' ' + string(seen) + ' ' + string(total) + ' '"
    " + string(second) + ' ' + string(mapped))\n";

// Events that arrive while the program is blocked: a `wait ... until` that
// another thread's event must cut short, and an anyof() whose event arm must
// beat a slow future.  A sliced run yields where the unbounded one blocks; it
// must still dispatch them then, or the until runs its full time and the
// slow future wins.
const char* kBlockedEvents =
    "type Sig event\n"
    "type W actor:\n"
    "  func slow(n :int) -> int:\n"
    "    wait(ms=1500)\n"
    "    return n\n"
    "  proc emit_after(ms :int):\n"
    "    wait(ms=ms)\n"
    "    emit Sig()\n"
    "var w = W()\n"
    "var t0 = Time.steady_now()\n"
    "w.emit_after(20)\n"
    "wait(s=3) until Sig\n"
    "var waited = Time.steady_now().since(t0).total_micros()\n"
    "print('until cut short: ' + string(waited < 1000000))\n"
    "w.emit_after(20)\n"
    "var got = wait(for=anyof(w.slow(99), Sig))\n"
    "print('anyof winner: ' + string(got.index))\n";

const Program kPrograms[] = {
    { "continuations", kContinuations, "80000\n" },
    { "blocked_events", kBlockedEvents, "until cut short: true\nanyof winner: 1\n" },
    { "events",        kEvents,        "40 780 40 820 40 3240\n" },
};

// The host's driver: one OS thread for the whole process (the runtime latches
// the first thread that drives it), slicing whatever is submitted with the
// current budget, inside a GC yield section as a real-time host must.
class Driver {
public:
    explicit Driver(EmbeddedRuntime& runtime) : runtime_(runtime) {}

    void start()
    {
        thread_ = std::thread([this] {
            while (!stop_.load(std::memory_order_acquire)) {
                SimpleMarkSweepGC::GCYieldScope section{};
                if (!section) {
                    std::this_thread::yield();
                    continue;
                }
                const SliceResult slice = runtime_.driveFor(
                    TimeDuration::microSecs(budgetUs_.load(std::memory_order_acquire)));
                if (slice.state == SliceState::ShuttingDown)
                    break;
                if (slice.state == SliceState::Idle)
                    std::this_thread::yield();
            }
        });
    }
    void stop()
    {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }
    void setBudget(int us) { budgetUs_.store(us, std::memory_order_release); }

private:
    EmbeddedRuntime& runtime_;
    std::thread thread_;
    std::atomic<bool> stop_ { false };
    std::atomic<int> budgetUs_ { 1 };
};

// Runs the program to its end with the driver slicing it at `us`.
std::string runSliced(VM& vm, EmbeddedRuntime& runtime, Driver& driver, CaptureSink& sink,
                      const char* program, int us, const std::string& name)
{
    driver.setBudget(us);

    std::stringstream source(program);
    ProgramOptions options;
    options.sourceName = name;
    PrepareProgramResult prepared = vm.prepareProgram(source, std::move(options));
    expect(prepared.status == PrepareStatus::Ready,
           name + ": the program prepares (" + sink.takeDiagnostics() + ")");
    if (prepared.status == PrepareStatus::Ready) {
        SubmitResult submitted = runtime.submit(std::move(prepared.program));
        expect(submitted.status == SubmitStatus::Accepted, name + ": the program is accepted");
        if (submitted.status == SubmitStatus::Accepted) {
            // A generous deadlock guard, not a timing assertion: a run the
            // drift wedges (a wait() that never completes) fails here
            // rather than as an unexplained ctest timeout.
            const auto guard = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            while (submitted.run.state() == RunState::Queued
                   || submitted.run.state() == RunState::Running) {
                if (std::chrono::steady_clock::now() > guard) {
                    std::cerr << "FAIL: " << name << ": the run did not end within 60s (output so far: '"
                              << sink.takeText() << "')" << std::endl;
                    std::_Exit(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            FinalizeResult finalized = submitted.run.wait();
            expect(finalized.state == RunState::Completed,
                   name + ": the run completes (diagnostic: '" + submitted.run.diagnostic()
                       + "', stderr: '" + sink.takeDiagnostics() + "')");
        }
    }

    return sink.takeText();
}

} // namespace

int main(int argc, char** argv)
{
    VM& vm = VM::instance();
    CaptureSink sink;
    OutputRouter::setSink(&sink);

    AttachDriverResult attached = vm.attachEmbeddedRuntime();
    expect(attached.status == AttachStatus::Attached && attached.runtime,
           "the host attaches an embedded runtime");
    if (!attached.runtime)
        return 1;

    Driver driver(*attached.runtime);
    driver.start();

    // From a deadline after nearly every instruction to a few dozen
    // instructions per slice: each places the yields differently.
    std::vector<int> budgets { 1, 3, 10, 40 };
    if (argc > 1) {
        budgets.clear();
        for (int i = 1; i < argc; ++i)
            budgets.push_back(std::atoi(argv[i]));
    }
    for (const Program& program : kPrograms)
        for (int us : budgets) {
            const std::string name = std::string(program.name) + "_" + std::to_string(us) + "us";
            const std::string output =
                runSliced(vm, *attached.runtime, driver, sink, program.source, us, name);
            expect(output == program.expected,
                   name + ": output '" + output + "', expected '" + program.expected + "'");
        }

    driver.stop();
    if (g_failures == 0)
        std::cout << "sliced dispatch: all checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
