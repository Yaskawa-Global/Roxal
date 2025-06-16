#include <optional>


#include "types.h"

using namespace roxal::type;



// must match index order in BuiltinType
const std::vector<std::string> builtinTypeToString = {
    "nil", "bool", "byte", "number", "int", "real", "decimal",
    "string", "range", "enum", "list", "dict", "vector", "matrix",
    "tensor", "orient", "func", "object", "actor", "type"
};

std::string roxal::type::to_string(BuiltinType t)
{
    if (int(t) < 0 || int(t) >= builtinTypeToString.size())
        throw std::runtime_error("to_string(BuiltinType) unhandled alternative");

    return builtinTypeToString[size_t(t)];
}


std::string Type::toString() const
{
    auto tspec = to_string(builtin);

    if (builtin == BuiltinType::Func) {
        if (func.has_value()) {
            auto funcType { func.value() };
            if (funcType.isProc)
                tspec = "proc";
            tspec += "(";
            for(auto i=0; i<funcType.params.size(); i++) {
                auto param { funcType.params[i] };
                if (param.has_value()) {
                    auto paramType { param.value() };
                    tspec += toUTF8StdString(paramType.name);
                    if (paramType.type.has_value()) {
                        tspec += " :"+paramType.type.value()->toString();
                        if (paramType.hasDefault)
                            tspec += "=";
                    }
                    else {
                        if (paramType.hasDefault)
                            tspec += "=";
                    }
                }
                else
                    tspec += ".";

                if (i != funcType.params.size()-1)
                    tspec += ", ";
            }
            tspec += ")";
            if (funcType.returnType.has_value()) {
                auto returnType { funcType.returnType.value() };
                tspec += "→"+returnType->toString();
            }
        }
        else
            tspec += "(...)";
    }
    else if (builtin == BuiltinType::Enum) {
        if (enumer.has_value()) {
            auto enumType { enumer.value() };
            tspec += "(";
            const auto& enumValues { enumType.values };
            for(auto i=0; i<enumValues.size(); i++) {
                tspec += toUTF8StdString(enumValues[i].first);
                tspec += "="+std::to_string(enumValues[i].second);
                if (i != enumValues.size()-1)
                    tspec += ", ";
            }
            tspec += ")";
        }
    }

    return tspec;
}
