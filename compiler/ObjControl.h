#pragma once
#include <atomic>
#include <cstdint>

namespace roxal {

struct Obj;
struct ObjControl {
    std::atomic_int32_t strong;
    std::atomic_int32_t weak;
    Obj* obj;
    // Set when the GC or the reference counting path has scheduled the
    // object for destruction. Prevents double-enqueueing the same Obj while
    // its destructor runs (for example, for containers that store self
    // references).
    std::atomic<bool> collecting;
    // Records the last collection epoch that marked this object. The GC bumps
    // its global epoch each cycle, letting us treat "marked" as
    // (markEpoch == currentEpoch) without clearing a separate bit on every
    // allocation.
    std::atomic<uint32_t> markEpoch;
};

}
