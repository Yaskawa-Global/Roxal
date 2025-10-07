#include "ValueGC.h"

#include "Object.h"
#include "Thread.h"
#include "Value.h"
#include "VM.h"

#include <unordered_set>
#include <vector>

namespace {

using namespace roxal;

void visitStrongValue(GCVisitor& visitor, const Value& value)
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
void visitStrongValues(GCVisitor& visitor, const Range& values)
{
    for (const auto& value : values) {
        visitStrongValue(visitor, value);
    }
}

void visitCallFrameRoots(const CallFrame& frame, GCVisitor& visitor)
{
    visitStrongValue(visitor, frame.closure);
    visitStrongValues(visitor, frame.tailArgValues);
}

void visitThreadRoots(Thread& thread, GCVisitor& visitor)
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
}

} // namespace

namespace roxal {

ValueGC& ValueGC::instance() {
    static ValueGC gc;
    return gc;
}

void ValueGC::registerAllocation(ObjControl* control) {
    if (!control) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto [_, inserted] = controls_.insert(control);
    (void)inserted;
    // New allocations start out "unmarked" by setting their mark epoch to 0.
    // During a collection we compare this against the collector's global
    // epoch to decide whether the object was reached, avoiding the need to
    // reset a separate boolean on every cycle.
    control->collecting.store(false, std::memory_order_relaxed);
    control->markEpoch.store(0u, std::memory_order_relaxed);
}

void ValueGC::unregisterAllocation(ObjControl* control) {
    if (!control) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    controls_.erase(control);
}

void ValueGC::requestCollect() {
    collectionRequested_.store(true, std::memory_order_release);
    VM::instance().wakeAllThreadsForGC();
}

bool ValueGC::isCollectionRequested() const noexcept {
    return collectionRequested_.load(std::memory_order_acquire);
}

uint32_t ValueGC::currentEpoch() const noexcept {
    return epoch_.load(std::memory_order_relaxed);
}

void ValueGC::onThreadEnter() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++activeThreads_;
    safepointCv_.notify_all();
}

void ValueGC::onThreadExit() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeThreads_ > 0) {
        --activeThreads_;
    }
    safepointCv_.notify_all();
}

void ValueGC::safepoint(Thread& currentThread) {
    if (!collectionRequested_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<Obj*> toDestroy;

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
        toDestroy = performCollection(lock);
        collectionRequested_.store(false, std::memory_order_release);
        collector_ = nullptr;
        --threadsAtSafepoint_;
    }

    safepointCv_.notify_all();

    for (Obj* obj : toDestroy) {
        if (!obj) {
            continue;
        }
        Obj::unrefedObjs.push_back(obj);
    }

    if (!toDestroy.empty()) {
        VM::instance().freeObjects();
    }
}

std::vector<Obj*> ValueGC::performCollection(std::unique_lock<std::mutex>& lock) {
    (void)lock;

    const uint32_t epoch = epoch_.fetch_add(1u, std::memory_order_relaxed) + 1u;

    struct MarkWorklist : GCVisitor {
        explicit MarkWorklist(uint32_t epoch) : epoch(epoch) {}

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

            uint32_t previous = control->markEpoch.load(std::memory_order_relaxed);
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

        uint32_t epoch;
        std::vector<Obj*> worklist;
    } marker(epoch);

    visitRoots(marker);
    marker.drain();

    std::vector<Obj*> unreachable;
    unreachable.reserve(controls_.size());
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
        unreachable.push_back(obj);
    }

    return unreachable;
}

void ValueGC::visitRoots(GCVisitor& visitor) {
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

    visitInternedStrings([&visitor](ObjString* str) {
        if (str) {
            visitStrongValue(visitor, Value::objRef(str));
        }
    });
}

} // namespace roxal
