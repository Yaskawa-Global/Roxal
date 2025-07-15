#include <cassert>
#include <bitset>
#include <core/common.h>

#include "Value.h"

#include "Object.h"
#include "dataflow/Signal.h"
#include "dataflow/FuncNode.h"
#include "dataflow/DataflowEngine.h"
#include "Thread.h"
#include "VM.h"
#include <core/types.h>
#include <Eigen/Dense>
#include <functional>
#include <memory>
#include <cmath>
#include <sstream>


namespace roxal {
static thread_local ptr<SerializationContext> writeCtx = nullptr;
static thread_local int writeDepth = 0;
static thread_local ptr<SerializationContext> readCtx = nullptr;
static thread_local int readDepth = 0;

SerializationContext* serializationWriteContext() { return writeCtx.get(); }
SerializationContext* serializationReadContext() { return readCtx.get(); }
void enterWriteContext() {
    if(writeDepth==0) writeCtx = make_ptr<SerializationContext>();
    writeDepth++;
}
void exitWriteContext() {
    writeDepth--;
    if(writeDepth==0) { writeCtx = nullptr; }
}
void enterReadContext() {
    if(readDepth==0) readCtx = make_ptr<SerializationContext>();
    readDepth++;
}
void exitReadContext() {
    readDepth--;
    if(readDepth==0) { readCtx = nullptr; }
}
} // namespace roxal


namespace roxal {
    // forward from Object.h
    std::string objToString(const Value& v);
    bool objsEqual(const Value& l, const Value& r);
    static Value signalUnaryOp(const std::string& name,
                               const std::function<Value(Value)>& op,
                               Value v);
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
    case ValueType::Event: return "event"; break;
    case ValueType::Function: return "function"; break;
    case ValueType::Closure: return "closure"; break;
    case ValueType::Upvalue: return "upvalue"; break;
    default:
        throw std::runtime_error("Unhandled type for to_string "+std::to_string(int(t)));
    }
}



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

void roxal::Value::incWeakObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't incWeak non-object type "+typeName());
    #endif
    asControl()->weak.fetch_add(1,std::memory_order_relaxed);
}

void roxal::Value::decWeakObj()
{
    #ifdef DEBUG_BUILD
    if (!isObj() && !isBoxable())
        throw std::runtime_error("Can't decWeak non-object type "+typeName());
    #endif
    if (asControl()->weak.fetch_sub(1,std::memory_order_relaxed) == 1)
        delete[] reinterpret_cast<char*>(asControl());
}



bool Value::asBool(bool strict) const
{
    if (isFuture(*this))
        return asFuture(*this)->asValue().asBool(strict);
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
    if (isFuture(*this))
        return asFuture(*this)->asValue().asByte(strict);
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
    if (isFuture(*this))
        return asFuture(*this)->asValue().asInt(strict);
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
    if (isFuture(*this))
        return asFuture(*this)->asValue().asReal(strict);
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
    // FIXME: For efficiency, the enum values between ValueType and ObjType should be synchronized
    // and the Value::type() made efficient by returning a cast from ObjType -> ValueType for all
    // object cases to avoid a cascade of is*() checks for every single type.
    return isNil() ? ValueType::Nil
                    : (isBool() ? ValueType::Bool
                                : (isByte() ? ValueType::Byte
                                            : (isInt() ? ValueType::Int
                                                       : (isReal() ? ValueType::Real
                                                                   : (isObj() ? asObj()->valueType()
                                                                              : ValueType((val & TypeTag) >> TypeTagOffset) ) ) ) ) );
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



bool Value::equals(const Value& rhs, bool strict) const
{
    // TODO: handle unboxing

    // Handle nil cases
    if (isNil())
        return rhs.isNil();
    if (rhs.isNil())
        return isNil();

    // Fast path for same primitive types
    if (isBool())
        return rhs.isBool() && asBool() == rhs.asBool();
    else if (isInt())
        return rhs.isInt() && asInt() == rhs.asInt();
    else if (isReal())
        return rhs.isReal() && asReal() == rhs.asReal();
    else if (isType())
        return rhs.isType() && asType() == rhs.asType();
    else if (isEnum())
        return rhs.isEnum() && (enumTypeId() == rhs.enumTypeId()) && (asEnum() == rhs.asEnum());
    else if (isString(*this))
        return objsEqual(*this, rhs); // compares strings intelligently (e.g. using immutability & hash)

    // Handle mixed numeric type comparisons
    else if (isNumber() && rhs.isNumber()) {
        ValueType compType(binaryOpType(*this, rhs));
        switch(compType) {
            case ValueType::Int:  return asInt() == rhs.asInt();
            case ValueType::Real: return asReal() == rhs.asReal();
            default: throw std::runtime_error("unimplemented mixed numeric equality for types " + typeName() + " and " + rhs.typeName());
        }
    }
    // Vector comparisons
    else if (isVector(*this) && isVector(rhs)) {
        // Deep equality for vectors - compare elements
        return asVector(*this)->equals(asVector(rhs));
    }
    else if (isVector(*this) && isList(rhs)) {
        // Vector compared to list - auto-conversion based on strict mode
        ObjList* rhsList = asList(rhs);
        if (strict && rhsList->length() > 1) {
            // In strict mode, only allow 0 or 1 element lists (due to [] and [1] ambiguity)
            return false;
        }

        // Check if all elements are numeric
        bool allNumeric = true;
        for (int i = 0; i < rhsList->length(); i++) {
            if (!rhsList->elts.at(i).isNumber()) {
                allNumeric = false;
                break;
            }
        }

        if (allNumeric && rhsList->length() <= 1) {
            // For 0-1 element lists, do manual comparison (both strict and non-strict)
            ObjVector* lhsVec = asVector(*this);
            if (lhsVec->length() != rhsList->length())
                return false;
            for (int i = 0; i < rhsList->length(); i++) {
                if (std::abs(lhsVec->vec[i] - rhsList->elts.at(i).asReal()) > 1e-15)
                    return false;
            }
            return true;
        }
        return false;
    }
    else if (isList(*this) && isVector(rhs)) {
        // List compared to vector - symmetric case
        return rhs.equals(*this, strict);
    }
    // Matrix comparisons
    else if (isMatrix(*this) && isMatrix(rhs)) {
        // Deep equality for matrices - compare elements
        return asMatrix(*this)->equals(asMatrix(rhs));
    }
    else if (isMatrix(*this) && isList(rhs)) {
        // Matrix compared to list - auto-conversion based on strict mode
        ObjList* rhsList = asList(rhs);
        if (strict && rhsList->length() > 1) {
            // In strict mode, only allow 0 or 1 element lists (due to [] and [1] ambiguity)
            return false;
        }

        // Check if all elements are numeric
        bool allNumeric = true;
        for (int i = 0; i < rhsList->length(); i++) {
            if (!rhsList->elts.at(i).isNumber()) {
                allNumeric = false;
                break;
            }
        }

        if (allNumeric) {
            // In non-strict mode, try to convert list to matrix using construct()
            if (!strict && rhsList->length() > 1) {
                try {
                    std::vector<Value> args{rhs};
                    Value convertedMatrix = construct(ValueType::Matrix, args.begin(), args.end());
                    return asMatrix(*this)->equals(asMatrix(convertedMatrix));
                } catch (...) {
                    return false;
                }
            } else {
                // For 0-1 element lists, do manual comparison (both strict and non-strict)
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
        return rhs.equals(*this, strict);
    }
    else if (isObj()) {
        if (!rhs.isObj())
            return false;
        if (!isAlive() || !rhs.isAlive())
            return false;
        return asObj() == rhs.asObj(); // identity (by ptr/address)
    }
    return false;
}

bool Value::is(const Value& rhs, bool strict) const
{
    if (isTypeSpec(*this)) {
        if (isTypeSpec(rhs))
            return asTypeSpec(*this) == asTypeSpec(rhs);
        if (rhs.isType())
            return asTypeSpec(*this)->typeValue == rhs.asType();
    }
    else if (isType() && isTypeSpec(rhs)) {
        return asType() == asTypeSpec(rhs)->typeValue;
    }
    if (isTypeSpec(rhs)) {
        ObjTypeSpec* ts = asTypeSpec(rhs);
        switch (ts->typeValue) {
            case ValueType::Object:
                if (isObjectInstance(*this)) {
                    ObjObjectType* t = asObjectInstance(*this)->instanceType;
                    while (t) {
                        if (t == ts)
                            return true;
                        if (t->superType.isNil()) break;
                        t = asObjectType(t->superType);
                    }
                    return false;
                }
                if (isException(*this) && isObjectType(rhs)) {
                    ObjException* ex = asException(*this);
                    if (isTypeSpec(ex->exType)) {
                        ObjObjectType* et = asObjectType(ex->exType);
                        ObjObjectType* target = asObjectType(rhs);
                        while (et) {
                            if (et == target)
                                return true;
                            if (et->superType.isNil()) break;
                            et = asObjectType(et->superType);
                        }
                    }
                }
                break;
            case ValueType::Actor:
                if (isActorInstance(*this)) {
                    ObjObjectType* t = asActorInstance(*this)->instanceType;
                    while (t) {
                        if (t == ts)
                            return true;
                        if (t->superType.isNil()) break;
                        t = asObjectType(t->superType);
                    }
                    return false;
                }
                break;
            default:
                return type() == ts->typeValue;
        }
        return false;
    }
    else if (rhs.isType()) {
        if (isType())
            return asType() == rhs.asType();
        if (isTypeSpec(*this))
            return asTypeSpec(*this)->typeValue == rhs.asType();
        return type() == rhs.asType();
    }
    else if (isObj()) // reference value identity
        return rhs.isObj() && (asObj() == rhs.asObj()); // same ptr/address

    // assume builtin value types, use equality
    return equals(rhs, strict);
}

bool Value::operator==(const Value& rhs) const
{
    return equals(rhs, false); // Default to non-strict mode
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

Value Value::weakRef() const
{
    if (!isObj())
        return *this; // primitives just copy
    Value v;
    ObjControl* c = asObj()->control;
    v.val = SignBit | QNAN | WeakMask | uint64_t(uintptr_t(c));
    v.incWeakObj();
    return v;
}

Value Value::strongRef() const
{
    if (!isWeak())
        return *this;

    if (!isAlive())
        return nilVal();

    Obj* obj = asControl()->obj;
    // Increment strong count before constructing Value to ensure object stays alive
    obj->incRef();
    Value v;
    v.val = SignBit | QNAN | uint64_t(uintptr_t(obj));
    return v;
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
        case ValueType::Event:   return BuiltinType::Event;
        case ValueType::Function:
        case ValueType::Closure: return BuiltinType::Func;
        case ValueType::Upvalue: return BuiltinType::Object;
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

std::vector<std::tuple<std::string,bool,std::string>> roxal::testValueSerialization()
{
    using Result = std::tuple<std::string,bool,std::string>;
    std::vector<Result> results;

    auto roundTrip = [&results](const std::string& name, const Value& v)
    {
        try {
            std::stringstream ss(std::ios::in | std::ios::out | std::ios::binary);
            writeValue(ss, v);
            ss.seekg(0);
            Value read = readValue(ss);
            bool pass = false;

            if (isRange(v) && isRange(read)) {
                auto r1 = asRange(v); auto r2 = asRange(read);
                pass = r1->start.equals(r2->start, true) &&
                       r1->stop.equals(r2->stop, true) &&
                       r1->step.equals(r2->step, true) &&
                       r1->closed == r2->closed;
            }
            else if (isList(v) && isList(read)) {
                auto l1 = asList(v); auto l2 = asList(read);
                pass = l1->length() == l2->length();
                if (pass) {
                    for(int i=0;i<l1->length();i++)
                        if(!l1->elts.at(i).equals(l2->elts.at(i), true)) { pass=false; break; }
                }
            }
            else if (isDict(v) && isDict(read)) {
                auto d1 = asDict(v); auto d2 = asDict(read);
                auto items1 = d1->items();
                auto items2 = d2->items();
                pass = items1.size() == items2.size();
                if(pass) {
                    for(size_t i=0;i<items1.size();i++) {
                        if(!items1[i].first.equals(items2[i].first, true) ||
                           !items1[i].second.equals(items2[i].second, true)) { pass=false; break; }
                    }
                }
            }
            else if (isObjPrimitive(v) && isObjPrimitive(read)) {
                auto p1 = asObjPrimitive(v); auto p2 = asObjPrimitive(read);
                pass = p1->type == p2->type;
                if(pass) {
                    switch(p1->type) {
                        case ObjType::Bool:   pass = p1->as.boolean == p2->as.boolean; break;
                        case ObjType::Int:    pass = p1->as.integer == p2->as.integer; break;
                        case ObjType::Real:   pass = std::abs(p1->as.real - p2->as.real) < 1e-15; break;
                        case ObjType::Type:   pass = p1->as.btype == p2->as.btype; break;
                        default: pass = false; break;
                    }
                }
            }
            else {
                pass = v.equals(read, true);
            }

            results.push_back({name, pass, pass ? "ok" : std::string("got ") + toString(read)});
        } catch (std::exception& e) {
            results.push_back({name, false, std::string("exception: ") + e.what()});
        }
    };

    roundTrip("bool_true", boolVal(true));
    roundTrip("byte_val", byteVal(123));
    roundTrip("int_val", intVal(-42));
    roundTrip("real_val", realVal(3.5));
    roundTrip("string_val", objVal(stringVal(UnicodeString("hello"))));
    roundTrip("range_val", objVal(rangeVal(intVal(1), intVal(3), intVal(1), false)));

    ObjList* lst = listVal();
    lst->append(intVal(1));
    lst->append(intVal(2));
    roundTrip("list_val", objVal(lst));

    ObjDict* d = dictVal();
    d->store(intVal(1), intVal(2));
    roundTrip("dict_val", objVal(d));

    ObjVector* vec = vectorVal(2);
    vec->vec[0] = 1.0;
    vec->vec[1] = 2.0;
    roundTrip("vector_val", objVal(vec));

    Eigen::MatrixXd mat(1,2);
    mat(0,0) = 1.0; mat(0,1) = 2.0;
    roundTrip("matrix_val", objVal(matrixVal(mat)));

    roundTrip("boxed_bool", objVal(newObj<ObjPrimitive>(__func__, true)));
    roundTrip("boxed_int", objVal(newObj<ObjPrimitive>(__func__, int32_t(-7))));
    roundTrip("boxed_real", objVal(newObj<ObjPrimitive>(__func__, 1.25)));

    // simple chunk round trip
    {
        try {
            Chunk ch(toUnicodeString("pkg"), toUnicodeString("mod"), toUnicodeString("src"));
            ch.write(OpCode::ConstNil,0,0);
            ch.write(OpCode::Return,0,0);
            std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
            ch.serialize(ss);
            ss.seekg(0);
            Chunk ch2(toUnicodeString(""),toUnicodeString(""),toUnicodeString(""));
            ch2.deserialize(ss);
            bool pass = (ch.code == ch2.code) && (ch.constants.size()==ch2.constants.size());
            if(pass) {
                for(size_t i=0;i<ch.constants.size();i++)
                    if(!ch.constants[i].equals(ch2.constants[i],true)) { pass=false; break; }
            }
            results.push_back({"chunk_round", pass, pass?"ok":"mismatch"});
        } catch(std::exception& e) {
            results.push_back({"chunk_round", false, std::string("exception: ")+e.what()});
        }
    }

    // function round trip
    {
        try {
            ObjFunction* fn = functionVal(toUnicodeString("pkg"), toUnicodeString("mod"), toUnicodeString("src"));
            fn->name = toUnicodeString("fn");
            fn->arity = 0; fn->upvalueCount = 0; fn->strict=false; fn->fnType=FunctionType::Function;
            fn->chunk->write(OpCode::ConstNil,0,0);
            fn->chunk->write(OpCode::Return,0,0);
            std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
            fn->write(ss);
            ss.seekg(0);
            auto fn2 = newObj<ObjFunction>(__func__, toUnicodeString(""), toUnicodeString(""), toUnicodeString(""));
            fn2->read(ss);
            bool pass = fn->name == fn2->name && fn->arity==fn2->arity && fn->upvalueCount==fn2->upvalueCount && fn->chunk->code==fn2->chunk->code;
            results.push_back({"function_round", pass, pass?"ok":"mismatch"});
            delObj(fn2);
            delObj(fn);
        } catch(std::exception& e) {
            results.push_back({"function_round", false, std::string("exception: ")+e.what()});
        }
    }

    // closure round trip
    {
        try {
            ObjFunction* fn = functionVal(toUnicodeString("pkg"), toUnicodeString("mod"), toUnicodeString("src"));
            fn->name = toUnicodeString("cl");
            fn->arity = 0; fn->upvalueCount = 1;
            fn->chunk->write(OpCode::ConstNil,0,0);
            fn->chunk->write(OpCode::Return,0,0);
            ObjClosure* cl = closureVal(fn);
            Value* local = new Value(intVal(3));
            cl->upvalues[0] = upvalueVal(local);
            std::stringstream ss(std::ios::in|std::ios::out|std::ios::binary);
            cl->write(ss);
            ss.seekg(0);
            auto cl2 = newObj<ObjClosure>(__func__, nullptr);
            cl2->read(ss);
            bool pass = cl2->function->name == cl->function->name && cl2->upvalues.size()==cl->upvalues.size() && cl2->upvalues[0]->closed.equals(intVal(3), true);
            results.push_back({"closure_round", pass, pass?"ok":"mismatch"});
            delObj(cl2);
            delObj(cl);
            delete local;
        } catch(std::exception& e) {
            results.push_back({"closure_round", false, std::string("exception: ")+e.what()});
        }
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
        case ValueType::Event: return Value(eventVal());
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
            // allow conversion of typed exceptions to just the message text
            if (isException(v)) {
                ObjException* ex = asException(v);
                Value msg = ex->message;
                if (isString(msg))
                    return msg;
                if (!msg.isNil())
                    return Value(stringVal(toUnicodeString(toString(msg))));
                return Value(stringVal(UnicodeString()));
            }

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
                for(int32_t hash : vObjType->propertyOrder) {
                    const auto& prop { vObjType->properties.at(hash) };
                    if (prop.access != ast::Access::Public)
                        continue;
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


Value roxal::toType(const Value& typeSpec, Value v, bool strict)
{
    if (isTypeSpec(typeSpec)) {
        ObjTypeSpec* ts = asTypeSpec(typeSpec);

        if (ts->typeValue == ValueType::Nil)
            return v;

        if (ts->typeValue == ValueType::Object || ts->typeValue == ValueType::Actor) {
            if (v.is(typeSpec))
                return v;
            throw std::invalid_argument("unable to convert value of type "+to_string(v.type())+" to "+to_string(ts->typeValue));
        }

        return toType(ts->typeValue, v, strict);
    }

    if (typeSpec.isType())
        return toType(typeSpec.asType(), v, strict);

    throw std::invalid_argument("toType: typeSpec argument is not a type specification");
}


Value roxal::construct(ValueType type, std::vector<Value>::const_iterator begin, std::vector<Value>::const_iterator end)
{
    // If a single signal of the target built-in type is passed, sample it and use its value.
    if ((end - begin) == 1 && type != ValueType::Signal) {
        const Value& arg0 = *begin;
        if (isSignal(arg0)) {
            Value sample = asSignal(arg0)->signal->lastValue();
            if (sample.type() == type) {
                // If the sampled value is an object, make a fresh copy via its constructor;
                // otherwise return the primitive value directly.
                if (sample.isObj()) {
                    std::vector<Value> sampleArg{sample};
                    return construct(type, sampleArg.begin(), sampleArg.end());
                }
                return sample;
            }
        }
    }

    if (type == ValueType::Signal) {
        size_t count = end - begin;
        if (count < 1 || count > 2 || !(*begin).isNumber())
            throw std::runtime_error("signal constructor expects frequency and optional initial value");

        double freq = toType(ValueType::Real, *begin, false).asReal();
        Value initial = nilVal();
        if (count == 2)
            initial = *(begin + 1);

        auto sig = df::Signal::newSourceSignal(freq, initial);
        return objVal(signalVal(sig));
    }

    if (type == ValueType::Byte) {
        size_t count = end - begin;
        if (count == 1) {
            Value arg = *begin;
            if (isList(arg)) {
                auto bits = asList(arg)->elts.get();
                if (bits.size() != 8)
                    throw std::runtime_error("byte constructor expects list of 8 bools or 0/1 ints");
                uint8_t value = 0;
                for (size_t i = 0; i < bits.size(); ++i) {
                    Value b = bits[i];
                    bool bit;
                    if (b.isBool())
                        bit = b.asBool();
                    else if (b.isInt() || b.isByte()) {
                        int iv = b.asInt(false);
                        if (iv != 0 && iv != 1)
                            throw std::runtime_error("byte bit list elements must be 0 or 1");
                        bit = iv != 0;
                    } else {
                        throw std::runtime_error("byte constructor expects list of bools or ints");
                    }
                    if (bit)
                        value |= uint8_t(1u << (7 - i));
                }
                return byteVal(value);
            }
        }
    }

    if (type == ValueType::Int) {
        size_t count = end - begin;
        if (count == 1) {
            Value arg = *begin;
            if (isList(arg)) {
                auto parts = asList(arg)->elts.get();
                if (parts.size() == 32) {
                    uint32_t value = 0;
                    for (size_t i = 0; i < parts.size(); ++i) {
                        Value p = parts[i];
                        bool bit;
                        if (p.isBool())
                            bit = p.asBool();
                        else if (p.isInt() || p.isByte()) {
                            int iv = p.asInt(false);
                            if (iv != 0 && iv != 1)
                                throw std::runtime_error("int bit list elements must be 0 or 1");
                            bit = iv != 0;
                        } else {
                            throw std::runtime_error("int constructor expects list of bools or ints");
                        }
                        if (bit)
                            value |= (1u << (31 - i));
                    }
                    int32_t result = *reinterpret_cast<int32_t*>(&value);
                    return intVal(result);
                } else if (parts.size() == 4) {
                    uint32_t value = 0;
                    for (size_t i = 0; i < 4; ++i) {
                        uint8_t b = toType(ValueType::Byte, parts[i], false).asByte(false);
                        value |= uint32_t(b) << (8 * (3 - i));
                    }
                    int32_t result = *reinterpret_cast<int32_t*>(&value);
                    return intVal(result);
                } else {
                    throw std::runtime_error("int constructor expects list of 32 bits or 4 bytes");
                }
            }
        }
    }
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
                for(size_t i=0; i<listVals.size(); ++i) {
                    if (!listVals[i].isNumber() && !isSignal(listVals[i]))
                        throw std::runtime_error("vector constructor expects list of numeric elements");
                    vals[i] = toType(ValueType::Real, listVals[i], false).asReal();
                }
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




Value roxal::negate(Value v)
{
    if (v.isInt() || v.isByte())
        return intVal(-v.asInt());
    else if (v.isReal())
        return realVal(-v.asReal());
    else if (isVector(v)) {
        const ObjVector* vec = asVector(v);
        Eigen::VectorXd result = -vec->vec;
        return objVal(vectorVal(result));
    }
    else if (isMatrix(v)) {
        const ObjMatrix* mat = asMatrix(v);
        Eigen::MatrixXd result = -mat->mat;
        return objVal(matrixVal(result));
    }
    else if (v.isBool())
        return boolVal(!v.asBool());
    else if (v.isNil())
        return boolVal(true);

    // TODO: decimal

    if (isSignal(v)) {
        return signalUnaryOp("negate",
                             [](Value a) { return negate(a); },
                             v);
    }

    throw std::invalid_argument("Operand must be a number, bool or nil");
}

static Value roxal::signalUnaryOp(const std::string& name,
                                  const std::function<Value(Value)>& op,
                                  Value v)
{
    df::FuncNode::ConstArgMap constArgs;
    std::vector<ptr<df::Signal>> sigArgs;
    std::vector<std::string> paramNames{"val"};

    if (isSignal(v))
        sigArgs.push_back(asSignal(v)->signal);
    else
        constArgs["val"] = v;

    auto uniqueName = df::DataflowEngine::uniqueFuncName(name);
    auto node = roxal::make_ptr<df::FuncNode>(
        uniqueName,
        [op](const df::Values& vals) -> df::Values {
            return df::Values{ op(vals[0]) };
        },
        paramNames,
        constArgs,
        sigArgs);

    node->addToEngine();
    auto outputs = node->outputs();
    df::DataflowEngine::instance()->evaluate();
    return objVal(signalVal(outputs[0]));
}

static Value signalBinaryOp(const std::string& name,
                            const std::function<Value(Value, Value)>& op,
                            Value l, Value r)
{
    df::FuncNode::ConstArgMap constArgs;
    std::vector<ptr<df::Signal>> sigArgs;
    std::vector<std::string> paramNames{"lhs", "rhs"};

    if (isSignal(l))
        sigArgs.push_back(asSignal(l)->signal);
    else
        constArgs["lhs"] = l;

    if (isSignal(r))
        sigArgs.push_back(asSignal(r)->signal);
    else
        constArgs["rhs"] = r;

    auto uniqueName = df::DataflowEngine::uniqueFuncName(name);
    auto node = roxal::make_ptr<df::FuncNode>(
        uniqueName,
        [op](const df::Values& vals) -> df::Values {
            return df::Values{ op(vals[0], vals[1]) };
        },
        paramNames,
        constArgs,
        sigArgs);

    node->addToEngine();
    auto outputs = node->outputs();
    df::DataflowEngine::instance()->evaluate();
    return objVal(signalVal(outputs[0]));
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
    else if (isMatrix(l) && isMatrix(r)) {
        const ObjMatrix* lm = asMatrix(l);
        const ObjMatrix* rm = asMatrix(r);
        if (lm->rows() != rm->rows() || lm->cols() != rm->cols())
            throw std::invalid_argument("Matrix addition requires matrices of same size");
        Eigen::MatrixXd result = lm->mat + rm->mat;
        return objVal(matrixVal(result));
    }
    else if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("add",
                              [](Value a, Value b) { return add(a, b); },
                              l, r);
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
    else if (isMatrix(l) && isMatrix(r)) {
        const ObjMatrix* lm = asMatrix(l);
        const ObjMatrix* rm = asMatrix(r);
        if (lm->rows() != rm->rows() || lm->cols() != rm->cols())
            throw std::invalid_argument("Matrix subtraction requires matrices of same size");
        Eigen::MatrixXd result = lm->mat - rm->mat;
        return objVal(matrixVal(result));
    }
    else if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("subtract",
                              [](Value a, Value b) { return subtract(a, b); },
                              l, r);
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
    if (isMatrix(l) && isMatrix(r)) {
        const ObjMatrix* lm = asMatrix(l);
        const ObjMatrix* rm = asMatrix(r);
        if (lm->cols() != rm->rows())
            throw std::invalid_argument("Matrix multiplication dimension mismatch");
        Eigen::MatrixXd result = lm->mat * rm->mat;
        return objVal(matrixVal(result));
    }
    if (isMatrix(l) && isVector(r)) {
        const ObjMatrix* lm = asMatrix(l);
        const ObjVector* rv = asVector(r);
        if (lm->cols() != rv->length())
            throw std::invalid_argument("Matrix and vector dimension mismatch");
        Eigen::VectorXd result = lm->mat * rv->vec;
        return objVal(vectorVal(result));
    }
    if (isVector(l) && isMatrix(r)) {
        const ObjVector* lv = asVector(l);
        const ObjMatrix* rm = asMatrix(r);
        if (lv->length() != rm->rows())
            throw std::invalid_argument("Vector and matrix dimension mismatch");
        Eigen::VectorXd result = lv->vec.transpose() * rm->mat;
        return objVal(vectorVal(result));
    }
    if (isMatrix(l) && r.isNumber()) {
        const ObjMatrix* lm = asMatrix(l);
        double scalar = toType(ValueType::Real, r, false).asReal();
        Eigen::MatrixXd result = lm->mat * scalar;
        return objVal(matrixVal(result));
    }
    if (l.isNumber() && isMatrix(r)) {
        const ObjMatrix* rm = asMatrix(r);
        double scalar = toType(ValueType::Real, l, false).asReal();
        Eigen::MatrixXd result = scalar * rm->mat;
        return objVal(matrixVal(result));
    }
    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("multiply",
                              [](Value a, Value b) { return multiply(a, b); },
                              l, r);
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
    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("divide",
                              [](Value a, Value b) { return divide(a, b); },
                              l, r);
    }

    if (!l.isNumber())
        throw std::invalid_argument("LHS must be a number");
    if (!r.isNumber())
        throw std::invalid_argument("RHS must be a number");

    if (toType(ValueType::Real, r, false).asReal() == 0.0)
        throw std::invalid_argument("Divide by 0");

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

    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("mod", [](Value a, Value b) { return mod(a, b); }, l, r);
    }

    if (!l.isNumber() && !l.isBool())
        throw std::invalid_argument("LHS must be an integer");
    if (!r.isNumber() && !r.isBool())
        throw std::invalid_argument("RHS must be an integer");

    int32_t lhs = toType(ValueType::Int, l, false).asInt();
    int32_t rhs = toType(ValueType::Int, r, false).asInt();
    return intVal(lhs % rhs);
}

void roxal::copyAssign(Value& lhs, const Value& rhs)
{
    if (!lhs.isObj()) {
        lhs = rhs;
        return;
    }

    switch (objType(lhs)) {
        case ObjType::List:
            if (!isList(rhs))
                throw std::invalid_argument("copy assign list requires list RHS");
            asList(lhs)->set(asList(rhs));
            break;
        case ObjType::Dict:
            if (!isDict(rhs))
                throw std::invalid_argument("copy assign dict requires dict RHS");
            asDict(lhs)->set(asDict(rhs));
            break;
        case ObjType::Vector:
            if (!isVector(rhs))
                throw std::invalid_argument("copy assign vector requires vector RHS");
            asVector(lhs)->set(asVector(rhs));
            break;
        case ObjType::Matrix:
            if (!isMatrix(rhs))
                throw std::invalid_argument("copy assign matrix requires matrix RHS");
            asMatrix(lhs)->set(asMatrix(rhs));
            break;
        case ObjType::Signal:
            if (!isSignal(rhs))
                throw std::invalid_argument("copy assign signal requires signal RHS");
            {
                auto lhsSig = asSignal(lhs)->signal;
                auto rhsSig = asSignal(rhs)->signal;

                df::FuncNode::ConstArgMap constArgs;
                std::vector<ptr<df::Signal>> sigArgs{ rhsSig };
                std::vector<std::string> paramNames{"in"};
                std::vector<ptr<df::Signal>> outSignals{ lhsSig };

                auto name = df::DataflowEngine::uniqueFuncName("copyAssignSig");
                auto node = roxal::make_ptr<df::FuncNode>(
                    name,
                    [](const df::Values& vals) -> df::Values { return df::Values{ vals[0] }; },
                    paramNames,
                    constArgs,
                    sigArgs,
                    df::Names{"result"},
                    outSignals);
                node->addToEngine();
                df::DataflowEngine::instance()->markNetworkModified();
                df::DataflowEngine::instance()->evaluate();
            }
            break;
        case ObjType::Instance:
        case ObjType::Actor:
            throw std::runtime_error("copy assignment not supported for user-defined types");
        default:
            // Immutable or unsupported types - behave like normal assignment
            lhs = rhs;
            break;
    }
}


Value roxal::land(Value l, Value r)
{
    if (l.isBool() && r.isBool())
        return boolVal(l.asBool() && r.asBool());

    if (!l.isBool() && !isSignal(l))
        throw std::invalid_argument("LHS must be a bool");
    if (!r.isBool() && !isSignal(r))
        throw std::invalid_argument("RHS must be a bool");

    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("and",
                              [](Value a, Value b) { return land(a, b); },
                              l, r);
    }

    return boolVal(l.asBool() && r.asBool());
}


Value roxal::lor(Value l, Value r)
{
    if (l.isBool() && r.isBool())
        return boolVal(l.asBool() || r.asBool());

    if (!l.isBool() && !isSignal(l))
        throw std::invalid_argument("LHS must be a bool");
    if (!r.isBool() && !isSignal(r))
        throw std::invalid_argument("RHS must be a bool");

    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("or",
                              [](Value a, Value b) { return lor(a, b); },
                              l, r);
    }

    return boolVal(l.asBool() || r.asBool());
}


Value roxal::band(Value l, Value r)
{
    if ((l.isBool() || l.isByte() || l.isInt()) &&
        (r.isBool() || r.isByte() || r.isInt())) {
        if (l.isBool() && r.isBool())
            return boolVal(l.asBool() & r.asBool());

        if (l.isByte() && r.isByte())
            return byteVal(l.asByte(false) & r.asByte(false));

        uint32_t lhs = static_cast<uint32_t>(toType(ValueType::Int, l, false).asInt());
        uint32_t rhs = static_cast<uint32_t>(toType(ValueType::Int, r, false).asInt());
        return intVal(static_cast<int32_t>(lhs & rhs));
    }

    throw std::invalid_argument("Operands must be bool, byte or int");
}

Value roxal::bor(Value l, Value r)
{
    if ((l.isBool() || l.isByte() || l.isInt()) &&
        (r.isBool() || r.isByte() || r.isInt())) {
        if (l.isBool() && r.isBool())
            return boolVal(l.asBool() | r.asBool());

        if (l.isByte() && r.isByte())
            return byteVal(l.asByte(false) | r.asByte(false));

        uint32_t lhs = static_cast<uint32_t>(toType(ValueType::Int, l, false).asInt());
        uint32_t rhs = static_cast<uint32_t>(toType(ValueType::Int, r, false).asInt());
        return intVal(static_cast<int32_t>(lhs | rhs));
    }

    throw std::invalid_argument("Operands must be bool, byte or int");
}

Value roxal::bxor(Value l, Value r)
{
    if ((l.isBool() || l.isByte() || l.isInt()) &&
        (r.isBool() || r.isByte() || r.isInt())) {
        if (l.isBool() && r.isBool())
            return boolVal(l.asBool() ^ r.asBool());

        if (l.isByte() && r.isByte())
            return byteVal(l.asByte(false) ^ r.asByte(false));

        uint32_t lhs = static_cast<uint32_t>(toType(ValueType::Int, l, false).asInt());
        uint32_t rhs = static_cast<uint32_t>(toType(ValueType::Int, r, false).asInt());
        return intVal(static_cast<int32_t>(lhs ^ rhs));
    }

    throw std::invalid_argument("Operands must be bool, byte or int");
}

Value roxal::bnot(Value v)
{
    if (v.isBool())
        return boolVal(!v.asBool());
    if (v.isByte())
        return byteVal(~v.asByte(false));
    if (v.isInt()) {
        uint32_t val = static_cast<uint32_t>(v.asInt());
        return intVal(static_cast<int32_t>(~val));
    }

    throw std::invalid_argument("Operand must be bool, byte or int");
}



Value roxal::greater(Value l, Value r)
{
    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("greater",
                              [](Value a, Value b) { return greater(a, b); },
                              l, r);
    }

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
    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("less",
                              [](Value a, Value b) { return less(a, b); },
                              l, r);
    }

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


Value roxal::equal(Value l, Value r, bool strict)
{
    if (isSignal(l) || isSignal(r)) {
        return signalBinaryOp("equal",
                              [strict](Value a, Value b) { return equal(a, b, strict); },
                              l, r);
    }

    return boolVal(l.equals(r, strict));
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

void roxal::writeValue(std::ostream& out, const Value& v)
{
    enterWriteContext();
    auto guard = std::unique_ptr<int, std::function<void(int*)>>(new int(0), [](int*){ exitWriteContext(); });
    if (isForeignPtr(v))
        throw std::runtime_error("Cannot serialize foreign pointers");

    if (isFuture(v)) {
        Value resolved = v;
        resolved.resolveFuture();
        writeValue(out, resolved);
        return;
    }

    if (v.isBoxed()) {
        uint8_t type = static_cast<uint8_t>(ValueType::Boxed);
        out.write(reinterpret_cast<char*>(&type), 1);
        v.asObj()->write(out);
        return;
    }

    uint8_t type = static_cast<uint8_t>(v.type());
    out.write(reinterpret_cast<char*>(&type), 1);

    switch(v.type()) {
        case ValueType::Nil:
            break;
        case ValueType::Bool: {
            uint8_t b = v.asBool();
            out.write(reinterpret_cast<char*>(&b),1);
            break; }
        case ValueType::Byte: {
            uint8_t b = v.asByte();
            out.write(reinterpret_cast<char*>(&b),1);
            break; }
        case ValueType::Int: {
            int32_t i = v.asInt();
            out.write(reinterpret_cast<char*>(&i),4);
            break; }
        case ValueType::Real: {
            double d = v.asReal();
            out.write(reinterpret_cast<char*>(&d),8);
            break; }
        case ValueType::Enum: {
            int16_t val = v.asEnum();
            uint16_t id = v.enumTypeId();
            out.write(reinterpret_cast<char*>(&val),2);
            out.write(reinterpret_cast<char*>(&id),2);
            break; }
        case ValueType::Type:
            if (v.isObj())
                v.asObj()->write(out);
            else {
                uint8_t tv = static_cast<uint8_t>(v.asType());
                out.write(reinterpret_cast<char*>(&tv),1);
            }
            break;
        case ValueType::String:
        case ValueType::Range:
        case ValueType::List:
        case ValueType::Dict:
        case ValueType::Vector:
        case ValueType::Matrix:
            v.asObj()->write(out);
            break;
        case ValueType::Object:
        case ValueType::Actor:
            v.asObj()->write(out);
            break;
        case ValueType::Function:
        case ValueType::Closure:
        case ValueType::Upvalue:
            v.asObj()->write(out);
            break;
        default:
            throw std::runtime_error("writeValue: unsupported type " + v.typeName());
    }
}

Value roxal::readValue(std::istream& in)
{
    enterReadContext();
    auto guard = std::unique_ptr<int, std::function<void(int*)>>(new int(0), [](int*){ exitReadContext(); });
    uint8_t typeByte;
    if(!in.read(reinterpret_cast<char*>(&typeByte),1))
        throw std::runtime_error("readValue: unable to read type");
    ValueType t = static_cast<ValueType>(typeByte);

    if (t == ValueType::Boxed) {
        auto obj = newObj<ObjPrimitive>(__func__, false);
        obj->read(in);
        return objVal(obj);
    }

    switch(t) {
        case ValueType::Nil:
            return nilVal();
        case ValueType::Bool: {
            uint8_t b; in.read(reinterpret_cast<char*>(&b),1);
            return boolVal(b!=0); }
        case ValueType::Byte: {
            uint8_t b; in.read(reinterpret_cast<char*>(&b),1);
            return byteVal(b); }
        case ValueType::Int: {
            int32_t i; in.read(reinterpret_cast<char*>(&i),4);
            return intVal(i); }
        case ValueType::Real: {
            double d; in.read(reinterpret_cast<char*>(&d),8);
            return realVal(d); }
        case ValueType::Enum: {
            int16_t val; uint16_t id;
            in.read(reinterpret_cast<char*>(&val),2);
            in.read(reinterpret_cast<char*>(&id),2);
            return enumVal(val, id); }
        case ValueType::Type: {
            uint8_t subType;
            in.read(reinterpret_cast<char*>(&subType),1);
            ValueType tv = static_cast<ValueType>(subType);
            if (tv == ValueType::Object || tv == ValueType::Actor || tv == ValueType::Enum || tv == ValueType::Module) {
                in.putback(static_cast<char>(subType));
                if (tv == ValueType::Object || tv == ValueType::Actor || tv == ValueType::Enum) {
                    auto obj = newObj<ObjObjectType>(__func__, icu::UnicodeString(), false, false, false);
                    obj->read(in);
                    return objVal(obj);
                } else { // Module
                    auto obj = newObj<ObjModuleType>(__func__, icu::UnicodeString());
                    obj->read(in);
                    return objVal(obj);
                }
            }
            return typeVal(tv);
        }
        case ValueType::String: {
            auto obj = newObj<ObjString>(__func__);
            obj->read(in);
            return objVal(obj); }
        case ValueType::Range: {
            auto obj = newObj<ObjRange>(__func__);
            obj->read(in);
            return objVal(obj); }
        case ValueType::List: {
            auto obj = newObj<ObjList>(__func__);
            obj->read(in);
            return objVal(obj); }
        case ValueType::Dict: {
            auto obj = newObj<ObjDict>(__func__);
            obj->read(in);
            return objVal(obj); }
        case ValueType::Vector: {
            auto obj = newObj<ObjVector>(__func__);
            obj->read(in);
            return objVal(obj); }
        case ValueType::Matrix: {
            auto obj = newObj<ObjMatrix>(__func__);
            obj->read(in);
            return objVal(obj); }
        case ValueType::Object: {
            uint8_t flag; in.read(reinterpret_cast<char*>(&flag),1);
            uint64_t id; in.read(reinterpret_cast<char*>(&id),8);
            auto* ctx = serializationReadContext();
            if(flag==0) {
                auto it = ctx->idToObj.find(id);
                if(it==ctx->idToObj.end()) throw std::runtime_error("Unknown object ref id");
                return objVal(it->second);
            }
            Value typeVal = readValue(in);
            ObjObjectType* t = asObjectType(typeVal);
            ObjectInstance* obj = objectInstanceVal(t);
            ctx->idToObj[id] = obj;
            uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
            obj->properties.clear();
            for(uint32_t i=0;i<count;i++) {
                int32_t h; in.read(reinterpret_cast<char*>(&h),4);
                obj->properties[h] = readValue(in);
            }
            return objVal(obj); }
        case ValueType::Actor: {
            uint8_t flag; in.read(reinterpret_cast<char*>(&flag),1);
            uint64_t id; in.read(reinterpret_cast<char*>(&id),8);
            auto* ctx = serializationReadContext();
            if(flag==0) {
                auto it = ctx->idToObj.find(id);
                if(it==ctx->idToObj.end()) throw std::runtime_error("Unknown actor ref id");
                return objVal(it->second);
            }
            Value typeVal = readValue(in);
            ObjObjectType* t = asObjectType(typeVal);
            ActorInstance* obj = actorInstanceVal(t);
            ctx->idToObj[id] = obj;
            uint32_t count; in.read(reinterpret_cast<char*>(&count),4);
            obj->properties.clear();
            for(uint32_t i=0;i<count;i++) {
                int32_t h; in.read(reinterpret_cast<char*>(&h),4);
                obj->properties[h] = readValue(in);
            }
            auto newThread = std::make_shared<Thread>();
            newThread->act(objVal(obj));
            return objVal(obj); }
        case ValueType::Function: {
            auto obj = newObj<ObjFunction>(__func__, icu::UnicodeString(), icu::UnicodeString(), icu::UnicodeString());
            obj->read(in);
            return objVal(obj); }
        case ValueType::Closure: {
            auto obj = newObj<ObjClosure>(__func__, nullptr);
            obj->read(in);
            return objVal(obj); }
        case ValueType::Upvalue: {
            auto obj = newObj<ObjUpvalue>(__func__, nullptr);
            obj->read(in);
            return objVal(obj); }
        default:
            throw std::runtime_error("readValue: unsupported type " + std::to_string(static_cast<int>(t)));
    }
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
