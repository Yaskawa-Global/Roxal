#include "ThreadManager.h"

#include "Object.h"
#include "SimpleMarkSweepGC.h"
#include "debug/StopCoordinator.h"
#include "VM.h"   // defines CallFrame (Thread.h only forward-declares it)

namespace roxal {

ThreadManager& ThreadManager::instance()
{
    static ThreadManager manager;
    return manager;
}

void ThreadManager::registerThread(const ptr<Thread>& t)
{
    if (!t) {
        return;
    }
    Entry entry;
    entry.ref = t;
    entry.id = t->id();
    entry.domainId = t->domain ? t->domain->id() : 0;
    entry.kind = t->kind;
    std::lock_guard<std::mutex> lock(mutex_);
    // Domain admission gate: a thread created while a stop epoch is
    // forming/active starts pre-acknowledged and externally stopped -- the
    // execute-entry gate keeps it from running user code until release, so
    // a new thread can never escape the epoch.
    // Under the registry mutex, so a concurrent coordinator snapshot sees
    // either no entry or a fully pre-acknowledged one.
    if (t->domain && t->domain->admissionClosed.load(std::memory_order_acquire)) {
        t->debugOwnership.store(Thread::DebugOwnership::StoppedExternal,
                                std::memory_order_release);
        t->debugAckEpoch.store(t->domain->stopEpoch.load(std::memory_order_acquire),
                               std::memory_order_release);
        // ...and lease it into the epoch so membership stays strongly
        // retained.
        if (auto* coord = t->domain->coordinator.load(std::memory_order_acquire))
            coord->addLateEpochLease(t);
    }
    threads_.emplace(t.get(), std::move(entry));
}

void ThreadManager::unregisterThread(Thread* t)
{
    if (!t) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    threads_.erase(t);
}

std::vector<ptr<Thread>> ThreadManager::snapshotThreads()
{
    std::vector<ptr<Thread>> leases;
    std::lock_guard<std::mutex> lock(mutex_);
    leases.reserve(threads_.size());
    for (auto it = threads_.begin(); it != threads_.end(); ) {
        if (auto t = it->second.ref.lock()) {
            leases.push_back(std::move(t));
            ++it;
        } else {
            // Dying Thread whose destructor has not reached unregister yet
            // (or is blocked on this mutex): prune -- it can never be locked
            // again, and erasing here keeps snapshots O(live).
            it = threads_.erase(it);
        }
    }
    return leases;
}

std::vector<ptr<Thread>> ThreadManager::snapshotLeases(uint64_t domainId)
{
    std::vector<ptr<Thread>> leases;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = threads_.begin(); it != threads_.end(); ) {
        if (auto t = it->second.ref.lock()) {
            if (it->second.domainId == domainId)
                leases.push_back(std::move(t));
            ++it;
        } else {
            it = threads_.erase(it);   // dying Thread: prune (see snapshotThreads)
        }
    }
    return leases;
}

void ThreadManager::wakeAll()
{
    // Strong leases first; wake outside the registry mutex so a Thread's
    // own wake/sleep mutex never nests inside the registry lock.
    auto leases = snapshotThreads();
    for (auto& t : leases) {
        t->wake();
    }
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
    // The weak back-reference is read only under queueMutex (Thread::join
    // clears it there); the lock is released before joining.
    ptr<Thread> t;
    {
        std::lock_guard<std::mutex> lock { inst->queueMutex };
        t = inst->thread.lock();
    }
    if (t) {
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
