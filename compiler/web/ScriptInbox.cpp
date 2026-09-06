#ifdef ROXAL_ENABLE_WEB

#include "ScriptInbox.h"

#include "VM.h"

using namespace roxal;
using namespace roxal::web;

void ScriptInbox::submit(std::string source, std::string name)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inbox_.emplace_back(std::move(source), std::move(name));
    }
    cv_.notify_one();
}

void ScriptInbox::requestQuit()
{
    quit_.store(true, std::memory_order_release);
    cv_.notify_all();
}

void ScriptInbox::setCompletionHandler(CompletionFn fn)
{
    std::lock_guard<std::mutex> lock(mutex_);
    onComplete_ = std::move(fn);
}

void ScriptInbox::serve(const RunFn& run)
{
    for (;;) {
        std::pair<std::string, std::string> job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return !inbox_.empty() || quit_.load(std::memory_order_acquire);
            });
            if (inbox_.empty() && quit_.load(std::memory_order_acquire))
                return;
            job = std::move(inbox_.front());
            inbox_.pop_front();
        }
        running_.store(true, std::memory_order_release);
        const int rc = run(job.first, job.second);
        running_.store(false, std::memory_order_release);
        // An interrupted (requestExit) or exit()ed script leaves the exit flag
        // latched and the dataflow engine's run loop stopped; both must reset
        // or every LATER script is stillborn/signal-dead.
        {
            VM& vm = VM::instance();
            vm.clearExitFlag();
            vm.restartDataflowEngineIfStopped();
        }
        lastResult_.store(rc, std::memory_order_relaxed);
        CompletionFn onComplete;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            onComplete = onComplete_;
        }
        const int completed = completed_.load(std::memory_order_relaxed) + 1;
        if (onComplete) onComplete(rc, completed);
        // Release-store LAST: a poller that sees the new count must also see the
        // result and all output written before it.
        completed_.store(completed, std::memory_order_release);
    }
}

#endif // ROXAL_ENABLE_WEB
