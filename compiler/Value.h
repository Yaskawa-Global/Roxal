#pragma once

#include "common.h"

namespace roxal {



enum class ValueType {
    Nil   = 1, // 0001 (both a type and singular value)
    Bool  = 4, // 0100
    Byte,  
    Int,
    Real,
    Decimal,
    Char,
    String,
    List,
    Dict,
    Vector,
    Matrix,
    Tensor,
    Orient,
    Stream,
    Object,
    Actor
};


struct Obj;
struct ObjString;


#if defined(NAN_TAGGING)

const uint64_t QNAN = ((uint64_t)0x7ffc000000000000);

const uint8_t TagNil   = int(ValueType::Nil); 
const uint8_t TagFalse = 2; // 0010
const uint8_t TagTrue  = 3; // 0011

typedef uint64_t Value;

inline Value nilVal() { return ((Value)(uint64_t)(QNAN | TagNil)); }
inline bool isNil(Value v) { return v==nilVal(); }

inline Value trueVal() { return ((Value)(uint64_t)(QNAN | TagTrue )); }
inline Value falseVal() { return ((Value)(uint64_t)(QNAN | TagFalse )); }
inline Value boolVal(bool b) { return b ? trueVal() : falseVal(); }
inline bool isBool(Value v) { return (v|1) == trueVal(); }
inline bool asBool(Value v) { return v == trueVal(); }

inline Value realVal(double v) { return *reinterpret_cast<uint64_t*>(&v); }
inline bool isReal(Value v) { return (v&QNAN) != QNAN; }
inline double asReal(Value v) { return *reinterpret_cast<double*>(&v); }

//inline Value objVal()



#else


class Value {
public:
    Value() : _type(ValueType::Nil) {}

    explicit Value(bool b) : _type(ValueType::Bool) { as.boolean = b; }
    explicit Value(double r) : _type(ValueType::Real) { as.real=r; }
    explicit Value(int32_t i) : _type(ValueType::Int) { as.integer=i; }
    explicit Value(Obj* o);

    Value(const Value& v) 
    {
        // TODO: optimize with union copy (memcpy?)
        _type = v._type;

        switch(_type) {            
            case ValueType::Nil: break;
            case ValueType::Bool: as.boolean = v.as.boolean; break;
            case ValueType::Int: as.integer = v.as.integer; break;
            case ValueType::Real: as.real = v.as.real; break;
            case ValueType::Object: {
                as.obj = v.as.obj;
                incRefObj();
            } break;
            default: throw std::runtime_error("unhandled Value type copy "+typeName());
        }
    }


    Value& operator=(const Value& v)
    {
        if (isObj())
            decRefObj();

        _type = v._type;

        switch(_type) {            
            case ValueType::Nil: break;
            case ValueType::Bool: as.boolean = v.as.boolean; break;
            case ValueType::Int: as.integer = v.as.integer; break;
            case ValueType::Real: as.real = v.as.real; break;
            case ValueType::Object: {
                as.obj = v.as.obj;
                incRefObj();
            } break;
            default: throw std::runtime_error("unhandled Value type copy "+typeName());
        }
        return *this;
    }


    ~Value() {
        if (isObj())
            decRefObj();
    }

    inline ValueType type() const { return _type; }
    std::string typeName() const;

    inline bool isNil() const { return _type == ValueType::Nil; }

    inline bool isBool() const { return _type == ValueType::Bool; }
    inline bool asBool() const { return _type==ValueType::Bool ? as.boolean : false; }

    inline bool isInt() const { return _type == ValueType::Int; }
    int32_t asInt() const;

    inline bool isReal() const { return _type==ValueType::Real; }
    double asReal() const; 

    inline bool isNumber() const { return isInt() || isReal(); } // TODO: || isByte() || isDecimal(v)

    inline bool isObj() const { return _type == ValueType::Object; }
    inline Obj* asObj() const { return as.obj; }

protected:
    ValueType _type;
    union {
        bool boolean;
        double real;
        int32_t integer;
        Obj* obj;
    } as;


    void incRefObj();
    void decRefObj();
};


// value factories
inline Value nilVal() { return Value(); }

inline Value trueVal() { return Value(true); }
inline Value falseVal() { return Value(false); }
inline Value boolVal(bool b) { return Value(b); }

inline Value intVal(int32_t i) { return Value(i); }

inline Value realVal(double r) { return Value(r); }



#endif

Value toType(ValueType t, Value v);
ValueType binaryOpType(Value l, Value r);

bool isFalsey(const Value& v);
bool isTruthy(const Value& v);

bool valuesEqual(Value a, Value b);

Value negate(Value v);
Value add(Value l, Value r);
Value subtract(Value l, Value r);
Value multiply(Value l, Value r);
Value divide(Value l, Value r);

Value land(Value l, Value r);
Value lor(Value l, Value r);

Value greater(Value l, Value r);
Value less(Value l, Value r);

std::string toString(const Value& v);

std::ostream& operator<<(std::ostream& out, const Value& v);


}