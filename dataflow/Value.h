#pragma once
#include <atomic>
#include <optional>
#include <functional>
#include <variant>
#include <map>

#include "core/common.h"

#include <cassert>

// NB: this will be switched for roxal::Value in the future
namespace df {

enum class ValueType {
    Nil,
    Bool,
    Byte,
    Int,
    Real,
    Vector
};


class Value {
public:
    Value();
    explicit Value(bool bit);
    explicit Value(uint8_t byte);
    explicit Value(int32_t integer);
    explicit Value(double real);
    explicit Value(const ptr<std::vector<Value>> vec);

    static Value doubleVector(const std::vector<double>& elts);

    bool isNil() const { return type_ == ValueType::Nil; }
    bool isBool() const { return type_ == ValueType::Bool; }
    bool isByte() const { return type_ == ValueType::Byte; }
    bool isInt() const { return type_ == ValueType::Int; }
    bool isReal() const { return type_ == ValueType::Real; }
    bool isVector() const { return type_ == ValueType::Vector; }

    bool isNumber() const { return isInt() || isReal() || isByte(); }

    bool asBool() const;
    uint8_t asByte() const;
    int32_t asInt() const;
    double asReal() const;
    ptr<std::vector<Value>> asVector() const;

    size_t vectorSize() const;

    Value& operator=(const Value& v);
    bool operator==(const Value& v) const;
    bool operator!=(const Value& v) const { return !operator==(v); }

    bool equals(const Value& v, double eps=1e-15) const;

    std::string toString() const;

protected:
    ValueType type_;
    std::variant<bool, uint8_t, int32_t, double, ptr<std::vector<Value>>> value_;

    friend class DataflowEngine;
};


Value vecMult(double s, const Value& v);
Value vecMult(int32_t s, const Value& v);
Value vecAdd(const Value& v1, const Value& v2);
Value vecSub(const Value& v1, const Value& v2);


inline std::ostream& operator<<(std::ostream& os, const Value& v) {
    os << v.toString();
    return os;
}

typedef std::vector<Value> Values;
typedef std::map<std::string, Value> NamedValues;


}
