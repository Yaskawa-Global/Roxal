#pragma once
#include "compiler/Value.h"
#include "compiler/Object.h"
#include <Eigen/Dense>
#include <vector>
#include <cmath>
#include <sstream>
#include "core/common.h"
#include "core/memory.h"

namespace df {

using roxal::Value;
using roxal::ptr;
using roxal::make_ptr;

inline Value doubleVector(const std::vector<double>& elts) {
    Eigen::VectorXd vec(elts.size());
    for (size_t i = 0; i < elts.size(); ++i)
        vec(i) = elts[i];
    return Value::vectorVal(vec);
}

inline size_t vectorSize(const Value& v) {
    if (roxal::isVector(v))
        return roxal::asVector(v)->length();
    return 1;
}

inline bool equals(const Value& a, const Value& b, double eps = 1e-15) {
    bool bothReal = a.isReal() && b.isReal();
    bool bothVectors = roxal::isVector(a) && roxal::isVector(b);
    if (!bothReal && !bothVectors)
        return a == b;
    if (bothReal)
        return std::abs(a.asReal() - b.asReal()) < eps;
    if (roxal::asVector(a)->length() != roxal::asVector(b)->length())
        return false;
    const auto& va = roxal::asVector(a)->vec;
    const auto& vb = roxal::asVector(b)->vec;
    for (int i = 0; i < va.size(); ++i)
        if (std::abs(va[i] - vb[i]) >= eps)
            return false;
    return true;
}

inline Value vecMult(double s, const Value& v) {
    assert(roxal::isVector(v));
    auto result = roxal::asVector(v)->vec * s;
    return Value::vectorVal(result);
}

inline Value vecMult(int32_t s, const Value& v) {
    assert(roxal::isVector(v));
    auto result = roxal::asVector(v)->vec * static_cast<double>(s);
    return Value::vectorVal(result);
}

inline Value vecAdd(const Value& v1, const Value& v2) {
    assert(roxal::isVector(v1) && roxal::isVector(v2));
    auto result = roxal::asVector(v1)->vec + roxal::asVector(v2)->vec;
    return Value::vectorVal(result);
}

inline Value vecSub(const Value& v1, const Value& v2) {
    assert(roxal::isVector(v1) && roxal::isVector(v2));
    auto result = roxal::asVector(v1)->vec - roxal::asVector(v2)->vec;
    return Value::vectorVal(result);
}

inline std::ostream& operator<<(std::ostream& os, const Value& v) {
    os << roxal::toString(v);
    return os;
}

using Values = std::vector<Value>;
using NamedValues = std::map<std::string, Value>;

} // namespace df
