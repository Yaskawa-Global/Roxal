#include "ModuleMath.h"
#include "VM.h"
#include "Object.h"
#include "FFI.h"
#include <math.h>
#include <Eigen/Dense>

using namespace roxal;

ModuleMath::ModuleMath()
{
    moduleTypeValue = objVal(moduleTypeVal(toUnicodeString("math")));
}

void ModuleMath::registerBuiltins(VM& vm)
{
    auto addMath = [&](const std::string& name, void* fnPtr,
                       std::vector<ffi_type*> args){
        void* spec = createFFIWrapper(fnPtr, &ffi_type_double, args);
        moduleType()->vars.store(toUnicodeString(name),
            objVal(nativeVal(std::mem_fn(&VM::ffi_native), spec, nullptr, {})));
    };

    auto addMathBuiltin = [&](const std::string& name, NativeFn fn){
        moduleType()->vars.store(toUnicodeString(name),
            objVal(nativeVal(fn, nullptr, nullptr, {})));
    };

    addMath("sin",  (void*)(double (*)(double))sin,  {&ffi_type_double});
    addMath("cos",  (void*)(double (*)(double))cos,  {&ffi_type_double});
    addMath("tan",  (void*)(double (*)(double))tan,  {&ffi_type_double});
    addMath("asin", (void*)(double (*)(double))asin, {&ffi_type_double});
    addMath("acos", (void*)(double (*)(double))acos, {&ffi_type_double});
    addMath("atan", (void*)(double (*)(double))atan, {&ffi_type_double});
    addMath("atan2", (void*)(double (*)(double,double))atan2,
             {&ffi_type_double, &ffi_type_double});
    addMath("sinh", (void*)(double (*)(double))sinh, {&ffi_type_double});
    addMath("cosh", (void*)(double (*)(double))cosh, {&ffi_type_double});
    addMath("tanh", (void*)(double (*)(double))tanh, {&ffi_type_double});
    addMath("asinh", (void*)(double (*)(double))asinh, {&ffi_type_double});
    addMath("acosh", (void*)(double (*)(double))acosh, {&ffi_type_double});
    addMath("atanh", (void*)(double (*)(double))atanh, {&ffi_type_double});
    addMath("exp",  (void*)(double (*)(double))exp,  {&ffi_type_double});
    addMath("log",  (void*)(double (*)(double))log,  {&ffi_type_double});
    addMath("log10",(void*)(double (*)(double))log10,{&ffi_type_double});
    addMath("log2", (void*)(double (*)(double))log2, {&ffi_type_double});
    addMath("sqrt", (void*)(double (*)(double))sqrt, {&ffi_type_double});
    addMath("cbrt", (void*)(double (*)(double))cbrt, {&ffi_type_double});
    addMath("ceil", (void*)(double (*)(double))ceil, {&ffi_type_double});
    addMath("floor",(void*)(double (*)(double))floor,{&ffi_type_double});
    addMath("round",(void*)(double (*)(double))round,{&ffi_type_double});
    addMath("trunc",(void*)(double (*)(double))trunc,{&ffi_type_double});
    addMath("fabs", (void*)(double (*)(double))fabs, {&ffi_type_double});
    addMath("hypot",(void*)(double (*)(double,double))hypot,
             {&ffi_type_double, &ffi_type_double});
    addMath("fmod", (void*)(double (*)(double,double))fmod,
             {&ffi_type_double, &ffi_type_double});
    addMath("remainder", (void*)(double (*)(double,double))remainder,
             {&ffi_type_double, &ffi_type_double});
    addMath("fmax", (void*)(double (*)(double,double))fmax,
             {&ffi_type_double, &ffi_type_double});
    addMath("fmin", (void*)(double (*)(double,double))fmin,
             {&ffi_type_double, &ffi_type_double});
    addMath("pow",  (void*)(double (*)(double,double))pow,
             {&ffi_type_double, &ffi_type_double});
    addMath("fma",  (void*)(double (*)(double,double,double))fma,
             {&ffi_type_double, &ffi_type_double, &ffi_type_double});

    addMath("copysign", (void*)(double (*)(double,double))copysign,
            {&ffi_type_double, &ffi_type_double});
    addMath("erf",  (void*)(double (*)(double))erf,  {&ffi_type_double});
    addMath("erfc", (void*)(double (*)(double))erfc, {&ffi_type_double});
    addMath("exp2", (void*)(double (*)(double))exp2, {&ffi_type_double});
    addMath("expm1", (void*)(double (*)(double))expm1, {&ffi_type_double});
    addMath("fdim", (void*)(double (*)(double,double))fdim,
            {&ffi_type_double, &ffi_type_double});
    addMath("lgamma", (void*)(double (*)(double))lgamma, {&ffi_type_double});
    addMath("log1p", (void*)(double (*)(double))log1p, {&ffi_type_double});
    addMath("logb", (void*)(double (*)(double))logb, {&ffi_type_double});
    addMath("nearbyint", (void*)(double (*)(double))nearbyint,
            {&ffi_type_double});
    addMath("nextafter", (void*)(double (*)(double,double))nextafter,
            {&ffi_type_double, &ffi_type_double});
    addMath("rint", (void*)(double (*)(double))rint, {&ffi_type_double});
    addMath("tgamma", (void*)(double (*)(double))tgamma, {&ffi_type_double});

    addMathBuiltin("identity", [this](VM& vm, int c, Value* a){ return math_identity_builtin(vm,c,a); });
    addMathBuiltin("zeros", [this](VM& vm, int c, Value* a){ return math_zeros_builtin(vm,c,a); });
    addMathBuiltin("ones", [this](VM& vm, int c, Value* a){ return math_ones_builtin(vm,c,a); });
    addMathBuiltin("dot", [this](VM& vm, int c, Value* a){ return math_dot_builtin(vm,c,a); });
    addMathBuiltin("cross", [this](VM& vm, int c, Value* a){ return math_cross_builtin(vm,c,a); });
}

Value ModuleMath::math_identity_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 1 || !args[0].isNumber())
        throw std::invalid_argument("math.identity expects single integer size");

    int n = toType(ValueType::Int, args[0], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Identity(n, n);
    return objVal(matrixVal(m));
}

Value ModuleMath::math_zeros_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.zeros expects two integer arguments");

    int r = toType(ValueType::Int, args[0], false).asInt();
    int c = toType(ValueType::Int, args[1], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Zero(r, c);
    return objVal(matrixVal(m));
}

Value ModuleMath::math_ones_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 2 || !args[0].isNumber() || !args[1].isNumber())
        throw std::invalid_argument("math.ones expects two integer arguments");

    int r = toType(ValueType::Int, args[0], false).asInt();
    int c = toType(ValueType::Int, args[1], false).asInt();
    Eigen::MatrixXd m = Eigen::MatrixXd::Ones(r, c);
    return objVal(matrixVal(m));
}

Value ModuleMath::math_dot_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.dot expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != v2->length())
        throw std::invalid_argument("math.dot requires vectors of same length");

    double d = v1->vec.dot(v2->vec);
    return realVal(d);
}

Value ModuleMath::math_cross_builtin(VM& vm, int argCount, Value* args)
{
    if (argCount != 2 || !isVector(args[0]) || !isVector(args[1]))
        throw std::invalid_argument("math.cross expects two vector arguments");

    ObjVector* v1 = asVector(args[0]);
    ObjVector* v2 = asVector(args[1]);
    if (v1->length() != 3 || v2->length() != 3)
        throw std::invalid_argument("math.cross requires 3 element vectors");

    Eigen::Vector3d r = v1->vec.head<3>().cross(v2->vec.head<3>());
    Eigen::VectorXd res = r;
    return objVal(vectorVal(res));
}

