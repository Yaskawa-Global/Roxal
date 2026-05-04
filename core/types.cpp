#include <optional>


#include "types.h"

using namespace roxal::type;



// must match index order in BuiltinType
const std::vector<std::string> builtinTypeToString = {
    "nil", "bool", "byte", "number", "int", "real", "decimal",
    "string", "range", "enum", "list", "dict", "vector", "matrix",
    "signal", "tensor", "orient", "func", "object", "actor", "type", "event"
};

std::string roxal::type::to_string(BuiltinType t)
{
    if (int(t) < 0 || int(t) >= builtinTypeToString.size())
        throw std::runtime_error("to_string(BuiltinType) unhandled alternative");

    return builtinTypeToString[size_t(t)];
}

std::optional<BuiltinType> roxal::type::builtinTypeFromName(const std::string& name)
{
    for (size_t i = 0; i < builtinTypeToString.size(); ++i) {
        if (builtinTypeToString[i] == name)
            return static_cast<BuiltinType>(i);
    }
    return std::nullopt;
}

bool roxal::type::convertibleTo(BuiltinType from, BuiltinType to, bool strict)
{
    if (from == to)
        return true;

    // nil flows into reference-identity target types; rejected for value-shaped
    // types (range, vector, matrix, orient, enum, primitives).
    if (from == BuiltinType::Nil)
        return isNilAcceptableTargetBuiltinType(to);

    auto idx = [](BuiltinType t) -> int {
        switch(t) {
            case BuiltinType::Bool:    return 0;
            case BuiltinType::Byte:    return 1;
            case BuiltinType::Int:     return 2;
            case BuiltinType::Real:    return 3;
            case BuiltinType::Decimal: return 4;
            case BuiltinType::String:  return 5;
            case BuiltinType::Enum:    return 6;
            default: return -1;
        }
    };

    int fi = idx(from);
    int ti = idx(to);
    if (fi >= 0 && ti >= 0) {
        static const bool nonStrict[7][7] = {
            // to:  bool, byte, int, real, decimal, string, enum
            /*bool*/   {true, true, true, true, true, true, false},
            /*byte*/   {true, true, true, true, true, true, false},
            /*int*/    {true, true, true, true, true, true, false},
            /*real*/   {true, true, true, true, true, true, false},
            /*decimal*/{true, true, true, true, true, true, false},
            /*string*/ {true, true, true, true, true, true, true},
            /*enum*/   {false,false,true,false,false,true,true}
        };

        static const bool strictTab[7][7] = {
            /*bool*/   {true, true, true, true, true, true, false},
            /*byte*/   {false,true, true, true, true, true, false},
            /*int*/    {false,false,true, true, true, true, false},
            /*real*/   {false,false,false,true,false, true, false},
            /*decimal*/{false,false,false,false,true, true, false},
            /*string*/ {false,false,false,false,false,true, false},
            /*enum*/   {false,false,true, false,false,true, true}
        };

        return strict ? strictTab[fi][ti] : nonStrict[fi][ti];
    }

    if (to == BuiltinType::Range)
        return !strict; // any type to range in non-strict mode
    if (to == BuiltinType::List && from == BuiltinType::Range)
        return !strict;
    if (to == BuiltinType::Dict && from == BuiltinType::Object)
        return !strict;

    // Object/Actor → builtin: allow at compile time because user-defined
    // conversion operators (operator->T) may handle it at runtime.
    // The VM's tryConvertValue will produce a runtime error if no operator exists.
    if ((from == BuiltinType::Object || from == BuiltinType::Actor) && ti >= 0)
        return true;

    return false;
}


bool roxal::type::isAssignableFrom(BuiltinType target, BuiltinType source)
{
    if (target == source)
        return true;
    // Object and Actor are their own categories — no cross-assignment.
    // Inheritance checking (subtype walks) requires ObjObjectType at runtime;
    // see VM::isTypeAssignable() for the full check.
    return false;
}


std::string Type::toString() const
{
    auto tspec = (isConst ? "const " : "") + to_string(builtin);

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
            if (!funcType.returnTypes.empty()) {
                tspec += "→";
                if (funcType.returnTypes.size() == 1) {
                    tspec += funcType.returnTypes[0]->toString();
                } else {
                    tspec += "[";
                    for (size_t i = 0; i < funcType.returnTypes.size(); i++) {
                        if (i > 0) tspec += ", ";
                        tspec += funcType.returnTypes[i]->toString();
                    }
                    tspec += "]";
                }
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
