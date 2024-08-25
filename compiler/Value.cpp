#include <cassert>
#include <bitset>
#include <core/common.h>

#include "Value.h"

#include "Object.h"
#include "Stream.h"


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
    case ValueType::Tensor: return "tensor"; break;
    case ValueType::Orient: return "orient"; break;
    case ValueType::Stream: return "stream"; break;
    case ValueType::Object: return "object"; break;
    case ValueType::Actor: return "actor"; break;
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
    if (!isBoxed()) {
        return val == (QNAN | TagTrue);
    }
    else {
        switch (asObj()->type) {
        case ObjType::Bool: return asObjPrimitive(*this)->as.boolean;
        default: ;
        }
    }
    return false;
}



uint8_t Value::asByte(bool strict) const
{
    Value unboxed;
    Value const* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->type()) {
        case ValueType::Int: { uint64_t i {v->val & ~(QNAN | TypeTag)} ; return *reinterpret_cast<uint8_t*>(&i); }
        case ValueType::Real: return uint8_t(*reinterpret_cast<const double*>(&val));
        case ValueType::Bool: return (val == (QNAN | TagTrue)) ? 1 : 0;
        case ValueType::Decimal: return 0; // TODO: implement
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
    return 0;
}


int32_t Value::asInt(bool strict) const
{
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
        case ValueType::Int: { uint64_t i {v->val & ~(QNAN | TypeTag)} ; return *reinterpret_cast<int32_t*>(&i); }
        case ValueType::Real: return int32_t(*reinterpret_cast<const double*>(&val));
        case ValueType::Bool: return (val == (QNAN | TagTrue)) ? 1 : 0;
        case ValueType::Decimal: return 0; // TODO: implement
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
    Value unboxed;
    Value const* v { this };
    if (isBoxed()) {
        unboxed = *this;
        unboxed.unbox();
        v = &unboxed;
    }

    if (!v->isObj()) {
        switch (v->type()) {
        case ValueType::Real: return *reinterpret_cast<const double*>(&val);
        case ValueType::Int: { uint64_t i {v->val & ~(QNAN | TypeTag)}; return double(*reinterpret_cast<int32_t*>(&i)); }
        case ValueType::Bool: return (val == (QNAN | TagTrue)) ? 1.0 : 0.0;
        case ValueType::Decimal: return 0.0; // TODO: implement
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
    if (!isBoxed()) {
        return as.boolean; // TODO: should this convert?
    }
    else {
        switch (valueType(_type)) {
        case ValueType::Bool: return asPrimitive(*this)->as.boolean;
        default: ;
        }
    }
    return false;
}





int32_t Value::asInt(bool strict) const
{
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
        case ValueType::Real: return int32_t(v->as.real);
        case ValueType::Bool: return v->as.boolean ? 1 : 0;
        case ValueType::Decimal: return 0; // TODO: implement
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
    return 0;
}


double Value::asReal(bool strict) const
{
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
        case ValueType::Decimal: return 0.0; // TODO: implement
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



void Value::resolveFuture()
{
    if (isFuture(*this))
        *this = asFuture(*this)->asValue();
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
        case ValueType::Stream: return Value(streamVal(0.0,intVal(0))); // manually (or runtime?) clocked seq of 0s
        case ValueType::Vector:
        case ValueType::Matrix:
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
                    auto propName { stringVal(std::get<0>(property.second)) };
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
    auto argCount = end - begin;
    if (type == ValueType::Stream) {
        Value stream;
        if (argCount == 0)
            stream = objVal(streamVal(1.0,intVal(0)));
        else if (argCount == 1)
            stream = objVal(streamVal(1.0,*begin));
        else if (argCount == 2)
            stream = objVal(streamVal((begin+1)->asReal(false),*begin));
        else
            throw std::runtime_error("stream(initial=0,freq=1) constructor requires 0,1 or 2 arguments");
        return stream;
    }
    if (end - 1 == begin)
        // pass non-stict as this is an explicit construction/conversion
        return toType(type, *begin, /*strict=*/false);
    throw std::runtime_error("type constructors with >1 arg unimplemented");
    return nilVal();
}




ValueType roxal::binaryOpType(Value l, Value r)
{
    ValueType resultType { ValueType::Real };
    if (l.type() == r.type())
        resultType = l.type();
    else if (l.isReal() || r.isReal())
        resultType = ValueType::Real;
    else if (l.isInt() || r.isInt())
        resultType = ValueType::Int;
    // TODO: ... byte, decimal
    return resultType;
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
    if (a.type() != b.type()) {
        if (!a.isNumber() || !b.isNumber())
            return false;

        ValueType compType(binaryOpType(a,b));
        switch(compType) {
            case ValueType::Int: return a.asInt() == b.asInt();
            case ValueType::Real: return a.asReal() == b.asReal();
            default: throw std::runtime_error("unimplemented equality test for types "+a.typeName()+" and "+b.typeName());
        }
    }
    else {
        switch (a.type()) {
            case ValueType::Bool: return a.asBool() == b.asBool();
            case ValueType::Nil:  return true; // only single Nil, so must both be Nil
            case ValueType::Int:  return a.asInt() == b.asInt();
            case ValueType::Real: return a.asReal() == b.asReal();
            // same type & label value:
            case ValueType::Enum: return (a.enumTypeId() == b.enumTypeId()) && (a.asEnum() == b.asEnum());
            case ValueType::Object: return objsEqual(a,b);
            default: throw std::runtime_error("unimplemented equality test for types "+a.typeName()+" and "+b.typeName());
        }
    }
    return false;
}


Value roxal::negate(Value v)
{
    if (!v.isNumber() && !v.isBool())
        throw std::invalid_argument("Operand must be a number or bool");

    if (v.isInt())
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
            //... decimal, byte
            default: ;
        }
    }
    else if (isStream(l) || isStream(r)) {
        return Stream::add(l,r);
    }
    throw std::invalid_argument("unsupported operand types to add() - "+l.typeName()+" and "+r.typeName());
}


Value roxal::subtract(Value l, Value r)
{
    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return intVal(l.asInt()-r.asInt());
        case ValueType::Real: return realVal(l.asReal()-r.asReal());
        //... decimal, byte
        default: ;
    }
    return Value();
}


Value roxal::multiply(Value l, Value r)
{
    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return intVal(l.asInt()*r.asInt());
        case ValueType::Real: return realVal(l.asReal()*r.asReal());
        //... decimal, byte
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
        //... decimal, byte
        default: ;
    }
    return Value();
}


Value roxal::mod(Value l, Value r)
{
    // TODO: support Decimal
    if (!l.isInt())
        throw std::invalid_argument("LHS must be an integer");
    if (!r.isInt())
        throw std::invalid_argument("RHS must be an integer");

    return intVal(l.asInt() % r.asInt());
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
            //... decimal, byte
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
            //... decimal, byte
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
