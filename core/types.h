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

// Member visibility — paralleled by ast::Access (which uses the same
// underlying ordering). Defined here so the static type system can carry
// access information without requiring AST.h.
enum class Access : uint8_t { Public = 0, Private = 1 };

std::string to_string(BuiltinType t);
std::optional<BuiltinType> builtinTypeFromName(const std::string& name);

// True when nil is a valid coercion target for this builtin type.
// Mirrors isNilAcceptableTargetType(ValueType) in compiler/Value.h — the
// reference-identity types (handles whose "no value yet" state is meaningful).
inline bool isNilAcceptableTargetBuiltinType(BuiltinType t) {
    switch (t) {
        case BuiltinType::String:
        case BuiltinType::List:
        case BuiltinType::Dict:
        case BuiltinType::Object:
        case BuiltinType::Actor:
        case BuiltinType::Signal:
        case BuiltinType::Event:
        case BuiltinType::Func:
        case BuiltinType::Tensor:
            return true;
        default:
            return false;
    }
}

// check if a value of builtin type `from` can be converted to builtin type
// `to` using the same rules as `toType` in compiler/Value.cpp. The strict flag
// determines whether strict conversions are required (as per conversions.md).
bool convertibleTo(BuiltinType from, BuiltinType to, bool strict=true);

// Check if a value of sourceType is acceptable where targetType is expected,
// without any conversion. For builtin types: identity only. For object/actor
// types: identity or sourceType is a subtype (extends/implements) of targetType.
// This is distinct from convertibleTo() which checks if conversion is possible.
// Note: for object/actor types, this only checks the BuiltinType-level match
// (Object==Object, Actor==Actor). Full inheritance checking requires runtime
// ObjObjectType access — see VM::isTypeAssignable() for the complete check.
bool isAssignableFrom(BuiltinType target, BuiltinType source);


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
            bool hasDefault = false;
            bool variadic = false;  // true if ...name syntax (collects remaining positional args)
        };

        bool isProc; // proc or func?
        std::vector<std::optional<ParamType>> params;
        std::vector<ptr<Type>> returnTypes; // if specified and not a proc

        std::string toString() const;

        // Returns true if this function has a variadic parameter (must be last param)
        bool hasVariadic() const {
            return !params.empty() &&
                   params.back().has_value() &&
                   params.back().value().variadic;
        }
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
            Access access { Access::Public };

            // Property accessor flags
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
    bool isConst { false };  // type is const-qualified (e.g. const List, const O)

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
