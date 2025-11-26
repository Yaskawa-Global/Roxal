#pragma once

#ifdef ROXAL_ENABLE_DDS

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>

#include "Value.h"

namespace roxal {

class DdsAdapter {
public:
    DdsAdapter();
    ~DdsAdapter();

    std::vector<Value> allocateTypes(const std::string& idlFile);
    std::string packageName() const { return lastPackage_; }

private:
    struct ParsedType {
        std::string fullName;
        Value value;
    };

    std::string lastPackage_;
};

} // namespace roxal

#endif // ROXAL_ENABLE_DDS
