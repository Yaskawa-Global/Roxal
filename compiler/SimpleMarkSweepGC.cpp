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

    for (const auto& pending : thread.pendingConversions) {
        visitStrongValue(visitor, pending.savedLHS);
        visitStrongValue(visitor, pending.convReceiver);
    }
    for (const auto& guard : thread.conversionInProgress) {
        visitStrongValue(visitor, guard.receiver);
    }

    for (const auto& entry : thread.eventHandlers) {
        visitStrongValue(visitor, entry.first);
        for (const auto& reg : entry.second) {
            visitStrongValue(visitor, reg.closure);
            if (reg.matchValue.has_value())
                visitStrongValue(visitor, reg.matchValue.value());
            if (reg.targetFilter.has_value())
                visitStrongValue(visitor, reg.targetFilter.value());
        }
    }

    for (const auto& entry : thread.eventToSignal) {
        visitStrongValue(visitor, entry.first);
        visitStrongValue(visitor, entry.second);
    }

    thread.pendingEvents.unsafeVisit([&visitor](const auto& pendingEvents) {
        for (const auto& pending : pendingEvents) {
            visitStrongValue(visitor, pending.eventType);
            visitStrongValue(visitor, pending.instance);
        }
    });

    // Trace event dispatch state (active handler dispatch in progress)
    if (thread.eventDispatch.active) {
        visitStrongValue(visitor, thread.eventDispatch.currentEvent.eventType);
        visitStrongValue(visitor, thread.eventDispatch.currentEvent.instance);
        for (const auto& reg : thread.eventDispatch.handlerSnapshot) {
            visitStrongValue(visitor, reg.closure);
            if (reg.matchValue.has_value())
                visitStrongValue(visitor, reg.matchValue.value());
            if (reg.targetFilter.has_value())
                visitStrongValue(visitor, reg.targetFilter.value());
        }
    }

    visitStrongValue(visitor, thread.currentActorCall);
    visitStrongValue(visitor, thread.currentBoundCall);
#ifdef ROXAL_COMPUTE_SERVER
    if (thread.remoteComputeCallState.active) {
        for (const auto& arg : thread.remoteComputeCallState.args) {
            visitStrongValue(visitor, arg);
        }
        visitStrongValue(visitor, thread.remoteComputeCallState.completionFuture);
        visitStrongValue(visitor, thread.remoteComputeCallState.result);
    }
#endif
    visitStrongValue(visitor, thread.pendingWaitFor);
    visitStrongValue(visitor, thread.awaitedFuture);
    for (const auto& session : thread.stmtActionStack) {
        visitStrongValue(visitor, session.lastReceiver);
    }
    visitStrongValue(visitor, thread.pendingConstructorInstance);
    if (thread.waitSuspension.active) {
        visitStrongValue(visitor, thread.waitSuspension.storedValue);
    }

    // Trace native continuation stack (e.g., filter/map/reduce iteration, param conversions)
    for (const auto& cont : thread.nativeContinuationStack) {
        visitStrongValue(visitor, cont.state);
    }

    // Trace native default param state stack
    for (const auto& state : thread.nativeDefaultParamStack) {
        visitStrongValue(visitor, state.receiver);
        visitStrongValue(visitor, state.declFunction);
        for (const auto& val : state.argsBuffer) {
            visitStrongValue(visitor, val);
        }
        for (const auto& entry : state.paramDefaultFuncs) {
            visitStrongValue(visitor, entry.second);
        }
    }

    // Trace native param conversion state stack
    for (const auto& state : thread.nativeParamConversionStack) {
        visitStrongValue(visitor, state.receiver);
        visitStrongValue(visitor, state.declFunction);
        for (const auto& val : state.argsBuffer) {
            visitStrongValue(visitor, val);
        }
    }

    // Trace closure param conversion stack (deferred closure call with async param conversions)
    for (const auto& state : thread.closureParamConversionStack) {
        visitStrongValue(visitor, state.moduleType);
    }
}

} // namespace

namespace roxal {

SimpleMarkSweepGC& SimpleMarkSweepGC::instance() {
    static SimpleMarkSweepGC gc;
    return gc;
}

SimpleMarkSweepGC::ExternalParticipant::ExternalParticipant(SimpleMarkSweepGC& gc)
    : gc_(&gc), id_(gc.nextExternalParticipantId_.fetch_add(1, std::memory_order_relaxed))
{
    gc_->externalParticipantEnter(id_);
}

SimpleMarkSweepGC::ExternalParticipant::~ExternalParticipant()
{
    if (gc_ != nullptr) {
        gc_->externalParticipantExit(id_);
    }
}

void SimpleMarkSweepGC::ExternalParticipant::setRootVisitor(std::function<void(ValueVisitor&)> visitor)
{
    if (gc_ != nullptr) {
        gc_->externalParticipantSetVisitor(id_, std::move(visitor));
    }
}

void SimpleMarkSweepGC::ExternalParticipant::clearRootVisitor()
{
    if (gc_ != nullptr) {
        gc_->externalParticipantClearVisitor(id_);
    }
}

void SimpleMarkSweepGC::ExternalParticipant::pollSafepointIfRequested()
{
    if (gc_ != nullptr) {
        gc_->externalParticipantSafepoint(id_);
    }
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
    if (!gcEnabled_.load(std::memory_order_acquire)) {
        return;
    }
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

void SimpleMarkSweepGC::setEnabled(bool enabled) noexcept {
    gcEnabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        collectionRequested_.store(false, std::memory_order_release);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
        lastRequestedBytes_.store(0, std::memory_order_relaxed);
    }
}

bool SimpleMarkSweepGC::isEnabled() const noexcept {
    return gcEnabled_.load(std::memory_order_acquire);
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

void SimpleMarkSweepGC::registerExternalRootProvider(ExternalRootProvider* provider)
{
    if (!provider) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    externalRootProviders_.insert(provider);
}

void SimpleMarkSweepGC::unregisterExternalRootProvider(ExternalRootProvider* provider)
{
    if (!provider) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    externalRootProviders_.erase(provider);
}

void SimpleMarkSweepGC::externalParticipantEnter(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    externalParticipants_[id] = ExternalParticipantState {};
    ++activeThreads_;
    safepointCv_.notify_all();
}

void SimpleMarkSweepGC::externalParticipantExit(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = externalParticipants_.find(id);
    if (it != externalParticipants_.end()) {
        if (it->second.atSafepoint && threadsAtSafepoint_ > 0) {
            --threadsAtSafepoint_;
        }
        externalParticipants_.erase(it);
    }
    if (activeThreads_ > 0) {
        --activeThreads_;
    }
    safepointCv_.notify_all();
}

void SimpleMarkSweepGC::externalParticipantSetVisitor(
        std::uint64_t id, std::function<void(ValueVisitor&)> visitor)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = externalParticipants_.find(id);
    if (it != externalParticipants_.end()) {
        it->second.rootVisitor = std::move(visitor);
    }
}

void SimpleMarkSweepGC::externalParticipantClearVisitor(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = externalParticipants_.find(id);
    if (it != externalParticipants_.end()) {
        it->second.rootVisitor = nullptr;
    }
}

void SimpleMarkSweepGC::safepoint(Thread& currentThread) {
    if (!gcEnabled_.load(std::memory_order_acquire) ||
        !collectionRequested_.load(std::memory_order_acquire)) {
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

        if ((collector_ && collector_ != &currentThread) || externalCollectorActive_) {
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
        externalCollectorActive_ = false;
        externalCollectorThread_ = std::thread::id {};
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

void SimpleMarkSweepGC::externalParticipantSafepoint(std::uint64_t id)
{
    if (!gcEnabled_.load(std::memory_order_acquire) ||
        !collectionRequested_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<Obj*> toDestroy;

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!collectionRequested_.load(std::memory_order_acquire)) {
            return;
        }

        auto stateIt = externalParticipants_.find(id);
        if (stateIt == externalParticipants_.end()) {
            return;
        }

        if (!stateIt->second.atSafepoint) {
            stateIt->second.atSafepoint = true;
            ++threadsAtSafepoint_;
            safepointCv_.notify_all();
        }

        while (collectionRequested_.load(std::memory_order_acquire) &&
               threadsAtSafepoint_ < activeThreads_) {
            safepointCv_.wait(lock);
        }

        if (!collectionRequested_.load(std::memory_order_acquire)) {
            stateIt = externalParticipants_.find(id);
            if (stateIt != externalParticipants_.end() && stateIt->second.atSafepoint) {
                stateIt->second.atSafepoint = false;
                --threadsAtSafepoint_;
            }
            safepointCv_.notify_all();
            return;
        }

        const bool currentIsExternalCollector =
                externalCollectorActive_ && externalCollectorThread_ == std::this_thread::get_id();
        if ((collector_ != nullptr) || (externalCollectorActive_ && !currentIsExternalCollector)) {
            while (collectionRequested_.load(std::memory_order_acquire)) {
                safepointCv_.wait(lock);
            }
            stateIt = externalParticipants_.find(id);
            if (stateIt != externalParticipants_.end() && stateIt->second.atSafepoint) {
                stateIt->second.atSafepoint = false;
                --threadsAtSafepoint_;
            }
            safepointCv_.notify_all();
            return;
        }

        externalCollectorActive_ = true;
        externalCollectorThread_ = std::this_thread::get_id();
        CollectionResult result = performCollection(lock);
        toDestroy = std::move(result.unreachable);
        collectionRequested_.store(false, std::memory_order_release);
        bytesAllocatedSinceLastCollect_.store(0, std::memory_order_relaxed);
        externalCollectorActive_ = false;
        externalCollectorThread_ = std::thread::id {};

        stateIt = externalParticipants_.find(id);
        if (stateIt != externalParticipants_.end() && stateIt->second.atSafepoint) {
            stateIt->second.atSafepoint = false;
            --threadsAtSafepoint_;
        }
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

    // MVCC: mark version chain snapshots.
    // Version chain entries hold raw Obj* snapshots that aren't reachable via
    // normal Value tracing.  We must mark them so the sweep doesn't collect
    // objects that an active snapshot may still need for version resolution.
    for (ObjControl* control : controls_) {
        if (!control || !control->obj)
            continue;
        if (control->markEpoch.load(std::memory_order_relaxed) != epoch)
            continue;
        ObjVersion* ver = control->versionChain.load(std::memory_order_relaxed);
        while (ver) {
            if (ver->snapshot && ver->snapshot->control) {
                ObjControl* snapCtrl = ver->snapshot->control;
                if (!snapCtrl->collecting.load(std::memory_order_relaxed) &&
                    snapCtrl->markEpoch.load(std::memory_order_relaxed) != epoch) {
                    snapCtrl->markEpoch.store(epoch, std::memory_order_relaxed);
                    // Trace the snapshot's children too (they may hold refs)
                    marker.worklist.push_back(ver->snapshot);
                }
            }
            ver = ver->prev;
        }
    }
    marker.drain();

    // MVCC: trim version chains on live objects.
    // All threads are at a safepoint, so no concurrent readers.
    {
        uint64_t minEpoch = snapshotEpochTracker.minEpoch();
        for (ObjControl* control : controls_) {
            if (!control || !control->obj)
                continue;
            if (control->collecting.load(std::memory_order_relaxed))
                continue;
            // Only trim marked (live) objects that have a version chain
            if (control->markEpoch.load(std::memory_order_relaxed) == epoch
                && control->versionChain.load(std::memory_order_relaxed) != nullptr) {
                control->obj->trimVersionChain(minEpoch);
            }
        }
    }

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
        visitStrongValue(visitor, entry.second.value);
        if (entry.second.hasSignal())
            visitStrongValue(visitor, entry.second.signal);
    });
    vm.globals.unsafeForEachGlobal([&visitor](const auto& entry) {
        visitStrongValue(visitor, entry.second.value);
        if (entry.second.hasSignal())
            visitStrongValue(visitor, entry.second.signal);
    });

    visitStrongValue(visitor, vm.dataflowEngineActor);
    visitStrongValue(visitor, vm.conditionalInterruptClosure);
    visitStrongValue(visitor, vm.initString);

    // RT REPL pending closure (in-flight between setupLine and runFor)
    if (vm.pendingRTClosure_.isObj())
        visitStrongValue(visitor, vm.pendingRTClosure_);

    for (const auto& typeEntry : vm.builtinMethods) {
        for (const auto& methodEntry : typeEntry.second) {
            methodEntry.second.trace(visitor);
        }
    }

    ObjModuleType::allModules.unsafeApply([&visitor](const auto& modules) {
        for (const auto& moduleVal : modules) {
            visitStrongValue(visitor, moduleVal);
        }
    });

    // Note: ObjObjectType::enumTypes is NOT visited here. It holds raw pointers for
    // fast enum value -> type lookup, but enum types are already rooted through modules:
    // - User-defined enums are stored in module vars via DefineModuleVar opcode
    // - DDS/Proto enums are stored in module vars via registerGeneratedTypes()
    // - ObjModuleType::trace() traces all vars, so enum types are visited via allModules above
    for (ExternalRootProvider* provider : externalRootProviders_) {
        if (provider != nullptr) {
            provider->visitRoots(visitor);
        }
    }
    for (auto& entry : externalParticipants_) {
        if (entry.second.rootVisitor) {
            entry.second.rootVisitor(visitor);
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
