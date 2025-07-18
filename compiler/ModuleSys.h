#pragma once

#include "BuiltinModule.h"
#include "Value.h"
#include "Object.h"

namespace roxal {

class ModuleSys : public BuiltinModule {
public:
    ModuleSys();

    // Register builtin sys functions and natives
    void registerBuiltins(VM& vm) override;

    inline ObjModuleType* moduleType() const { return asModuleType(moduleTypeValue); }

private:
    Value moduleTypeValue; // ObjModuleType*
};

}
