#pragma once

#ifdef ROXAL_ENABLE_WEB

// The seed of the host -> Roxal direction shared by every web host: any thread
// may submit a script; only the VM thread runs one. This is what lets a host
// keep a VM alive across scripts instead of exiting after one, and it is the
// mechanism web.serve() sits on.
//
// The asymmetry is deliberate and permanent (rule 1 of the web design):
// submitting NEVER blocks the caller. A browser main thread cannot wait on the
// VM, and a socket reader must not either. Callers observe completion through
// completedCount() or the completion handler.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>

namespace roxal {
namespace web {

class ScriptInbox {
public:
    // Runs one script on the calling thread; returns its exit code.
    using RunFn = std::function<int(const std::string& source, const std::string& name)>;
    // Observes each completion: (exit code, completed count so far). Called on
    // the VM thread after the run's cleanup, before the count is published.
    using CompletionFn = std::function<void(int rc, int completed)>;

    // Queue a script for the VM thread. Returns immediately.
    void submit(std::string source, std::string name);

    // Make serve() return once the queue is empty. Any thread.
    void requestQuit();
    bool quitRequested() const { return quit_.load(std::memory_order_acquire); }

    // Run submitted scripts until requestQuit(). VM thread only. Between
    // scripts the thread services NOTHING: host event loops are pumped by the
    // VM's dispatch loop, which only runs while a script does -- a UI app
    // therefore keeps a script alive (web.serve() parks in the dispatch loop).
    void serve(const RunFn& run);

    void setCompletionHandler(CompletionFn fn);

    int completedCount() const { return completed_.load(std::memory_order_acquire); }
    int lastResult() const { return lastResult_.load(std::memory_order_relaxed); }
    // True while serve() is inside a script (running or parked).
    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::string>> inbox_;   // (source, name)
    std::atomic<bool> quit_ { false };
    std::atomic<int> completed_ { 0 };
    std::atomic<int> lastResult_ { 0 };
    std::atomic<bool> running_ { false };
    CompletionFn onComplete_;
};

} // namespace web
} // namespace roxal

#endif // ROXAL_ENABLE_WEB
