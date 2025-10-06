#pragma once

#include <atomic>
#include <mutex>
#include <unordered_set>

#include "ObjControl.h"

namespace roxal {

class Value;

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
    void collectNow();

    bool isCollectionRequested() const noexcept;
    uint32_t currentEpoch() const noexcept;

    void visitRoots(GCVisitor& visitor);

private:
    ValueGC() = default;

    void performCollection(std::unique_lock<std::mutex>& lock);

    mutable std::mutex mutex_;
    std::unordered_set<ObjControl*> controls_;
    std::atomic<uint32_t> epoch_{1};
    std::atomic<bool> collectionRequested_{false};
};

} // namespace roxal
