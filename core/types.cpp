#include <optional>


#include "types.h"

using namespace roxal::type;




std::string roxal::type::to_string(BuiltinType t)
{
    switch (t) {
        case BuiltinType::Nil: return "nil";
        case BuiltinType::Bool : return "bool";
        case BuiltinType::Byte : return "byte";
        case BuiltinType::Number : return "number";
        case BuiltinType::Int : return "int";
        case BuiltinType::Real : return "real";
        case BuiltinType::Decimal : return "decimal";
        case BuiltinType::String : return "string";
        case BuiltinType::Range : return "range";
        case BuiltinType::List : return "list";
        case BuiltinType::Dict : return "dict";
        case BuiltinType::Vector : return "vector";
        case BuiltinType::Matrix : return "matrix";
        case BuiltinType::Tensor : return "tensor";
        case BuiltinType::Orient : return "orient";
        case BuiltinType::Stream : return "stream";
        case BuiltinType::Func : return "func";
        case BuiltinType::Object : return "object";
        case BuiltinType::Actor : return "actor";
        case BuiltinType::Type : return "type";
        default: throw std::runtime_error("to_string(BuiltinType) unhandled alternative");
    }
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

    return tspec;
}
