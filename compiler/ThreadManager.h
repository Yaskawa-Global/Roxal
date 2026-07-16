#pragma once

// The complete, NON-OWNING
// index of every live roxal::Thread.  Threads self-register in the Thread
// constructor and unregister in the destructor -- there are no exceptions
// and no way to forget, so this index is the collector's SOLE
// interpreter-root source (it replaces the vm.threads / replThread /
// dataflowEngineThread / VM::thread special-case gathering, whose blind
// spots produced root-hole bugs).
//
// Ownership is deliberately unchanged: whoever holds a ptr<Thread>
// today keeps holding it; this is an index, not an owner (avoids the
// register-at-ctor/strong-registry circularity).  C0b/C1 move ownership and
// the join authority here as MutatorContext lands.

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace roxal {

class Thread;
struct ActorInstance;

class ThreadManager {
public:
    static ThreadManager& instance();

    void registerThread(Thread* t);
    void unregisterThread(Thread* t);

    // Collector-only iteration: the caller must guarantee the world is
    // stopped (mark phase).  The mutex is still taken so a foreign thread
    // constructing/destructing a Thread can never mutate the set
    // mid-iteration.
    template<typename Fn>
    void forEachThread(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Thread* t : threads_) {
            if (t) {
                fn(*t);
            }
        }
    }

    std::size_t threadCount() const;

    // ---- Actor lifecycle (join authority) ----------------------------------
    // The reclaimer never joins: an actor whose references were dropped in a
    // reclaim batch is handed here; the lifecycle thread (lazily started,
    // non-RT) requests the worker's exit, joins it, and only then destroys
    // the instance.  Holding the instance in this queue IS the Finalizing
    // hold: the object stays un-destroyed until its worker is gone.
    void enqueueActorFinalize(ActorInstance* inst);
    // Block until the lifecycle queue is empty and no finalization is in
    // flight.  Script-facing (the gc() builtin): makes actor teardown
    // deterministic for scripts without ever blocking the collector.
    void waitLifecycleIdle();
    // Drain the queue and join the lifecycle thread (VM shutdown).
    // Idempotent; the queue keeps accepting (and inline-finalizing) entries
    // afterwards, since shutdown sweeps can still retire actors.
    void stopLifecycle();

private:
    ThreadManager() = default;

    void lifecycleMain();
    static void finalizeActor(ActorInstance* inst);

    mutable std::mutex mutex_;
    std::unordered_set<Thread*> threads_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCv_;
    std::deque<ActorInstance*> lifecycleQueue_;
    std::thread lifecycleThread_;
    bool lifecycleRunning_ { false };
    bool lifecycleStopped_ { false };
    bool lifecycleBusy_ { false };   // a finalization is in flight
};

} // namespace roxal
