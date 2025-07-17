#pragma once

#include <core/common.h>

namespace roxal {

class VM;
struct ObjModuleType;

class BuiltinModule {
public:
    virtual ~BuiltinModule() = default;
    virtual void registerBuiltins(VM& vm) = 0;
    virtual ObjModuleType* moduleType() const = 0;
};

}
