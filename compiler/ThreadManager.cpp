#include "ThreadManager.h"

#include "Object.h"
#include "SimpleMarkSweepGC.h"
#include "VM.h"   // defines CallFrame (Thread.h only forward-declares it)

namespace roxal {

ThreadManager& ThreadManager::instance()
{
    static ThreadManager manager;
    return manager;
}

void ThreadManager::registerThread(Thread* t)
{
    if (!t) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.insert(t);
}

void ThreadManager::unregisterThread(Thread* t)
{
    if (!t) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.erase(t);
}

std::size_t ThreadManager::threadCount() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return threads_.size();
}

void ThreadManager::finalizeActor(ActorInstance* inst)
{
    // References were already dropped in the reclaim batch that routed the
    // instance here; request the worker's exit, wait it out, then destroy.
    if (!inst) {
        return;
    }
    if (auto t = inst->thread.lock()) {
        t->join(inst);
    }
    delObj(inst);
}

void ThreadManager::lifecycleMain()
{
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    while (true) {
        lifecycleCv_.wait(lock, [&] {
            return lifecycleStopped_ || !lifecycleQueue_.empty();
        });
        if (lifecycleQueue_.empty() && lifecycleStopped_) {
            return;
        }
        while (!lifecycleQueue_.empty()) {
            ActorInstance* inst = lifecycleQueue_.front();
            lifecycleQueue_.pop_front();
            lifecycleBusy_ = true;
            lock.unlock();
            finalizeActor(inst);   // join happens OFF the reclaimer thread
            lock.lock();
            lifecycleBusy_ = false;
        }
        lifecycleCv_.notify_all();   // waitLifecycleIdle() waiters
    }
}

void ThreadManager::enqueueActorFinalize(ActorInstance* inst)
{
    if (!inst) {
        return;
    }
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    if (lifecycleStopped_) {
        // Shutdown already drained the lifecycle thread: finalize inline
        // (shutdown context is a safe, non-worker thread).
        lock.unlock();
        finalizeActor(inst);
        return;
    }
    if (!lifecycleRunning_) {
        lifecycleRunning_ = true;
        lifecycleThread_ = std::thread([this] { lifecycleMain(); });
    }
    lifecycleQueue_.push_back(inst);
    lifecycleCv_.notify_one();
}

void ThreadManager::waitLifecycleIdle()
{
    // Stay quiescent while blocked: the lifecycle's joins may need a
    // collection to complete, and the barrier must not wait on us.
    SimpleMarkSweepGC::GCSafeBlockScope blockScope;
    std::unique_lock<std::mutex> lock(lifecycleMutex_);
    lifecycleCv_.wait(lock, [&] {
        return lifecycleQueue_.empty() && !lifecycleBusy_;
    });
}

void ThreadManager::stopLifecycle()
{
    std::thread joiner;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        if (lifecycleStopped_) {
            return;
        }
        lifecycleStopped_ = true;
        if (!lifecycleRunning_) {
            return;
        }
        joiner = std::move(lifecycleThread_);
        lifecycleRunning_ = false;
    }
    lifecycleCv_.notify_all();
    if (joiner.joinable()) {
        joiner.join();
    }
}

} // namespace roxal
