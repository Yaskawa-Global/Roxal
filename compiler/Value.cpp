#include <cassert>
#include <bitset>
#include <core/common.h>

#include "Value.h"

#include "Object.h"
#include "dataflow/Signal.h"
#include <core/types.h>
#include <Eigen/Dense>


namespace roxal {
    // forward from Object.h
    std::string objToString(const Value& v);
    bool objsEqual(const Value& l, const Value& r);
}


using namespace roxal;


std::string roxal::to_string(ValueType t)
{
    switch (t) {
    case ValueType::Nil: return "nil"; break;
    case ValueType::Bool: return "bool"; break;
    case ValueType::Byte: return "byte"; break;
    case ValueType::Int: return "int"; break;
    case ValueType::Real: return "real"; break;
    case ValueType::Decimal: return "decimal"; break;
    case ValueType::Enum: return "enum"; break;
    case ValueType::String: return "string"; break;
    case ValueType::Range: return "range"; break;
    case ValueType::Type: return "type"; break;
    case ValueType::List: return "list"; break;
    case ValueType::Dict: return "dict"; break;
    case ValueType::Vector: return "vector"; break;
    case ValueType::Matrix: return "matrix"; break;
    case ValueType::Signal: return "signal"; break;
    case ValueType::Tensor: return "tensor"; break;
    case ValueType::Orient: return "orient"; break;
    case ValueType::Object: return "object"; break;
    case ValueType::Actor: return "actor"; break;
    case ValueType::Module: return "module"; break;
    default:
        throw std::runtime_error("Unhandled type for to_string "+std::to_string(int(t)));
    }
}


#if defined(NAN_TAGGING)


Value::Value(Obj* o)
{
    o->incRef();
    val = SignBit | QNAN | uint64_t(uintptr_t(o));
}


void Value::box() {
    if (isBoxed() || !isBoxable()) return;
    // allocate value on heap
    Obj* obj;
    if (isBool())
        obj = newObj<ObjPrimitive>(__func__,asBool());
    else if (isInt())
        obj = newObj<ObjPrimitive>(__func__,asInt());
    else if (isReal())
        obj = newObj<ObjPrimitive>(__func__,asReal());
    else if (isType())
        obj = newObj<ObjPrimitive>(__func__,asType());
    else
        throw std::runtime_error("Unsupported type for auto-boxing "+typeName());

    obj->incRef();
    val = SignBit | QNAN | uint64_t(uintptr_t(obj));
}


void Value::unbox() {
    if (!isBoxed()) return;

    Obj* obj = asObj();

    if (isBool())
        *this = Value(asObjPrimitive(*this)->as.boolean);
    else if (isInt())
        *this = Value(asObjPrimitive(*this)->as.integer);
    else if (isReal())
        *this = Value(asObjPrimitive(*this)->as.real);
    else if (isType())
        *this = Value(asObjPrimitive(*this)->as.btype);
    else
        throw std::runtime_error("Unsupported type for auto-unboxing "+typeName());

    obj->decRef();
}


void roxal::Value::incRefObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't incRef non-object type "+typeName());
    #endif
    asObj()->incRef();
}

void roxal::Value::decRefObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't decRef non-object type "+typeName());
    #endif
    asObj()->decRef();
}



bool Value::asBool(bool strict) const
{
    if (isSignal(*this))
        return asSignal(*this)->signal->lastValue().asBool(strict);
    Value unboxed;
    const Value* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->type()) {
        case ValueType::Bool:
            return v->val == (QNAN | TagTrue);
        case ValueType::Byte:
            if (!strict)
                return v->asByte(false) != 0;
            throw std::invalid_argument("unable to convert byte to bool in strict mode");
        case ValueType::Int:
            if (!strict)
                return v->asInt(false) != 0;
            throw std::invalid_argument("unable to convert int to bool in strict mode");
        case ValueType::Real:
            if (!strict)
                return v->asReal(false) != 0.0;
            throw std::invalid_argument("unable to convert real to bool in strict mode");
        case ValueType::Decimal:
            throw std::runtime_error("decimal unimplemented");
        default: ;
        }
    }
    else {
        if (isString(*v) && !strict) {
            auto str { toUTF8StdString(asString(*v)->s) };
            // TODO: warn if the string contains "false", since it'll evaluate to true but that may not be the intent
            //  (make such warnings suppressable per-instance)
            return !str.empty();
        }
        else if (v->isBoxed()) {
            // boxed primitive handled above after unboxing
        }
        else if (v->asObj()->type == ObjType::Bool) {
            return asObjPrimitive(*v)->as.boolean;
        }
    }
    return false;
}



uint8_t Value::asByte(bool strict) const
{
    if (isSignal(*this))
        return asSignal(*this)->signal->lastValue().asByte(strict);
    Value unboxed;
    Value const* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->type()) {
        case ValueType::Byte:
            return uint8_t(v->val & 0xff);
        case ValueType::Int:
            if (!strict) {
                uint64_t i { v->val & ~(QNAN | TypeTag) };
                return uint8_t(*reinterpret_cast<int32_t*>(&i));
            }
            throw std::invalid_argument("unable to convert int to byte in strict mode");
        case ValueType::Real:
            if (!strict) {
                uint64_t bits = v->val.load();
                double d = *reinterpret_cast<double*>(&bits);
                return uint8_t(int32_t(d));
            }
            throw std::invalid_argument("unable to convert real to byte in strict mode");
        case ValueType::Bool:
            return (v->val == (QNAN | TagTrue)) ? 1 : 0;
        case ValueType::Decimal:
            throw std::runtime_error("decimal unimplemented");
        default: ;
        }
    } else {
        if (isString(*v) && !strict) {
            try {
                auto str { toUTF8StdString(asString(*v)->s) };
                if ((str.size() > 2) && (str[0] == '0')) {
                    if (str[1] == 'x' || str[1]=='X')
                        return std::stoi(str.substr(2),nullptr,16);
                    else if (str[1] == 'b' || str[1]=='B')
                        return std::stoi(str.substr(2),nullptr,2);
                    else if (str[1] == 'o' || str[1]=='O')
                        return std::stoi(str.substr(2),nullptr,8);
                }
                return std::stoi(str,nullptr,10);
            } catch(...) { return 0; }
        }
    }
    if (strict)
        throw std::invalid_argument("unable to convert " + to_string(v->type()) + " to byte in strict mode");
    return 0;
}


int32_t Value::asInt(bool strict) const
{
    if (isSignal(*this))
        return asSignal(*this)->signal->lastValue().asInt(strict);
    Value unboxed;
    Value const* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->type()) {
        case ValueType::Enum: return int32_t(asEnum());
        case ValueType::Byte: return int32_t(uint8_t(v->val & 0xff));
        case ValueType::Int: { uint64_t i {v->val & ~(QNAN | TypeTag)} ; return *reinterpret_cast<int32_t*>(&i); }
        case ValueType::Real:
            if (!strict) {
                uint64_t bits = v->val.load();
                double d = *reinterpret_cast<double*>(&bits);
                return int32_t(d);
            }
            throw std::invalid_argument("unable to convert real to int in strict mode");
        case ValueType::Bool: return (v->val == (QNAN | TagTrue)) ? 1 : 0;
        case ValueType::Decimal: throw std::runtime_error("decimal unimplemented");
        default: ;
        }
    } else {
        if (isString(*v) && !strict) {
            try {
                auto str { toUTF8StdString(asString(*v)->s) };
                if ((str.size() > 2) && (str[0] == '0')) {
                    if (str[1] == 'x' || str[1]=='X')
                        return std::stol(str.substr(2),nullptr,16);
                    else if (str[1] == 'b' || str[1]=='B')
                        return std::stol(str.substr(2),nullptr,2);
                    else if (str[1] == 'o' || str[1]=='O')
                        return std::stol(str.substr(2),nullptr,8);
                }
                return std::stol(str,nullptr,10);
            } catch(...) { return 0; }
        }
    }
    if (strict)
        throw std::invalid_argument("unable to convert " + to_string(v->type()) + " to int in strict mode");
    return 0;
}


int16_t Value::asEnum() const
{
    // TODO: handle boxed enum value
    if (isEnum())
        return int16_t(val & 0xffff);
    return 0;
}


double Value::asReal(bool strict) const
{
    if (isSignal(*this))
        return asSignal(*this)->signal->lastValue().asReal(strict);
    Value unboxed;
    Value const* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->type()) {
        case ValueType::Real:
            {
                uint64_t bits = v->val.load();
                return *reinterpret_cast<double*>(&bits);
            }
        case ValueType::Int: {
                uint64_t i { v->val & ~(QNAN | TypeTag) };
                return double(*reinterpret_cast<int32_t*>(&i)); }
        case ValueType::Byte:
                return double(uint8_t(v->val & 0xff));
        case ValueType::Bool: return (v->val == (QNAN | TagTrue)) ? 1.0 : 0.0;
        case ValueType::Decimal: throw std::runtime_error("decimal unimplemented");
        default: ;
        }
    }
    else {
        if (isString(*v) && !strict) {
            try {
                auto str { toUTF8StdString(asString(*v)->s) };
                return std::stod(str);
            } catch(...) { return 0.0; }
        }
    }
    if (strict)
        throw std::invalid_argument("unable to convert " + to_string(v->type()) + " to real in strict mode");
    return 0.0;
}



ValueType Value::asType(bool strict) const
{
    if (!isBoxed()) {
        if (type()==ValueType::Type) {
            uint64_t t {val & ~(QNAN | TypeTag)};
            return ValueType(*reinterpret_cast<uint64_t*>(&t));
        }
    }
    else {
        if (asObj()->type==ObjType::Type)
            return asObjPrimitive(*this)->as.btype;
    }
    return ValueType::Nil;
}



ValueType Value::type() const {
    return isNil() ? ValueType::Nil
                    : (isBool() ? ValueType::Bool
                                : (isReal() ? ValueType::Real
                                            : (isObj() ? asObj()->valueType()
                                                        : ValueType((val & TypeTag) >> TypeTagOffset) ) ) );
}


std::string Value::typeName() const
{
    if (isNil())
        return "nil";
    else if (isBool())
        return "bool";
    else if (isInt())
        return "int";
    else if (isReal())
        return "real";
    else if (isEnum())
        return "enum";
    else if (isType())
        return "type";

    if (isBoxed()) {
        auto pobj = asObjPrimitive(*this);
        if (pobj->isBool())
            return "bool";
        else if (pobj->isInt())
            return "int";
        else if (pobj->isReal())
            return "real";
        else if (pobj->isType())
            return "type";
        else
            return "unknown";
    }
    else if (isObj())
        return "object";
    return "unknown";
}


#else

roxal::Value::Value(Obj* o)
    : _type(ValueType::Object)
{
    as.obj=o; o->incRef();
}


void Value::box() {
    if (isBoxed() || !isBoxable()) return;
    _type = ValueType(int(_type) & int(ValueType::Boxed)); // mark boxed
    // allocate value on heap
    if (isBool())
        as.obj = newObj<ObjPrimitive>(__func__,as.boolean);
    else if (isInt())
        as.obj = newObj<ObjPrimitive>(__func__,as.integer);
    else if (isReal())
        as.obj = newObj<ObjPrimitive>(__func__,as.real);
    else if (isType())
        as.obj = newObj<ObjPrimitive>(__func__,as.btype);
    else
        throw std::runtime_error("Unsupported type for auto-boxing "+typeName());

    as.obj->incRef();
}


void Value::unbox() {
    if (!isBoxed()) return;
    _type = ValueType(int(_type) & ~int(ValueType::Boxed)); // remove boxed marker
    if (isBool())
        as.boolean = asPrimitive(*this)->as.boolean;
    else if (isInt())
        as.integer = asPrimitive(*this)->as.integer;
    else if (isReal())
        as.real = asPrimitive(*this)->as.real;
    else if (isType())
        as.btype = asPrimitive(*this)->as.btype;
    else
        throw std::runtime_error("Unsupported type for auto-unboxing "+typeName());
    as.obj->decRef();
}



void roxal::Value::incRefObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't incRef non-object type "+typeName());
    #endif
    as.obj->incRef();
}

void roxal::Value::decRefObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't decRef non-object type "+typeName());
    #endif
    as.obj->decRef();
}



bool Value::asBool(bool strict) const
{
    if (isSignal(*this))
        return asSignal(*this)->signal->lastValue().asBool(strict);
    Value unboxed;
    const Value* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!isObj()) {
        switch(valueType(v->_type)) {
            case ValueType::Bool:
                return v->as.boolean;
            case ValueType::Int:
                if (!strict) return v->as.integer != 0;
                throw std::invalid_argument("unable to convert int to bool in strict mode");
            case ValueType::Real:
                if (!strict) return v->as.real != 0.0;
                throw std::invalid_argument("unable to convert real to bool in strict mode");
            case ValueType::Decimal:
                throw std::runtime_error("decimal unimplemented");
            default: break;
        }
    }
    else {
        if (isString(*v) && !strict) {
            auto str { toUTF8StdString(asString(*v)->s) };
            return !str.empty();
        }
        else if (valueType(v->_type) == ValueType::Bool) {
            return asPrimitive(*v)->as.boolean;
        }
    }
    return false;
}





int32_t Value::asInt(bool strict) const
{
    if (isSignal(*this))
        return asSignal(*this)->signal->lastValue().asInt(strict);
    Value unboxed;
    Value const* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->_type) {
        case ValueType::Int: return v->as.integer;
        case ValueType::Real:
            if (!strict)
                return int32_t(v->as.real);
            throw std::invalid_argument("unable to convert real to int in strict mode");
        case ValueType::Bool: return v->as.boolean ? 1 : 0;
        case ValueType::Decimal: throw std::runtime_error("decimal unimplemented");
        default: ;
        }
    } else {
        if (isString(*v) && !strict) {
            try {
                auto str { toUTF8StdString(asString(*v)->s) };
                if ((str.size() > 2) && (str[0] == '0')) {
                    if (str[1] == 'x' || str[1]=='X')
                        return std::stol(str.substr(2),nullptr,16);
                    else if (str[1] == 'b' || str[1]=='B')
                        return std::stol(str.substr(2),nullptr,2);
                    else if (str[1] == 'o' || str[1]=='O')
                        return std::stol(str.substr(2),nullptr,8);
                }
                return std::stol(str,nullptr,10);
            } catch(...) { return 0; }
        }
    }
    if (strict)
        throw std::invalid_argument("unable to convert " + to_string(v->type()) + " to int in strict mode");
    return 0;
}


double Value::asReal(bool strict) const
{
    if (isSignal(*this))
        return asSignal(*this)->signal->lastValue().asReal(strict);
    Value unboxed;
    Value const* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->_type) {
        case ValueType::Real: return v->as.real;
        case ValueType::Int: return double(v->as.integer);
        case ValueType::Bool: return v->as.boolean ? 1.0 : 0.0;
        case ValueType::Decimal: throw std::runtime_error("decimal unimplemented");
        default: ;
        }
    }
    else {
        if (isString(*v) && !strict) {
            try {
                auto str { toUTF8StdString(asString(*v)->s) };
                return std::stod(str);
            } catch(...) { return 0.0; }
        }
    }
    if (strict)
        throw std::invalid_argument("unable to convert " + to_string(v->type()) + " to real in strict mode");
    return 0.0;
}


ValueType Value::asType(bool strict) const
{
    if (!isBoxed()) {
        if (_type==ValueType::Type)
            return as.btype;
    }
    else {
        if (valueType(_type)==ValueType::Type)
            return asPrimitive(*this)->as.btype;
    }
    return ValueType::Nil;
}



std::string roxal::Value::typeName() const
{
    if (isNil())
        return "nil";
    else if (isBool())
        return "bool";
    else if (isInt())
        return "int";
    else if (isReal())
        return "real";
    else if (isType())
        return "type";
    else if (isObj())
        return "object";
    //TODO: ...
    return (_type >= ValueType::Boxed ? "boxed ":"")+std::string("unknown");
}

#endif




bool Value::operator==(const Value& rhs) const
{
    // TODO: handle unboxing

    if (isNil())
        return rhs.isNil();
    if (rhs.isNil())
        return isNil();

    if (isBool())
        return asBool() == rhs.asBool();
    else if (isInt())
        return asInt() == rhs.asInt();
    else if (isReal())
        return asReal() == rhs.asReal();
    else if (isType())
        return asType() == rhs.asType();
    else if (isString(*this))
        return objsEqual(*this,rhs); // compares strings intelligently (e.g. using immutability & hash)
    else if (isVector(*this) && isVector(rhs)) {
        // Deep equality for vectors - compare elements
        return asVector(*this)->equals(asVector(rhs));
    }
    else if (isVector(*this) && isList(rhs)) {
        // Vector compared to list - auto-convert list if it has 0 or 1 numeric elements
        ObjList* rhsList = asList(rhs);
        if (rhsList->length() <= 1) {
            bool allNumeric = true;
            for (int i = 0; i < rhsList->length(); i++) {
                if (!rhsList->elts.at(i).isNumber()) {
                    allNumeric = false;
                    break;
                }
            }
            if (allNumeric) {
                // Convert list to vector and compare
                ObjVector* lhsVec = asVector(*this);
                if (lhsVec->length() != rhsList->length())
                    return false;
                for (int i = 0; i < rhsList->length(); i++) {
                    if (std::abs(lhsVec->vec[i] - rhsList->elts.at(i).asReal()) > 1e-15)
                        return false;
                }
                return true;
            }
        }
        // Multi-element lists or non-numeric lists should not auto-convert
        return false;
    }
    else if (isList(*this) && isVector(rhs)) {
        // List compared to vector - symmetric case
        return rhs == *this;  // Use the vector-list comparison above
    }
    else if (isMatrix(*this) && isMatrix(rhs)) {
        // Deep equality for matrices - compare elements  
        return asMatrix(*this)->equals(asMatrix(rhs));
    }
    else if (isMatrix(*this) && isList(rhs)) {
        // Matrix compared to list - auto-convert list if it has 0 or 1 numeric elements
        ObjList* rhsList = asList(rhs);
        if (rhsList->length() <= 1) {
            bool allNumeric = true;
            for (int i = 0; i < rhsList->length(); i++) {
                if (!rhsList->elts.at(i).isNumber()) {
                    allNumeric = false;
                    break;
                }
            }
            if (allNumeric) {
                // Convert list to matrix and compare
                ObjMatrix* lhsMat = asMatrix(*this);
                int expectedSize = rhsList->length();
                if (expectedSize == 0) {
                    return (lhsMat->rows() == 0 && lhsMat->cols() == 0);
                } else if (expectedSize == 1) {
                    return (lhsMat->rows() == 1 && lhsMat->cols() == 1 && 
                            std::abs(lhsMat->mat(0,0) - rhsList->elts.at(0).asReal()) <= 1e-15);
                }
            }
        }
        return false;
    }
    else if (isList(*this) && isMatrix(rhs)) {
        // List compared to matrix - symmetric case
        return rhs == *this;  // Use the matrix-list comparison above
    }
    else if (isObj())
        return rhs.isObj() && (asObj() == rhs.asObj()); // identity (by ptr/address)
    return false;
}


// deep copy if reference type
Value Value::clone() const
{
    if (isPrimitive()) // value type
        return *this;
    else if (isObj())
        return Value(asObj()->clone());

    throw std::runtime_error("unhandled clone()");
}

static type::BuiltinType valueTypeToBuiltin(ValueType t)
{
    using namespace type;
    switch (t) {
        case ValueType::Nil:     return BuiltinType::Nil;
        case ValueType::Bool:    return BuiltinType::Bool;
        case ValueType::Byte:    return BuiltinType::Byte;
        case ValueType::Int:     return BuiltinType::Int;
        case ValueType::Real:    return BuiltinType::Real;
        case ValueType::Decimal: return BuiltinType::Decimal;
        case ValueType::String:  return BuiltinType::String;
        case ValueType::Range:   return BuiltinType::Range;
        case ValueType::Enum:    return BuiltinType::Enum;
        case ValueType::List:    return BuiltinType::List;
        case ValueType::Dict:    return BuiltinType::Dict;
        case ValueType::Vector:  return BuiltinType::Vector;
        case ValueType::Matrix:  return BuiltinType::Matrix;
        case ValueType::Signal:  return BuiltinType::Signal;
        case ValueType::Tensor:  return BuiltinType::Tensor;
        case ValueType::Orient:  return BuiltinType::Orient;
        case ValueType::Object:  return BuiltinType::Object;
        case ValueType::Actor:   return BuiltinType::Actor;
        case ValueType::Type:    return BuiltinType::Type;
        default:                 return BuiltinType::Nil;
    }
}

bool roxal::convertibleTo(ValueType from, ValueType to, bool strict)
{
    return type::convertibleTo(valueTypeToBuiltin(from),
                               valueTypeToBuiltin(to), strict);
}

bool Value::convertibleTo(ValueType to, bool strict) const
{
    return roxal::convertibleTo(type(), to, strict);
}

std::vector<std::tuple<std::string,bool,std::string>> roxal::testConversions()
{
    auto testConvertible = []() -> bool {
        using VT = ValueType;
        bool ok = true;
        ok = ok && convertibleTo(VT::Int, VT::Real, true);
        ok = ok && !convertibleTo(VT::Int, VT::Bool, true);
        ok = ok && convertibleTo(VT::String, VT::Int, false);
        ok = ok && !convertibleTo(VT::String, VT::Int, true);
        ok = ok && convertibleTo(VT::Range, VT::List, false);
        ok = ok && !convertibleTo(VT::Range, VT::List, true);
        return ok;
    };

    std::vector<std::tuple<std::string,bool,std::string>> results;
    try {
        bool pass = testConvertible();
        results.push_back({"convertibleTo", pass, pass ? "ok" : "failed"});
    } catch (std::exception& e) {
        results.push_back({"convertibleTo", false, std::string("exception: ")+e.what()});
    }
    return results;
}



void Value::resolveFuture()
{
    if (isFuture(*this))
        *this = asFuture(*this)->asValue();
}

void Value::resolveSignal()
{
    if (isSignal(*this))
        *this = asSignal(*this)->signal->lastValue();
}

void Value::resolve()
{
    resolveFuture();
    resolveSignal();
}



Value roxal::defaultValue(ValueType t)
{
    switch (t) {
        case ValueType::Nil: return nilVal();
        case ValueType::Bool: return falseVal();
        case ValueType::Byte: return byteVal(0);
        case ValueType::Int: return intVal(0);
        case ValueType::Real: return realVal(0.0);
        case ValueType::Decimal: throw std::runtime_error("decimal unimplemented");
        case ValueType::Enum: throw std::runtime_error("Can't create default enum value without type"); // shouldn't be called for this t
        case ValueType::Type: return typeVal(ValueType::Nil);
        case ValueType::String: return Value(stringVal(UnicodeString()));
        case ValueType::Range: return Value(rangeVal());
        case ValueType::List: return Value(listVal());
        case ValueType::Dict: return Value(dictVal());
        case ValueType::Vector: return Value(vectorVal());
        case ValueType::Matrix: return Value(matrixVal());
        case ValueType::Signal: throw std::runtime_error("Can't default-construct signal");
        case ValueType::Tensor:
        case ValueType::Orient:
        case ValueType::Object:
        case ValueType::Actor:
        default:
            throw std::runtime_error("default value unimplemented for type "+to_string(t));
    }
    return nilVal();
}





Value roxal::toType(ValueType t, Value v, bool strict)
{
    if (!v.isBoxed()) {
        if (v.type() == t)
            return v;
    }
    else {
        auto pobj = asObjPrimitive(v);
        if (pobj->valueType() == t)
            return v;
        Value unboxedv { v };
        unboxedv.unbox();
        return toType(t, unboxedv, strict);
    }

    switch (t) {
        case ValueType::Bool: return boolVal(v.asBool(strict));
        case ValueType::Byte: return byteVal(v.asByte(strict));
        case ValueType::Real: return realVal(v.asReal(strict));
        case ValueType::Int: return intVal(v.asInt(strict));
        case ValueType::String: {
            // TODO: use alternate 'non-debug' string conversion only utilizing UnicodeString
            return Value(stringVal(toUnicodeString(toString(v))));
        } break;
        case ValueType::Range: {
            if (v.type() == ValueType::Range)
                return v;
            if (!strict)
                return objVal(rangeVal(v,v,intVal(1),true));
        } break;
        case ValueType::List: {
            if ((v.type() == ValueType::Range) && !strict)
                return objVal(listVal(asRange(v)));
        } break;
        case ValueType::Dict: {
            // can convert objects to dict of property, value pairs (non-strict only)

            // FIXME: current object instances don't store property names for properties
            //  added by assignment at runtime (the properties map keys are property name string hashes only)
            //  Hence, currently, only the properties declared in the type object declaration are
            //  included in the dict!

            if (isObjectInstance(v) && !strict) {
                ObjDict* dictValue = dictVal({});

                ObjectInstance* vObj = asObjectInstance(v);
                ObjObjectType* vObjType = vObj->instanceType;
                for(const auto& property : vObjType->properties) {
                    const auto& prop { property.second };
                    auto propName { stringVal(prop.name) };
                    #ifdef DEBUG_BUILD
                    assert(vObj->properties.find(propName->hash) != vObj->properties.end());
                    #endif
                    dictValue->store(Value(propName),
                                     vObj->properties[propName->hash]);
                }
                return Value(dictValue);
            }
        } break;
        //TODO: add more conversions
    }
    throw std::invalid_argument("unable to convert value of type "+to_string(v.type())+" to "+to_string(t));
}


Value roxal::construct(ValueType type, std::vector<Value>::const_iterator begin, std::vector<Value>::const_iterator end)
{
    if (type == ValueType::Vector) {
        size_t count = end - begin;
        if (count == 0) {
            return objVal(vectorVal());
        } else if (count == 1) {
            Value arg = *begin;
            if (arg.isInt())
                return objVal(vectorVal(arg.asInt()));
            if (isList(arg)) {
                auto listVals = asList(arg)->elts.get();
                Eigen::VectorXd vals(listVals.size());
                for(size_t i=0; i<listVals.size(); ++i)
                    vals[i] = toType(ValueType::Real, listVals[i], false).asReal();
                return objVal(vectorVal(vals));
            }
            if (isVector(arg)) {
                return objVal(cloneVector(asVector(arg)));
            }
            throw std::runtime_error("vector constructor expects int length, list of reals, or vector");
        } else {
            throw std::runtime_error("vector constructor with >1 arg unimplemented");
        }
    }

    if (type == ValueType::Matrix) {
        size_t count = end - begin;
        if (count == 1) {
            Value arg = *begin;
            if (isList(arg)) {
                auto rowsVals = asList(arg)->elts.get();
                if (rowsVals.size() == 0)
                    return objVal(matrixVal());

                if (!isList(rowsVals[0]) && !isVector(rowsVals[0])) {
                    int colCount = rowsVals.size();
                    Eigen::MatrixXd vals(1, colCount);
                    for(int c=0; c<colCount; ++c)
                        vals(0,c) = toType(ValueType::Real, rowsVals[c], false).asReal();
                    return objVal(matrixVal(vals));
                }

                size_t rowCount = rowsVals.size();
                int colCount = -1;
                // determine column count from first row
                if (isList(rowsVals[0]))
                    colCount = asList(rowsVals[0])->length();
                else if (isVector(rowsVals[0]))
                    colCount = asVector(rowsVals[0])->length();
                else
                    throw std::runtime_error("matrix constructor expects list of lists or vectors");
                Eigen::MatrixXd vals(rowCount, colCount);
                for(size_t r=0; r<rowCount; ++r) {
                    auto rowVal = rowsVals[r];
                    if (isList(rowVal)) {
                        auto rowList = asList(rowVal)->elts.get();
                        if ((int)rowList.size() != colCount)
                            throw std::runtime_error("matrix rows must have equal length");
                        for(int c=0; c<colCount; ++c)
                            vals(r,c) = toType(ValueType::Real, rowList[c], false).asReal();
                    } else if (isVector(rowVal)) {
                        auto vec = asVector(rowVal);
                        if (vec->length() != colCount)
                            throw std::runtime_error("matrix rows must have equal length");
                        for(int c=0; c<colCount; ++c)
                            vals(r,c) = vec->vec[c];
                    } else {
                        throw std::runtime_error("matrix constructor expects list of lists or vectors");
                    }
                }
                return objVal(matrixVal(vals));
            }
            if (isVector(arg)) {
                auto vec = asVector(arg);
                Eigen::MatrixXd vals(1, vec->length());
                for(int c=0; c<vec->length(); ++c)
                    vals(0,c) = vec->vec[c];
                return objVal(matrixVal(vals));
            }
            if (isMatrix(arg)) {
                return objVal(cloneMatrix(asMatrix(arg)));
            }
            throw std::runtime_error("matrix constructor expects list, vector, or matrix");
        } else {
            throw std::runtime_error("matrix constructor with incorrect arg count");
        }
    }

    if (end - 1 == begin)
        // pass non-stict as this is an explicit construction/conversion
        return toType(type, *begin, /*strict=*/false);
    throw std::runtime_error("type constructors with >1 arg unimplemented");
    return nilVal();
}




ValueType roxal::binaryOpType(Value l, Value r)
{
    // Determine result builtin type of numeric/bool binary operations
    // according to conversions.md.

    // bool op bool -> bool
    if (l.isBool() && r.isBool())
        return ValueType::Bool;

    // Decimal has highest precedence after bool/bool
    if (l.type() == ValueType::Decimal || r.type() == ValueType::Decimal)
        return ValueType::Decimal;

    // Next is real if either operand is real
    if (l.isReal() || r.isReal())
        return ValueType::Real;

    // All remaining numeric combinations yield int
    return ValueType::Int;
}


bool roxal::isFalsey(const Value& v)
{
    return v.isNil() || (v.isBool() && !v.asBool());
}

bool roxal::isTruthy(const Value& v)
{
    return (v.isBool() && v.asBool());
}


bool roxal::valuesEqual(Value a, Value b)
{
    // First handle cases where both values are the same builtin type and
    // that type is expected to be common.
    if (a.type() == b.type()) {
        switch (a.type()) {
            case ValueType::Bool:   return a.asBool() == b.asBool();
            case ValueType::Nil:    return true; // only single Nil value
            case ValueType::Int:    return a.asInt() == b.asInt();
            case ValueType::Real:   return a.asReal() == b.asReal();
            case ValueType::Enum:   return (a.enumTypeId() == b.enumTypeId()) && (a.asEnum() == b.asEnum());
            case ValueType::Object: return objsEqual(a,b);
            case ValueType::Matrix: {
                ObjMatrix* am = asMatrix(a);
                ObjMatrix* bm = asMatrix(b);
                if (am->rows() != bm->rows() || am->cols() != bm->cols())
                    return false;
                for(int r=0;r<am->rows();++r)
                    for(int c=0;c<am->cols();++c)
                        if (am->mat(r,c) != bm->mat(r,c))
                            return false;
                return true;
            }
            case ValueType::Vector: {
                ObjVector* av = asVector(a);
                ObjVector* bv = asVector(b);
                if (av->length() != bv->length())
                    return false;
                for(int i=0;i<av->length();++i)
                    if (av->vec[i] != bv->vec[i])
                        return false;
                return true;
            }
            default:
                throw std::runtime_error("unimplemented equality test for types " + a.typeName() + " and " + b.typeName());
        }
    }

    // If both are numbers but of different type, compare as common numeric type.
    if (a.isNumber() && b.isNumber()) {
        ValueType compType(binaryOpType(a,b));
        switch(compType) {
            case ValueType::Int:  return a.asInt() == b.asInt();
            case ValueType::Real: return a.asReal() == b.asReal();
            default: throw std::runtime_error("unimplemented equality test for types " + a.typeName() + " and " + b.typeName());
        }
    }

    // Handle matrix and vector comparisons after common types.
    if (isMatrix(a) || isMatrix(b)) {
        try {
            if (!isMatrix(a)) {
                std::vector<Value> args{a};
                a = construct(ValueType::Matrix, args.begin(), args.end());
            }
            if (!isMatrix(b)) {
                std::vector<Value> args{b};
                b = construct(ValueType::Matrix, args.begin(), args.end());
            }
        } catch (...) {
            return false;
        }

        ObjMatrix* am = asMatrix(a);
        ObjMatrix* bm = asMatrix(b);
        if (am->rows() != bm->rows() || am->cols() != bm->cols())
            return false;
        for(int r=0;r<am->rows();++r)
            for(int c=0;c<am->cols();++c)
                if (am->mat(r,c) != bm->mat(r,c))
                    return false;
        return true;
    }

    if (isVector(a) || isVector(b)) {
        try {
            if (!isVector(a)) {
                std::vector<Value> args{a};
                a = construct(ValueType::Vector, args.begin(), args.end());
            }
            if (!isVector(b)) {
                std::vector<Value> args{b};
                b = construct(ValueType::Vector, args.begin(), args.end());
            }
        } catch (...) {
            return false;
        }

        ObjVector* av = asVector(a);
        ObjVector* bv = asVector(b);
        if (av->length() != bv->length())
            return false;
        for(int i=0;i<av->length();++i)
            if (av->vec[i] != bv->vec[i])
                return false;
        return true;
    }

    return false;
}


Value roxal::negate(Value v)
{
    if (!v.isNumber() && !v.isBool())
        throw std::invalid_argument("Operand must be a number or bool");

    if (v.isInt() || v.isByte())
        return intVal(-v.asInt());
    else if (v.isReal())
        return realVal(-v.asReal());
    else if (v.isBool())
        return boolVal(!v.asBool());
    // TODO: decimal

    throw std::runtime_error("unimplemented negation for type:"+v.typeName());
}


Value roxal::add(Value l, Value r)
{
    if (l.isNumber() && r.isNumber()) {
        ValueType resultType(binaryOpType(l,r));
        switch (resultType) {
            case ValueType::Int: return intVal(l.asInt()+r.asInt());
            case ValueType::Real: return realVal(l.asReal()+r.asReal());
            case ValueType::Byte: return byteVal(l.asByte()+r.asByte());
            //... decimal
            default: ;
        }
    }
    else if (isVector(l) && isVector(r)) {
        const ObjVector* lv = asVector(l);
        const ObjVector* rv = asVector(r);
        if (lv->length() != rv->length())
            throw std::invalid_argument("Vector addition requires vectors of same length");
        Eigen::VectorXd result = lv->vec + rv->vec;
        return objVal(vectorVal(result));
    }
    else if (isList(l) && isList(r)) {
        // List + List → concatenation (clone LHS, then concatenate RHS)
        ObjList* lv = asList(l);
        const ObjList* rv = asList(r);
        
        ObjList* result = cloneList(lv);  // Clone LHS for by-value semantics
        result->concatenate(rv);          // Concatenate RHS in-place
        
        return objVal(result);
    }
    else if (isList(l)) {
        // List + anything → append (clone LHS, then append RHS)
        ObjList* lv = asList(l);
        
        ObjList* result = cloneList(lv);  // Clone LHS for by-value semantics
        result->append(r);                // Append RHS in-place
        
        return objVal(result);
    }
    throw std::invalid_argument("unsupported operand types to add() - "+l.typeName()+" and "+r.typeName());
}


Value roxal::subtract(Value l, Value r)
{
    if (isVector(l) && isVector(r)) {
        const ObjVector* lv = asVector(l);
        const ObjVector* rv = asVector(r);
        if (lv->length() != rv->length())
            throw std::invalid_argument("Vector subtraction requires vectors of same length");
        Eigen::VectorXd result = lv->vec - rv->vec;
        return objVal(vectorVal(result));
    }

    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return intVal(l.asInt()-r.asInt());
        case ValueType::Real: return realVal(l.asReal()-r.asReal());
        case ValueType::Byte: return byteVal(l.asByte()-r.asByte());
        //... decimal
        default: ;
    }
    return Value();
}


Value roxal::multiply(Value l, Value r)
{
    if (isVector(l) && isVector(r)) {
        const ObjVector* lv = asVector(l);
        const ObjVector* rv = asVector(r);
        if (lv->length() != rv->length())
            throw std::invalid_argument("Vector dot product requires vectors of same length");
        double dot = lv->vec.dot(rv->vec);
        return realVal(dot);
    }
    if (isVector(l) && r.isNumber()) {
        const ObjVector* lv = asVector(l);
        double scalar = toType(ValueType::Real, r, false).asReal();
        Eigen::VectorXd result = lv->vec * scalar;
        return objVal(vectorVal(result));
    }
    if (l.isNumber() && isVector(r)) {
        const ObjVector* rv = asVector(r);
        double scalar = toType(ValueType::Real, l, false).asReal();
        Eigen::VectorXd result = rv->vec * scalar;
        return objVal(vectorVal(result));
    }

    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return intVal(l.asInt()*r.asInt());
        case ValueType::Real: return realVal(l.asReal()*r.asReal());
        case ValueType::Byte: return byteVal(l.asByte()*r.asByte());
        //... decimal
        default: ;
    }
    return Value();
}


Value roxal::divide(Value l, Value r)
{
    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return intVal(l.asInt()/r.asInt());
        case ValueType::Real: return realVal(l.asReal()/r.asReal());
        case ValueType::Byte: return byteVal(l.asByte()/r.asByte());
        //... decimal
        default: ;
    }
    return Value();
}


Value roxal::mod(Value l, Value r)
{
    // TODO: support Decimal

    if (!l.isNumber() && !l.isBool())
        throw std::invalid_argument("LHS must be an integer");
    if (!r.isNumber() && !r.isBool())
        throw std::invalid_argument("RHS must be an integer");

    int32_t lhs = toType(ValueType::Int, l, false).asInt();
    int32_t rhs = toType(ValueType::Int, r, false).asInt();
    return intVal(lhs % rhs);
}


Value roxal::land(Value l, Value r)
{
    if (!l.isBool())
        throw std::invalid_argument("LHS must be a bool");
    if (!r.isBool())
        throw std::invalid_argument("RHS must be a bool");

    return boolVal(l.asBool() && r.asBool());
}


Value roxal::lor(Value l, Value r)
{
    if (!l.isBool())
        throw std::invalid_argument("LHS must be a bool");
    if (!r.isBool())
        throw std::invalid_argument("RHS must be a bool");

    return boolVal(l.asBool() || r.asBool());
}



Value roxal::greater(Value l, Value r)
{
    if (l.isNumber() && r.isNumber()) {
        ValueType resultType(binaryOpType(l,r));
        switch (resultType) {
            case ValueType::Int: return boolVal(l.asInt() > r.asInt());
            case ValueType::Real: return boolVal(l.asReal() > r.asReal());
            case ValueType::Byte: return boolVal(l.asByte() > r.asByte());
            //... decimal
            default: return falseVal();
        }
    }
    else if (l.isObj() && r.isObj()) {
        if (isString(l) && isString(r)) {
            auto lstr = asString(l);
            auto rstr = asString(r);

            // if lhs & rhs are the same string, one is not greater than the other
            if (lstr->hash == rstr->hash)
                return falseVal();

            return boolVal(lstr->s.compareCodePointOrder(rstr->s) > 0);
        }
    }
    throw std::invalid_argument("Invalid arguments to greater operator - "+l.typeName()+" and "+r.typeName());
}


Value roxal::less(Value l, Value r)
{
    if (l.isNumber() && r.isNumber()) {
        ValueType resultType(binaryOpType(l,r));
        switch (resultType) {
            case ValueType::Int: return boolVal(l.asInt() < r.asInt());
            case ValueType::Real: return boolVal(l.asReal() < r.asReal());
            case ValueType::Byte: return boolVal(l.asByte() < r.asByte());
            //... decimal
            default: return falseVal();
        }
    }
    else if (l.isObj() && r.isObj()) {
        if (isString(l) && isString(r)) {
            auto lstr = asString(l);
            auto rstr = asString(r);

            // if lhs & rhs are the same string, one is not less than the other
            if (lstr->hash == rstr->hash)
                return falseVal();

            return boolVal(lstr->s.compareCodePointOrder(rstr->s) < 0);
        }
    }
    throw std::invalid_argument("Invalid arguments to less operator - "+l.typeName()+" and "+r.typeName());
}


std::string roxal::toString(const Value& v)
{
    if (v.isInt())
        return std::to_string(v.asInt());
    else if (v.isReal())
        return format("%g", v.asReal());
    else if (v.isByte())
        return std::to_string(v.asByte());
    else if (v.isBool())
        return v.asBool() ? "true" : "false";
    else if (v.isEnum()) {
        // lookup the enum type
        uint16_t enumTypeId = v.enumTypeId();
        auto enumIt = ObjObjectType::enumTypes.find(enumTypeId);
        if (enumIt == ObjObjectType::enumTypes.end())
            throw std::runtime_error("unknown enum type id: "+std::to_string(enumTypeId)+" value is "+std::to_string(v.asEnum()));
        ObjObjectType* enumTypeObj = enumIt->second;
        for(const auto& enumHashLabelValue : enumTypeObj->enumLabelValues) {
            const auto& labelValue {enumHashLabelValue.second };
            if (labelValue.second.asEnum() == v.asEnum())
                return toUTF8StdString(labelValue.first);
        }
        return ""; // throw? return default enum value?
    }
    else if (v.isType())
        return to_string(v.asType());
    else if (v.isNil())
        return "nil";
    else if (v.isObj())
        return objToString(v);
    throw std::runtime_error("unimplemented toString() for type:"+v.typeName());
}


std::ostream& roxal::operator<<(std::ostream& out, const Value& v)
{
    out << toString(v);
    return out;
}


std::mutex VariablesMap::globalsLock {};
VariablesMap::VarsMap VariablesMap::globals {};




#ifdef DEBUG_BUILD
void Value::testPrimitiveValues()
{
    #define bin64(v) std::bitset<64>(v)
    #define bin64v(v) std::bitset<64>(v.getVal())

    #ifdef NAN_TAGGING
    assert(sizeof(Value) == 64/8);
    #endif

    Value n {};
    assert(n.isNil());
    assert(!n.isBool());
    assert(!n.isInt());
    assert(!n.isReal());
    assert(!n.isObj());

    Value t(true);
    assert(t.isBool());
    assert(!t.isNil());
    assert(!t.isInt());
    assert(!t.isReal());
    assert(!t.isObj());
    assert(t.asBool() == true);
    Value f(false);
    assert(f.isBool());
    assert(!f.isNil());
    assert(!f.isInt());
    assert(!f.isReal());
    assert(!f.isObj());
    assert(f.asBool() == false);

    Value i1(1);
    assert(i1.isInt());
    assert(!i1.isNil());
    assert(!i1.isBool());
    assert(!i1.isReal());
    assert(!i1.isObj());
    assert(i1.asInt() == 1);

    i1 = Value(7);
    assert(i1.isInt());
    assert(i1.asInt() == 7);

    #undef bin64
    #undef bin64v
}
#endif
