#include "ModuleMath.h"
#include "VM.h"
#include "Object.h"
#include <cmath>
#include <Eigen/Dense>

using namespace roxal;

ModuleMath::ModuleMath()
{
    moduleTypeValue = objVal(newModuleTypeObj(toUnicodeString("math")));
    ObjModuleType::allModules.push_back(moduleTypeValue);
}

void ModuleMath::registerBuiltins(VM& vm)
{

    auto unary = [&](const std::string& name, double(*fn)(double)) {
        link(name, [fn,name](VM& vm, ArgsView a)->Value{
            if (a.size() != 1)
                throw std::invalid_argument("math." + name + " expects one argument");
            double x = toType(ValueType::Real, a[0], false).asReal();
            return Value::realVal(fn(x));
        });
    };

    auto binary = [&](const std::string& name, double(*fn)(double,double)) {
        link(name, [fn,name](VM& vm, ArgsView a)->Value{
            if (a.size() != 2)
                throw std::invalid_argument("math." + name + " expects two arguments");
            double x = toType(ValueType::Real, a[0], false).asReal();
            double y = toType(ValueType::Real, a[1], false).asReal();
            return Value::realVal(fn(x,y));
        });
    };

    auto ternary = [&](const std::string& name, double(*fn)(double,double,double)) {
        link(name, [fn,name](VM& vm, ArgsView a)->Value{
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

    link("identity", [this](VM& vm, ArgsView a){ return math_identity_builtin(vm,a); });
    link("zeros", [this](VM& vm, ArgsView a){ return math_zeros_builtin(vm,a); });
    link("ones", [this](VM& vm, ArgsView a){ return math_ones_builtin(vm,a); });
    link("dot", [this](VM& vm, ArgsView a){ return math_dot_builtin(vm,a); });
    link("cross", [this](VM& vm, ArgsView a){ return math_cross_builtin(vm,a); });

    // Link builtin Counter methods
    linkMethod("_Counter", "init", [this](VM& vm, ArgsView a){ return counter_init_builtin(vm,a); }, { Value::intVal(0) });
    linkMethod("_Counter", "inc", [this](VM& vm, ArgsView a){ return counter_inc_builtin(vm,a); }, { Value::intVal(1) });
    linkMethod("_Counter", "value", [this](VM& vm, ArgsView a){ return counter_value_builtin(vm,a); }, {});
}

Value ModuleMath::math_identity_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 1 || !args[0].isNumber())
        throw std::invalid_argument("math.identity expects single integer size");

    int n = toType(ValueType::Int, args[0], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Identity(n, n);
    return Value::matrixVal(m);
}

Value ModuleMath::math_zeros_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.zeros expects two integer arguments");

    int r = toType(ValueType::Int, args[0], false).asInt();
    int c = toType(ValueType::Int, args[1], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Zero(r, c);
    return Value::matrixVal(m);
}

Value ModuleMath::math_ones_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.ones expects two integer arguments");

    int r = toType(ValueType::Int, args[0], false).asInt();
    int c = toType(ValueType::Int, args[1], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Ones(r, c);
    return Value::matrixVal(m);
}

Value ModuleMath::math_dot_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.dot expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != v2->length())
        throw std::invalid_argument("math.dot requires vectors of same length");

    double d = v1->vec.dot(v2->vec);
    return Value::realVal(d);
}

Value ModuleMath::math_cross_builtin(VM& vm, ArgsView args)
{
    if (args.size() != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.cross expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != 3 || v2->length() != 3)
        throw std::invalid_argument("math.cross requires 3 element vectors");

    Eigen::Vector3d r = v1->vec.head<3>().cross(v2->vec.head<3>());
    Eigen::VectorXd res = r;
    return Value::vectorVal(res);
}



// Example

Value ModuleMath::counter_init_builtin(VM& vm, ArgsView args)
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

Value ModuleMath::counter_inc_builtin(VM& vm, ArgsView args)
{
    // Check for safety, but can probably assume all this is true as Roxal already checked the callSpec against the func type
    if (args.size() < 1 || args.size() > 2 || !isObjectInstance(args[0]))
        throw std::invalid_argument("Counter.inc expects receiver and optional int");

    ObjectInstance* inst = asObjectInstance(args[0]);
    int n = 1;
    if (args.size() == 2) {
        if (!args[1].isNumber())
            throw std::invalid_argument("Counter.inc expects numeric increment");
        n = args[1].asInt();
    }

    #ifdef DEBUG_BUILD
    assert(!inst->getProperty("_this").isNil());
    assert(isForeignPtr(inst->getProperty("_this")));
    #endif
    auto counter = static_cast<Counter*>(asForeignPtr(inst->getProperty("_this"))->ptr);

    counter->inc(n);

    return Value::intVal(counter->value());
}

Value ModuleMath::counter_value_builtin(VM& vm, ArgsView args)
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
