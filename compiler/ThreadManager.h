#pragma once

// The complete, NON-OWNING index of every live roxal::Thread.
//
// Threads are constructed ONLY through the Thread::create factory, which
// registers a weak reference here immediately after the strong owner exists
// (and unregistration happens at the start of ~Thread).  There are no other
// construction paths and no way to forget, so this index is the collector's
// SOLE interpreter-root source (it replaced the vm.threads / replThread /
// dataflowEngineThread / VM::thread special-case gathering, whose blind
// spots produced root-hole bugs).
//
// Why weak references: a raw-pointer index cannot safely produce a strong
// reference -- if the last owner has already dropped the count and the
// destructor is waiting on this mutex, shared_from_this on the raw entry is
// UB.  Locking a weak_ptr under the registry mutex either yields a live
// strong lease or fails cleanly for a dying Thread.  This is what lets the
// debugger's stop coordinator take strong leases on epoch members, and what
// makes forEachThread/wakeAll safe against concurrent Thread destruction.
//
// Ownership is deliberately unchanged: whoever holds a ptr<Thread> today
// keeps holding it; this is an index, not an owner (avoids the
// register-at-create/strong-registry circularity).

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/memory.h"

namespace roxal {

class Thread;
struct ActorInstance;

// What a Thread executes for -- registered metadata, later used by the
// debugger to classify epoch members without touching the Thread itself.
enum class ThreadKind : uint8_t {
    Main,      // the script's main execution thread
    Init,      // builtin/module script initialization
    Repl,      // a fragment session's persistent thread
    Actor,     // an actor instance's worker (local, deserialized, or remote proxy)
    Dataflow,  // the dataflow engine's actor thread
};

class ThreadManager {
public:
    static ThreadManager& instance();

    // Called by Thread::create only, with the freshly constructed strong
    // owner; stores a weak reference plus stable metadata.
    void registerThread(const ptr<Thread>& t);
    // Called from ~Thread; removes the (now unlockable) entry.
    void unregisterThread(Thread* t);

    // Collector-only iteration: the caller must guarantee the world is
    // stopped (mark phase).  The mutex is still taken so a foreign thread
    // creating/destroying a Thread can never mutate the map mid-iteration;
    // each weak entry is locked so a Thread whose destruction has begun
    // (strong count zero, entry not yet erased) is skipped, never touched.
    template<typename Fn>
    void forEachThread(Fn&& fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [raw, entry] : threads_) {
            if (auto t = entry.ref.lock()) {
                fn(*t);
            }
        }
    }

    // Strong leases on every currently live Thread (expired entries pruned).
    // Safe from any thread; the leases keep each Thread alive until dropped.
    std::vector<ptr<Thread>> snapshotThreads();

    // Strong leases on every live Thread of ONE execution domain -- the
    // debugger stop coordinator's epoch-membership snapshot.
    // Registration-vs-epoch admission (a thread created during a forming
    // stop starts stopped) belongs to the stop coordinator; the registration
    // funnel and this filter are its seam.
    std::vector<ptr<Thread>> snapshotLeases(uint64_t domainId);

    // Wake every live Thread (sleep condvars) so blocked threads reach their
    // GC poll / termination checks promptly.  Takes strong leases first and
    // wakes OUTSIDE the registry mutex, so a Thread's own wake/sleep mutex is
    // never nested inside the registry lock.
    void wakeAll();

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

    struct Entry {
        weak_ptr<Thread> ref;
        uint64_t id { 0 };
        uint64_t domainId { 0 };
        ThreadKind kind { ThreadKind::Main };
    };

    mutable std::mutex mutex_;
    std::unordered_map<Thread*, Entry> threads_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleCv_;
    std::deque<ActorInstance*> lifecycleQueue_;
    std::thread lifecycleThread_;
    bool lifecycleRunning_ { false };
    bool lifecycleStopped_ { false };
    bool lifecycleBusy_ { false };   // a finalization is in flight
};

} // namespace roxal
