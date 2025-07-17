#pragma once

#include <core/common.h>
#include "Value.h"
#include <core/types.h>

namespace roxal {

class VM;
struct ObjModuleType;

class BuiltinModule {
public:
    virtual ~BuiltinModule() = default;
    virtual void registerBuiltins(VM& vm) = 0;
    virtual ObjModuleType* moduleType() const = 0;

protected:
    // Helper to construct function parameter type information
    static std::vector<type::Type::FuncType::ParamType>
    constructParams(const std::vector<std::pair<std::string, type::BuiltinType>>& infos,
                    const std::vector<Value>& defaults);
};

inline std::vector<type::Type::FuncType::ParamType>
BuiltinModule::constructParams(const std::vector<std::pair<std::string, type::BuiltinType>>& infos,
                               const std::vector<Value>& defaults)
{
    using PT = type::Type::FuncType::ParamType;
    std::vector<PT> params;
    params.reserve(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
        PT p(toUnicodeString(infos[i].first));
        p.type = make_ptr<type::Type>(infos[i].second);
        if (i < defaults.size() && !defaults[i].isNil())
            p.hasDefault = true;
        else
            p.hasDefault = false;
        params.push_back(p);
    }
    return params;
}

} // namespace roxal
