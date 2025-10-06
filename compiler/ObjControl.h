#pragma once
#include <atomic>
#include <cstdint>

namespace roxal {

struct Obj;
struct ObjControl {
    std::atomic_int32_t strong;
    std::atomic_int32_t weak;
    Obj* obj;
    // Records the last collection epoch that marked this object. The GC bumps
    // its global epoch each cycle, letting us treat "marked" as
    // (markEpoch == currentEpoch) without clearing a separate bit on every
    // allocation.
    std::atomic<uint32_t> markEpoch;
};

}
