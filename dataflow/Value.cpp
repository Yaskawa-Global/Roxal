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

Value::Value(const ptr<Eigen::VectorXd> vec)
{
    type_ = ValueType::Vector;
    value_ = vec;
}


Value Value::doubleVector(const std::vector<double>& elts)
{
    auto vec = make_ptr<Eigen::VectorXd>(elts.size());
    for (size_t i = 0; i < elts.size(); ++i)
        (*vec)(i) = elts[i];
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
        const auto& lhs = *std::get<ptr<Eigen::VectorXd>>(value_);
        const auto& rhs = *std::get<ptr<Eigen::VectorXd>>(v.value_);
        if (lhs.size() != rhs.size())
            return false;
        for (int i = 0; i < lhs.size(); ++i)
            if (lhs[i] != rhs[i])
                return false;
        return true;
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

    const auto& value { *std::get<ptr<Eigen::VectorXd>>(value_) };
    const auto& other { *std::get<ptr<Eigen::VectorXd>>(v.value_) };
    if (value.size() != other.size())
        return false;
    for (int i = 0; i < value.size(); ++i) {
        if (std::abs(value[i] - other[i]) >= eps)
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
        return std::get<ptr<Eigen::VectorXd>>(value_)->size() > 0;
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

ptr<Eigen::VectorXd> Value::asVector() const
{
    if (type_ == ValueType::Vector) {
        return std::get<ptr<Eigen::VectorXd>>(value_);
    }
    else if (type_ == ValueType::Bool) {
        auto vec = make_ptr<Eigen::VectorXd>(1);
        (*vec)(0) = std::get<bool>(value_) ? 1.0 : 0.0;
        return vec;
    }
    else if (type_ == ValueType::Byte) {
        auto vec = make_ptr<Eigen::VectorXd>(1);
        (*vec)(0) = std::get<uint8_t>(value_);
        return vec;
    }
    else if (type_ == ValueType::Int) {
        auto vec = make_ptr<Eigen::VectorXd>(1);
        (*vec)(0) = std::get<int32_t>(value_);
        return vec;
    }
    else if (type_ == ValueType::Real) {
        auto vec = make_ptr<Eigen::VectorXd>(1);
        (*vec)(0) = std::get<double>(value_);
        return vec;
    }
    throw std::runtime_error("Cannot convert value to vector");
}


size_t Value::vectorSize() const
{
    if (type_ == ValueType::Vector)
        return std::get<ptr<Eigen::VectorXd>>(value_)->size();
    return 1;
}


std::string Value::toString() const
{
    if (type_ == ValueType::Vector) {
        std::stringstream ss;
        const auto& vec = *std::get<ptr<Eigen::VectorXd>>(value_);
        ss << "[";
        for (int i = 0; i < vec.size(); i++) {
            if (i > 0)
                ss << ' ';
            ss << vec[i];
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
    auto resultVec = make_ptr<Eigen::VectorXd>(vec->size());
    *resultVec = (*vec) * s;
    return Value(resultVec);
}


Value df::vecMult(int32_t s, const Value& v)
{
    if (s==1) return v;
    assert(v.isVector());
    const auto& vec { v.asVector() };
    auto resultVec = make_ptr<Eigen::VectorXd>(vec->size());
    *resultVec = (*vec) * static_cast<double>(s);
    return Value(resultVec);
}




Value df::vecAdd(const Value& v1, const Value& v2)
{
    assert(v1.isVector() && v2.isVector());
    const auto& vec1 { v1.asVector() };
    const auto& vec2 { v2.asVector() };
    assert(vec1->size() == vec2->size());
    auto resultVec = make_ptr<Eigen::VectorXd>(vec1->size());
    *resultVec = (*vec1) + (*vec2);
    return Value(resultVec);
}

Value df::vecSub(const Value& v1, const Value& v2)
{
    auto negv2 { vecMult(-1.0, v2) };
    return vecAdd(v1, negv2);
}
