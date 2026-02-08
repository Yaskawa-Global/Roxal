#include "ModuleMath.h"
#include "VM.h"
#include "Object.h"
#include <cmath>
#include <algorithm>
#include <Eigen/Dense>

#include "dataflow/Signal.h"

using namespace roxal;

ModuleMath::ModuleMath()
{
    moduleTypeValue = Value::objVal(newModuleTypeObj(toUnicodeString("math")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

ModuleMath::~ModuleMath()
{
    destroyModuleType(moduleTypeValue);
}

void ModuleMath::registerBuiltins(VM& vm)
{
    setVM(vm);

    auto unary = [&](const std::string& name, double(*fn)(double)) {
        link(name, [fn,name](VM&, ArgsView a)->Value{
            if (a.size() != 1)
                throw std::invalid_argument("math." + name + " expects one argument");
            double x = toType(ValueType::Real, a[0], false).asReal();
            return Value::realVal(fn(x));
        });
    };

    auto binary = [&](const std::string& name, double(*fn)(double,double)) {
        link(name, [fn,name](VM&, ArgsView a)->Value{
            if (a.size() != 2)
                throw std::invalid_argument("math." + name + " expects two arguments");
            double x = toType(ValueType::Real, a[0], false).asReal();
            double y = toType(ValueType::Real, a[1], false).asReal();
            return Value::realVal(fn(x,y));
        });
    };

    auto ternary = [&](const std::string& name, double(*fn)(double,double,double)) {
        link(name, [fn,name](VM&, ArgsView a)->Value{
            if (a.size() != 3)
                throw std::invalid_argument("math." + name + " expects three arguments");
            double x = toType(ValueType::Real, a[0], false).asReal();
            double y = toType(ValueType::Real, a[1], false).asReal();
            double z = toType(ValueType::Real, a[2], false).asReal();
            return Value::realVal(fn(x,y,z));
        });
    };

    unary("sin",  std::sin);
    unary("cos",  std::cos);
    unary("tan",  std::tan);
    unary("asin", std::asin);
    unary("acos", std::acos);
    unary("atan", std::atan);
    binary("atan2", std::atan2);
    unary("sinh", std::sinh);
    unary("cosh", std::cosh);
    unary("tanh", std::tanh);
    unary("asinh", std::asinh);
    unary("acosh", std::acosh);
    unary("atanh", std::atanh);
    unary("exp",  std::exp);
    unary("log",  std::log);
    unary("log10", std::log10);
    unary("log2", std::log2);
    unary("sqrt", std::sqrt);
    unary("cbrt", std::cbrt);
    unary("ceil", std::ceil);
    unary("floor", std::floor);
    unary("round", std::round);
    unary("trunc", std::trunc);
    unary("fabs", std::fabs);
    binary("hypot", std::hypot);
    binary("fmod", std::fmod);
    binary("remainder", std::remainder);
    binary("fmax", std::fmax);
    binary("fmin", std::fmin);
    binary("pow",  std::pow);
    ternary("fma", std::fma);
    binary("copysign", std::copysign);
    unary("erf",  std::erf);
    unary("erfc", std::erfc);
    unary("exp2", std::exp2);
    unary("expm1", std::expm1);
    binary("fdim", std::fdim);
    unary("lgamma", std::lgamma);
    unary("log1p", std::log1p);
    unary("logb", std::logb);
    unary("nearbyint", std::nearbyint);
    binary("nextafter", std::nextafter);
    unary("rint", std::rint);
    unary("tgamma", std::tgamma);

    link("identity", [this](VM&, ArgsView a){ return math_identity_builtin(a); });
    link("zeros", [this](VM&, ArgsView a){ return math_zeros_builtin(a); });
    link("ones", [this](VM&, ArgsView a){ return math_ones_builtin(a); });
    link("dot", [this](VM&, ArgsView a){ return math_dot_builtin(a); });
    link("cross", [this](VM&, ArgsView a){ return math_cross_builtin(a); });
    link("_setVecSignal", [this](VM&, ArgsView a){ return math_setVecSignal_builtin(a); });
    link("relu", [this](VM&, ArgsView a){ return math_relu_builtin(a); });
    link("softmax", [this](VM&, ArgsView a){ return math_softmax_builtin(a); });
    link("argmax", [this](VM&, ArgsView a){ return math_argmax_builtin(a); });

    // Link builtin Counter methods
    linkMethod("_Counter", "init", [this](VM&, ArgsView a){ return counter_init_builtin(a); });
    linkMethod("_Counter", "inc", [this](VM&, ArgsView a){ return counter_inc_builtin(a); });
    linkMethod("_Counter", "value", [this](VM&, ArgsView a){ return counter_value_builtin(a); }, {});

    if (auto vecSignal = moduleSourceSignal("_vecSignal", false))
        vecSignal->setInternal(true);

}

Value ModuleMath::math_identity_builtin(ArgsView args)
{
    if (args.size() != 1 || !args[0].isNumber())
        throw std::invalid_argument("math.identity expects single integer size");

    Eigen::Index n = static_cast<Eigen::Index>(toType(ValueType::Int, args[0], false).asInt());
    Eigen::MatrixXd m = Eigen::MatrixXd::Identity(n, n);
    return Value::matrixVal(m);
}

Value ModuleMath::math_zeros_builtin(ArgsView args)
{
    if (args.size() != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.zeros expects two integer arguments");

    Eigen::Index r = static_cast<Eigen::Index>(toType(ValueType::Int, args[0], false).asInt());
    Eigen::Index c = static_cast<Eigen::Index>(toType(ValueType::Int, args[1], false).asInt());
    Eigen::MatrixXd m = Eigen::MatrixXd::Zero(r, c);
    return Value::matrixVal(m);
}

Value ModuleMath::math_ones_builtin(ArgsView args)
{
    if (args.size() != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.ones expects two integer arguments");

    Eigen::Index r = static_cast<Eigen::Index>(toType(ValueType::Int, args[0], false).asInt());
    Eigen::Index c = static_cast<Eigen::Index>(toType(ValueType::Int, args[1], false).asInt());
    Eigen::MatrixXd m = Eigen::MatrixXd::Ones(r, c);
    return Value::matrixVal(m);
}

Value ModuleMath::math_dot_builtin(ArgsView args)
{
    if (args.size() != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.dot expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != v2->length())
        throw std::invalid_argument("math.dot requires vectors of same length");

    double d = v1->vec().dot(v2->vec());
    return Value::realVal(d);
}

Value ModuleMath::math_cross_builtin(ArgsView args)
{
    if (args.size() != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.cross expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != 3 || v2->length() != 3)
        throw std::invalid_argument("math.cross requires 3 element vectors");

    Eigen::Vector3d r = v1->vec().head<3>().cross(v2->vec().head<3>());
    Eigen::VectorXd res = r;
    return Value::vectorVal(res);
}



// Example

Value ModuleMath::counter_init_builtin(ArgsView args)
{
    if (args.size() != 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Counter.init expects receiver and optional start int");

    ObjectInstance* inst = asObjectInstance(args[0]);
    int start = 0;
    if (args.size() == 2) {
        if (!args[1].isNumber())
            throw std::invalid_argument("Counter.init start must be int");
        start = args[1].asInt();
    }

    // C++ instance
    Counter* counter = new Counter(start);
    Value fp { Value::foreignPtrVal(counter) };
    asForeignPtr(fp)->registerCleanup([](void* p){ delete static_cast<Counter*>(p); });
    inst->setProperty("_this", fp); // store it in instance property

    return Value::nilVal();
}

Value ModuleMath::counter_inc_builtin(ArgsView args)
{
    // Check for safety, but can probably assume all this is true as Roxal already checked the callSpec against the func type
    if (args.size() < 1 || args.size() > 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Counter.inc expects receiver and optional int");

    ObjectInstance* inst = asObjectInstance(args[0]);
    int n = 1;
    if (args.size() == 2) {
        if (!args[1].isNumber())
            throw std::invalid_argument("Counter.inc expects numeric increment");
        n = static_cast<int>(args[1].asInt());
    }

    #ifdef DEBUG_BUILD
    assert(!inst->getProperty("_this").isNil());
    assert(isForeignPtr(inst->getProperty("_this")));
    #endif
    auto counter = static_cast<Counter*>(asForeignPtr(inst->getProperty("_this"))->ptr);

    counter->inc(n);

    return Value::intVal(counter->value());
}

Value ModuleMath::counter_value_builtin(ArgsView args)
{
    if (args.size() != 1 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Counter.value expects receiver");

    ObjectInstance* inst = asObjectInstance(args[0]);
    #ifdef DEBUG_BUILD
    assert(!inst->getProperty("_this").isNil());
    assert(isForeignPtr(inst->getProperty("_this")));
    #endif
    auto counter = static_cast<Counter*>(asForeignPtr(inst->getProperty("_this"))->ptr);

    return Value::intVal(counter->value());
}

Value ModuleMath::math_setVecSignal_builtin(ArgsView args)
{
    if (args.size() != 1 || !isVector(args[0]))
        throw std::invalid_argument("math._setVecSignal expects a single vector argument");

    setModuleSourceSignalValue("_vecSignal", args[0]);

    return Value::nilVal();
}

Value ModuleMath::math_relu_builtin(ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("math.relu expects one argument");

    const Value& x = args[0];

    if (x.isNumber()) {
        // Scalar
        double val = toType(ValueType::Real, x, false).asReal();
        return Value::realVal(std::max(0.0, val));
    }
    else if (isVector(x)) {
        // Vector
        ObjVector* v = asVector(x);
        Eigen::VectorXd result = v->vec().cwiseMax(0.0);
        return Value::vectorVal(result);
    }
    else if (isMatrix(x)) {
        // Matrix
        ObjMatrix* m = asMatrix(x);
        Eigen::MatrixXd result = m->mat().cwiseMax(0.0);
        return Value::matrixVal(result);
    }
    else if (isTensor(x)) {
        // Tensor
        ObjTensor* t = asTensor(x);
        const std::vector<int64_t>& shape = t->shape();
        int64_t n = t->numel();
        std::vector<double> resultData(n);
        for (int64_t i = 0; i < n; ++i) {
            resultData[i] = std::max(0.0, t->at(i));
        }
        return Value::tensorVal(shape, resultData, t->dtype());
    }
    else {
        throw std::invalid_argument("math.relu expects scalar, vector, matrix, or tensor");
    }
}

Value ModuleMath::math_softmax_builtin(ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("math.softmax expects one argument");

    const Value& x = args[0];

    if (isVector(x)) {
        ObjVector* v = asVector(x);
        // Subtract max for numerical stability
        double maxVal = v->vec().maxCoeff();
        Eigen::VectorXd expVec = (v->vec().array() - maxVal).exp();
        double sum = expVec.sum();
        Eigen::VectorXd result = expVec / sum;
        return Value::vectorVal(result);
    }
    else if (isTensor(x)) {
        ObjTensor* t = asTensor(x);
        if (t->rank() != 1)
            throw std::invalid_argument("math.softmax requires a 1D tensor");

        int64_t n = t->numel();

        // Find max for numerical stability
        double maxVal = t->at(0);
        for (int64_t i = 1; i < n; ++i) {
            maxVal = std::max(maxVal, t->at(i));
        }

        // Compute exp, sum, and normalize
        std::vector<double> resultData(n);
        double sum = 0.0;
        for (int64_t i = 0; i < n; ++i) {
            resultData[i] = std::exp(t->at(i) - maxVal);
            sum += resultData[i];
        }
        for (int64_t i = 0; i < n; ++i) {
            resultData[i] /= sum;
        }

        return Value::tensorVal(t->shape(), resultData, t->dtype());
    }
    else {
        throw std::invalid_argument("math.softmax expects vector or 1D tensor");
    }
}

Value ModuleMath::math_argmax_builtin(ArgsView args)
{
    if (args.size() != 1)
        throw std::invalid_argument("math.argmax expects one argument");

    const Value& x = args[0];

    if (isVector(x)) {
        ObjVector* v = asVector(x);
        Eigen::Index maxIdx;
        v->vec().maxCoeff(&maxIdx);
        return Value::intVal(static_cast<int64_t>(maxIdx));
    }
    else if (isTensor(x)) {
        ObjTensor* t = asTensor(x);
        if (t->rank() != 1)
            throw std::invalid_argument("math.argmax requires a 1D tensor");

        int64_t n = t->numel();
        int64_t maxIdx = 0;
        double maxVal = t->at(0);
        for (int64_t i = 1; i < n; ++i) {
            double v = t->at(i);
            if (v > maxVal) {
                maxVal = v;
                maxIdx = i;
            }
        }
        return Value::intVal(maxIdx);
    }
    else {
        throw std::invalid_argument("math.argmax expects vector or 1D tensor");
    }
}
