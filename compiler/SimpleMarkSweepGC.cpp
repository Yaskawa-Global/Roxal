#include "SimpleMarkSweepGC.h"

#include "Object.h"
#include "Thread.h"
#include "Value.h"
#include "VM.h"

#include <chrono>
#include <iostream>
#include <unordered_set>
#include <vector>

namespace {

using namespace roxal;

void visitStrongValue(ValueVisitor& visitor, const Value& value)
{
    if (!value.isObj()) {
        return;
    }
    if (value.isWeak()) {
        return;
    }
    visitor.visit(value);
}

template <typename Range>
void visitStrongValues(ValueVisitor& visitor, const Range& values)
{
    for (const auto& value : values) {
        visitStrongValue(visitor, value);
    }
}

void visitCallFrameRoots(const CallFrame& frame, ValueVisitor& visitor)
{
    visitStrongValue(visitor, frame.closure);
    visitStrongValues(visitor, frame.tailArgValues);
}

void visitThreadRoots(Thread& thread, ValueVisitor& visitor)
{
    for (auto it = thread.stack.begin(); it != thread.stackTop; ++it) {
        visitStrongValue(visitor, *it);
    }

    for (const auto& value : thread.openUpvalues) {
        visitStrongValue(visitor, value);
    }

    for (const auto& frame : thread.frames) {
        visitCallFrameRoots(frame, visitor);
    }

    for (const auto& entry : thread.eventHandlers) {
        visitStrongValue(visitor, entry.first);
        visitStrongValues(visitor, entry.second);
    }

    for (const auto& entry : thread.eventToSignal) {
        visitStrongValue(visitor, entry.first);
        visitStrongValue(visitor, entry.second);
    }

    thread.pendingEvents.unsafeVisit([&visitor](const auto& pendingEvents) {
        for (const auto& pending : pendingEvents) {
            visitStrongValue(visitor, pending.event);
        }
    });

    visitStrongValue(visitor, thread.currentActorCall);
    visitStrongValue(visitor, thread.currentBoundCall);
}

} // namespace

namespace roxal {

SimpleMarkSweepGC& SimpleMarkSweepGC::instance() {
    static SimpleMarkSweepGC gc;
    return gc;
}

void SimpleMarkSweepGC::registerAllocation(ObjControl* control) {
    if (!control) {
        return;
    }

    std::uint64_t allocationSize = control->allocationSize;
    currentAllocatedBytes_.fetch_add(allocationSize, std::memory_order_relaxed);

    if (allocationSize > 0) {
        std::uint64_t previous = bytesAllocatedSinceLastCollect_.fetch_add(allocationSize, std::memory_order_relaxed);
        std::uint64_t updated = previous + allocationSize;
        std::uint64_t threshold = autoTriggerThreshold_.load(std::memory_order_relaxed);
        if (threshold > 0 && previous < threshold && updated >= threshold) {
            // requestCollect() clears bytesAllocatedSinceLastCollect_ once it flips
            // collectionRequested_ from false to true so that the next GC cycle
            // starts counting from zero immediately while still reporting the
            // bytes that triggered the request (including manual gc() calls when
            // the threshold is disabled).
            requestCollect();
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [_, inserted] = controls_.insert(control);
        (void)inserted;
        // New allocations start out "unmarked" by setting their mark epoch to 0.
        // During a collection we compare this against the collector's global
        // epoch to decide whether the object was reached, avoiding the need to
        // reset a separate boolean on every cycle.
        control->collecting.store(false, std::memory_order_relaxed);
        control->markEpoch.store(0uLL, std::memory_order_relaxed);
    }
}

void SimpleMarkSweepGC::unregisterAllocation(ObjControl* control) {
    if (!control) {
        return;
    }

    std::uint64_t allocationSize = control->allocationSize;
    if (allocationSize > 0) {
        currentAllocatedBytes_.fetch_sub(allocationSize, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    controls_.erase(control);
}

void SimpleMarkSweepGC::requestCollect() {
    std::uint64_t allocated = bytesAllocatedSinceLastCollect_.load(std::memory_order_relaxed);
    bool expected = false;
    if (collectionRequested_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        lastRequestedBytes_.store(allocated, std::memory_order_relaxed);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
    }
    VM::instance().wakeAllThreadsForGC();
}

void SimpleMarkSweepGC::setVM(VM* vm) {
    vm_.store(vm, std::memory_order_release);
}

void SimpleMarkSweepGC::notifyCleanupPending() {
    if (VM* vm = vm_.load(std::memory_order_acquire)) {
        vm->requestObjectCleanup();
    }
}

void SimpleMarkSweepGC::setAutoTriggerThreshold(std::uint64_t threshold) {
    autoTriggerThreshold_.store(threshold, std::memory_order_relaxed);
    bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
    lastRequestedBytes_.store(0, std::memory_order_relaxed);
}

std::uint64_t SimpleMarkSweepGC::autoTriggerThreshold() const noexcept {
    return autoTriggerThreshold_.load(std::memory_order_relaxed);
}

bool SimpleMarkSweepGC::isCollectionRequested() const noexcept {
    return collectionRequested_.load(std::memory_order_acquire);
}

std::uint64_t SimpleMarkSweepGC::currentEpoch() const noexcept {
    return epoch_.load(std::memory_order_relaxed);
}

size_t SimpleMarkSweepGC::lastCollectionFreed() const noexcept {
    return lastFreedCount_.load(std::memory_order_relaxed);
}

bool SimpleMarkSweepGC::isCollectionInProgress() const noexcept {
    return collectionInProgress_.load(std::memory_order_acquire);
}

void SimpleMarkSweepGC::onThreadEnter() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++activeThreads_;
    safepointCv_.notify_all();
}

void SimpleMarkSweepGC::onThreadExit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeThreads_ > 0) {
        --activeThreads_;
    }
    safepointCv_.notify_all();
}

void SimpleMarkSweepGC::safepoint(Thread& currentThread) {
    if (!collectionRequested_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<Obj*> toDestroy;
#ifdef DEBUG_TRACE_GC
    bool emitTrace = false;
    std::uint64_t triggerBytes = 0;
    std::uint64_t thresholdBytes = 0;
    std::uint64_t allocatedBefore = 0;
    std::uint64_t epochStarted = 0;
    std::uint64_t freedBytes = 0;
    std::chrono::steady_clock::time_point collectionStart;
#endif

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!collectionRequested_.load(std::memory_order_acquire)) {
            return;
        }

        ++threadsAtSafepoint_;
        safepointCv_.notify_all();

        while (collectionRequested_.load(std::memory_order_acquire) &&
               threadsAtSafepoint_ < activeThreads_) {
            safepointCv_.wait(lock);
        }

        if (!collectionRequested_.load(std::memory_order_acquire)) {
            --threadsAtSafepoint_;
            safepointCv_.notify_all();
            return;
        }

        if (collector_ && collector_ != &currentThread) {
            // Another thread is handling the collection; wait for it to complete.
            while (collectionRequested_.load(std::memory_order_acquire)) {
                safepointCv_.wait(lock);
            }
            --threadsAtSafepoint_;
            safepointCv_.notify_all();
            return;
        }

        collector_ = &currentThread;
#ifdef DEBUG_TRACE_GC
        emitTrace = true;
        triggerBytes = lastRequestedBytes_.load(std::memory_order_relaxed);
        thresholdBytes = autoTriggerThreshold_.load(std::memory_order_relaxed);
        allocatedBefore = currentAllocatedBytes_.load(std::memory_order_relaxed);
        epochStarted = epoch_.load(std::memory_order_relaxed) + 1uLL;
        collectionStart = std::chrono::steady_clock::now();
        std::cout << "[GC] start epoch " << epochStarted
                  << ": allocated since last " << triggerBytes << " bytes";
        if (thresholdBytes > 0) {
            std::cout << " (threshold " << thresholdBytes << ")";
        }
        std::cout << "; currently allocated " << allocatedBefore << " bytes" << std::endl;
#endif
        CollectionResult result = performCollection(lock);
        toDestroy = std::move(result.unreachable);
#ifdef DEBUG_TRACE_GC
        freedBytes = result.freedBytes;
#endif
        collectionRequested_.store(false, std::memory_order_release);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
        collector_ = nullptr;
        --threadsAtSafepoint_;
    }

    safepointCv_.notify_all();

    bool pushed = false;
    for (Obj* obj : toDestroy) {
        if (!obj) {
            continue;
        }
        Obj::unrefedObjs.push_back(obj);
        pushed = true;
    }

    if (pushed) {
        if (VM* vm = vm_.load(std::memory_order_acquire)) {
            vm->requestObjectCleanup();
            vm->freeObjects();
        }
    }

#ifdef DEBUG_TRACE_GC
    if (emitTrace) {
        auto collectionEnd = std::chrono::steady_clock::now();
        double durationMs = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(collectionEnd - collectionStart).count();
        std::uint64_t allocatedAfter = currentAllocatedBytes_.load(std::memory_order_relaxed);
        std::cout << "[GC] end epoch " << epochStarted
                  << ": reclaimed " << toDestroy.size() << " objects ("
                  << freedBytes << " bytes) in " << durationMs
                  << " ms; allocated now " << allocatedAfter << " bytes" << std::endl;
    }
#endif
}

SimpleMarkSweepGC::CollectionResult SimpleMarkSweepGC::performCollection(std::unique_lock<std::mutex>& lock) {
    (void)lock;

    collectionInProgress_.store(true, std::memory_order_release);
    struct CollectionScope {
        explicit CollectionScope(SimpleMarkSweepGC& owner) : gc(owner) {}
        ~CollectionScope() {
            gc.collectionInProgress_.store(false, std::memory_order_release);
        }

        SimpleMarkSweepGC& gc;
    } scope(*this);

    const std::uint64_t previousEpoch = epoch_.fetch_add(1uLL, std::memory_order_relaxed);
    std::uint64_t epoch = previousEpoch + 1uLL;
    if (epoch == 0) {
        epoch = 1uLL;
        epoch_.store(epoch, std::memory_order_relaxed);
        for (ObjControl* control : controls_) {
            if (control) {
                control->markEpoch.store(0uLL, std::memory_order_relaxed);
            }
        }
    }

    struct MarkWorklist : ValueVisitor {
        explicit MarkWorklist(std::uint64_t epoch) : epoch(epoch) {}

        void visit(const Value& value) override {
            if (!value.isObj()) {
                return;
            }
            if (value.isWeak()) {
                return;
            }

            Obj* obj = value.asObj();
            if (!obj) {
                return;
            }

            ObjControl* control = obj->control;
            if (!control) {
                return;
            }

            if (control->collecting.load(std::memory_order_relaxed)) {
                return;
            }

            std::uint64_t previous = control->markEpoch.load(std::memory_order_relaxed);
            if (previous == epoch) {
                return;
            }

            control->markEpoch.store(epoch, std::memory_order_relaxed);
            worklist.push_back(obj);
        }

        void drain() {
            while (!worklist.empty()) {
                Obj* current = worklist.back();
                worklist.pop_back();
                if (current) {
                    current->trace(*this);
                }
            }
        }

        std::uint64_t epoch;
        std::vector<Obj*> worklist;
    } marker(epoch);

    visitRoots(marker);
    marker.drain();

    std::vector<Obj*> unreachable;
    unreachable.reserve(controls_.size());
    std::uint64_t totalFreedBytes = 0;
    for (ObjControl* control : controls_) {
        if (!control) {
            continue;
        }

        Obj* obj = control->obj;
        if (!obj) {
            continue;
        }

        if (control->collecting.load(std::memory_order_relaxed)) {
            continue;
        }

        if (control->markEpoch.load(std::memory_order_relaxed) == epoch) {
            continue;
        }

        control->collecting.store(true, std::memory_order_relaxed);
        control->obj = nullptr;

        totalFreedBytes += control->allocationSize;

        // Break any strong edges the object holds so that cycles see their
        // reference counts fall to zero once we hand them back to the regular
        // destruction path. Modules reuse the same helper when being torn down
        // explicitly.
        obj->dropReferences();

        unreachable.push_back(obj);
    }

    lastFreedCount_.store(unreachable.size(), std::memory_order_relaxed);
    lastFreedBytes_.store(totalFreedBytes, std::memory_order_relaxed);
    VM::instance().cleanupWeakRegistries();

    CollectionResult result;
    result.unreachable = std::move(unreachable);
    result.freedBytes = totalFreedBytes;
    return result;
}

void SimpleMarkSweepGC::visitRoots(ValueVisitor& visitor) {
    VM& vm = VM::instance();

    std::vector<ptr<Thread>> threadsToVisit;
    threadsToVisit.reserve(vm.threads.size() + 3);
    vm.threads.unsafeApply([&threadsToVisit](const auto& registered) {
        for (const auto& entry : registered) {
            if (entry.second) {
                threadsToVisit.push_back(entry.second);
            }
        }
    });
    if (vm.replThread) {
        threadsToVisit.push_back(vm.replThread);
    }
    if (vm.dataflowEngineThread) {
        threadsToVisit.push_back(vm.dataflowEngineThread);
    }
    if (VM::thread) {
        threadsToVisit.push_back(VM::thread);
    }

    std::unordered_set<Thread*> seenThreads;
    for (const auto& threadPtr : threadsToVisit) {
        if (!threadPtr) {
            continue;
        }
        Thread* raw = threadPtr.get();
        if (!seenThreads.insert(raw).second) {
            continue;
        }
        visitThreadRoots(*raw, visitor);
    }

    vm.globals.unsafeForEachModuleVar([&visitor](const auto& entry) {
        visitStrongValue(visitor, entry.second);
    });
    vm.globals.unsafeForEachGlobal([&visitor](const auto& entry) {
        visitStrongValue(visitor, entry.second);
    });

    visitStrongValue(visitor, vm.dataflowEngineActor);
    visitStrongValue(visitor, vm.conditionalInterruptClosure);
    visitStrongValue(visitor, vm.initString);

    for (const auto& typeEntry : vm.builtinMethods) {
        for (const auto& methodEntry : typeEntry.second) {
            visitStrongValues(visitor, methodEntry.second.defaultValues);
        }
    }

    ObjModuleType::allModules.unsafeApply([&visitor](const auto& modules) {
        for (const auto& moduleVal : modules) {
            visitStrongValue(visitor, moduleVal);
        }
    });

    for (const auto& entry : ObjObjectType::enumTypes) {
        if (entry.second) {
            visitStrongValue(visitor, Value::objRef(entry.second));
        }
    }

}

size_t SimpleMarkSweepGC::collectNowForShutdown() {
    std::vector<Obj*> toDestroy;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        debug_assert_msg(activeThreads_ == 0,
                         "collectNowForShutdown requires no active threads");
        if (collector_) {
            return 0;
        }

        CollectionResult result = performCollection(lock);
        toDestroy = std::move(result.unreachable);
        collectionRequested_.store(false, std::memory_order_release);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
    }

    for (Obj* obj : toDestroy) {
        if (!obj) {
            continue;
        }
        Obj::unrefedObjs.push_back(obj);
    }

    return toDestroy.size();
}

} // namespace roxal
