#include "Value.h"

#include <stdexcept>
#include <sstream>

using namespace df;

using roxal::ptr;
using roxal::make_ptr;


Value::Value()
{
    type_ = ValueType::Nil;
}

Value::Value(bool bit)
{
    type_ = ValueType::Bool;
    value_ = bit;
}

Value::Value(uint8_t byte)
{
    type_ = ValueType::Byte;
    value_ = byte;
}

Value::Value(int32_t integer)
{
    type_ = ValueType::Int;
    value_ = integer;
}

Value::Value(double real)
{
    type_ = ValueType::Real;
    value_ = real;
}

Value::Value(const ptr<std::vector<Value>> vec)
{
    type_ = ValueType::Vector;
    value_ = vec;
}


Value Value::doubleVector(const std::vector<double>& elts)
{
    auto vec = make_ptr<std::vector<Value>>();
    vec->reserve(elts.size());
    for (auto& elt : elts)
        vec->push_back(Value(elt));
    return Value(vec);
}




Value& Value::operator=(const Value& v)
{
    type_ = v.type_;
    value_ = v.value_;
    return *this;
}


bool Value::operator==(const Value& v) const
{
    if (type_ != v.type_) {
        return false;
    }
    else if (type_ == ValueType::Vector) {
        return *std::get<ptr<std::vector<Value>>>(value_) == *std::get<ptr<std::vector<Value>>>(v.value_);
    }
    return value_ == v.value_;
}


bool Value::equals(const Value& v, double eps) const
{
    bool bothReal = (type_ == ValueType::Real) && (v.type_ == ValueType::Real);
    bool bothVectors = (type_ == ValueType::Vector) && (v.type_ == ValueType::Vector);

    if (!bothReal && !bothVectors)
        return *this == v;

    if (bothReal)
        return std::abs(std::get<double>(value_) - std::get<double>(v.value_)) < eps;

    if (std::get<ptr<std::vector<Value>>>(value_)->size() != std::get<ptr<std::vector<Value>>>(v.value_)->size())
        return false;

    const auto& value { std::get<ptr<std::vector<Value>>>(value_) };
    const auto& other { std::get<ptr<std::vector<Value>>>(v.value_) };
    for (size_t i = 0; i < value->size(); ++i) {
        if (!value->at(i).equals(other->at(i), eps))
            return false;
    }
    return true;
}


bool Value::asBool() const
{
    if (type_ == ValueType::Bool) {
        return std::get<bool>(value_);
    }
    else if (type_ == ValueType::Byte) {
        return std::get<uint8_t>(value_) > 0;
    }
    else if (type_ == ValueType::Int) {
        return std::get<int32_t>(value_) > 0;
    }
    else if (type_ == ValueType::Real) {
        return std::get<double>(value_) != 0;
    }
    else if (type_ == ValueType::Vector) {
        return !std::get<ptr<std::vector<Value>>>(value_)->empty();
    }
    return false;
}

uint8_t Value::asByte() const
{
    if (type_ == ValueType::Byte) {
        return std::get<uint8_t>(value_);
    }
    else if (type_ == ValueType::Bool) {
        return std::get<bool>(value_) ? 1 : 0;
    }
    else if (type_ == ValueType::Int) {
        return std::get<int32_t>(value_);
    }
    else if (type_ == ValueType::Real) {
        return std::get<double>(value_);
    }
    else if (type_ == ValueType::Vector) {
        throw std::runtime_error("Cannot convert vector to byte");
    }
    return 0;
}

int32_t Value::asInt() const
{
    if (type_ == ValueType::Int) {
        return std::get<int32_t>(value_);
    }
    else if (type_ == ValueType::Bool) {
        return std::get<bool>(value_) ? 1 : 0;
    }
    else if (type_ == ValueType::Byte) {
        return std::get<uint8_t>(value_);
    }
    else if (type_ == ValueType::Real) {
        return std::get<double>(value_);
    }
    else if (type_ == ValueType::Vector) {
        throw std::runtime_error("Cannot convert vector to int");
    }
    return 0;
}

double Value::asReal() const
{
    if (type_ == ValueType::Real) {
        return std::get<double>(value_);
    }
    else if (type_ == ValueType::Bool) {
        return std::get<bool>(value_) ? 1.0 : 0.0;
    }
    else if (type_ == ValueType::Byte) {
        return std::get<uint8_t>(value_);
    }
    else if (type_ == ValueType::Int) {
        return std::get<int32_t>(value_);
    }
    else if (type_ == ValueType::Vector) {
        throw std::runtime_error("Cannot convert vector to real");
    }
    return 0;
}

ptr<std::vector<Value>> Value::asVector() const
{
    if (type_ == ValueType::Vector) {
        return std::get<ptr<std::vector<Value>>>(value_);
    }
    else if (type_ == ValueType::Bool) {
        return std::make_shared<std::vector<Value>>(1, Value(std::get<bool>(value_)));
    }
    else if (type_ == ValueType::Byte) {
        return std::make_shared<std::vector<Value>>(1, Value(std::get<uint8_t>(value_)));
    }
    else if (type_ == ValueType::Int) {
        return std::make_shared<std::vector<Value>>(1, Value(std::get<int32_t>(value_)));
    }
    else if (type_ == ValueType::Real) {
        return std::make_shared<std::vector<Value>>(1, Value(std::get<double>(value_)));
    }
    throw std::runtime_error("Cannot convert value to vector");
}


size_t Value::vectorSize() const
{
    if (type_ == ValueType::Vector)
        return std::get<ptr<std::vector<Value>>>(value_)->size();
    return 1;
}


std::string Value::toString() const
{
    if (type_ == ValueType::Vector) {
        std::stringstream ss;
        ss << "[";
        for (size_t i = 0; i < std::get<ptr<std::vector<Value>>>(value_)->size(); i++) {
            if (i > 0)
                ss << ", ";
            ss << std::get<ptr<std::vector<Value>>>(value_)->at(i).toString();
        }
        ss << "]";
        return ss.str();
    }
    else if (type_ == ValueType::Real) {
        std::stringstream ss;
        ss << std::get<double>(value_);
        return ss.str();
    }
    else if (type_ == ValueType::Int) {
        std::stringstream ss;
        ss << std::get<int32_t>(value_);
        return ss.str();
    }
    else if (type_ == ValueType::Byte) {
        std::stringstream ss;
        ss << std::get<uint8_t>(value_);
        return ss.str();
    }
    else if (type_ == ValueType::Bool) {
        std::stringstream ss;
        ss << std::get<bool>(value_);
        return ss.str();
    }
    else if (type_ == ValueType::Nil) {
        return "nil";
    }
    else {
        return "?";
    }
}



Value df::vecMult(double s, const Value& v)
{
    if (s==1.0) return v;
    assert(v.isVector());
    const auto& vec { v.asVector() };
    if (!vec->at(0).isNumber())
        throw std::runtime_error("Cannot multiply scalar & non-numeric vector");
    auto resultVec = std::make_shared<std::vector<Value>>();
    resultVec->resize(vec->size());
    // if all elements of vector are ints, keep int result elements
    //  otherwise, convert everything to real
    bool areAllInt = (int(s) == s);
    if (areAllInt)
        for (size_t i = 0; i < vec->size(); i++) {
            if (!vec->at(i).isInt())
                areAllInt = false;
        }
    for (size_t i = 0; i < vec->size(); i++) {
        if (areAllInt)
            resultVec->at(i) = Value(int32_t(s * vec->at(i).asInt()));
        else
            resultVec->at(i) = Value(double(s * vec->at(i).asReal()));
    }
    return Value(resultVec);
}


Value df::vecMult(int32_t s, const Value& v)
{
    if (s==1) return v;
    assert(v.isVector());
    const auto& vec { v.asVector() };
    if (!vec->at(0).isNumber())
        throw std::runtime_error("Cannot multiply scalar & non-numeric vector");
    auto resultVec = std::make_shared<std::vector<Value>>();
    resultVec->resize(vec->size());
    // if all elements of vector are ints, keep int result elements
    //  otherwise, convert everything to real
    bool areAllInt = true;
    for (size_t i = 0; i < vec->size(); i++) {
        if (!vec->at(i).isInt())
            areAllInt = false;
    }
    for (size_t i = 0; i < vec->size(); i++) {
        if (areAllInt)
            resultVec->at(i) = Value(int32_t(s * vec->at(i).asInt()));
        else
            resultVec->at(i) = Value(double(s * vec->at(i).asReal()));
    }
    return Value(resultVec);
}




Value df::vecAdd(const Value& v1, const Value& v2)
{
    assert(v1.isVector() && v2.isVector());
    const auto& vec1 { v1.asVector() };
    const auto& vec2 { v2.asVector() };
    assert(vec1->size() == vec2->size());
    auto resultVec = std::make_shared<std::vector<Value>>();
    // if all elements of both vectors are ints, keep int result elements
    //  otherwise, convert everything to real
    bool areAllInt = true;
    for (size_t i = 0; i < vec1->size(); i++) {
        if (!vec1->at(i).isInt() || !vec2->at(i).isInt())
            areAllInt = false;
    }
    resultVec->resize(vec1->size());
    for (size_t i = 0; i < vec1->size(); i++) {
        if (areAllInt)
            resultVec->at(i) = Value(vec1->at(i).asInt() + vec2->at(i).asInt());
        else
            resultVec->at(i) = Value(vec1->at(i).asReal() + vec2->at(i).asReal());
    }
    return Value(resultVec);
}

Value df::vecSub(const Value& v1, const Value& v2)
{
    auto negv2 { vecMult(-1.0, v2) };
    return vecAdd(v1, negv2);
}
