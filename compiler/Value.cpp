#include "common.h"

#include "Value.h"

#include "Object.h"


namespace roxal {
    // forward from Object.h
    std::string objToString(const Value& v);
    bool objsEqual(const Value& l, const Value& r);
}


using namespace roxal;


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
    as.obj->decRef();
}



void roxal::Value::incRefObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't incRef non-object");
    #endif
    as.obj->incRef();
}

void roxal::Value::decRefObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't decRef non-object");
    #endif
    as.obj->decRef();
}



int32_t Value::asInt() const
{
    if (!isBoxed()) {
        switch (_type) {
        case ValueType::Int: return as.integer;
        case ValueType::Real: return int32_t(as.real);
        case ValueType::Bool: return as.boolean ? 1 : 0;
        case ValueType::Decimal: return 0; // TODO: implement
        //case ValueType::String ... (if non-strict)
        default: ;
        }
    }
    else {
        switch (valueType(_type)) {
        case ValueType::Int: return asPrimitive(*this)->as.integer;
        case ValueType::Real: return int32_t(asPrimitive(*this)->as.real);
        case ValueType::Bool: return asPrimitive(*this)->as.boolean ? 1 : 0;
        default: ;
        }
    }
    return 0;
}


double Value::asReal() const
{ 
    if (!isBoxed()) {
        switch (_type) {
        case ValueType::Real: return as.real;
        case ValueType::Int: return double(as.integer);
        case ValueType::Bool: return as.boolean ? 1.0 : 0.0;
        case ValueType::Decimal: return 0.0; // TODO: implement
        //case ValueType::String ... (if non-strict)
        default: ;
        }
    }
    else {
        switch (_type) {
        case ValueType::Real: return asPrimitive(*this)->as.real;
        case ValueType::Int: return double(asPrimitive(*this)->as.integer);
        case ValueType::Bool: return asPrimitive(*this)->as.boolean ? 1.0 : 0.0;
        default: ;
        }
    }
    return 0.0;
}


std::string roxal::Value::typeName() const
{
    if (isNil())
        return "nil";
    else if (isBool())
        return "bool";
    else if (isInt())
        return "integer";
    else if (isReal())
        return "real";
    else if (isObj())
        return "object";
    //TODO: ...
    return "unknown";
}


bool Value::asBool() const
{ 
    return valueType(_type)==ValueType::Bool ? 
              (!isBoxed() ? as.boolean : asPrimitive(*this)->as.boolean) 
            : false; 
}




Value toType(ValueType t, Value v)
{
    if (v.type() == t)
        return v;

    switch (t) {
        case ValueType::Real: return realVal(v.asReal());
        case ValueType::Int: return intVal(v.asInt());
        //...
    }
    return Value(); // nil
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
    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return intVal(l.asInt()+r.asInt());
        case ValueType::Real: return realVal(l.asReal()+r.asReal());
        //... decimal, byte
        default: ;
    }
    return Value();
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
    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return boolVal(l.asInt() > r.asInt());
        case ValueType::Real: return boolVal(l.asReal() > r.asReal());
        //... decimal, byte
        default: ;
    }
    return falseVal();
}


Value roxal::less(Value l, Value r)
{
    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    ValueType resultType(binaryOpType(l,r));
    switch (resultType) {
        case ValueType::Int: return boolVal(l.asInt() < r.asInt());
        case ValueType::Real: return boolVal(l.asReal() < r.asReal());
        //... decimal, byte
        default: ;
    }
    return falseVal();
}


std::string roxal::toString(const Value& v)
{
    if (v.isInt())
        return std::to_string(v.asInt());
    else if (v.isReal())
        return format("%g", v.asReal());
    else if (v.isBool())
        return v.asBool() ? "true" : "false";
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

