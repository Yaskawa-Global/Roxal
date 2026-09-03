#include "DebugWorker.h"

#include "StopCoordinator.h"
#include "../SimpleMarkSweepGC.h"
#include "../VM.h"

namespace roxal {

DebugWorker::DebugWorker(StopCoordinator& coordinator)
    : coordinator_(coordinator), thread_([this] { run(); })
{
}

DebugWorker::~DebugWorker()
{
    shutdownAndJoin();
}

void DebugWorker::notifyStopRequested()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_ = true;
    }
    cv_.notify_one();
}

void DebugWorker::shutdownAndJoin()
{
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (shutdown_ && !thread_.joinable())
            return;
        shutdown_ = true;
    }
    cv_.notify_one();
    if (thread_.joinable())
        thread_.join();
}

void DebugWorker::notifyRecheck()
{
    // Wake without setting pending_ (a completeAsyncStop no-ops anyway):
    // used when the wait MODE must be re-evaluated (slow path armed).  The
    // empty critical section orders the wake against a waiter between its
    // predicate check and the wait.
    { std::lock_guard<std::mutex> lk(mutex_); }
    cv_.notify_one();
}

void DebugWorker::post(std::function<void()> fn)
{
    bool overflowed = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (shutdown_)
            return;   // late post during teardown: dropped deliberately
        // Bounded: a flooding client must not grow this queue without
        // limit while the worker is busy (a pause can legitimately hold it
        // for seconds).  Overflow drops the command deterministically; the
        // client sees its request time out.
        if (commands_.size() >= kMaxCommands) {
            overflowed = !overflowWarned_;
            overflowWarned_ = true;
        } else {
            commands_.push_back(std::move(fn));
        }
    }
    if (overflowed)
        VM::emitDiagnostic("debugger: command queue overflow; dropping requests",
                           OutputSeverity::Warning, "debug");
    cv_.notify_one();
}

void DebugWorker::run()
{
    // Lifetime GC participation: the worker is a registered mutator for its
    // whole life, so anything it does between its GC-safe waits --
    // completing stops, and the inspection requests the adapter posts -- is
    // visible to the collection barrier.
    ScopedGCMutatorCover mutatorCover;
    std::deque<std::function<void()>> commands;
    for (;;) {
        {
            // Idle wait is GC-safe-blocked: the collection barrier does not
            // wait for this thread while it sleeps, and the scope's exit
            // waits out an in-flight collection before we touch anything.
            SimpleMarkSweepGC::GCSafeBlockScope blockScope;
            std::unique_lock<std::mutex> lk(mutex_);
            auto ready = [&] {
                return pending_ || shutdown_ || !commands_.empty()
                       || coordinator_.hasAsyncPending();
            };
            // EVERY wait is bounded: an RT trap can publish asyncPending_
            // WITHOUT a wake at any point -- including between this mode
            // decision and the park -- and the demand may drop to zero in
            // the same window (step consumed, last breakpoint cleared), so
            // an indefinite wait could otherwise strand a published stop
            // forever.  20ms while the slow path is armed (trap-completion
            // latency), 500ms idle backstop (the worker exists only while a
            // debugger is engaged; two wakeups a second is noise).
            const bool fastPoll =
                coordinator_.slowPathArmed() || coordinator_.hasAsyncPending();
            cv_.wait_for(lk,
                         std::chrono::milliseconds(fastPoll ? 20 : 500),
                         ready);
            if (shutdown_)
                return;
            pending_ = false;
            commands.swap(commands_);
        }
        coordinator_.completeAsyncStop();
        for (auto& fn : commands) {
            try { fn(); } catch (...) { /* a command must not kill the worker */ }
        }
        commands.clear();
    }
}

} // namespace roxal
