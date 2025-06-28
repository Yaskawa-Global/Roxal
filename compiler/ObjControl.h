#pragma once
#include <atomic>

namespace roxal {

struct Obj;
struct ObjControl {
    std::atomic_int32_t strong;
    std::atomic_int32_t weak;
    Obj* obj;
};

}
