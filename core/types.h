#pragma once

#include <variant>
#include <optional>

#include <core/common.h>


//
// Representations of Roxal types

namespace roxal::type {



enum class BuiltinType {
    Nil, 
    Bool, Byte, Number, Int, Real, Decimal, 
    String, 
    List, Dict, 
    Vector, Matrix, Tensor, 
    Orient, Stream,
    Func,
    Object, Actor,
    Type
};



struct Type {
    Type() {}
    Type(BuiltinType t) : type(t) {}

    struct FuncType {

        struct Parameter {
            icu::UnicodeString name;
            std::optional<ptr<Type>> type;
            bool hasDefault;
        };

        bool isProc; // proc or func?
        std::vector<ptr<Parameter>> params;
        std::optional<ptr<Type>> returnType; // if specified and not a proc
    };

    struct ObjectType { // Object or Actor type
        icu::UnicodeString name;
        std::optional<ptr<Type>> extends;
        std::vector<ptr<Type>> implements;

        std::vector<std::pair<icu::UnicodeString, ptr<FuncType>>> methods;
    };


    BuiltinType type;

    std::optional<FuncType> func;   // if Func
    std::optional<ObjectType> obj;  // if Object or Actor
};



}
