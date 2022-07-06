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
    auto type = valueType(t);
    switch (type) {
    case ValueType::Nil: return "nil"; break; 
    case ValueType::Bool: return "bool"; break; 
    case ValueType::Byte: return "byte"; break;  
    case ValueType::Int: return "int"; break;
    case ValueType::Real: return "real"; break;
    case ValueType::Decimal: return "decimal"; break;
    case ValueType::String: return "string"; break;
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
        throw std::runtime_error("Unhandled type for to_string "+std::to_string(int(type)));
    }
}


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
        as.boolean = asPrimitive(*this)->as.integer;
    else if (isReal())
        as.boolean = asPrimitive(*this)->as.real;
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
        return as.boolean;
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


bool Value::operator==(const Value& rhs) const
{
    if (isNil() && rhs.isNil()) return true;
    if (isBool())
        return  asBool() == rhs.asBool();
    else if (isInt())
        return asInt() == rhs.asInt();
    else if (isReal())
        return asReal() == rhs.asReal();
    else if (isType())
        return asType() == rhs.asType();
    else if (isString(*this))
        return objsEqual(*this,rhs); // compares strings intelligently (e.g. using immutability & hash)
    else if (isObj())
        return asObj() == rhs.asObj(); // identity (by ptr/address)
    return false;
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





Value roxal::toType(ValueType t, Value v, bool strict)
{
    if (valueType(v.type()) == t)
        return v;

    switch (t) {
        case ValueType::Real: return realVal(v.asReal(strict));
        case ValueType::Int: return intVal(v.asInt(strict));
        case ValueType::String: {

        }
        //...
    }
    return nilVal(); 
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
        return toType(type, *begin, false);
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

