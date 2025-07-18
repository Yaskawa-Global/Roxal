#pragma once

#include <core/common.h>
#include "Value.h"
#include <core/types.h>
#include <optional>

namespace roxal {

class VM;
struct ObjModuleType;

class BuiltinModule {
public:
    virtual ~BuiltinModule() = default;
    virtual void registerBuiltins(VM& vm) = 0;
    virtual ObjModuleType* moduleType() const = 0;

protected:
    // Helper to construct function parameter type information. Parameter types
    // may be omitted (nullopt) for untyped parameters.
    static std::vector<type::Type::FuncType::ParamType>
    constructParams(const std::vector<std::pair<std::string,
                                                std::optional<type::BuiltinType>>>& infos,
                    const std::vector<Value>& defaults);

    // Convenience helper to build a Func type descriptor
    static ptr<type::Type>
    makeFuncType(const std::vector<std::pair<std::string,
                                             std::optional<type::BuiltinType>>>& infos,
                 const std::vector<Value>& defaults = {},
                 bool isProc = false);
};

inline std::vector<type::Type::FuncType::ParamType>
BuiltinModule::constructParams(const std::vector<std::pair<std::string,
                                                std::optional<type::BuiltinType>>>& infos,
                               const std::vector<Value>& defaults)
{
    using PT = type::Type::FuncType::ParamType;
    std::vector<PT> params;
    params.reserve(infos.size());
    for (size_t i = 0; i < infos.size(); ++i) {
        PT p(toUnicodeString(infos[i].first));
        if (infos[i].second.has_value())
            p.type = make_ptr<type::Type>(infos[i].second.value());
        if (i < defaults.size() && !defaults[i].isNil())
            p.hasDefault = true;
        else
            p.hasDefault = false;
        params.push_back(p);
    }
    return params;
}

inline ptr<type::Type>
BuiltinModule::makeFuncType(const std::vector<std::pair<std::string,
                                            std::optional<type::BuiltinType>>>& infos,
                            const std::vector<Value>& defaults,
                            bool isProc)
{
    auto t = make_ptr<type::Type>(type::BuiltinType::Func);
    t->func = type::Type::FuncType();
    t->func->isProc = isProc;
    auto params = constructParams(infos, defaults);
    t->func->params.resize(params.size());
    for(size_t i=0;i<params.size();++i) t->func->params[i]=params[i];
    return t;
}

} // namespace roxal
