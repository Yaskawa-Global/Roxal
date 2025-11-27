#pragma once

#include <optional>
#include <string>

#include "Value.h"

namespace roxal {

struct CallableInfo {
    std::optional<std::string> signature;
    std::string doc;
};

CallableInfo describeCallable(const Value& target);

} // namespace roxal

