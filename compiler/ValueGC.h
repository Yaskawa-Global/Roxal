#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "ObjControl.h"

namespace roxal {

class Value;
class Thread;

class GCVisitor {
public:
    virtual ~GCVisitor() = default;

    /// Visit a strong reference held in a Value payload.
    virtual void visit(const Value& value) = 0;
};

class ValueGC {
public:
    static ValueGC& instance();

    void registerAllocation(ObjControl* control);
    void unregisterAllocation(ObjControl* control);

    void requestCollect();
    void safepoint(Thread& currentThread);

    void onThreadEnter();
    void onThreadExit();

    bool isCollectionRequested() const noexcept;
    uint32_t currentEpoch() const noexcept;

    void visitRoots(GCVisitor& visitor);

private:
    ValueGC() = default;

    std::vector<Obj*> performCollection(std::unique_lock<std::mutex>& lock);

    mutable std::mutex mutex_;
    std::unordered_set<ObjControl*> controls_;
    std::condition_variable safepointCv_;
    std::atomic<uint32_t> epoch_{1};
    std::atomic<bool> collectionRequested_{false};
    size_t activeThreads_{0};
    size_t threadsAtSafepoint_{0};
    Thread* collector_{nullptr};
};

} // namespace roxal
