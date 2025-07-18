#pragma once

#include <vector>
#include <core/common.h>
#include "Value.h"
#include "ArgsView.h"
#include <core/AST.h>

namespace roxal {

struct BuiltinWrapper {
    NativeFn fn;
    ptr<type::Type> funcType;
    std::vector<Value> defaults;
};

bool callBuiltin(ObjClosure* closure, const CallSpec& callSpec);

}
