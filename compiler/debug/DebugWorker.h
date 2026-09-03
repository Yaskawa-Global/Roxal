#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace roxal {

class StopCoordinator;

// The debugger's non-RT service thread.  Its one job: complete
// asynchronously published stop requests -- a thread that discovers a
// breakpoint/step hit publishes the stop with bounded atomic work only and
// never invokes the host hold hook itself; this worker then performs the
// hold notification, the wakes, and the acknowledgement collection.  Idle
// waits are GC-safe-blocked (invisible to the collection barrier); the
// coordinator joins the worker during VM shutdown BEFORE the debug index
// drops its roots.
class DebugWorker {
public:
    explicit DebugWorker(StopCoordinator& coordinator);
    ~DebugWorker();

    // NON-RT paths only (takes the worker mutex + futex wake): sets the
    // pending flag and pokes the condvar for low-latency stop completion.
    // RT trap publications never call this -- they rely on the armed-mode
    // timed poll in run().
    void notifyStopRequested();

    // Wake the worker so it re-evaluates its wait mode (indefinite idle vs
    // armed timed poll).  Non-RT (called when slow-path demand arms).
    void notifyRecheck();

    // Post a command onto the worker's queue: the worker is the debugger's
    // SINGLE controlling thread, so transport adapters route
    // stop/resume/step/inspection commands here instead of driving the
    // coordinator from their own threads.  Commands run outside the GC-safe
    // idle scope under the worker's lifetime mutator cover.  Non-RT callers
    // only.
    void post(std::function<void()> fn);

    void shutdownAndJoin();

private:
    void run();

    StopCoordinator& coordinator_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool pending_ { false };
    bool shutdown_ { false };
    std::deque<std::function<void()>> commands_;
    static constexpr size_t kMaxCommands = 1024;
    bool overflowWarned_ { false };
    std::thread thread_;
};

} // namespace roxal
