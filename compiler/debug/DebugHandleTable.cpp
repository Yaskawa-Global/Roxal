#include "DebugHandleTable.h"

#include <limits>

#include "../Object.h"   // isObjPrimitive inline (Value.h forward-declares it)

namespace roxal {

DebugHandleTable::DebugHandleTable() = default;

void DebugHandleTable::traceEntries(
    ValueVisitor& visitor, const std::unordered_map<int32_t, DebugHandle>& m)
{
    for (const auto& kv : m)
        if (kv.second.value.isObj())
            visitor.visit(kv.second.value);
}

int32_t DebugHandleTable::create(const DebugHandle& h)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_->size() >= maxHandles_)
        return 0;
    if (nextId_ == std::numeric_limits<int32_t>::max())
        return 0;   // id space exhausted for this session; never wrap/reuse
    const int32_t id = nextId_++;
    entries_->emplace(id, h);
    return id;
}

std::optional<DebugHandle> DebugHandleTable::lookup(int32_t id, uint64_t currentEpoch)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_->find(id);
    if (it == entries_->end())
        return std::nullopt;
    if (it->second.epoch != currentEpoch)
        return std::nullopt;   // stale generation: reject deterministically
    return it->second;
}

void DebugHandleTable::clearForResume()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_->clear();
}

size_t DebugHandleTable::size()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_->size();
}

} // namespace roxal
