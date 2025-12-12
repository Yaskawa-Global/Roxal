 #pragma once

#include <variant>
#include <optional>
#include <map>

#include <core/common.h>
#include <core/memory.h>


//
// Representations of Roxal types

namespace roxal::type {



enum class BuiltinType {
    Nil,
    Bool, Byte, Number, Int, Real, Decimal,
    String, Range, Enum,
    List, Dict,
    Vector, Matrix, Signal, Tensor,
    Orient,
    Func,
    Object, Actor,
    Type,
    Event
};

std::string to_string(BuiltinType t);

// check if a value of builtin type `from` can be converted to builtin type
// `to` using the same rules as `toType` in compiler/Value.cpp. The strict flag
// determines whether strict conversions are required (as per conversions.md).
bool convertibleTo(BuiltinType from, BuiltinType to, bool strict=true);



struct Type {
    Type() {}
    Type(BuiltinType bt) : builtin(bt) {}

    struct FuncType {
        FuncType() : isProc(false) {}

        struct ParamType {
            ParamType() {}
            ParamType(const icu::UnicodeString& n) : name(n) {
                nameHashCode = name.hashCode();
            }
            icu::UnicodeString name;
            int32_t nameHashCode; // hashCode() of above (for use at runtime)
            std::optional<ptr<Type>> type;
            bool hasDefault;
        };

        bool isProc; // proc or func?
        std::vector<std::optional<ParamType>> params;
        std::vector<ptr<Type>> returnTypes; // if specified and not a proc

        std::string toString() const;
    };

    struct ObjectType { // Object or Actor type
        icu::UnicodeString name;
        std::optional<ptr<Type>> extends;
        std::vector<ptr<Type>> implements;

       struct PropType {
            PropType() {}
            PropType(const icu::UnicodeString& n) : name(n) {
                nameHashCode = name.hashCode();
            }
            icu::UnicodeString name;
            int32_t nameHashCode; // hashCode() of above (for use at runtime)
            std::optional<ptr<Type>> type;
            bool hasDefault;

            // Property accessor flags (Phase 4 of property accessor implementation)
            // If true, accessing this property calls __get_<name>() or __set_<name>(value)
            bool hasGetter { false };
            bool hasSetter { false };
        };

        std::vector<PropType> properties;
        std::vector<std::pair<icu::UnicodeString, ptr<FuncType>>> methods;

        std::string toString() const;
    };

    struct EnumType {
        icu::UnicodeString name;
        std::optional<ptr<Type>> extends;
        std::vector<std::pair<icu::UnicodeString, int32_t>> values;
    };

    BuiltinType builtin;

    std::optional<FuncType> func;   // if Func
    std::optional<ObjectType> obj;  // if Object or Actor
    std::optional<EnumType> enumer;  // if Enum

    std::string toString() const;
};


struct LexicalScope {
    // func name for func scope, object/actor type name for object/actor type decl scope
    icu::UnicodeString name;

    // symbols declared in this scope (and type if known)
    std::map<icu::UnicodeString, std::optional<ptr<Type>>> symbols;
};


}
