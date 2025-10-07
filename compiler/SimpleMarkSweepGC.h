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

class ValueVisitor;

class SimpleMarkSweepGC {
public:
    static SimpleMarkSweepGC& instance();

    void registerAllocation(ObjControl* control);
    void unregisterAllocation(ObjControl* control);

    void requestCollect();
    void safepoint(Thread& currentThread);

    void onThreadEnter();
    void onThreadExit();

    bool isCollectionRequested() const noexcept;
    uint32_t currentEpoch() const noexcept;
    size_t lastCollectionFreed() const noexcept;

    void visitRoots(ValueVisitor& visitor);

private:
    SimpleMarkSweepGC() = default;

    std::vector<Obj*> performCollection(std::unique_lock<std::mutex>& lock);

    mutable std::mutex mutex_;
    std::unordered_set<ObjControl*> controls_;
    std::condition_variable safepointCv_;
    std::atomic<uint32_t> epoch_{1};
    std::atomic<bool> collectionRequested_{false};
    std::atomic<size_t> lastFreedCount_{0};
    size_t activeThreads_{0};
    size_t threadsAtSafepoint_{0};
    Thread* collector_{nullptr};
};

} // namespace roxal
